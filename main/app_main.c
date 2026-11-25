/*
 * Application entry point.
 *
 * Startup: NVS -> WiFi STA -> BLE connect to cat printer -> mDNS + IPP.
 * Job path: iPhone -> IPP Print-Job (ACK) -> URF decode -> 1-bit page buffer
 *           -> BLE 0xAE01 -> cat printer.
 *
 * WiFi and BLE share one 2.4 GHz radio on C3. The two phases are separated:
 * IPP upload finishes before BLE printing starts, so the coexistence window
 * is kept minimal (no architectural changes needed on classic ESP32/S3).
 */
#include <stdlib.h>
#include <string.h>

#include "catprinter.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "pagebuf.h"
#include "printserver.h"
#include "settings.h"
#include "urf.h"

static const char *TAG = "app";

#define WIFI_CONNECTED_BIT BIT0
#define PRINT_TIMEOUT_MS   (180000u)  /* BLE thermal printing is slow by design */
#define CONNECT_WAIT_MS    (30000u)
#define WIFI_WAIT_MS       (25000u)   /* then continue without IP instead of blocking */

static EventGroupHandle_t s_wifi_events;
static page_buf_t s_page;
static bool s_printer_ready = false;
static bool s_ps_started = false;
static bool s_ap_mode = false;   /* true = SoftAP provisioning, no STA */
static unsigned s_wifi_retries = 0;
static TaskHandle_t s_wifi_retry_task = NULL;

/* Dedicated reconnect task: exponential backoff (1,2,4,…,30 s), never quits.
 * Runs outside the WiFi event loop because blocking that loop breaks IP events. */
static void wifi_retry_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        unsigned n = s_wifi_retries;
        uint32_t backoff_ms = 1000u << (n > 5 ? 5 : (n > 0 ? n - 1 : 0));
        if (backoff_ms > 30000u) {
            backoff_ms = 30000u;
        }
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        esp_err_t rc = esp_wifi_connect();
        if (rc != ESP_OK && rc != ESP_ERR_WIFI_STATE) {
            ESP_LOGW(TAG, "reconnect failed: %s", esp_err_to_name(rc));
        }
    }
}

static void on_ble_state(cp_state_t state, void *ctx)
{
    (void)ctx;
    switch (state) {
    case CP_STATE_SCANNING:
        ESP_LOGI(TAG, "BLE: scanning for printer...");
        break;
    case CP_STATE_CONNECTING:
        ESP_LOGI(TAG, "BLE: connecting...");
        break;
    case CP_STATE_READY:
        ESP_LOGI(TAG, "BLE: printer ready");
        s_printer_ready = true;
        break;
    case CP_STATE_PRINTING:
        ESP_LOGI(TAG, "BLE: printing");
        break;
    case CP_STATE_ERROR:
        ESP_LOGE(TAG, "BLE: error - check printer power and name prefix");
        s_printer_ready = false;
        break;
    case CP_STATE_IDLE:
        s_printer_ready = false;
        break;
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        /* Connection is kicked off explicitly after the diagnostic scan in
         * wifi_init_sta(), so nothing to do here. */
        ESP_LOGI(TAG, "wifi STA started");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /* reason: 15 = 4-way handshake timeout (wrong key, or AP dropped the
         *         EAPOL frames / band-steering kick)
         *         2 = auth expire, 205 = connection fail, 201 = no AP found
         *         202 = auth fail ... see wifi_err_reason_t */
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)data;
        int reason = disc ? disc->reason : -1;
        s_wifi_retries++;

        if (s_wifi_retries <= 5 || (s_wifi_retries % 10) == 0) {
            ESP_LOGW(TAG, "WiFi disconnected (reason=%d ssid=%.*s), attempt %u",
                     reason,
                     disc ? (int)disc->ssid_len : 0,
                     disc ? (const char *)disc->ssid : "",
                     (unsigned)s_wifi_retries);
        }
        /* Never give up permanently (an earlier version stopped after 10
         * attempts and the device stayed offline for good). The actual delay +
         * reconnect happens in a dedicated task: this handler must not block
         * the system event loop. */
        if (s_wifi_retry_task != NULL) {
            xTaskNotifyGive(s_wifi_retry_task);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        s_wifi_retries = 0;
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            ESP_LOGI(TAG, "WiFi connected: " IPSTR " via bssid %02x:%02x:%02x:%02x:%02x:%02x "
                     "ch=%u rssi=%d",
                     IP2STR(&event->ip_info.ip),
                     ap.bssid[0], ap.bssid[1], ap.bssid[2],
                     ap.bssid[3], ap.bssid[4], ap.bssid[5],
                     ap.primary, ap.rssi);
        } else {
            ESP_LOGI(TAG, "WiFi connected: " IPSTR, IP2STR(&event->ip_info.ip));
        }
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);

        /* mDNS/IPP can only bind after we have an interface address, so the
         * print server is (re)tried from here, not from app_main only. */
        if (!s_ps_started) {
            esp_err_t perr = ps_start();
            if (perr == ESP_OK) {
                s_ps_started = true;
                ESP_LOGI(TAG, "print server up: AirPrint name '%s'",
                         CONFIG_AIRPRINT_PRINTER_NAME);
            } else {
                ESP_LOGE(TAG, "print server start failed: %s", esp_err_to_name(perr));
            }
        }
    }
}

