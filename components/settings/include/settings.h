/*
 * Runtime settings, persisted in NVS ("catprint" namespace).
 *
 * The original firmware had every credential and print parameter compiled in
 * via Kconfig. This component keeps the Kconfig values as DEFAULTS and lets
 * the WebUI override them at runtime. All readers take a snapshot so a
 * concurrent WebUI update cannot tear a multi-byte value mid-print.
 */
#ifndef APP_SETTINGS_H
#define APP_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SETTINGS_SSID_MAX   32
#define SETTINGS_PASS_MAX   64
#define SETTINGS_NAME_MAX   63
#define SETTINGS_MAC_LEN    12   /* hex chars, no separators */

typedef enum {
    ORI_NORMAL = 0,          /* as rendered by iOS */
    ORI_ROTATE180 = 1,       /* flip the page end-for-end */
    ORI_MIRROR = 2,          /* mirror left-right (for label/transfer media) */
    ORI_MIRROR_ROT180 = 3,   /* both */
    ORI_MAX
} print_orientation_t;

typedef struct {
    char wifi_ssid[SETTINGS_SSID_MAX + 1];
    char wifi_pass[SETTINGS_PASS_MAX + 1];
    char wifi_bssid[13];        /* 12 hex chars or empty = auto */
    char airprint_name[SETTINGS_NAME_MAX + 1];
    char ble_prefix[24];
    char ble_mac[SETTINGS_MAC_LEN + 1]; /* empty = auto-scan by prefix */
    uint8_t ble_addr_type;      /* 0 = public, 1 = random */
    uint8_t orientation;        /* print_orientation_t */
    uint8_t darkness;           /* 0..15, maps to head energy */
    uint8_t speed;              /* 1..15, maps to BLE pacing */
    uint8_t threshold;          /* 0..255, raster binarisation */
    uint8_t write_delay_ms;     /* 8..120, per BLE write */
    uint16_t feed_pixels;       /* trailing blank feed after a page */
} app_settings_t;

/* Load from NVS (falls back to sdkconfig defaults). Call once after nvs_flash_init. */
void settings_init(void);

/* Pointer to the live struct. For long-running print jobs use settings_snapshot(). */
const app_settings_t *settings_get(void);
void settings_snapshot(app_settings_t *out);

/* Persist the current struct. */
esp_err_t settings_commit(void);

/* Replace the whole live config (used by the WebUI after parsing a form). */
esp_err_t settings_replace(const app_settings_t *s);

/* Parse an application/x-www-form-urlencoded body into `s`, clamping each
 * field to its valid range. Unknown keys are ignored (forward compatible). */
void settings_apply_form_into(app_settings_t *s, const char *body, size_t len);

/* Convenience writers used by the WebUI. */
esp_err_t settings_set_wifi(const char *ssid, const char *pass, const char *bssid);
esp_err_t settings_forget_wifi(void);
esp_err_t settings_factory_reset(void);

bool settings_wifi_configured(void);

/* Derived values used by the print path. */
uint16_t settings_energy(void);          /* darkness -> head energy word */
uint8_t settings_write_delay(void);      /* ms between BLE writes */
const char *settings_orientation_name(uint8_t o);
const char *settings_ble_state_name(int state);

#ifdef __cplusplus
}
#endif
#endif
