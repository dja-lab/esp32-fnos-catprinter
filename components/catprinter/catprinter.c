/*
 * BLE cat printer driver - ESP-IDF NimBLE central.
 *
 * Reverse-engineered protocol source: NaitLee/Cat-Printer (CC0-1.0).
 * GATT: service 0xAE30, write char 0xAE01, notify char 0xAE02.
 *
 * SKELETON NOTICE:
 *  - Flow control, chunk pacing and the full command sequence are implemented.
 *  - Verify `ble_gattc_*` signatures against your IDF version; only this file
 *    touches NimBLE APIs.
 *  - TODO(multidevice): X6h may use a different service UUID - extend
 *    s_expected_svc once you confirm your printer's model.
 */
#include "catprinter.h"
#include "printer_protocol.h"
#include "settings.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_nimble_hci.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"

static const char *TAG = "catprinter";

/* Set to 1 if your printer advertises as a random static address. */
#ifndef CP_SCAN_RANDOM_ADDR
#define CP_SCAN_RANDOM_ADDR 0
#endif

/* NimBLE 1.5+ (ESP-IDF 5.x/6.x) owns the address type: ble_gap_disc() and
 * ble_gap_connect() take `own_addr_type`, not the peer's address type. */
#if CP_SCAN_RANDOM_ADDR
#define CP_OWN_ADDR_TYPE BLE_OWN_ADDR_RANDOM
#else
#define CP_OWN_ADDR_TYPE BLE_OWN_ADDR_PUBLIC
#endif

static char s_name_prefix[24] = "GB01";
static cp_event_cb s_event_cb = NULL;
static void *s_event_ctx = NULL;
static cp_state_t s_state = CP_STATE_IDLE;

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_write_handle = 0;   /* 0xAE01 value handle */
static uint8_t s_write_props = 0;     /* GATT properties of the write characteristic */
static uint16_t s_notify_handle = 0;  /* 0xAE02 value handle */
static uint16_t s_mtu = 23;
static volatile bool s_flow_paused = false;
static SemaphoreHandle_t s_write_done = NULL;
static SemaphoreHandle_t s_scan_done = NULL;
static ble_addr_t s_peer_addr;
static char s_peer_mac[13] = "";

/* WebUI-driven passive scan. s_scan_mode: 0 = connect-by-prefix,
 * 1 = collect device list (never connects). */
static volatile int s_scan_mode = 0;
static cp_device_t *s_scan_out = NULL;
static size_t s_scan_cap = 0;
static size_t s_scan_count = 0;

#define CP_UUID16_SVC   0xAE30
#define CP_UUID16_TX    0xAE01
#define CP_UUID16_RX    0xAE02

static void set_state(cp_state_t st)
{
    s_state = st;
    if (s_event_cb) {
        s_event_cb(st, s_event_ctx);
    }
}

static uint16_t uuid16_value(const ble_uuid_t *u)
{
    if (u->type != BLE_UUID_TYPE_16) {
        return 0;
    }
    return ((const ble_uuid16_t *)u)->value;
}

/* Scan advertising data: [len][AD type][payload...]; name types 0x08/0x09. */
static bool adv_name_matches(const uint8_t *data, size_t len)
{
    size_t i = 0;
    while (i + 1 < len) {
        size_t field_len = data[i];
        if (field_len == 0 || i + 1 + field_len > len) {
            break;
        }
        uint8_t type = data[i + 1];
        if ((type == 0x08 || type == 0x09) && field_len > 1) {
            const uint8_t *name = &data[i + 2];
            size_t name_len = field_len - 1;
            size_t prefix_len = strlen(s_name_prefix);
            if (name_len >= prefix_len && memcmp(name, s_name_prefix, prefix_len) == 0) {
                return true;
            }
        }
        i += 1 + field_len;
    }
    return false;
}

/* Extract the advertised name (AD types 0x08 short / 0x09 complete). */
static void adv_extract_name(const uint8_t *data, size_t len,
                             char *out, size_t cap)
{
    out[0] = '\0';
    size_t i = 0;
    while (i + 1 < len) {
        size_t field_len = data[i];
        if (field_len == 0 || i + 1 + field_len > len) break;
        uint8_t type = data[i + 1];
        if ((type == 0x08 || type == 0x09) && field_len > 1) {
            size_t n = field_len - 1;
            if (n >= cap) n = cap - 1;
            memcpy(out, &data[i + 2], n);
            out[n] = '\0';
            return;
        }
        i += 1 + field_len;
    }
}