/* Diagnostic: what does the C3 actually see for our SSID? Distinguishes
 * wrong password (AP found, auth fails) from 5GHz-only (nothing found) and
 * from a WPA3-only AP (authmode 6/7). */
static void wifi_scan_dump(void)
{
    app_settings_t scfg;
    settings_snapshot(&scfg);
    wifi_scan_config_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.ssid = (uint8_t *)scfg.wifi_ssid;
    sc.show_hidden = false;

    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        return;
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    ESP_LOGI(TAG, "scan: %u AP(s) named '%s'", (unsigned)n, scfg.wifi_ssid);
    if (n == 0) {
        ESP_LOGW(TAG, "SSID not visible on 2.4GHz (C3 has no 5GHz radio)");
        return;
    }

    wifi_ap_record_t *recs = (wifi_ap_record_t *)calloc(n, sizeof(wifi_ap_record_t));
    if (recs != NULL && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
        for (uint16_t i = 0; i < n; i++) {
            /* authmode: 0 open, 3 WPA2-PSK, 4 WPA/WPA2, 6 WPA3-SAE, 7 WPA2/WPA3
             * phy flags tell us whether the AP still accepts 11n clients: if
             * an AP is configured 11ax-only, a 11n-only C3 gets auth timeouts
             * (reason=2) that look exactly like a wrong password. */
            ESP_LOGI(TAG, "  ch=%u rssi=%d authmode=%u bssid=%02x:%02x:%02x:%02x:%02x:%02x",
                     recs[i].primary, recs[i].rssi, recs[i].authmode,
                     recs[i].bssid[0], recs[i].bssid[1], recs[i].bssid[2],
                     recs[i].bssid[3], recs[i].bssid[4], recs[i].bssid[5]);
            ESP_LOGI(TAG, "    phy: 11b=%u 11g=%u 11n=%u lr=%u 11ax=%u  country=%c%c",
                     recs[i].phy_11b, recs[i].phy_11g, recs[i].phy_11n,
                     recs[i].phy_lr, recs[i].phy_11ax,
                     recs[i].country.cc[0] ? recs[i].country.cc[0] : '?',
                     recs[i].country.cc[1] ? recs[i].country.cc[1] : '?');
        }
    }
    free(recs);
    esp_wifi_clear_ap_list();
}

