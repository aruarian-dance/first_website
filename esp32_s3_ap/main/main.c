#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char *TAG = "S3_TELEMETRY";
static char last_message[256] = "ESP32-S3 ready";
static portMUX_TYPE msg_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t payload[2048];

static const char web_page[] =
"<!doctype html><html lang='ko'><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32-S3 TCP Test</title>"
"<style>body{font:16px sans-serif;max-width:650px;margin:24px auto;padding:0 14px}"
"button,select,input{font-size:16px;padding:10px;margin:5px 2px}"
"button{background:#125abc;color:white;border:0;border-radius:6px}"
"pre{background:#eee;white-space:pre-wrap;padding:12px;border-radius:8px}</style></head>"
"<body><h2>ESP32-S3 Wi-Fi / TCP Test</h2>"
"<p>AP: ESP32_S3_TEST | IP: 192.168.4.1</p>"
"<h3>1. Message TX/RX</h3>"
"<input id='msg' placeholder='Hello ESP32!' maxlength='200'>"
"<button onclick='sendMsg()'>Send</button>"
"<button onclick='readMsg()'>Read from ESP</button>"
"<h3>2. TCP / HTTP throughput</h3>"
"<select id='n'><option value='262144'>256 KiB</option>"
"<option value='524288' selected>512 KiB</option>"
"<option value='1048576'>1 MiB</option></select>"
"<button onclick='benchUp()'>Phone to ESP (UPLOAD)</button>"
"<button onclick='benchDown()'>ESP to Phone (DOWNLOAD)</button>"
"<p>USB serial monitor: 115200 baud. This measures HTTP/TCP application data throughput, not Wi-Fi PHY speed.</p>"
"<pre id='result'>Ready</pre>"
"<script>"
"const el=document.getElementById('result');"
"function report(x){el.textContent=x+'\\n'+el.textContent.slice(0,2000)}"
"function speed(bytes,ms){return (8*bytes/(Math.max(ms,1)*1000)).toFixed(2)}"
"async function sendMsg(){try{let s=document.getElementById('msg').value;"
"let t=performance.now();let r=await fetch('/message',{method:'POST',body:s});"
"if(!r.ok)throw Error('HTTP '+r.status);"
"report('TX / RX: '+await r.text()+' | RTT '+(performance.now()-t).toFixed(1)+' ms')}"
"catch(e){report('ERROR '+e)}}"
"async function readMsg(){try{let r=await fetch('/message');report('ESP stored: '+await r.text())}"
"catch(e){report('ERROR '+e)}}"
"async function benchUp(){try{let n=+document.getElementById('n').value;"
"let a=new Uint8Array(n);for(let i=0;i<n;i++)a[i]=i%251;"
"let t=performance.now();let r=await fetch('/upload',{method:'POST',"
"headers:{'Content-Type':'application/octet-stream'},body:a});"
"if(!r.ok)throw Error('HTTP '+r.status);let server=await r.json();"
"let ms=performance.now()-t;"
"report('PHONE -> ESP TCP/HTTP RX: '+n+' bytes, '+ms.toFixed(1)+' ms, '+speed(n,ms)+' Mbps (browser end-to-end); ESP receive loop '+server.server_mbps+' Mbps')}"
"catch(e){report('ERROR '+e)}}"
"async function benchDown(){try{let n=+document.getElementById('n').value;"
"let t=performance.now();let r=await fetch('/download?n='+n,{cache:'no-store'});"
"if(!r.ok)throw Error('HTTP '+r.status);let a=await r.arrayBuffer();"
"let ms=performance.now()-t;"
"report('ESP -> PHONE TCP/HTTP TX: '+a.byteLength+' bytes, '+ms.toFixed(1)+' ms, '+speed(a.byteLength,ms)+' Mbps (browser end-to-end)')}"
"catch(e){report('ERROR '+e)}}"
"</script></body></html>";

