#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "settings";
static const char *NS = "catprint";
static nvs_handle_t s_nvs = 0;
static app_settings_t s_cfg;

/* ---- defaults come from the existing Kconfig, so a fresh device behaves
 * exactly like the old compiled-in firmware. ------------------------------- */
static void load_defaults(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    strncpy(s_cfg.wifi_ssid, CONFIG_AIRPRINT_WIFI_SSID, SETTINGS_SSID_MAX);
    strncpy(s_cfg.wifi_pass, CONFIG_AIRPRINT_WIFI_PASSWORD, SETTINGS_PASS_MAX);
    strncpy(s_cfg.wifi_bssid, CONFIG_AIRPRINT_WIFI_BSSID, sizeof(s_cfg.wifi_bssid) - 1);
    strncpy(s_cfg.airprint_name, CONFIG_AIRPRINT_PRINTER_NAME, SETTINGS_NAME_MAX);
    strncpy(s_cfg.ble_prefix, CONFIG_CATPRINTER_BLE_NAME_PREFIX,
            sizeof(s_cfg.ble_prefix) - 1);
    s_cfg.ble_addr_type = 0;
    s_cfg.orientation = ORI_NORMAL;
    s_cfg.darkness = CONFIG_CATPRINTER_DARKNESS;
    s_cfg.speed = CONFIG_CATPRINTER_SPEED;
    s_cfg.threshold = CONFIG_AIRPRINT_THRESHOLD;
    s_cfg.write_delay_ms = CONFIG_CATPRINTER_WRITE_DELAY_MS;
    s_cfg.feed_pixels = 128;
}

static void get_str(const char *key, char *dst, size_t cap)
{
    size_t len = cap;
    if (nvs_get_str(s_nvs, key, dst, &len) != ESP_OK) {
        /* keep default */
    }
    dst[cap - 1] = '\0';
}

static void get_u8(const char *key, uint8_t *dst)
{
    uint8_t v;
    if (nvs_get_u8(s_nvs, key, &v) == ESP_OK) {
        *dst = v;
    }
}

static void get_u16(const char *key, uint16_t *dst)
{
    uint16_t v;
    if (nvs_get_u16(s_nvs, key, &v) == ESP_OK) {
        *dst = v;
    }
}

static void set_str(const char *key, const char *v)
{
    esp_err_t e = nvs_set_str(s_nvs, key, v);
    if (e != ESP_OK) ESP_LOGW(TAG, "set %s: %s", key, esp_err_to_name(e));
}

void settings_init(void)
{
    load_defaults();
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &s_nvs);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open(%s): %s - using defaults", NS, esp_err_to_name(e));
        return;
    }
    get_str("wifi_ssid", s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid));
    get_str("wifi_pass", s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass));
    get_str("wifi_bssid", s_cfg.wifi_bssid, sizeof(s_cfg.wifi_bssid));
    get_str("ap_name", s_cfg.airprint_name, sizeof(s_cfg.airprint_name));
    get_str("ble_prefix", s_cfg.ble_prefix, sizeof(s_cfg.ble_prefix));
    get_str("ble_mac", s_cfg.ble_mac, sizeof(s_cfg.ble_mac));
    get_u8("ble_addr_type", &s_cfg.ble_addr_type);
    get_u8("orientation", &s_cfg.orientation);
    get_u8("darkness", &s_cfg.darkness);
    get_u8("speed", &s_cfg.speed);
    get_u8("threshold", &s_cfg.threshold);
    get_u8("write_delay", &s_cfg.write_delay_ms);
    get_u16("feed_pixels", &s_cfg.feed_pixels);

    /* Clamp anything corrupt/backwards-compatible. */
    if (s_cfg.orientation >= ORI_MAX) s_cfg.orientation = ORI_NORMAL;
    if (s_cfg.darkness > 15) s_cfg.darkness = 8;
    if (s_cfg.speed < 1 || s_cfg.speed > 15) s_cfg.speed = 5;
    if (s_cfg.write_delay_ms < 8 || s_cfg.write_delay_ms > 120) s_cfg.write_delay_ms = 40;
    if (s_cfg.feed_pixels > 1024) s_cfg.feed_pixels = 128;
    if (s_cfg.ble_addr_type > 1) s_cfg.ble_addr_type = 0;

    ESP_LOGI(TAG, "loaded: wifi='%s' ble_prefix='%s' bind='%s' ori=%u dark=%u spd=%u thr=%u delay=%ums",
             s_cfg.wifi_ssid, s_cfg.ble_prefix, s_cfg.ble_mac,
             s_cfg.orientation, s_cfg.darkness, s_cfg.speed,
             s_cfg.threshold, s_cfg.write_delay_ms);
}