static bool wifi_init_sta(void)
{
    app_settings_t scfg;
    settings_snapshot(&scfg);
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, NULL, NULL));

    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config));
    strncpy((char *)wifi_config.sta.ssid, scfg.wifi_ssid,
            sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, scfg.wifi_pass,
            sizeof(wifi_config.sta.password) - 1);
    /* Some WPA2 APs require management-frame protection (802.11w). Being
     * "capable" is harmless for APs that do not use it, and without it the
     * association dies at auth with reason=2. */
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    /* Optional BSSID pinning: when one SSID is served by several APs and only
     * some of them accept our key, "auto pick strongest" makes the connection
     * random. 12 hex chars, no separators. */
    const char *pin = scfg.wifi_bssid;
    if (pin != NULL && strlen(pin) == 12) {
        bool ok = true;
        for (int i = 0; i < 6 && ok; i++) {
            char hex[3] = { pin[i * 2], pin[i * 2 + 1], 0 };
            char *end = NULL;
            unsigned long v = strtoul(hex, &end, 16);
            if (end == hex || *end != '\0') {
                ok = false;
            } else {
                wifi_config.sta.bssid[i] = (uint8_t)v;
            }
        }
        if (ok) {
            wifi_config.sta.bssid_set = true;
            ESP_LOGI(TAG, "BSSID pinned to %s", pin);
        } else {
            ESP_LOGW(TAG, "bad BSSID '%s' (want 12 hex chars), using auto", pin);
        }
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    /* Scan every channel first: one SSID here is served by two BSSIDs and only
     * one of them accepts our key, so the driver must see all candidates before
     * we pin (fast scan + bssid pinning alone yields NO_AP_FOUND). */
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    /* Explicit WPA2 threshold (default OPEN leaves the driver guessing) and a
     * wide-open RSSI threshold so a -76 dBm BSSID is not silently skipped. */
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.threshold.rssi = -100;
    /* NOTE: do NOT enable 802.11r FT here. ft_enabled=true makes the driver use
     * the FT authentication algorithm, and APs without FT support silently drop
     * those auth frames (symptom: reason=2 AUTH_EXPIRE on every attempt, even
     * against an iPhone hotspot). It was tried for mesh roaming and made things
     * strictly worse - keep both off. */
    wifi_config.sta.ft_enabled = false;
    wifi_config.sta.btm_enabled = false;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Force 20 MHz: some APs advertise HT40 on 2.4G and mis-handle a HT20-only
     * client during association. */
    esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);

    /* No modem sleep: with aggressive power save some 802.11ax APs stop
     * answering our null-data/PS-Poll frames and the connection is dropped
     * seconds after it comes up (see reason=15/2 right after "connected"). */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    /* ESP32-C3 SuperMini quirk: at full TX power (80 = 20 dBm) the on-board
     * ceramic antenna distorts / reflects, the auth + 4-way-handshake frames
     * get corrupted and the AP never answers (scan works, connect fails, or it
     * connects then drops within seconds). Field-proven cure: cap TX power at
     * 8.5 dBm. Must be set AFTER esp_wifi_start() (else ESP_ERR_WIFI_NOT_STARTED)
     * and BEFORE esp_wifi_connect(). */
    {
        int8_t txp = 0;
        esp_wifi_get_max_tx_power(&txp);
        ESP_LOGI(TAG, "max tx power (default): %d (%.2f dBm)", txp, txp * 0.25);
        esp_err_t terr = esp_wifi_set_max_tx_power(CONFIG_AIRPRINT_TX_POWER_QDBM);
        esp_wifi_get_max_tx_power(&txp);
        ESP_LOGI(TAG, "max tx power (now): %d (%.2f dBm), set rc=%s",
                 txp, txp * 0.25, esp_err_to_name(terr));
    }

    /* Read back what actually landed in the driver (catches SSID truncation). */
    wifi_config_t rb;
    memset(&rb, 0, sizeof(rb));
    if (esp_wifi_get_config(WIFI_IF_STA, &rb) == ESP_OK) {
        ESP_LOGI(TAG, "sta cfg: ssid='%.*s' pwlen=%u",
                 (int)sizeof(rb.sta.ssid), (const char *)rb.sta.ssid,
                 (unsigned)strlen((const char *)rb.sta.password));
    }

    if (xTaskCreate(wifi_retry_task, "wifi_retry", 3072, NULL, 5,
                    &s_wifi_retry_task) != pdPASS) {
        ESP_LOGE(TAG, "cannot create wifi retry task");
        s_wifi_retry_task = NULL;
    }

    wifi_scan_dump();
    esp_wifi_connect();

    /* Do NOT block forever: BLE cat-printer output works without IP, and the
     * IPP server is started from the GOT_IP handler once WiFi does come up. */
    ESP_LOGI(TAG, "waiting up to %u ms for WiFi (SSID '%s')...",
             (unsigned)WIFI_WAIT_MS, scfg.wifi_ssid);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(WIFI_WAIT_MS));
    if (bits & WIFI_CONNECTED_BIT) {
        return true;
    }
    ESP_LOGW(TAG, "no WiFi yet - continuing to init BLE; IPP starts on connect");
    return false;
}

