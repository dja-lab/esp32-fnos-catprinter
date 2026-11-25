/*
 * BLE cat printer driver (ESP-IDF NimBLE, central role).
 * Forwarding path: page buffer -> BLE GATT 0xAE01 -> cat printer.
 *
 * NOTE: NimBLE API signatures below follow ESP-IDF 5.x. If your IDF reports
 *       a different prototype, adjust in catprinter.c only - the public
 *       interface here is intentionally IDF-version-independent.
 */
#ifndef CATPINTER_H
#define CATPINTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CP_STATE_IDLE = 0,
    CP_STATE_SCANNING,
    CP_STATE_CONNECTING,
    CP_STATE_READY,      /* connected, services discovered, ready to print */
    CP_STATE_PRINTING,
    CP_STATE_ERROR
} cp_state_t;

typedef struct {
    cp_state_t state;
    uint16_t mtu;
    uint16_t conn_handle;
    bool flow_paused;
} cp_status_t;

#define CP_SCAN_MAX_DEVICES 12

typedef struct {
    char name[32];
    char mac[13];        /* 12 uppercase hex chars, no separators */
    uint8_t addr_type;   /* 0 = public, 1 = random */
    int8_t rssi;
} cp_device_t;

typedef void (*cp_event_cb)(cp_state_t state, void *ctx);

/* Initialize NimBLE host (central only). Call once after NVS init. */
esp_err_t cp_init(const char *ble_name_prefix, cp_event_cb cb, void *ctx);

/* Scan for a printer whose advertised name starts with the configured
 * prefix, then connect and discover 0xAE30 / 0xAE01 / 0xAE02. Non-blocking:
 * completion is reported through the event callback (CP_STATE_READY). */
esp_err_t cp_scan_and_connect(uint32_t timeout_ms);

/* Passive scan for the WebUI: collect nearby BLE devices (name/MAC/RSSI)
 * without connecting. Blocks up to timeout_ms + 1 s. */
esp_err_t cp_scan_devices(uint32_t timeout_ms, cp_device_t *out, size_t cap,
                          size_t *count);

/* Connect directly to a bound printer by MAC (12 hex chars). Terminates an
 * existing connection first, if any. */
esp_err_t cp_connect_mac(const char *mac_hex, uint8_t addr_type);

/* Change the name prefix used by cp_scan_and_connect() at runtime. */
void cp_set_name_prefix(const char *prefix);

/* MAC of the currently connected peer ("" when disconnected). */
const char *cp_peer_mac(void);

/* Peer address type of the current connection (0 public / 1 random). */
uint8_t cp_peer_addr_type(void);

esp_err_t cp_disconnect(void);
bool cp_is_ready(void);
esp_err_t cp_get_status(cp_status_t *out);
uint16_t cp_get_mtu(void);

/* Print one page. `rows` is a contiguous 1-bit bitmap, PP_ROW_BYTES bytes
 * per row, MSB-first (PBM P4 convention). Bit reversal is done by the
 * protocol layer. Blocks until the page is sent or timeout/flow stall. */
esp_err_t cp_print_page(const uint8_t *rows, size_t nrows, uint32_t timeout_ms);

/* Same, but with an explicit trailing paper feed in dots: pass 0 to append
 * more content seamlessly on continuous roll paper (multi-page jobs), or -1
 * to use the configured feed_pixels setting (single / final pages). */
esp_err_t cp_print_page_ex(const uint8_t *rows, size_t nrows, uint32_t timeout_ms,
                           int feed_pixels);

/* Send arbitrary bytes through 0xAE01, chunked to the negotiated MTU.
 * Useful for byte-for-byte comparison against the Python program. */
esp_err_t cp_send_raw(const uint8_t *data, size_t len, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
#endif