const app_settings_t *settings_get(void)
{
    return &s_cfg;
}

void settings_snapshot(app_settings_t *out)
{
    if (out) *out = s_cfg;
}

esp_err_t settings_commit(void)
{
    if (s_nvs == 0) return ESP_ERR_INVALID_STATE;
    set_str("wifi_ssid", s_cfg.wifi_ssid);
    set_str("wifi_pass", s_cfg.wifi_pass);
    set_str("wifi_bssid", s_cfg.wifi_bssid);
    set_str("ap_name", s_cfg.airprint_name);
    set_str("ble_prefix", s_cfg.ble_prefix);
    set_str("ble_mac", s_cfg.ble_mac);
    nvs_set_u8(s_nvs, "ble_addr_type", s_cfg.ble_addr_type);
    nvs_set_u8(s_nvs, "orientation", s_cfg.orientation);
    nvs_set_u8(s_nvs, "darkness", s_cfg.darkness);
    nvs_set_u8(s_nvs, "speed", s_cfg.speed);
    nvs_set_u8(s_nvs, "threshold", s_cfg.threshold);
    nvs_set_u8(s_nvs, "write_delay", s_cfg.write_delay_ms);
    nvs_set_u16(s_nvs, "feed_pixels", s_cfg.feed_pixels);
    esp_err_t e = nvs_commit(s_nvs);
    ESP_LOGI(TAG, "commit: %s", esp_err_to_name(e));
    return e;
}

esp_err_t settings_replace(const app_settings_t *s)
{
    if (s == NULL) return ESP_ERR_INVALID_ARG;
    s_cfg = *s;
    return settings_commit();
}

bool settings_wifi_configured(void)
{
    return s_cfg.wifi_ssid[0] != '\0';
}

/* ---- minimal urlencoded form parser ----------------------------------- */
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t url_decode(const char *src, size_t n, char *dst, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; i++) {
        char c = src[i];
        if (c == '+') {
            dst[o++] = ' ';
        } else if (c == '%' && i + 2 < n) {
            int a = hexval(src[i + 1]), b = hexval(src[i + 2]);
            if (a >= 0 && b >= 0) {
                dst[o++] = (char)((a << 4) | b);
                i += 2;
            } else {
                dst[o++] = c;
            }
        } else {
            dst[o++] = c;
        }
    }
    dst[o] = '\0';
    return o;
}

static void put_str(char *dst, size_t cap, const char *v, size_t vlen)
{
    if (vlen >= cap) vlen = cap - 1;
    memcpy(dst, v, vlen);
    dst[vlen] = '\0';
}

static uint32_t clamp_u32(const char *v, uint32_t lo, uint32_t hi)
{
    unsigned long n = strtoul(v, NULL, 10);
    if (n < lo) return lo;
    if (n > hi) return hi;
    return (uint32_t)n;
}