/* ---- SoftAP provisioning ------------------------------------------------
 * When NVS has no WiFi credentials (or the user chose "forget"), the board
 * comes up as an open AP "CatPrint-Setup" at 192.168.4.1. A tiny DNS server
 * answers every A query with our own address so phones that probe for a
 * captive portal land on the WebUI automatically. After the WebUI saves a
 * network the device restarts and boots into STA mode. */
#define PROV_AP_SSID  "CatPrint-Setup"

static void dns_hijack_task(void *arg)
{
    (void)arg;
    static uint8_t req[512], resp[512];
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(s);
        vTaskDelete(NULL);
        return;
    }
    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int n = recvfrom(s, req, sizeof(req), 0,
                         (struct sockaddr *)&peer, &plen);
        if (n < 12) continue;
        /* Walk the QNAME (sequence of length-prefixed labels). */
        size_t q = 12;
        while (q < (size_t)n && req[q] != 0) {
            q += (size_t)req[q] + 1u;
        }
        q++;                       /* terminating zero label */
        if (q + 4 > (size_t)n) continue;
        size_t end = q + 4;       /* QTYPE + QCLASS */
        memcpy(resp, req, end);
        resp[2] = 0x81; resp[3] = 0x80;    /* response, recursion available */
        resp[6] = 0x00; resp[7] = 0x01;    /* ANCOUNT = 1 */
        size_t o = end;
        resp[o++] = 0xC0; resp[o++] = 0x0C;  /* name pointer to QNAME */
        resp[o++] = 0x00; resp[o++] = 0x01;  /* TYPE A */
        resp[o++] = 0x00; resp[o++] = 0x01;  /* CLASS IN */
        resp[o++] = 0x00; resp[o++] = 0x00;
        resp[o++] = 0x00; resp[o++] = 0x3C;  /* TTL 60 s */
        resp[o++] = 0x00; resp[o++] = 0x04;  /* RDLENGTH 4 */
        resp[o++] = 192; resp[o++] = 168;
        resp[o++] = 4;   resp[o++] = 1;      /* 192.168.4.1 */
        sendto(s, resp, o, 0, (struct sockaddr *)&peer, plen);
    }
}

static bool wifi_init_ap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    size_t slen = strlen(PROV_AP_SSID);
    memcpy(wc.ap.ssid, PROV_AP_SSID, slen);
    wc.ap.ssid_len = (uint8_t)slen;
    wc.ap.channel = 1;
    wc.ap.max_connection = 4;
    wc.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    /* Same ceramic-antenna quirk as STA mode. */
    esp_wifi_set_max_tx_power(CONFIG_AIRPRINT_TX_POWER_QDBM);

    ESP_LOGI(TAG, "PROVISIONING MODE: open AP '%s', open http://192.168.4.1/",
             PROV_AP_SSID);
    if (xTaskCreate(dns_hijack_task, "dns_hijack", 2048, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "DNS hijack task not started (open the IP directly)");
    }
    return true;
}

/* Orientation is applied by pb_apply_orientation() from the raster component
 * (shared with the WebUI test page). */

/* Runs in the dedicated print task (see printserver.c), AFTER the job has been
 * acknowledged to iOS. */