/* NimBLE stores addr.val in reverse (LSB first); print it MSB-first like
 * every other BLE tool does. */
static void mac_to_string(const ble_addr_t *a, char out[13])
{
    for (int i = 0; i < 6; i++) {
        snprintf(out + i * 2, 3, "%02X", a->val[5 - i]);
    }
    out[12] = '\0';
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void add_scan_device(const ble_addr_t *addr, const char *name, int8_t rssi)
{
    if (s_scan_out == NULL || s_scan_count >= s_scan_cap) return;
    char mac[13];
    mac_to_string(addr, mac);
    for (size_t i = 0; i < s_scan_count; i++) {
        if (strcmp(s_scan_out[i].mac, mac) == 0) {
            if (rssi > s_scan_out[i].rssi) s_scan_out[i].rssi = rssi;
            return;
        }
    }
    cp_device_t *d = &s_scan_out[s_scan_count++];
    memset(d, 0, sizeof(*d));
    strncpy(d->name, name[0] ? name : "(unknown)", sizeof(d->name) - 1);
    memcpy(d->mac, mac, sizeof(d->mac));
    d->addr_type = (uint8_t)(addr->type == BLE_ADDR_RANDOM ? 1 : 0);
    d->rssi = rssi;
}

static int on_write_complete(uint16_t conn_handle,
                             const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle;
    (void)attr;
    (void)arg;
    if (error != NULL && error->status != 0) {
        ESP_LOGE(TAG, "GATT write failed, status=%d", error->status);
    }
    if (s_write_done) {
        xSemaphoreGive(s_write_done);
    }
    return 0;
}

static int on_mtu_exchange(uint16_t conn_handle,
                           const struct ble_gatt_error *error,
                           uint16_t mtu, void *arg)
{
    (void)conn_handle;
    (void)arg;
    if (error == NULL || error->status == 0) {
        s_mtu = mtu;
        ESP_LOGI(TAG, "MTU negotiated: %u", mtu);
    }
    return 0;
}

static bool is_done_status(int status)
{
    /* NimBLE reports a completed GATT procedure either as status 0 with a NULL
     * item, or as BLE_HS_EDONE (14). Treat both as success. */
    return status == 0 || status == BLE_HS_EDONE;
}

static int on_discover_chr(uint16_t conn_handle,
                           const struct ble_gatt_error *error,
                           const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    int status = (error != NULL) ? error->status : 0;
    if (!is_done_status(status)) {
        ESP_LOGE(TAG, "characteristic discovery failed, status=%d", status);
        set_state(CP_STATE_ERROR);
        return 0;
    }
    if (chr == NULL || status == BLE_HS_EDONE) {
        /* Discovery finished */
        if (s_write_handle != 0 && s_notify_handle != 0) {
            set_state(CP_STATE_READY);
            ESP_LOGI(TAG, "printer ready: tx=0x%04x rx=0x%04x mtu=%u props=0x%02x",
                     s_write_handle, s_notify_handle, s_mtu, s_write_props);
        } else {
            ESP_LOGE(TAG, "required characteristics not found");
            set_state(CP_STATE_ERROR);
        }
        return 0;
    }

    uint16_t v = uuid16_value(&chr->uuid.u);
    if (v == CP_UUID16_TX) {
        s_write_handle = chr->val_handle;
        s_write_props = chr->properties;
    } else if (v == CP_UUID16_RX) {
        s_notify_handle = chr->val_handle;
        /* Subscribe by writing CCCD = 0x0001 (notifications enabled). */
        const uint8_t cccd[2] = {0x01, 0x00};
        ble_gattc_write_flat(conn_handle, (uint16_t)(chr->val_handle + 1),
                             cccd, sizeof(cccd), on_write_complete, NULL);
    }
    return 0;
}

static int on_discover_svc(uint16_t conn_handle,
                           const struct ble_gatt_error *error,
                           const struct ble_gatt_svc *service, void *arg)
{
    (void)arg;
    int status = (error != NULL) ? error->status : 0;
    if (!is_done_status(status)) {
        ESP_LOGE(TAG, "service discovery failed, status=%d", status);
        set_state(CP_STATE_ERROR);
        return 0;
    }
    if (service != NULL) {
        ble_gattc_disc_all_chrs(conn_handle, service->start_handle,
                                service->end_handle, on_discover_chr, NULL);
    }
    return 0;
}

static int on_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {

    case BLE_GAP_EVENT_DISC: {
        if (s_scan_mode == 1) {
            char name[32];
            adv_extract_name(event->disc.data, event->disc.length_data,
                             name, sizeof(name));
            add_scan_device(&event->disc.addr, name, event->disc.rssi);
            break;
        }
        if (adv_name_matches(event->disc.data, event->disc.length_data)) {
            ble_gap_disc_cancel();
            s_peer_addr = event->disc.addr;
            mac_to_string(&s_peer_addr, s_peer_mac);
            ESP_LOGI(TAG, "found printer %s (%s), connecting...",
                     s_peer_mac, event->disc.addr.type == BLE_ADDR_RANDOM ? "random" : "public");
            set_state(CP_STATE_CONNECTING);

            struct ble_gap_conn_params params;
            memset(&params, 0, sizeof(params));
            params.scan_itvl = 0x0010;
            params.scan_window = 0x0010;
            params.itvl_min = 0x0010;
            params.itvl_max = 0x0020;
            params.supervision_timeout = 200;
            params.min_ce_len = 0;
            params.max_ce_len = 0;

            ble_gap_connect(CP_OWN_ADDR_TYPE, &s_peer_addr, 10000, &params,
                            on_gap_event, NULL);
        }
        break;
    }

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            mac_to_string(&s_peer_addr, s_peer_mac);
            s_write_handle = 0;
            s_notify_handle = 0;
            s_mtu = 23;
            s_flow_paused = false;
            ble_gattc_exchange_mtu(s_conn_handle, on_mtu_exchange, NULL);

            /* Ask for a short connection interval: with the default (often
             * 30-50 ms) the row stream queues up faster than it drains and the
             * controller silently drops writes -> "random stripes" on paper. */
            {
                struct ble_gap_upd_params up = {
                    .itvl_min = 12,           /* 15 ms */
                    .itvl_max = 24,           /* 30 ms */
                    .latency = 0,
                    .supervision_timeout = 400,   /* 4 s */
                    .min_ce_len = 0,
                    .max_ce_len = 0,
                };
                int urc = ble_gap_update_params(s_conn_handle, &up);
                ESP_LOGI(TAG, "conn params update rc=%d", urc);
            }

            ble_uuid16_t svc_uuid;
            svc_uuid.u.type = BLE_UUID_TYPE_16;
            svc_uuid.value = CP_UUID16_SVC;
            ble_gattc_disc_svc_by_uuid(s_conn_handle, &svc_uuid.u,
                                       on_discover_svc, NULL);
        } else {
            ESP_LOGE(TAG, "connect failed, status=%d", event->connect.status);
            set_state(CP_STATE_ERROR);
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "disconnected, reason=%d",
                 event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_peer_mac[0] = '\0';
        s_write_handle = 0;
        s_notify_handle = 0;
        s_flow_paused = false;
        set_state(CP_STATE_IDLE);
        break;

    case BLE_GAP_EVENT_NOTIFY_RX:
        if (event->notify_rx.attr_handle == s_notify_handle &&
            event->notify_rx.om != NULL) {
            struct os_mbuf *om = event->notify_rx.om;
            size_t len = OS_MBUF_PKTLEN(om);
            uint8_t notify[PP_FLOW_LEN];
            if (len == PP_FLOW_LEN &&
                os_mbuf_copydata(om, 0, PP_FLOW_LEN, notify) == 0) {
                static int logged = 0;
                if (logged < 10) {
                    ESP_LOGI(TAG, "notify %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                             notify[0], notify[1], notify[2], notify[3], notify[4],
                             notify[5], notify[6], notify[7], notify[8]);
                    logged++;
                }
                if (memcmp(notify, PP_FLOW_PAUSE, PP_FLOW_LEN) == 0) {
                    s_flow_paused = true;
                } else if (memcmp(notify, PP_FLOW_RESUME, PP_FLOW_LEN) == 0) {
                    s_flow_paused = false;
                }
            }
        }
        break;

    case BLE_GAP_EVENT_CONN_UPDATE:
    case BLE_GAP_EVENT_MTU:
        break;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (s_scan_mode == 1) {
            /* Passive scan finished: hand the list back to the WebUI without
             * touching the connection state. */
            s_scan_mode = 0;
            if (s_scan_done) xSemaphoreGive(s_scan_done);
            break;
        }
        /* Scan window ended (printer was off / out of range). Go back to IDLE so
         * the application can simply call cp_scan_and_connect() again. */
        ESP_LOGW(TAG, "scan window ended, printer not found");
        if (s_state == CP_STATE_SCANNING) {
            set_state(CP_STATE_IDLE);
        }
        break;

    default:
        break;
    }
    return 0;
}