void settings_apply_form_into(app_settings_t *s, const char *body, size_t len)
{
    if (s == NULL || body == NULL) return;
    char key[40], val[256];
    size_t p = 0;
    bool speed_seen = false, delay_seen = false;
    while (p < len) {
        size_t e = p;
        while (e < len && body[e] != '&') e++;
        const char *pair = body + p;
        size_t plen = e - p;
        p = e + 1;

        const char *eq = memchr(pair, '=', plen);
        if (eq == NULL) continue;
        size_t klen = (size_t)(eq - pair);
        size_t vlen = plen - klen - 1;
        if (klen >= sizeof(key)) klen = sizeof(key) - 1;
        url_decode(pair, klen, key, sizeof(key));
        vlen = url_decode(eq + 1, vlen, val, sizeof(val));

        if (strcmp(key, "wifi_ssid") == 0) {
            put_str(s->wifi_ssid, sizeof(s->wifi_ssid), val, vlen);
        } else if (strcmp(key, "wifi_pass") == 0) {
            put_str(s->wifi_pass, sizeof(s->wifi_pass), val, vlen);
        } else if (strcmp(key, "wifi_bssid") == 0) {
            put_str(s->wifi_bssid, sizeof(s->wifi_bssid), val, vlen);
        } else if (strcmp(key, "airprint_name") == 0) {
            if (vlen) put_str(s->airprint_name, sizeof(s->airprint_name), val, vlen);
        } else if (strcmp(key, "ble_prefix") == 0) {
            if (vlen) put_str(s->ble_prefix, sizeof(s->ble_prefix), val, vlen);
        } else if (strcmp(key, "ble_mac") == 0) {
            /* accept AA:BB:.. or AABB.. ; keep hex only */
            char clean[SETTINGS_MAC_LEN + 1];
            size_t n = 0;
            for (size_t i = 0; i < vlen && n < SETTINGS_MAC_LEN; i++) {
                if (hexval(val[i]) >= 0) clean[n++] = val[i];
            }
            clean[n] = '\0';
            memcpy(s->ble_mac, clean, sizeof(clean));
        } else if (strcmp(key, "ble_addr_type") == 0) {
            s->ble_addr_type = (uint8_t)clamp_u32(val, 0, 1);
        } else if (strcmp(key, "orientation") == 0) {
            s->orientation = (uint8_t)clamp_u32(val, 0, ORI_MAX - 1);
        } else if (strcmp(key, "darkness") == 0) {
            s->darkness = (uint8_t)clamp_u32(val, 0, 15);
        } else if (strcmp(key, "speed") == 0) {
            s->speed = (uint8_t)clamp_u32(val, 1, 15);
            speed_seen = true;
        } else if (strcmp(key, "threshold") == 0) {
            s->threshold = (uint8_t)clamp_u32(val, 0, 255);
        } else if (strcmp(key, "write_delay") == 0) {
            s->write_delay_ms = (uint8_t)clamp_u32(val, 8, 120);
            delay_seen = true;
        } else if (strcmp(key, "feed_pixels") == 0) {
            s->feed_pixels = (uint16_t)clamp_u32(val, 0, 1024);
        }
    }
    /* Speed is the user-facing knob; derive the BLE pacing from it unless the
     * caller explicitly supplied an advanced write_delay value. speed 5 -> 40 ms
     * (the proven stable default), 15 -> 8 ms (fast, may drop rows). */
    if (speed_seen && !delay_seen) {
        int d = 60 - (int)s->speed * 4;
        if (d < 8) d = 8;
        if (d > 120) d = 120;
        s->write_delay_ms = (uint8_t)d;
    }
}

esp_err_t settings_set_wifi(const char *ssid, const char *pass, const char *bssid)
{
    if (ssid == NULL) return ESP_ERR_INVALID_ARG;
    put_str(s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid), ssid, strlen(ssid));
    put_str(s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass), pass ? pass : "", pass ? strlen(pass) : 0);
    put_str(s_cfg.wifi_bssid, sizeof(s_cfg.wifi_bssid), bssid ? bssid : "",
            bssid ? strlen(bssid) : 0);
    return settings_commit();
}

esp_err_t settings_forget_wifi(void)
{
    s_cfg.wifi_ssid[0] = '\0';
    s_cfg.wifi_pass[0] = '\0';
    s_cfg.wifi_bssid[0] = '\0';
    return settings_commit();
}

esp_err_t settings_factory_reset(void)
{
    if (s_nvs == 0) return ESP_ERR_INVALID_STATE;
    esp_err_t e = nvs_erase_all(s_nvs);
    if (e == ESP_OK) e = nvs_commit(s_nvs);
    load_defaults();
    return e;
}

uint16_t settings_energy(void)
{
    /* 0x1000 .. 0x5000. 0x3000 (darkness 8) is the known-good default;
     * values above ~0x6000 overheat the head and smear the page. */
    return (uint16_t)(0x1000u + (uint16_t)s_cfg.darkness * 0x400u);
}

uint8_t settings_write_delay(void)
{
    return s_cfg.write_delay_ms;
}

const char *settings_orientation_name(uint8_t o)
{
    static const char *n[] = {"normal", "rotate180", "mirror", "mirror+180"};
    return (o < ORI_MAX) ? n[o] : "unknown";
}

const char *settings_ble_state_name(int state)
{
    static const char *n[] = {"idle", "scanning", "connecting", "ready", "printing", "error"};
    return (state >= 0 && state <= 5) ? n[state] : "unknown";
}