static esp_err_t on_print_job(const ps_job_t *job, void *ctx)
{
    (void)ctx;
    if (!s_printer_ready) {
        ESP_LOGW(TAG, "printer not ready, waiting up to %u ms", CONNECT_WAIT_MS);
        for (uint32_t waited = 0; waited < CONNECT_WAIT_MS && !s_printer_ready; waited += 500) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (!s_printer_ready) {
            ESP_LOGE(TAG, "printer unavailable, dropping job %lu",
                     (unsigned long)job->job_id);
            return ESP_ERR_INVALID_STATE;
        }
    }

    if (!job->page_ready) {
        /* Fallback path: a raw document buffer (tests / non-streaming callers). */
        urf_doc_t doc = {
            .mem = job->in_flash ? NULL : job->document,
            .flash_offset = job->flash_offset,
            .len = job->document_len,
        };
        esp_err_t err = raster_urf_to_pagebuf(&doc, &s_page);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "raster decode failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(TAG, "printing %u rows", (unsigned)s_page.nrows);

    /* iOS always renders the whole page (A4 etc.), so a two-line document arrives
     * followed by hundreds of blank rows and we would feed ~7 cm of empty paper.
     * Trim trailing blank rows. The FINAL page keeps a small gap so the content
     * isn't at the tear edge; intermediate pages of a multi-page job are trimmed
     * completely so the next page continues seamlessly on the roll. */
    {
        uint32_t orig = (uint32_t)s_page.nrows;
        uint32_t n = pb_trim_trailing_blank(&s_page);
        if (job->last_page && n > 0 && n + 24 < orig) {
            ESP_LOGI(TAG, "trimmed %u trailing blank rows",
                     (unsigned)(orig - n - 24));
            s_page.nrows = n + 24;
        }
    }
    if (s_page.nrows == 0) {
        ESP_LOGI(TAG, "job %lu: blank page, nothing to print",
                 (unsigned long)job->job_id);
        return ESP_OK;
    }

    /* WebUI setting: 0/180/mirror, applied to the finished 1-bit page. */
    pb_apply_orientation(&s_page, settings_get()->orientation);
    ESP_LOGI(TAG, "orientation: %s (%u rows)",
             settings_orientation_name(settings_get()->orientation),
             (unsigned)s_page.nrows);

    /* Give the BLE link some air time while printing: with WIFI_PS_NONE the WiFi
     * radio never sleeps and, together with iOS polling, it starves the BLE
     * writes (dropped rows -> stripes). MIN_MODEM only costs latency, which is
     * fine while the phone is just waiting for the job to finish. */
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

#if CONFIG_AIRPRINT_DEBUG_RASTER_DUMP
    /* Dump the decoded 1-bit page as ASCII art (24 rows sampled across the page,
     * 96 columns sampled every 4th dot). This is what proved the decoder bug. */
    {
        const uint8_t *rows = pb_data(&s_page);
        uint32_t total = (uint32_t)s_page.nrows;
        char art[97];
        for (uint32_t i = 0; i < 24 && total > 0; i++) {
            uint32_t y = (uint32_t)((uint64_t)i * total / 24);
            const uint8_t *r = rows + (size_t)y * RASTER_ROW_BYTES;
            for (uint32_t x = 0; x < 96; x++) {
                uint32_t dot = x * 4;
                int bit = (r[dot >> 3] >> (7 - (dot & 7))) & 1;
                art[x] = bit ? '#' : '.';
            }
            art[96] = '\0';
            ESP_LOGI(TAG, "%s", art);
        }
    }
#endif

    esp_err_t err;
    if (job->last_page) {
        err = cp_print_page(pb_data(&s_page), s_page.nrows, PRINT_TIMEOUT_MS);
    } else {
        /* Intermediate page of a multi-page job: no trailing feed, the next
         * page continues on the same paper. */
        err = cp_print_page_ex(pb_data(&s_page), s_page.nrows, PRINT_TIMEOUT_MS, 0);
    }
    esp_wifi_set_ps(WIFI_PS_NONE);
    return err;
}

/* The page buffer is one big contiguous malloc (~103 KiB at 2200 rows).
 * On a C3 it MUST be taken BEFORE WiFi/BLE come up: after both radios are
 * initialized the largest free internal block is typically well below that,
 * so a late allocation fails with ESP_ERR_NO_MEM. Fall back to shorter pages
 * instead of aborting, so long jobs get truncated rather than rejected. */
static esp_err_t pagebuf_init_best_effort(void)
{
    static const size_t candidates[] = {
        PAGEBUF_DEFAULT_MAX_ROWS, PAGEBUF_DEFAULT_MAX_ROWS / 2, PAGEBUF_DEFAULT_MAX_ROWS / 4
    };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        esp_err_t err = pb_init(&s_page, candidates[i]);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "page buffer: %u rows (%u KiB), free heap %u",
                     (unsigned)candidates[i],
                     (unsigned)(candidates[i] * RASTER_ROW_BYTES / 1024),
                     (unsigned)esp_get_free_heap_size());
            return ESP_OK;
        }
        ESP_LOGW(TAG, "page buffer %u rows failed (%s), retrying smaller",
                 (unsigned)candidates[i], esp_err_to_name(err));
    }
    return ESP_ERR_NO_MEM;
}

