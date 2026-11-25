#include "webui.h"
#include "webui_page.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catprinter.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "pagebuf.h"
#include "printserver.h"
#include "settings.h"

static const char *TAG = "webui";

/* ---- minimal HTTP writer ------------------------------------------------ */
static int send_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t off = 0;
    while (off < len) {
        int n = send(fd, p + off, len - off, 0);
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static void http_reply(int fd, int code, const char *type,
                       const char *body, size_t len)
{
    const char *reason = code == 200 ? "OK" :
                         code == 404 ? "Not Found" :
                         code == 400 ? "Bad Request" : "Error";
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
                     "Content-Length: %u\r\nCache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n",
                     code, reason, type, (unsigned)len);
    send_all(fd, hdr, (size_t)n);
    if (len) send_all(fd, body, len);
}

static void send_json(int fd, int code, const char *json)
{
    http_reply(fd, code, "application/json; charset=utf-8",
               json, strlen(json));
}

static void send_ok(int fd, const char *msg)
{
    char b[256];
    snprintf(b, sizeof(b), "{\"ok\":true,\"msg\":\"%s\"}", msg ? msg : "ok");
    send_json(fd, 200, b);
}

static void send_fail(int fd, int code, const char *msg)
{
    char b[256];
    snprintf(b, sizeof(b), "{\"ok\":false,\"msg\":\"%s\"}", msg);
    send_json(fd, code, b);
}

static void json_escape(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (; src && *src && o + 7 < cap; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') {
            dst[o++] = '\\'; dst[o++] = (char)c;
        } else if (c < 0x20) {
            o += (size_t)snprintf(dst + o, cap - o, "\\u%04x", c);
        } else {
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
}

/* ---- state -------------------------------------------------------------- */
/* Print-parameter settings live on the NAS console (Plan B); the device UI
 * only reports WiFi/BLE/system state, which is all a bridge needs. */
static void api_status(int fd)
{
    char *b = (char *)malloc(2048);
    if (b == NULL) { send_fail(fd, 500, "out of memory"); return; }

    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    const char *modestr = mode == WIFI_MODE_AP ? "ap" :
                         mode == WIFI_MODE_APSTA ? "apsta" : "sta";

    char ip[16] = "0.0.0.0";
    esp_netif_t *ni = esp_netif_get_handle_from_ifkey(
        mode == WIFI_MODE_AP ? "WIFI_AP_DEF" : "WIFI_STA_DEF");
    if (ni != NULL) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(ni, &info) == ESP_OK) {
            uint32_t a = info.ip.addr;
            snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
                     (unsigned)(a & 0xff), (unsigned)((a >> 8) & 0xff),
                     (unsigned)((a >> 16) & 0xff), (unsigned)((a >> 24) & 0xff));
        }
    }
    int rssi = 0;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;

    cp_status_t cp;
    cp_get_status(&cp);
    const app_settings_t *s = settings_get();

    char esc[128];
    json_escape(esc, sizeof(esc), s->wifi_ssid);
    char esc_mac[64];
    json_escape(esc_mac, sizeof(esc_mac), s->ble_mac);
    char esc_peer[64];
    json_escape(esc_peer, sizeof(esc_peer), cp_peer_mac());
    const char *fw = esp_get_idf_version();

    snprintf(b, 2048,
        "{\"ok\":true,"
        "\"wifi\":{\"mode\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"connected\":%s},"
        "\"ble\":{\"state\":%d,\"stateName\":\"%s\",\"mac\":\"%s\",\"mtu\":%u,\"bound\":\"%s\",\"ready\":%s},"
        "\"print\":{\"active\":%s,\"lastJob\":%u},"
        "\"sys\":{\"heap\":%u,\"uptime\":%u,\"fw\":\"%s\"}}",
        modestr, esc, ip, rssi,
        strcmp(ip, "0.0.0.0") == 0 ? "false" : "true",
        (int)cp.state, settings_ble_state_name((int)cp.state), esc_peer,
        (unsigned)cp.mtu, esc_mac, cp_is_ready() ? "true" : "false",
        ps_job_active() ? "true" : "false", (unsigned)ps_last_job_id(),
        (unsigned)esp_get_free_heap_size(),
        (unsigned)(esp_timer_get_time() / 1000000), fw);
    send_json(fd, 200, b);
    free(b);
}