static void on_host_sync(void)
{
    ESP_LOGI(TAG, "NimBLE host synchronized");
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t cp_init(const char *ble_name_prefix, cp_event_cb cb, void *ctx)
{
    if (ble_name_prefix != NULL) {
        size_t n = strlen(ble_name_prefix);
        if (n >= sizeof(s_name_prefix)) {
            n = sizeof(s_name_prefix) - 1;
        }
        memcpy(s_name_prefix, ble_name_prefix, n);
        s_name_prefix[n] = '\0';
    }
    s_event_cb = cb;
    s_event_ctx = ctx;

    s_write_done = xSemaphoreCreateBinary();
    s_scan_done = xSemaphoreCreateBinary();
    if (s_write_done == NULL || s_scan_done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_nimble_hci_init();
    nimble_port_init();

    ble_hs_cfg.sync_cb = on_host_sync;
    /* No GAP/GATT service registration: this build is NimBLE central only
     * (CONFIG_BT_NIMBLE_ROLE_PERIPHERAL/GATTS are off), so the gap service is
     * not compiled in and ble_svc_gap_init() would not link. */
    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "init done, scanning for prefix '%s'", s_name_prefix);
    return ESP_OK;
}

esp_err_t cp_scan_and_connect(uint32_t timeout_ms)
{
    if (s_scan_mode != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state != CP_STATE_IDLE && s_state != CP_STATE_ERROR) {
        return ESP_ERR_INVALID_STATE;
    }
    struct ble_gap_disc_params params;
    memset(&params, 0, sizeof(params));
    params.itvl = 0x0050;
    params.window = 0x0050;
    params.filter_duplicates = 1;

    set_state(CP_STATE_SCANNING);
    int rc = ble_gap_disc(CP_OWN_ADDR_TYPE, timeout_ms, &params, on_gap_event, NULL);
    if (rc != 0) {
        set_state(CP_STATE_ERROR);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ---- WebUI: passive scan / explicit pairing ---------------------------- */

esp_err_t cp_scan_devices(uint32_t timeout_ms, cp_device_t *out, size_t cap,
                          size_t *count)
{
    if (out == NULL || cap == 0 || s_scan_done == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_scan_mode != 0 || s_state == CP_STATE_SCANNING ||
        s_state == CP_STATE_CONNECTING || s_state == CP_STATE_PRINTING) {
        return ESP_ERR_INVALID_STATE;
    }

    s_scan_out = out;
    s_scan_cap = cap;
    s_scan_count = 0;
    xSemaphoreTake(s_scan_done, 0);      /* drain stale signal */
    s_scan_mode = 1;

    struct ble_gap_disc_params params;
    memset(&params, 0, sizeof(params));
    params.itvl = 0x0050;
    params.window = 0x0050;
    params.filter_duplicates = 1;

    int rc = ble_gap_disc(CP_OWN_ADDR_TYPE, timeout_ms, &params, on_gap_event, NULL);
    if (rc != 0) {
        s_scan_mode = 0;
        s_scan_out = NULL;
        return ESP_FAIL;
    }
    if (xSemaphoreTake(s_scan_done, pdMS_TO_TICKS(timeout_ms + 1500)) != pdTRUE) {
        ESP_LOGW(TAG, "device scan timed out");
        s_scan_mode = 0;
        ble_gap_disc_cancel();
    }
    s_scan_out = NULL;
    s_scan_cap = 0;
    if (count != NULL) {
        *count = s_scan_count;
    }
    ESP_LOGI(TAG, "scan list: %u device(s)", (unsigned)s_scan_count);
    return ESP_OK;
}

esp_err_t cp_connect_mac(const char *mac_hex, uint8_t addr_type)
{
    if (mac_hex == NULL || strlen(mac_hex) != SETTINGS_MAC_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    ble_addr_t a;
    memset(&a, 0, sizeof(a));
    a.type = addr_type ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC;
    for (int i = 0; i < 6; i++) {
        int h = hexval(mac_hex[i * 2]), l = hexval(mac_hex[i * 2 + 1]);
        if (h < 0 || l < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        a.val[5 - i] = (uint8_t)((h << 4) | l);
    }
    if (s_state == CP_STATE_PRINTING) {
        return ESP_ERR_INVALID_STATE;
    }

    /* NimBLE is configured for a single connection; wait for the old one to
     * actually close before issuing a new connect. */
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        cp_disconnect();
        for (int i = 0; i < 60 && s_conn_handle != BLE_HS_CONN_HANDLE_NONE; i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    s_peer_addr = a;
    mac_to_string(&a, s_peer_mac);
    set_state(CP_STATE_CONNECTING);

    struct ble_gap_conn_params params;
    memset(&params, 0, sizeof(params));
    params.scan_itvl = 0x0010;
    params.scan_window = 0x0010;
    params.itvl_min = 0x0010;
    params.itvl_max = 0x0020;
    params.supervision_timeout = 200;
    params.min_ce_len = 0;
    params.max_ce_len = 0;
    int rc = ble_gap_connect(CP_OWN_ADDR_TYPE, &a, 10000, &params,
                             on_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "connect to %s failed rc=%d", mac_hex, rc);
        set_state(CP_STATE_IDLE);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "connecting to bound printer %s", mac_hex);
    return ESP_OK;
}

void cp_set_name_prefix(const char *prefix)
{
    if (prefix == NULL) return;
    size_t n = strlen(prefix);
    if (n >= sizeof(s_name_prefix)) n = sizeof(s_name_prefix) - 1;
    memcpy(s_name_prefix, prefix, n);
    s_name_prefix[n] = '\0';
}

const char *cp_peer_mac(void)
{
    return s_peer_mac;
}

uint8_t cp_peer_addr_type(void)
{
    return (uint8_t)(s_peer_addr.type == BLE_ADDR_RANDOM ? 1 : 0);
}

esp_err_t cp_disconnect(void)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    int rc = ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

bool cp_is_ready(void)
{
    return s_state == CP_STATE_READY || s_state == CP_STATE_PRINTING;
}

uint16_t cp_get_mtu(void)
{
    return s_mtu;
}

esp_err_t cp_get_status(cp_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    out->state = s_state;
    out->mtu = s_mtu;
    out->conn_handle = s_conn_handle;
    out->flow_paused = s_flow_paused;
    return ESP_OK;
}

/* Send with printer flow control + the same ~20ms pacing as the Python
 * reference `flush()`. Each chunk waits for its GATT write callback. */
static esp_err_t send_chunked(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    size_t offset = 0;
    /* ATT protocol header takes 3 bytes out of the negotiated MTU. */
    size_t max_attr = s_mtu > 3 ? (size_t)(s_mtu - 3) : 20;
    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > max_attr) {
            chunk = max_attr;
        }

        TickType_t started = xTaskGetTickCount();
        while (s_flow_paused) {
            if ((xTaskGetTickCount() - started) > pdMS_TO_TICKS(timeout_ms)) {
                ESP_LOGE(TAG, "flow control stall (printer paused too long)");
                return ESP_ERR_TIMEOUT;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        xSemaphoreTake(s_write_done, 0); /* drain stale signal */
        /* Printers in this family often expose the write characteristic as
         * "write without response" only; a write-request then fails with
         * ATT 0x03 WRITE_NOT_PERMITTED (host status 259). Pick what the
         * characteristic actually supports. */
        bool no_rsp = ((s_write_props & BLE_GATT_CHR_PROP_WRITE) == 0) &&
                      ((s_write_props & BLE_GATT_CHR_PROP_WRITE_NO_RSP) != 0);
        int rc;
        if (no_rsp) {
            rc = ble_gattc_write_no_rsp_flat(s_conn_handle, s_write_handle,
                                             data + offset, (uint16_t)chunk);
        } else {
            rc = ble_gattc_write_flat(s_conn_handle, s_write_handle,
                                      data + offset, (uint16_t)chunk,
                                      on_write_complete, NULL);
        }
        if (rc != 0) {
            ESP_LOGE(TAG, "gatt write rc=%d (props=0x%02x)", rc, s_write_props);
            return ESP_FAIL;
        }
        if (!no_rsp &&
            xSemaphoreTake(s_write_done, pdMS_TO_TICKS(5000)) != pdTRUE) {
            ESP_LOGE(TAG, "GATT write callback timeout");
            return ESP_ERR_TIMEOUT;
        }
        /* Runtime-tunable from the WebUI: lower = faster, but too low drops
         * rows (visible as stripes). Default 40 ms from Kconfig. */
        vTaskDelay(pdMS_TO_TICKS(settings_write_delay()));
        offset += chunk;
    }
    return ESP_OK;
}

esp_err_t cp_send_raw(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_write_handle == 0 || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    return send_chunked(data, len, timeout_ms);
}

/* Mirrors PrinterDriver._prepare() / _print_bitmap() / _finish() from
 * NaitLee/Cat-Printer. `energy` and `speed` are Kconfig defaults. */
esp_err_t cp_print_page(const uint8_t *rows, size_t nrows, uint32_t timeout_ms)
{
    return cp_print_page_ex(rows, nrows, timeout_ms, -1);
}

esp_err_t cp_print_page_ex(const uint8_t *rows, size_t nrows, uint32_t timeout_ms,
                           int feed_pixels)
{
    if (rows == NULL || nrows == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!cp_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t frame[PP_FRAME_MAX];
    size_t n;
    esp_err_t err;

    set_state(CP_STATE_PRINTING);

#define SEND(call) do {                                                     \
        n = (call);                                                         \
        if (n == 0) { err = ESP_FAIL; goto done; }                          \
        err = send_chunked(frame, n, timeout_ms);                           \
        if (err != ESP_OK) { goto done; }                                   \
    } while (0)

    /* _prepare() */
    SEND(pp_cmd_get_state(frame, sizeof(frame)));
    SEND(pp_cmd_start_printing(frame, sizeof(frame), 0));
    SEND(pp_cmd_set_dpi_200(frame, sizeof(frame)));
    /* The reference PD01 implementation (pdprint.swift) does NOT touch the feed
     * speed during printing - only 0xBD 8 after the lattice session. Setting a
     * slow speed here meant every dot got far more heat than the firmware's
     * default and the page came out smeared. */
    /* Energy is now a runtime setting (darkness 0..15 -> 0x1000..0x5000).
     * Default darkness 8 lands exactly on the proven 0x3000. */
    SEND(pp_cmd_set_energy(frame, sizeof(frame), settings_energy()));
    SEND(pp_cmd_apply_energy(frame, sizeof(frame)));
    SEND(pp_cmd_update_device(frame, sizeof(frame)));
    SEND(pp_cmd_start_lattice(frame, sizeof(frame)));

    /* _print_bitmap(): one 48-byte row per command */
    for (size_t i = 0; i < nrows; i++) {
        SEND(pp_cmd_draw_row(frame, sizeof(frame), rows + i * PP_ROW_BYTES));
        if ((i % 200) == 199) {
            ESP_LOGI(TAG, "print progress: %u/%u rows, free heap %u",
                     (unsigned)(i + 1), (unsigned)nrows,
                     (unsigned)esp_get_free_heap_size());
        }
    }

    /* _finish() */
    SEND(pp_cmd_end_lattice(frame, sizeof(frame)));
    SEND(pp_cmd_set_speed(frame, sizeof(frame), 8));
    /* feed_pixels < 0: use the configured setting (single / final page).
     * feed_pixels == 0: no feed at all - the caller appends more content
     * right after this page (seamless multi-page printing on roll paper). */
    SEND(pp_cmd_feed(frame, sizeof(frame),
                     feed_pixels < 0 ? settings_get()->feed_pixels
                                     : (uint16_t)feed_pixels));
    SEND(pp_cmd_get_state(frame, sizeof(frame)));

#undef SEND

    err = ESP_OK;
done:
    if (s_state == CP_STATE_PRINTING) {
        set_state(CP_STATE_READY);
    }
    return err;
}
