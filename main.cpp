#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "driver/gpio.h"
#include "esp_http_server.h"
#include "lws/lws.h"
#include "lws/lws_display.h"
#include "lws/lws_html_parser.h"

#define FB_WIDTH 320
#define FB_HEIGHT 240
#define VNC_PORT 5900

static const char *TAG = "LHP_Browser";
static uint16_t framebuffer[FB_WIDTH * FB_HEIGHT];  // RGB565
static bool fb_dirty = true;

static struct lws_context_creation_info lws_info;
static struct lws_context *lws_ctx;
static struct lws_display_render_state rs;
static char current_url[512] = "https://httpbin.org/html";  // Test page with CSS

static int vnc_sock = -1;
static int vnc_client = -1;
static esp_netif_t *ap_netif = NULL;
static esp_netif_t *sta_netif = NULL;
static httpd_handle_t http_server = NULL;

// === FRAMEBUFFER & RENDER ===
static void fb_clear(uint16_t color) {
    memset(framebuffer, color >> 8, sizeof(framebuffer));  // Blue-ish
}

static int lhp_render_cb(struct lws_display_render_state_t *rs, int x, int y, int w, void *priv, uint8_t *linebuf) {
    if (y >= FB_HEIGHT || x >= FB_WIDTH) return 0;
    int pixels = (w < FB_WIDTH - x) ? w : FB_WIDTH - x;
    uint16_t *fb_line = framebuffer + y * FB_WIDTH + x;
    uint16_t *src = (uint16_t*)linebuf;
    for (int i = 0; i < pixels; i++) {
        fb_line[i] = src[i];
    }
    fb_dirty = true;
    return 0;
}

// === LHP BROWSER ENGINE ===
static void browser_load_url(const char *url) {
    strncpy(current_url, url, sizeof(current_url) - 1);
    ESP_LOGI(TAG, "Loading %s", url);
    fb_clear(0xF800);  // Red loading screen
    
    // LHP stream browse - fetches, parses HTML/CSS, renders line-by-line
    lws_lhp_ss_browse(lws_ctx, &rs, url, lhp_render_cb, NULL, LHP_SS_BROWSE_FLAGS_HTTP2_ACCEPT);
}

// === VNC SERVER ===
static void vnc_handshake(int client) {
    // RFB protocol 3.8
    const char *proto = "RFB 003.008\n";
    send(client, proto, 12, 0);
    
    // Security: none
    uint32_t sec_none = htonl(1);
    send(client, (uint8_t*)&sec_none, 4, 0);
    
    // Security result OK
    uint32_t ok = htonl(0);
    send(client, (uint8_t*)&ok, 4, 0);
    
    // ClientInit shared=false
    char shared = 0;
    recv(client, &shared, 1, 100);
    send(client, &shared, 1, 0);
    
    // ServerInit: name, width, height, pixelformat
    uint32_t name_len = htonl(2);  // "LHP"
    send(client, (uint8_t*)&name_len, 4, 0);
    send(client, "LHP", 3, 0);
    uint16_t w = htons(FB_WIDTH), h = htons(FB_HEIGHT);
    send(client, (uint8_t*)&w, 2, 0);
    send(client, (uint8_t*)&h, 2, 0);
    
    // PixelFormat: 16-bit RGB565 raw
    uint8_t pf[20] = {0, 16,  // bitsPerPixel, depth
                      1, 0,  // big endian false
                      1,  // true color
                      16, 8, 0, 0, 0,  // rMax, gMax, bMax
                      11, 5, 0};       // rPos, gPos, bPos
    send(client, pf, 20, 0);
}