/* ---- scans -------------------------------------------------------------- */
static void api_wifi_scan(int fd)
{
    wifi_scan_config_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.show_hidden = false;
    esp_err_t e = esp_wifi_scan_start(&sc, true);
    if (e != ESP_OK) {
        char m[96];
        snprintf(m, sizeof(m), "scan failed: %s", esp_err_to_name(e));
        send_fail(fd, 500, m);
        return;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 20) n = 20;
    wifi_ap_record_t *recs = n ? (wifi_ap_record_t *)calloc(n, sizeof(wifi_ap_record_t)) : NULL;
    if (n && recs != NULL) esp_wifi_scan_get_ap_records(&n, recs);

    char *b = (char *)malloc(3072);
    if (b == NULL) { free(recs); esp_wifi_clear_ap_list(); send_fail(fd, 500, "out of memory"); return; }
    size_t o = (size_t)snprintf(b, 3072, "{\"ok\":true,\"aps\":[");
    char esc[80];
    for (uint16_t i = 0; i < n && recs != NULL; i++) {
        json_escape(esc, sizeof(esc), (const char *)recs[i].ssid);
        o += (size_t)snprintf(b + o, 3072 - o,
            "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%u,\"bssid\":\"%02x%02x%02x%02x%02x%02x\"}",
            i ? "," : "", esc, recs[i].rssi, (unsigned)recs[i].authmode,
            recs[i].bssid[0], recs[i].bssid[1], recs[i].bssid[2],
            recs[i].bssid[3], recs[i].bssid[4], recs[i].bssid[5]);
    }
    snprintf(b + o, 3072 - o, "]}");
    send_json(fd, 200, b);
    free(b);
    free(recs);
    esp_wifi_clear_ap_list();
}

static void api_printer_scan(int fd)
{
    cp_device_t devs[CP_SCAN_MAX_DEVICES];
    size_t n = 0;
    esp_err_t e = cp_scan_devices(3000, devs, CP_SCAN_MAX_DEVICES, &n);
    if (e != ESP_OK) {
        send_fail(fd, 500, e == ESP_ERR_INVALID_STATE ? "BLE busy, retry later" : "BLE scan failed");
        return;
    }
    char *b = (char *)malloc(2048);
    if (b == NULL) { send_fail(fd, 500, "out of memory"); return; }
    size_t o = (size_t)snprintf(b, 2048, "{\"ok\":true,\"devices\":[");
    char esc[80];
    bool first = true;
    /* A connected cat printer usually stops advertising, so put the current
     * one at the top - otherwise it is impossible to bind it from the UI. */
    const char *cur = cp_peer_mac();
    if (cur != NULL && cur[0] != '\0') {
        char nm[64];
        snprintf(nm, sizeof(nm), "%s (当前连接)", settings_get()->ble_prefix);
        json_escape(esc, sizeof(esc), nm);
        o += (size_t)snprintf(b + o, 2048 - o,
            "{\"name\":\"%s\",\"mac\":\"%s\",\"type\":%u,\"rssi\":0}",
            esc, cur, (unsigned)cp_peer_addr_type());
        first = false;
    }
    for (size_t i = 0; i < n; i++) {
        json_escape(esc, sizeof(esc), devs[i].name);
        o += (size_t)snprintf(b + o, 2048 - o,
            "%s{\"name\":\"%s\",\"mac\":\"%s\",\"type\":%u,\"rssi\":%d}",
            first ? "" : ",", esc, devs[i].mac, devs[i].addr_type, devs[i].rssi);
        first = false;
    }
    snprintf(b + o, 2048 - o, "]}");
    send_json(fd, 200, b);
    free(b);
}

/* ---- form helpers ------------------------------------------------------- */
static void form_value(const char *body, size_t len, const char *key,
                       char *out, size_t cap)
{
    out[0] = '\0';
    size_t p = 0;
    while (p < len) {
        size_t e = p;
        while (e < len && body[e] != '&') e++;
        size_t plen = e - p;
        const char *eq = (const char *)memchr(body + p, '=', plen);
        if (eq) {
            size_t klen = (size_t)(eq - (body + p));
            if (klen == strlen(key) && memcmp(body + p, key, klen) == 0) {
                size_t vlen = plen - klen - 1;
                if (vlen >= cap) vlen = cap - 1;
                memcpy(out, eq + 1, vlen);
                out[vlen] = '\0';
                return;
            }
        }
        p = e + 1;
    }
}