/* Keeps looking for the cat printer forever: the BLE scan window is finite
 * (30 s), and the printer is usually off when the board boots. A bound MAC is
 * retried directly (no scan), otherwise fall back to prefix scanning. */
static void ble_rescan_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        if (cp_is_ready()) {
            continue;
        }
        const app_settings_t *cfg = settings_get();
        esp_err_t err = cfg->ble_mac[0]
            ? cp_connect_mac(cfg->ble_mac, cfg->ble_addr_type)
            : cp_scan_and_connect(CONNECT_WAIT_MS);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "rescan failed: %s", esp_err_to_name(err));
        }
    }
}

void app_main(void)
{
    /* NVS is required by WiFi and NimBLE for calibration/bond data. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* Runtime config (NVS overrides the compiled-in Kconfig defaults). */
    settings_init();
    const app_settings_t *cfg = settings_get();

    ESP_ERROR_CHECK(pagebuf_init_best_effort());

    /* ps_init() only records name + job handler, so it must happen BEFORE the
     * network comes up: the GOT_IP handler starts the print server. */
    ESP_ERROR_CHECK(ps_init(cfg->airprint_name, on_print_job, NULL));
    /* The IPP server decodes URF straight into this buffer as the body streams. */
    ps_set_page_buffer(&s_page);

    s_ap_mode = !settings_wifi_configured();
    if (s_ap_mode) {
        wifi_init_ap();
    } else {
        wifi_init_sta();
    }

    ESP_ERROR_CHECK(cp_init(cfg->ble_prefix, on_ble_state, NULL));
    /* In provisioning mode do not burn radio time on a BLE scan: the user is
     * busy joining the AP, and NimBLE/WiFi share the single C3 radio. The WebUI
     * can still scan/pair explicitly; the saved binding is used after reboot. */
    if (!s_ap_mode) {
        if (cfg->ble_mac[0]) {
            cp_connect_mac(cfg->ble_mac, cfg->ble_addr_type);
        } else {
            cp_scan_and_connect(CONNECT_WAIT_MS);
        }
        if (xTaskCreate(ble_rescan_task, "ble_rescan", 3072, NULL, 4, NULL) != pdPASS) {
            ESP_LOGW(TAG, "cannot create ble rescan task");
        }
    }

    /* STA normally starts it from GOT_IP; AP provisioning needs it here. */
    if (!s_ps_started) {
        esp_err_t perr = ps_start();
        if (perr == ESP_OK) {
            s_ps_started = true;
        } else {
            ESP_LOGW(TAG, "print server not up yet (%s); retried on WiFi connect",
                     esp_err_to_name(perr));
        }
    }

    ESP_LOGI(TAG, "ready. mode=%s AirPrint='%s' BLE prefix='%s' bound='%s', free heap %u",
             s_ap_mode ? "AP-PROVISION" : "STA", cfg->airprint_name,
             cfg->ble_prefix, cfg->ble_mac,
             (unsigned)esp_get_free_heap_size());
}
