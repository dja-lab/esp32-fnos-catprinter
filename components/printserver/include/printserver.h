/*
 * AirPrint print server facade: mDNS advertisement + IPP over HTTP :631.
 *
 * Design decision: TXT `pdl` advertises ONLY image/urf. iOS therefore
 * rasterizes documents itself and sends Apple Raster, so the ESP32 never
 * has to parse PDF. A Print-Job is acknowledged immediately, then the
 * document is handed to the registered handler for decode + BLE printing.
 */
#ifndef PRINTSERVER_H
#define PRINTSERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "pagebuf.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t job_id;
    const uint8_t *document;     /* NULL when the page is already decoded */
    size_t document_len;
    bool in_flash;               /* document is in the 'storage' partition ... */
    size_t flash_offset;         /* ... starting at this offset */
    bool page_ready;             /* rows are already in the page buffer */
    bool last_page;              /* false = intermediate page of a multi-page
                                  * job: no trailing feed (next page continues
                                  * on the same paper), no tear-off gap */
} ps_job_t;

typedef esp_err_t (*ps_job_handler)(const ps_job_t *job, void *ctx);

/* `instance_name` is the name shown in the iPhone print picker. */
esp_err_t ps_init(const char *instance_name, ps_job_handler handler, void *ctx);
esp_err_t ps_start(void);
esp_err_t ps_stop(void);
bool ps_is_accepting(void);

/* Called by the HTTP layer; dispatches into the registered handler. */
esp_err_t ps_dispatch_job(const ps_job_t *job);
uint32_t ps_next_job_id(void);
uint32_t ps_last_job_id(void);
int ps_job_state(void);
/* True from the moment a job is accepted until its handler returns. Get-Jobs
 * must report an empty list once this goes false, otherwise iOS thinks the
 * queue never drains and marks the printer unavailable after the first job. */
bool ps_job_active(void);

/* Multi-page support. While a document is still being decoded the decoder
 * holds the job in "processing" so state polls between pages don't flash
 * "completed" (which can make iOS stop tracking the job). Call
 * ps_job_set_hold(true) before dispatching the first page and
 * ps_job_set_hold(false) right before the final page's dispatch. If the
 * decode fails, ps_job_abort() releases the hold and marks the job aborted. */
void ps_job_set_hold(bool hold);
void ps_job_abort(void);
/* Number of pages (or jobs) whose handler has returned — used by the decoder
 * to wait for a dispatched page to finish printing before reusing the shared
 * page buffer. */
uint32_t ps_pages_printed(void);

/* The page buffer that streaming jobs decode into (owned by the app). */
void ps_set_page_buffer(page_buf_t *pb);
page_buf_t *ps_page_buffer(void);

/* mDNS instance name == what iOS shows in the printer picker. */
const char *ps_printer_name(void);

/* Rename the advertised AirPrint queue at runtime (no reboot): sends a Bonjour
 * goodbye for the old instance name, then re-announces the new one plus its TXT
 * records. Returns ESP_ERR_INVALID_STATE when mDNS is not running (e.g. SoftAP
 * provisioning) — the name is still stored and used on the next boot. */
esp_err_t ps_printer_rename(const char *name);

/* Paper keyword the last Print-Job asked for, e.g. "iso_a8_52x74mm" ("" when
 * the client did not send `media`, which means "use media-default"). Purely
 * diagnostic: iOS picks the size, we only report what it picked. */
void ps_note_media(const char *media);
const char *ps_last_media(void);

#ifdef __cplusplus
}
#endif
#endif