static void reboot_task(void *arg)
{
    uint32_t ms = (uint32_t)(uintptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(ms));
    esp_restart();
}

static void schedule_reboot(uint32_t ms)
{
    /* Let the HTTP response reach the browser before the socket dies. */
    if (xTaskCreate(reboot_task, "webreboot", 2048,
                    (void *)(uintptr_t)ms, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "cannot schedule reboot");
    }
}

/* ---- actions ------------------------------------------------------------ */
/* NOTE: there is no /api/settings endpoint any more. Print parameters
 * (algorithm/darkness/speed/feed) are owned by the NAS console and live in
 * /etc/catbridge.conf there; the firmware values in NVS only serve the
 * legacy /print URF path and its defaults. */

static void api_wifi_connect(int fd, const char *body, size_t len)
{
    app_settings_t s;
    settings_snapshot(&s);
    settings_apply_form_into(&s, body, len);
    if (s.wifi_ssid[0] == '\0') { send_fail(fd, 400, "SSID 不能为空"); return; }
    esp_err_t e = settings_set_wifi(s.wifi_ssid, s.wifi_pass, s.wifi_bssid);
    if (e != ESP_OK) { send_fail(fd, 500, "NVS write failed"); return; }
    send_ok(fd, "WiFi 已保存，设备即将重启并连接新网络");
    schedule_reboot(1500);
}

static void api_wifi_forget(int fd)
{
    settings_forget_wifi();
    send_ok(fd, "已清除 WiFi，重启后进入配网模式");
    schedule_reboot(1500);
}

static void api_printer_connect(int fd, const char *body, size_t len)
{
    char mac[32] = "", type[8] = "";
    form_value(body, len, "mac", mac, sizeof(mac));
    form_value(body, len, "type", type, sizeof(type));
    /* Strip separators so both AA:BB:.. and AABB.. are accepted. */
    char clean[SETTINGS_MAC_LEN + 1];
    size_t n = 0;
    for (size_t i = 0; i < strlen(mac) && n < SETTINGS_MAC_LEN; i++) {
        char c = mac[i];
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
            clean[n++] = c;
        }
    }
    clean[n] = '\0';
    if (n != SETTINGS_MAC_LEN) { send_fail(fd, 400, "MAC 格式不对"); return; }

    app_settings_t s;
    settings_snapshot(&s);
    memcpy(s.ble_mac, clean, sizeof(clean));
    s.ble_addr_type = (uint8_t)atoi(type);
    settings_replace(&s);

    esp_err_t e = cp_connect_mac(clean, s.ble_addr_type);
    if (e != ESP_OK) { send_fail(fd, 500, "连接请求失败"); return; }
    send_ok(fd, "已绑定，正在连接该打印机");
}

static void api_printer_disconnect(int fd)
{
    esp_err_t e = cp_disconnect();
    if (e != ESP_OK) { send_fail(fd, 400, "当前没有连接"); return; }
    send_ok(fd, "已断开，稍后会自动重连已绑定的打印机");
}

static void api_printer_unbind(int fd)
{
    app_settings_t s;
    settings_snapshot(&s);
    s.ble_mac[0] = '\0';
    settings_replace(&s);
    send_ok(fd, "已解除绑定，恢复按名称前缀自动扫描");
}

static void api_bind_current(int fd)
{
    const char *mac = cp_peer_mac();
    if (mac == NULL || mac[0] == '\0') {
        send_fail(fd, 400, "猫打印机未连接");
        return;
    }
    app_settings_t s;
    settings_snapshot(&s);
    memcpy(s.ble_mac, mac, sizeof(s.ble_mac));
    s.ble_addr_type = cp_peer_addr_type();
    settings_replace(&s);
    ESP_LOGI(TAG, "bound current printer %s type=%u", mac, s.ble_addr_type);
    send_ok(fd, "已绑定当前打印机，开机会优先直连");
}

