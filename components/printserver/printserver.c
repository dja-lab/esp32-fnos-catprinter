#include "printserver.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

/* Bridge build: no AirPrint/mDNS layer (protocol lives on the NAS). The
 * WebUI can still rename the printer (it labels the status page), and the
 * mdns stubs keep the public API shape identical to the AirPrint branch. */
static esp_err_t ps_mdns_start(const char *instance_name)
{
    (void)instance_name;
    return ESP_OK;
}
static esp_err_t ps_mdns_stop(void) { return ESP_OK; }
static esp_err_t ps_mdns_rename(const char *instance_name) { (void)instance_name; return ESP_OK; }
extern esp_err_t ps_http_start(void);
extern esp_err_t ps_http_stop(void);

static const char *TAG = "printserver";

static char s_instance_name[32] = "CatPrint-C3";
static ps_job_handler s_handler = NULL;
static void *s_handler_ctx = NULL;
static uint32_t s_next_id = 1;
static bool s_accepting = false;

/* A Print-Job is ACKed immediately and printed by a dedicated worker: printing a
 * page takes tens of seconds (BLE, 20 ms/row), and doing that inside the HTTP
 * task used to block every further request and eventually wedged the board. */
typedef struct {
    ps_job_t job;
} print_msg_t;

static QueueHandle_t s_queue = NULL;
static volatile uint32_t s_last_job = 0;
static volatile int s_job_state = 3;      /* 3 idle, 5 processing, 9 completed, 8 aborted */
static volatile bool s_job_active = false;
/* Multi-page hold: while set, a finished page keeps the job "processing"
 * because the decoder still has more pages of the same document to go. */
static volatile bool s_hold = false;
static volatile uint32_t s_pages_printed = 0;

static void print_worker(void *arg)
{
    (void)arg;
    print_msg_t m;
    for (;;) {
        if (xQueueReceive(s_queue, &m, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (s_handler == NULL) {
            continue;
        }
        ESP_LOGI(TAG, "printing job %lu (%u bytes)",
                 (unsigned long)m.job.job_id, (unsigned)m.job.document_len);
        s_last_job = m.job.job_id;
        s_job_state = 5;                 /* processing */
        s_job_active = true;
        esp_err_t err = s_handler(&m.job, s_handler_ctx);
        s_pages_printed++;
        if (s_hold) {
            /* More pages of this document are still being decoded; keep the
             * job visible as processing instead of flashing "completed". */
            s_job_state = (err == ESP_OK) ? 5 : 8;
        } else {
            s_job_state = (err == ESP_OK) ? 9 : 8;   /* completed / aborted */
            s_job_active = false;
        }
        ESP_LOGI(TAG, "job %lu %s", (unsigned long)m.job.job_id,
                 err == ESP_OK ? "done" : esp_err_to_name(err));
    }
}

uint32_t ps_last_job_id(void)
{
    return s_last_job;
}

int ps_job_state(void)
{
    return s_job_state;
}

bool ps_job_active(void)
{
    return s_job_active;
}

void ps_job_set_hold(bool hold)
{
    s_hold = hold;
    if (!hold && !s_job_active) {
        /* Hold released without a final page dispatched (e.g. the trailing
         * pages were blank): nothing else will clear the artificial
         * "processing" state, so finish the job here. */
        s_job_state = 9;
    }
}

void ps_job_abort(void)
{
    s_hold = false;
    s_job_state = 8;
    s_job_active = false;
    ESP_LOGW(TAG, "job aborted by decoder");
}

uint32_t ps_pages_printed(void)
{
    return s_pages_printed;
}

esp_err_t ps_init(const char *instance_name, ps_job_handler handler, void *ctx)
{
    if (instance_name != NULL) {
        size_t n = strlen(instance_name);
        if (n >= sizeof(s_instance_name)) {
            n = sizeof(s_instance_name) - 1;
        }
        memcpy(s_instance_name, instance_name, n);
        s_instance_name[n] = '\0';
    }
    s_handler = handler;
    s_handler_ctx = ctx;
    return ESP_OK;
}

esp_err_t ps_start(void)
{
    /* mDNS advertises AirPrint. In SoftAP provisioning mode it may not be able
     * to bind a usable interface, but the WebUI must still come up - otherwise
     * there is no way to configure WiFi at all. */
    esp_err_t err = ps_mdns_start(s_instance_name);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS unavailable (%s), continuing with HTTP only",
                 esp_err_to_name(err));
    }
    err = ps_http_start();
    if (err != ESP_OK) {
        ps_mdns_stop();
        return err;
    }
    if (s_queue == NULL) {
        s_queue = xQueueCreate(1, sizeof(print_msg_t));
        if (s_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
        if (xTaskCreate(print_worker, "print_job", 6144, NULL, 4, NULL) != pdPASS) {
            vQueueDelete(s_queue);
            s_queue = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    s_accepting = true;
    ESP_LOGI(TAG, "print server started as '%s'", s_instance_name);
    return ESP_OK;
}

esp_err_t ps_stop(void)
{
    s_accepting = false;
    ps_http_stop();
    ps_mdns_stop();
    return ESP_OK;
}

bool ps_is_accepting(void)
{
    return s_accepting;
}

uint32_t ps_next_job_id(void)
{
    return s_next_id++;
}

static page_buf_t *s_pagebuf = NULL;

void ps_set_page_buffer(page_buf_t *pb)
{
    s_pagebuf = pb;
}

page_buf_t *ps_page_buffer(void)
{
    return s_pagebuf;
}

const char *ps_printer_name(void)
{
    return s_instance_name;
}

static char s_last_media[40] = "";

void ps_note_media(const char *media)
{
    if (media == NULL) {
        return;
    }
    size_t n = strlen(media);
    if (n >= sizeof(s_last_media)) {
        n = sizeof(s_last_media) - 1;
    }
    memcpy(s_last_media, media, n);
    s_last_media[n] = '\0';
}

const char *ps_last_media(void)
{
    return s_last_media;
}

esp_err_t ps_printer_rename(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    size_t n = strlen(name);
    if (n >= sizeof(s_instance_name)) {
        n = sizeof(s_instance_name) - 1;
    }
    memcpy(s_instance_name, name, n);
    s_instance_name[n] = '\0';

    esp_err_t err = ps_mdns_rename(s_instance_name);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "live rename failed (%s), applies after reboot",
                 esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "renamed AirPrint queue to '%s'", s_instance_name);
    return ESP_OK;
}

esp_err_t ps_dispatch_job(const ps_job_t *job)
{
    if (job == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_handler == NULL || s_queue == NULL) {
        ESP_LOGW(TAG, "job handler not ready");
        return ESP_ERR_INVALID_STATE;
    }
    print_msg_t m = { .job = *job };
    /* Queue depth 1: if a job is already printing the caller gets BUSY, which is
     * the honest answer and keeps iOS from waiting on a silent printer. */
    if (xQueueSend(s_queue, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "printer busy, job rejected");
        return ESP_ERR_INVALID_STATE;
    }
    /* Publish the state BEFORE the HTTP ACK is sent. There is a small window
     * where the print worker has not been scheduled yet; without this, a
     * Get-Job-Attributes arriving immediately after the ACK would report the
     * PREVIOUS job's state (9 = completed) and iOS would close the job early. */
    s_last_job = job->job_id;
    s_job_state = 5;
    s_job_active = true;
    return ESP_OK;
}