static void vnc_task(void *arg) {
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(VNC_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };
    
    vnc_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    bind(vnc_sock, (struct sockaddr*)&addr, sizeof(addr));
    listen(vnc_sock, 2);
    ESP_LOGI(TAG, "VNC listening on port %d", VNC_PORT);
    
    uint8_t buf[256];
    while (1) {
        if (vnc_client <= 0) {
            vnc_client = accept(vnc_sock, NULL, NULL);
            if (vnc_client > 0) {
                ESP_LOGI(TAG, "VNC client connected");
                vnc_handshake(vnc_client);
            }
        }
        
        if (vnc_client > 0 && fb_dirty) {
            // Full framebuffer update
            uint8_t update[10] = {3,  // FramebufferUpdate
                                  0, 1,  // padding, 1 rect
                                  0, 0, 0,  // x,y
                                  FB_WIDTH >> 8, FB_WIDTH & 0xFF,
                                  FB_HEIGHT >> 8, FB_HEIGHT & 0xFF,
                                  0};  // raw encoding
            send(vnc_client, update, 10, 0);
            send(vnc_client, (uint8_t*)framebuffer, sizeof(framebuffer), 0);
            fb_dirty = false;
        }
        
        // Handle input
        if (vnc_client > 0 && recv(vnc_client, buf, sizeof(buf), MSG_DONTWAIT) > 0) {
            if (buf[0] == 3) {  // PointerEvent
                uint16_t x = (buf[2] << 8) | buf[3];
                uint16_t y = (buf[5] << 8) | buf[6];
                if (x < FB_WIDTH && y < FB_HEIGHT) {
                    framebuffer[y * FB_WIDTH + x] = 0xFFFF;  // White cursor
                    fb_dirty = true;
                }
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// === HTTP SERVER for Navigation ===
static esp_err_t http_get_handler(httpd_req_t *req) {
    char url_param[512] = {0};
    size_t query_len = httpd_req_get_url_query_str(req, url_param, sizeof(url_param) - 1);
    if (query_len && strstr(url_param, "url=")) {
        char *url_start = strstr(url_param, "url=") + 4;
        char *amp = strchr(url_start, '&');
        if (amp) *amp = 0;
        browser_load_url(url_start);
        httpd_resp_send(req, "Loading in VNC...", -1);
    } else {
        const char *resp = 
            "<html><body>"
            "<h1>ESP32-C3 LHP Browser</h1>"
            "<form method='GET'><input name='url' placeholder='https://example.com' style='width:300px'>"
            "<input type='submit' value='Browse'></form>"
            "<p>VNC: vnc://" ADDR_STR ":5900 (RGB565)</p>"
            "</body></html>";
        httpd_resp_send(req, resp, -1);
    }
    return ESP_OK;
}

static httpd_uri_t uri_get = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = http_get_handler
};

// === WIFI ===
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGI(TAG,"retry to connect to the AP");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "STA IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static void start_wifi(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    sta_netif = esp_netif_create_default_wifi_sta();
    ap_netif = esp_netif_create_default_wifi_ap();

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t sta_config = {
        .sta = {
            .ssid = "YourHomeWiFi",
            .password = "yourpass",
        },
    };
    wifi_config_t ap_config = {
        .ap = {
            .ssid = "ESP32C3-LHP",
            .password = "12345678",
            .ssid_len = strlen("ESP32C3-LHP"),
            .channel = 1,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .max_connection = 4,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(TAG, "AP SSID: ESP32C3-LHP");
}

// === MAIN ===
void app_main(void) {
    ESP_LOGI(TAG, "Starting LHP Browser + VNC");

    // NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // WiFi
    start_wifi();

    // HTTP Server
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    httpd_start(&http_server, &config);
    httpd_register_uri_handler(http_server, &uri_get);

    // Libwebsockets LHP context
    memset(&lws_info, 0, sizeof(lws_info));
    lws_info.port = CONTEXT_PORT_NO_LISTEN;
    lws_info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT | LWS_SERVER_OPTION_EXPLICIT_VHOSTS;
    lws_ctx = lws_create_context(&lws_info);

    // Virtual display for LHP (320x240 RGB565)
    lws_display_init(lws_ctx, &rs, NULL, 0, FB_WIDTH, FB_HEIGHT, 16, "rgb565");

    // VNC task
    xTaskCreate(vnc_task, "vnc", 8192, NULL, 5, NULL);

    // Initial load
    browser_load_url(current_url);

    ESP_LOGI(TAG, "Ready! HTTP:80 VNC:5900");
    
    while (1) {
        lws_service(lws_ctx, 0);  // LHP callbacks
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