static void peer_str(httpd_req_t *req, char *out, size_t len) {
    struct sockaddr_in addr = {0};
    socklen_t alen = sizeof(addr);
    int fd = httpd_req_to_sockfd(req);
    if (fd >= 0 && getpeername(fd, (struct sockaddr *)&addr, &alen) == 0) {
        char ip[20] = {0};
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        snprintf(out, len, "%s:%u", ip, (unsigned)ntohs(addr.sin_port));
    } else {
        snprintf(out, len, "unknown");
    }
}

static void wifi_events(void *arg, esp_event_base_t base, int32_t event_id, void *data) {
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "WIFI CLIENT CONNECTED MAC=%02X:%02X:%02X:%02X:%02X:%02X AID=%d",
            e->mac[0],e->mac[1],e->mac[2],e->mac[3],e->mac[4],e->mac[5],e->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
        ESP_LOGI(TAG, "WIFI CLIENT DISCONNECTED MAC=%02X:%02X:%02X:%02X:%02X:%02X",
            e->mac[0],e->mac[1],e->mac[2],e->mac[3],e->mac[4],e->mac[5]);
    }
}

static void http_events(void *arg, esp_event_base_t base, int32_t event_id, void *data) {
    if (event_id == HTTP_SERVER_EVENT_ON_CONNECTED) {
        ESP_LOGI(TAG, "TCP CONNECT fd=%d (HTTP server port 80)", *(int*)data);
    } else if (event_id == HTTP_SERVER_EVENT_DISCONNECTED) {
        ESP_LOGI(TAG, "TCP DISCONNECT fd=%d", *(int*)data);
    }
}

static esp_err_t root_get(httpd_req_t *req) {
    char peer[48]; peer_str(req, peer, sizeof(peer));
    ESP_LOGI(TAG, "HTTP/TCP RX GET / from %s", peer);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t e = httpd_resp_send(req, web_page, HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "HTTP/TCP TX webpage %u bytes to %s (%s)",
        (unsigned)strlen(web_page), peer, e == ESP_OK ? "OK" : "FAIL");
    return e;
}

static esp_err_t msg_get(httpd_req_t *req) {
    char local[sizeof(last_message)];
    portENTER_CRITICAL(&msg_lock);
    memcpy(local, last_message, sizeof(local));
    portEXIT_CRITICAL(&msg_lock);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    esp_err_t e = httpd_resp_send(req, local, HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "HTTP/TCP TX GET /message reply %u bytes status=%s",
        (unsigned)strlen(local), e == ESP_OK ? "OK" : "FAIL");
    return e;
}

static esp_err_t msg_post(httpd_req_t *req) {
    if (req->content_len <= 0 || req->content_len >= sizeof(last_message)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Message must be 1-255 bytes");
        return ESP_FAIL;
    }
    char local[sizeof(last_message)] = {0};
    int pos = 0;
    int64_t t0 = esp_timer_get_time();
    while (pos < req->content_len) {
        int r = httpd_req_recv(req, local + pos, req->content_len - pos);
        if (r <= 0) return ESP_FAIL;
        pos += r;
    }
    local[pos] = 0;
    portENTER_CRITICAL(&msg_lock);
    memcpy(last_message, local, sizeof(last_message));
    portEXIT_CRITICAL(&msg_lock);
    char peer[48]; peer_str(req, peer, sizeof(peer));
    ESP_LOGI(TAG, "HTTP/TCP RX /message from %s: %d bytes, %lld us, text='%s'",
             peer, pos, (long long)(esp_timer_get_time() - t0), local);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    esp_err_t e = httpd_resp_send(req, local, HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "HTTP/TCP TX /message echo to %s: %d bytes status=%s",
             peer, pos, e == ESP_OK ? "OK" : "FAIL");
    return e;
}