static void api_test_print(int fd)
{
    if (!cp_is_ready()) { send_fail(fd, 400, "猫打印机未连接"); return; }
    if (ps_job_active()) { send_fail(fd, 400, "正在打印，请稍候"); return; }
    cp_status_t st;
    cp_get_status(&st);
    if (st.state == CP_STATE_PRINTING) { send_fail(fd, 400, "打印机忙"); return; }

    const size_t rows_n = 64;
    uint8_t *rows = (uint8_t *)malloc(rows_n * RASTER_ROW_BYTES);
    if (rows == NULL) { send_fail(fd, 500, "out of memory"); return; }
    memset(rows, 0, rows_n * RASTER_ROW_BYTES);
    for (size_t r = 0; r < rows_n; r++) {
        uint8_t *row = rows + r * RASTER_ROW_BYTES;
        if (r < 3) {
            memset(row, 0xff, RASTER_ROW_BYTES);            /* black bar */
        } else if (r < 6) {
            row[0] = 0xff; row[RASTER_ROW_BYTES - 1] = 0xff; /* edges */
        } else if (r < 22) {
            memset(row, (r & 1) ? 0xaa : 0x55, RASTER_ROW_BYTES);
        } else if (r < 38) {
            memset(row, 0xf0, RASTER_ROW_BYTES);
        } else if (r < 56) {
            /* density ramp: more dots as r increases */
            for (size_t x = 0; x < RASTER_WIDTH_PX; x++) {
                if ((x + r) % (20 - (r - 38) / 2) < 2) {
                    row[x >> 3] |= (uint8_t)(0x80u >> (x & 7));
                }
            }
        } else {
            row[0] = 0xff; row[RASTER_ROW_BYTES - 1] = 0xff;
        }
    }
    /* Honour the same orientation setting as real AirPrint jobs. */
    page_buf_t pb = { .rows = rows, .nrows = rows_n, .capacity_rows = rows_n };
    pb_apply_orientation(&pb, settings_get()->orientation);
    esp_err_t e = cp_print_page(rows, rows_n, 60000);
    free(rows);
    if (e != ESP_OK) { send_fail(fd, 500, "测试打印失败"); return; }
    send_ok(fd, "测试页已打印");
}

/* ---- dispatch ----------------------------------------------------------- */
void webui_serve(int fd, const char *path)
{
    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        http_reply(fd, 200, "text/html; charset=utf-8",
                   WEBUI_HTML, strlen(WEBUI_HTML));
    } else if (strcmp(path, "/api/status") == 0) {
        api_status(fd);
    } else if (strcmp(path, "/api/wifi/scan") == 0) {
        api_wifi_scan(fd);
    } else if (strcmp(path, "/api/printer/scan") == 0) {
        api_printer_scan(fd);
    } else if (strcmp(path, "/favicon.ico") == 0) {
        http_reply(fd, 200, "image/x-icon", "", 0);
    } else {
        send_fail(fd, 404, "not found");
    }
}

void webui_api(int fd, const char *path, const char *body, size_t body_len)
{
    if (strcmp(path, "/api/wifi/connect") == 0) {
        api_wifi_connect(fd, body, body_len);
    } else if (strcmp(path, "/api/wifi/forget") == 0) {
        api_wifi_forget(fd);
    } else if (strcmp(path, "/api/printer/connect") == 0) {
        api_printer_connect(fd, body, body_len);
    } else if (strcmp(path, "/api/printer/disconnect") == 0) {
        api_printer_disconnect(fd);
    } else if (strcmp(path, "/api/printer/unbind") == 0) {
        api_printer_unbind(fd);
    } else if (strcmp(path, "/api/printer/bind-current") == 0) {
        api_bind_current(fd);
    } else if (strcmp(path, "/api/test-print") == 0) {
        api_test_print(fd);
    } else if (strcmp(path, "/api/reboot") == 0) {
        send_ok(fd, "正在重启");
        schedule_reboot(800);
    } else if (strcmp(path, "/api/factory-reset") == 0) {
        settings_factory_reset();
        send_ok(fd, "已恢复出厂设置，正在重启");
        schedule_reboot(800);
    } else {
        send_fail(fd, 404, "not found");
    }
}
