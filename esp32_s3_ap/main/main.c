#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "S3_AP";
static char last_message[256] = "Hello from ESP32-S3";
static portMUX_TYPE msg_lock = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t root_get(httpd_req_t *req) {
    const char *html = "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'></head><body><h2>ESP32-S3 Wi-Fi Test</h2><input id='msg' placeholder='Message'><button onclick='send()'>Send</button><p id='out'></p><script>async function send(){let v=document.getElementById('msg').value;let r=await fetch('/message',{method:'POST',body:v});document.getElementById('out').textContent=await r.text()}async function poll(){try{let r=await fetch('/message');document.getElementById('out').textContent=await r.text()}catch(e){}}setInterval(poll,2000);poll();</script></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}
static esp_err_t msg_get(httpd_req_t *req) {
    char local[sizeof(last_message)];
    portENTER_CRITICAL(&msg_lock);
    memcpy(local, last_message, sizeof(local));
    portEXIT_CRITICAL(&msg_lock);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, local, HTTPD_RESP_USE_STRLEN);
}
static esp_err_t msg_post(httpd_req_t *req) {
    if (req->content_len <= 0 || req->content_len >= sizeof(last_message)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Message must be 1-255 bytes");
        return ESP_FAIL;
    }
    char local[sizeof(last_message)] = {0};
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, local + got, req->content_len - got);
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    local[got] = 0;
    portENTER_CRITICAL(&msg_lock);
    memcpy(last_message, local, sizeof(last_message));
    portEXIT_CRITICAL(&msg_lock);
    ESP_LOGI(TAG, "Received: %s", local);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, local, HTTPD_RESP_USE_STRLEN);
}
static void start_web(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));
    httpd_uri_t root = {.uri="/", .method=HTTP_GET, .handler=root_get};
    httpd_uri_t get = {.uri="/message", .method=HTTP_GET, .handler=msg_get};
    httpd_uri_t post = {.uri="/message", .method=HTTP_POST, .handler=msg_post};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &post));
}
void app_main(void) {
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    wifi_config_t ap = { .ap = { .ssid = "ESP32_S3_TEST", .ssid_len = 0, .password = "12345678", .channel = 1, .max_connection = 4, .authmode = WIFI_AUTH_WPA2_PSK, .pmf_cfg = {.required = false} } };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    start_web();
    ESP_LOGI(TAG, "Connect to ESP32_S3_TEST then open http://192.168.4.1");
}