static esp_err_t upload_post(httpd_req_t *req) {
    int remaining = req->content_len;
    if (remaining < 1 || remaining > (1024 * 1024)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Upload must be 1..1048576 bytes");
        return ESP_FAIL;
    }
    uint8_t chunk[2048];
    size_t total = 0;
    int64_t t0 = esp_timer_get_time();
    while (remaining > 0) {
        int want = remaining > (int)sizeof(chunk) ? sizeof(chunk) : remaining;
        int got = httpd_req_recv(req, (char *)chunk, want);
        if (got <= 0) {
            ESP_LOGE(TAG, "HTTP/TCP RX /upload failed after %u bytes", (unsigned)total);
            return ESP_FAIL;
        }
        total += got;
        remaining -= got;
    }
    int64_t usec = esp_timer_get_time() - t0;
    double mbps = (total * 8.0) / (usec > 0 ? usec : 1);
    char peer[48]; peer_str(req, peer, sizeof(peer));
    ESP_LOGI(TAG, "BENCH TCP/HTTP RX from %s: %u bytes, %lld us, %.3f Mbps (receive loop)",
            peer, (unsigned)total, (long long)usec, mbps);
    char response[128];
    snprintf(response, sizeof(response), "{\"bytes\":%u,\"server_us\":%lld,\"server_mbps\":%.3f}",
             (unsigned)total, (long long)usec, mbps);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t download_get(httpd_req_t *req) {
    char query[48] = {0}, nstr[24] = {0};
    size_t n = 524288;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "n", nstr, sizeof(nstr)) == ESP_OK) {
        n = strtoul(nstr, NULL, 10);
    }
    if (n < 1 || n > (1024 * 1024)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Download must be 1..1048576 bytes");
        return ESP_FAIL;
    }
    char peer[48]; peer_str(req, peer, sizeof(peer));
    ESP_LOGI(TAG, "HTTP/TCP RX GET /download from %s requested=%u bytes",peer,(unsigned)n);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    int64_t t0 = esp_timer_get_time();
    size_t sent = 0;
    while (sent < n) {
        size_t k = n - sent < sizeof(payload) ? n - sent : sizeof(payload);
        if (httpd_resp_send_chunk(req, (const char *)payload, k) != ESP_OK) {
            ESP_LOGE(TAG, "BENCH TCP/HTTP TX failed at %u/%u bytes",
                     (unsigned)sent,(unsigned)n);
            return ESP_FAIL;
        }
        sent += k;
    }
    esp_err_t done = httpd_resp_send_chunk(req, NULL, 0);
    int64_t usec = esp_timer_get_time() - t0;
    double mbps = (sent * 8.0) / (usec > 0 ? usec : 1);
    ESP_LOGI(TAG, "BENCH TCP/HTTP TX to %s: %u bytes, %lld us, %.3f Mbps (send loop), status=%s",
             peer,(unsigned)sent,(long long)usec,mbps,done == ESP_OK ? "OK" : "FAIL");
    return done;
}

static void start_web(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(esp_event_handler_register(ESP_HTTP_SERVER_EVENT, ESP_EVENT_ANY_ID, &http_events, NULL));
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));
    httpd_uri_t routes[] = {
        { .uri="/",         .method=HTTP_GET,  .handler=root_get },
        { .uri="/message",  .method=HTTP_GET,  .handler=msg_get },
        { .uri="/message",  .method=HTTP_POST, .handler=msg_post },
        { .uri="/upload",   .method=HTTP_POST, .handler=upload_post },
        { .uri="/download", .method=HTTP_GET,  .handler=download_get },
    };
    for (size_t i=0;i<sizeof(routes)/sizeof(routes[0]);i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    }
}

void app_main(void) {
    for(size_t i=0;i<sizeof(payload);i++) payload[i] = (uint8_t)(i % 251);
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_events, NULL));
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    wifi_config_t ap = { .ap = { .ssid = "ESP32_S3_TEST", .ssid_len = 0,
        .password = "12345678", .channel = 1, .max_connection = 4,
        .authmode = WIFI_AUTH_WPA2_PSK, .pmf_cfg = { .required = false } } };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    start_web();
    ESP_LOGI(TAG, "FW STARTED: USB Serial/JTAG console, Wi-Fi AP ESP32_S3_TEST");
    ESP_LOGI(TAG, "AP http://192.168.4.1 | HTTP/TCP port 80 | USB COM 115200");
    ESP_LOGI(TAG, "Message logs + TCP connect/disconnect + 256 KiB/512 KiB/1 MiB throughput test");
}
