/*
 * Minimal IPP-over-HTTP server for port 631.
 *
 * Why not esp_http_server: iOS (PrintKit) sends IPP bodies with
 * Transfer-Encoding: chunked. ESP-IDF's httpd consumes the chunked body during
 * header parsing, exposes content_len == 0, caps httpd_req_recv() at
 * ra->remaining_len (0) and therefore cannot hand the body to a handler at all;
 * reading the socket directly only yields the trailing "0" chunk. So we speak
 * HTTP ourselves.
 *
 * Why no flash spool / no body buffering: a Print-Job raster is 100 KB - 2 MB,
 * while the board has ~13 KB of free heap, and erasing flash while receiving
 * starves the WiFi driver (the TCP connection died mid-upload). URF/PackBits is
 * sequential, so the body is decoded as it arrives straight into the page
 * buffer; only the finished 1-bit rows are kept.
 */
#include "printserver.h"
#include "ipp.h"
#include "settings.h"
#include "urf_stream.h"
#include "webui.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "ipp_srv";

#define IPP_PORT   631
/* iOS streams the URF raster as a long series of small HTTP chunks. With a
 * 512-byte socket buffer a 175 KB page needed ~350 recv() rounds, each costing
 * a Wi-Fi round trip - the upload alone measured 20 s. Use a bigger read and
 * keep it in the per-worker context (the 6 KB task stack cannot hold it). */
#define RD_BUF     2048u
/* Refuse new connections below this much free heap. Must stay BELOW the heap a
 * running job needs (measured ~10 KB while decoding an A6 page) so progress
 * polling still works during a print, yet high enough that lwIP can always
 * free a PCB. The board died at 3 KB, so 6 KB is the danger line. */
#define HEAP_FLOOR 6144u
#define PRE_MAX    2048u

/* ---- health watchdog ---------------------------------------------------
 * Failure mode seen in the field: the phone opens a burst of parallel
 * Get-Printer-Attributes connections, the two workers plus the accept queue
 * soak up every socket buffer, and the board ends up in a state where the
 * accept loop still runs (it keeps logging "alive") but NOTHING arrives any
 * more - no ping, no mDNS, no TCP. From the phone that looks like "printer
 * cannot be found" and the only cure used to be a power cycle.
 *
 * Two independent detectors, both skipped while a job is printing (heap and
 * workers are legitimately busy then):
 *   1. free heap parked under HEALTH_LOW_HEAP for HEALTH_LOW_LIMIT rounds;
 *   2. our own HTTP server stops answering a loopback request.
 * Either one restarts the board, which brings the network back in ~5 s. */
#define HEALTH_LOW_HEAP       10240u
#define HEALTH_LOW_LIMIT      8          /* 8 x 15 s = 120 s */
#define HEALTH_PROBE_FAIL_MAX 3
#define HEALTH_PERIOD_MS      15000
/* Two receive timeouts, chosen per phase:
 *   - short (2 s) while parsing the request line, headers and the IPP prefix;
 *   - long  (10 s) once a Print-Job is identified and a whole page streams in.
 * iOS opens a fresh chunked connection for EVERY poll. A client that stops
 * mid-chunk used to pin one of only two workers for BRD_IDLE_MAX * 10 s = 20 s,
 * so all later requests (including the real Print-Job) queued behind it. That
 * is the ~20 s gap between the phone's progress bar finishing and the printer
 * actually starting. BRD_IDLE_MAX is the number of consecutive idle rounds
 * tolerated; brd_t.idle_max lets the short path give up after a single round. */
#define BRD_SHORT_TIMEOUT_S 2
#define BRD_LONG_TIMEOUT_S  10
#define BRD_IDLE_MAX 2

static volatile bool s_running = false;
static TaskHandle_t s_task = NULL;

/* ---- buffered reader over a socket ------------------------------------- */
typedef struct {
    int fd;
    uint8_t *buf;      /* per-worker scratch (worker_ctx_t.rxbuf) */
    size_t cap;
    size_t len;
    size_t pos;
    int idle_max;      /* consecutive recv timeouts before giving up */
} brd_t;

static int brd_fill(brd_t *b)
{
    if (b->pos < b->len) {
        return 1;
    }
    int n = recv(b->fd, b->buf, b->cap, 0);
    if (n > 0) {
        b->len = (size_t)n;
        b->pos = 0;
        return 1;
    }
    if (n == 0) {
        return -1;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return 0;
    }
    return -1;
}

static int brd_line(brd_t *b, char *out, size_t cap)
{
    size_t n = 0;
    int idle = 0;
    for (;;) {
        int rc = brd_fill(b);
        if (rc == 0) {
            /* recv() timed out (SO_RCVTIMEO). Busy-looping here used to wedge
             * the worker forever when a client stopped mid-chunk. */
            if (++idle >= b->idle_max) {
                return -1;
            }
            continue;
        }
        if (rc < 0) {
            return -1;
        }
        idle = 0;
        char c = (char)b->buf[b->pos++];
        if (c == '\n') {
            break;
        }
        if (c != '\r' && n + 1 < cap) {
            out[n++] = c;
        }
    }
    out[n] = '\0';
    return (int)n;
}

static int brd_read(brd_t *b, uint8_t *dst, size_t n)
{
    size_t got = 0;
    int idle = 0;
    while (got < n) {
        int rc = brd_fill(b);
        if (rc == 0) {
            /* Peer stalled: give up instead of spinning so the worker can
             * answer an error and serve the next connection. */
            if (++idle >= b->idle_max) {
                return -1;
            }
            continue;
        }
        if (rc < 0) {
            break;
        }
        idle = 0;
        size_t avail = b->len - b->pos;
        size_t take = (n - got < avail) ? (n - got) : avail;
        memcpy(dst + got, b->buf + b->pos, take);
        b->pos += take;
        got += take;
    }
    return (int)got;
}

/* ---- body reader: hides Content-Length vs chunked framing ---------------- */
typedef struct {
    brd_t *b;
    bool chunked;
    size_t fixed_left;
    unsigned long chunk_left;
    bool eof;
} body_t;

static void body_init(body_t *r, brd_t *b, bool chunked, size_t content_len)
{
    r->b = b;
    r->chunked = chunked;
    r->fixed_left = chunked ? 0 : content_len;
    r->chunk_left = 0;
    r->eof = false;
}

/* Returns bytes delivered (>0), 0 at end of body, -1 on error. */
static int body_read(body_t *r, uint8_t *dst, size_t want)
{
    if (r->eof) {
        return 0;
    }
    if (!r->chunked) {
        if (r->fixed_left == 0) {
            r->eof = true;
            return 0;
        }
        if (want > r->fixed_left) {
            want = r->fixed_left;
        }
        int got = brd_read(r->b, dst, want);
        if (got <= 0) {
            r->eof = true;
            return got;
        }
        r->fixed_left -= (size_t)got;
        return got;
    }

    char line[64];
    while (r->chunk_left == 0) {
        if (brd_line(r->b, line, sizeof(line)) < 0) {
            r->eof = true;
            return -1;
        }
        if (line[0] == '\0') {
            continue;
        }
        unsigned long sz = strtoul(line, NULL, 16);
        if (sz == 0) {
            /* Consume the trailing CRLF of "0\r\n\r\n" if it is already in the
             * buffer. Do NOT call brd_line() here: some clients wait for our
             * reply before sending it, and blocking on a read would add 2 s to
             * every poll. Unread bytes make close() emit a TCP RST, which can
             * make iOS discard an otherwise valid IPP response. */
            while (r->b->pos < r->b->len && r->b->buf[r->b->pos] == '\r') {
                r->b->pos++;
            }
            if (r->b->pos < r->b->len && r->b->buf[r->b->pos] == '\n') {
                r->b->pos++;
            }
            r->eof = true;
            return 0;
        }
        r->chunk_left = sz;
    }
    if (want > r->chunk_left) {
        want = (size_t)r->chunk_left;
    }
    int got = brd_read(r->b, dst, want);
    if (got <= 0) {
        r->eof = true;
        return got;
    }
    r->chunk_left -= (unsigned long)got;
    if (r->chunk_left == 0) {
        brd_line(r->b, line, sizeof(line));   /* CRLF after chunk data */
    }
    return got;
}

/* `buf` is the worker's scratch (ctx->pre, no longer needed once the IPP
 * prefix has been handed to the decoder). Progress logs show whether the upload
 * is uniformly slow or stalls in bursts, which distinguishes a network RTT
 * problem from CPU-bound decoding. */
static esp_err_t body_feed_urf(body_t *r, urf_stream_t *us, size_t *bytes,
                               uint8_t *buf, size_t buf_len,
                               int64_t start_us, int64_t *eof_us)
{
    size_t total = 0;
    int64_t last_log_us = start_us;
    esp_err_t err = ESP_OK;
    for (;;) {
        int got = body_read(r, buf, buf_len);
        if (got < 0) {
            err = ESP_FAIL;
            break;
        }
        if (got == 0) {
            *eof_us = esp_timer_get_time();
            ESP_LOGI(TAG, "rx done: %u B in %u ms", (unsigned)total,
                     (unsigned)((*eof_us - start_us) / 1000));
            break;
        }
        total += (size_t)got;
        err = urf_stream_feed(us, buf, (size_t)got);
        if (err != ESP_OK) {
            break;
        }
        int64_t now = esp_timer_get_time();
        if (now - last_log_us >= 2000000) {
            last_log_us = now;
            ESP_LOGI(TAG, "rx %u B / %u ms", (unsigned)total,
                     (unsigned)((now - start_us) / 1000));
        }
    }
    if (bytes != NULL) {
        *bytes = total;
    }
    return err;
}

static void body_drain(body_t *r)
{
    uint8_t buf[RD_BUF];
    while (body_read(r, buf, sizeof(buf)) > 0) {
    }
}

/* ---- multi-page streaming ----------------------------------------------- */

/* Upper bound for waiting on one page to print (BLE printing is slow by
 * design; ~25 s per A8 page at the default speed). */
#define PAGE_FLUSH_TIMEOUT_MS (300000u)

/* urf_page_end_cb: an intermediate page of a multi-page document has been
 * decoded into the shared page buffer. Trim its trailing blank rows (the
 * bottom margin iOS renders), print it WITHOUT trailing feed so the next
 * page continues seamlessly on the roll, and block until the print worker
 * has finished: the same buffer is reused for the next page, and pausing
 * the decode applies TCP back-pressure to the phone (it simply stops
 * sending while we print, and resumes afterwards). */
static void page_flush_cb(void *arg, uint32_t page_no)
{
    uint32_t job_id = *(uint32_t *)arg;
    page_buf_t *pb = ps_page_buffer();
    if (pb == NULL || pb->rows == NULL) {
        return;
    }
    uint32_t n = pb_trim_trailing_blank(pb);
    if (n == 0) {
        ESP_LOGI(TAG, "page %u fully blank, nothing to print", (unsigned)page_no);
        return;
    }

    ps_job_t job = {
        .job_id = job_id,
        .document = NULL,
        .document_len = 0,
        .in_flash = false,
        .flash_offset = 0,
        .page_ready = true,
        .last_page = false,
    };
    uint32_t before = ps_pages_printed();
    int64_t deadline = esp_timer_get_time() +
                       (int64_t)PAGE_FLUSH_TIMEOUT_MS * 1000;

    for (;;) {
        if (ps_dispatch_job(&job) == ESP_OK) {
            break;
        }
        if (esp_timer_get_time() > deadline) {
            ESP_LOGE(TAG, "page %u: printer stayed busy, page dropped", (unsigned)page_no);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    while (ps_pages_printed() == before) {
        if (esp_timer_get_time() > deadline) {
            ESP_LOGE(TAG, "page %u: print timed out, continuing decode", (unsigned)page_no);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    /* Page is on the paper — free the buffer for the next page. Without this
     * the next page would be appended BEHIND the already-printed rows and the
     * earlier pages would be re-printed with every page. */
    pb_clear(pb);
    ESP_LOGI(TAG, "page %u printed (%u rows), continuing", (unsigned)page_no, (unsigned)n);
}

/* ---- responses --------------------------------------------------------- */

static size_t build_printer_attrs(uint8_t *buf, size_t cap, uint32_t request_id,
                                  uint16_t version)
{
    ipp_writer_t w;
    ipp_writer_init(&w, buf, cap);

    ipp_resp_begin(&w, version, IPP_STATUS_OK, request_id);
    ipp_add_string(&w, IPP_TAG_CHARSET, "attributes-charset", "utf-8");
    ipp_add_string(&w, IPP_TAG_NATURAL_LANG, "attributes-natural-language", "en");

    ipp_add_group(&w, IPP_TAG_PRINTER);

    /* identity */
    ipp_add_string(&w, IPP_TAG_URI, "printer-uri-supported",
                   "ipp://catprint-c3.local:631/ipp/print");
    ipp_add_string(&w, IPP_TAG_KEYWORD, "uri-security-supported", "none");
    ipp_add_string(&w, IPP_TAG_KEYWORD, "uri-authentication-supported", "none");
    ipp_add_string(&w, IPP_TAG_NAME, "printer-name", ps_printer_name());
    ipp_add_string(&w, IPP_TAG_TEXT, "printer-info", ps_printer_name());
    /* iOS shows this string in the print options once a printer is selected and
     * the mDNS instance name in the picker list. A hard-coded model string here
     * made the SAME printer appear under two different names ("CatPrint" ->
     * "ESP32 Cat Printer"), which reads like a different device. Keep it in
     * lock-step with the configured name. */
    ipp_add_string(&w, IPP_TAG_TEXT, "printer-make-and-model", ps_printer_name());
    ipp_add_string(&w, IPP_TAG_URI, "printer-uuid",
                   "urn:uuid:6b1d0000-0000-1000-8000-e83dc19ddc38");
    /* Advertised by every CUPS/PrintKit probe as expected metadata. Cheap to
     * add and it makes the printer look "complete" to macOS/iOS discovery. */
    ipp_add_string(&w, IPP_TAG_TEXT, "printer-location", "CatPrint C3");
    ipp_add_string(&w, IPP_TAG_URI, "printer-more-info",
                   "http://catprint-c3.local/");
    ipp_add_integer(&w, IPP_TAG_INTEGER, "printer-up-time",
                    (int32_t)(esp_timer_get_time() / 1000000));

    /* state — report "processing" while a job runs. Saying "idle" made iOS poll
     * ~10x/s for the whole print, and that WiFi traffic on the shared C3 radio
     * corrupted the BLE row stream ("stripes"). printer-state, queued-job-count
     * and Get-Jobs MUST agree, otherwise iOS keeps the picker in a retry loop. */
    bool busy = ps_job_active();
    ipp_add_integer(&w, IPP_TAG_ENUM, "printer-state", busy ? 4 : 3);
    ipp_add_string(&w, IPP_TAG_KEYWORD, "printer-state-reasons", "none");
    ipp_add_bool(&w, IPP_TAG_BOOLEAN, "printer-is-accepting-jobs", 1);
    ipp_add_integer(&w, IPP_TAG_INTEGER, "queued-job-count", busy ? 1 : 0);

    /* protocol / formats — iOS will not select a printer without these */
    ipp_add_string(&w, IPP_TAG_KEYWORD, "ipp-versions-supported", "1.1");
    ipp_add_string(&w, IPP_TAG_KEYWORD, "ipp-versions-supported", "2.0");
    ipp_add_string(&w, IPP_TAG_KEYWORD, "ipp-features-supported", "airprint-1.0");
    ipp_add_integer(&w, IPP_TAG_ENUM, "operations-supported", 0x0002);
    ipp_add_integer(&w, IPP_TAG_ENUM, "operations-supported", 0x0004);
    ipp_add_integer(&w, IPP_TAG_ENUM, "operations-supported", 0x0008);
    ipp_add_integer(&w, IPP_TAG_ENUM, "operations-supported", 0x0009);
    ipp_add_integer(&w, IPP_TAG_ENUM, "operations-supported", 0x000A);
    ipp_add_integer(&w, IPP_TAG_ENUM, "operations-supported", 0x000B);
    ipp_add_string(&w, IPP_TAG_CHARSET, "charset-configured", "utf-8");
    ipp_add_string(&w, IPP_TAG_CHARSET, "charset-supported", "utf-8");
    ipp_add_string(&w, IPP_TAG_NATURAL_LANG, "natural-language-configured", "en");
    ipp_add_string(&w, IPP_TAG_NATURAL_LANG, "generated-natural-language-supported", "en");
    ipp_add_string(&w, IPP_TAG_MIMETYPE, "document-format-default", "image/urf");
    ipp_add_string(&w, IPP_TAG_MIMETYPE, "document-format-supported", "image/urf");
    /* Required by the AirPrint spec whenever image/urf is advertised: iOS reads
     * these to decide which raster it may produce. */
    ipp_add_string(&w, IPP_TAG_KEYWORD, "urf-supported", "CP1");
    ipp_add_string(&w, IPP_TAG_KEYWORD, "urf-supported", "CR1");
    ipp_add_string(&w, IPP_TAG_KEYWORD, "urf-supported", "SRGB24");
    ipp_add_string(&w, IPP_TAG_KEYWORD, "urf-supported", "RS300");
    ipp_add_string(&w, IPP_TAG_KEYWORD, "urf-supported", "FB8");
    ipp_add_string(&w, IPP_TAG_KEYWORD, "urf-supported", "W8");
    ipp_add_string(&w, IPP_TAG_KEYWORD, "urf-supported", "DM1");
    ipp_add_string(&w, IPP_TAG_KEYWORD, "compression-supported", "none");
    ipp_add_bool(&w, IPP_TAG_BOOLEAN, "multiple-document-jobs-supported", 0);
    ipp_add_string(&w, IPP_TAG_KEYWORD, "job-creation-attributes-supported", "copies");
    ipp_add_string(&w, IPP_TAG_KEYWORD, "job-creation-attributes-supported", "document-format");
    ipp_add_string(&w, IPP_TAG_KEYWORD, "job-creation-attributes-supported", "media");
    ipp_add_string(&w, IPP_TAG_KEYWORD, "job-creation-attributes-supported", "print-color-mode");

    /* media / quality — advertise the common page sizes as well: if the app's
     * document page size is not in media-supported, iOS can refuse to submit the
     * job (the sheet just sits there). The device scales everything down to the
     * 384-dot head anyway, so accepting more sizes costs nothing.
     *
     * DEFAULT IS A6. Two hard constraints decided this:
     *  - A4/Letter @300 dpi is a 2480x3507 raster (~175 KB of URF) that we then
     *    downsample to 384 dots; the phone still has to render and upload every
     *    pixel, which measured 20 s before the TCP tweaks.
     *  - A8 (52x74 mm) is NOT in iOS's built-in media table. iOS never lists it
     *    in the print sheet, and - worse - when it is media-default the phone
     *    cannot build a paper setting at all: the picker spins and drops back to
     *    "select a printer". That is exactly what happened after the AirPrint
     *    name was changed (renaming invalidates the cached capabilities and
     *    forces a fresh negotiation). Never advertise A8 again.
     * A6 (105x148 mm) is the smallest size iOS really knows: ~63 KB raster,
     * ~4.5 s upload, and long documents split into pages that the multi-page
     * path already stitches seamlessly on the roll. */
    ipp_add_string(&w, IPP_TAG_KEYWORD, "media-supported", "iso_a4_210x297mm");
    ipp_add_string(&w, IPP_TAG_KEYWORD, "media-supported", "na_letter_8.5x11in");
    ipp_add_string(&w, IPP_TAG_KEYWORD, "media-supported", "iso_a5_148x210mm");
    ipp_add_string(&w, IPP_TAG_KEYWORD, "media-supported", "iso_a6_105x148mm");
    ipp_add_string(&w, IPP_TAG_KEYWORD, "media-supported", "na_index-4x6_4x6in");
    ipp_add_string(&w, IPP_TAG_KEYWORD, "media-default", "iso_a6_105x148mm");
    ipp_add_string(&w, IPP_TAG_KEYWORD, "media-ready", "iso_a6_105x148mm");
    /* media-col-default is deliberately NOT advertised.
     *
     * PWG collection encoding is a minefield: CUPS validates every member name
     * and reported "the named IPP attribute in the collection is invalid" for
     * a byte-for-byte correct media-col, and an invalid collection poisons the
     * ENTIRE attribute set - the client then re-probes Get-Printer-Attributes
     * forever (which is what bricked the board's heap). Flat media-supported /
     * media-default / media-ready already give iOS a working paper selection,
     * and ipptool only reports it as an EXPECT warning, not a protocol error. */
    ipp_add_string(&w, IPP_TAG_KEYWORD, "sides-supported", "one-sided");
    ipp_add_string(&w, IPP_TAG_KEYWORD, "sides-default", "one-sided");
    ipp_add_string(&w, IPP_TAG_KEYWORD, "print-color-mode-supported", "monochrome");
    ipp_add_string(&w, IPP_TAG_KEYWORD, "print-color-mode-default", "monochrome");
    ipp_add_bool(&w, IPP_TAG_BOOLEAN, "color-supported", 0);
    ipp_add_integer(&w, IPP_TAG_ENUM, "print-quality-supported", 4);
    ipp_add_integer(&w, IPP_TAG_ENUM, "print-quality-default", 4);
    {
        const uint8_t res[9] = {0x00, 0x00, 0x01, 0x2c,
                                0x00, 0x00, 0x01, 0x2c, 0x03};   /* 300 dpi */
        ipp_add_attr(&w, IPP_TAG_RESOLUTION, "printer-resolution-default", res, 9);
        ipp_add_attr(&w, IPP_TAG_RESOLUTION, "printer-resolution-supported", res, 9);
    }

    ipp_resp_finish(&w);
    return w.len;
}

static size_t build_job_ack(uint8_t *buf, size_t cap, uint32_t request_id,
                            uint32_t job_id, uint16_t status, uint16_t version)
{
    ipp_writer_t w;
    ipp_writer_init(&w, buf, cap);
    ipp_resp_begin(&w, version, status, request_id);
    ipp_add_string(&w, IPP_TAG_CHARSET, "attributes-charset", "utf-8");
    ipp_add_string(&w, IPP_TAG_NATURAL_LANG, "attributes-natural-language", "en");
    ipp_add_group(&w, IPP_TAG_JOB);
    /* job-uri MUST match job-id. It used to be hardcoded to "?job=1", so after
     * the first print iOS tracked a URI that no longer matched the job it had
     * just submitted and kept re-querying a phantom job. */
    char uri[96];
    snprintf(uri, sizeof(uri), "ipp://catprint-c3.local:631/ipp/print?job=%lu",
             (unsigned long)job_id);
    ipp_add_string(&w, IPP_TAG_URI, "job-uri", uri);
    ipp_add_integer(&w, IPP_TAG_INTEGER, "job-id", (int32_t)job_id);
    ipp_add_integer(&w, IPP_TAG_ENUM, "job-state", 3);           /* processing */
    ipp_add_string(&w, IPP_TAG_KEYWORD, "job-state-reasons", "none");
    ipp_resp_finish(&w);
    return w.len;
}

/* Shared job-attributes group for Get-Job-Attributes / Get-Jobs. */
static void add_job_group(ipp_writer_t *w, uint32_t jid, int st)
{
    char uri[96];
    snprintf(uri, sizeof(uri), "ipp://catprint-c3.local:631/ipp/print?job=%lu",
             (unsigned long)jid);
    ipp_add_group(w, IPP_TAG_JOB);
    ipp_add_integer(w, IPP_TAG_INTEGER, "job-id", (int32_t)jid);
    ipp_add_string(w, IPP_TAG_URI, "job-uri", uri);
    ipp_add_string(w, IPP_TAG_URI, "job-printer-uri",
                   "ipp://catprint-c3.local:631/ipp/print");
    ipp_add_integer(w, IPP_TAG_ENUM, "job-state", st);
    /* A terminal job needs a terminal reason. Reporting "none" together with
     * job-state=9 made iOS treat the job as unfinished and keep polling. */
    const char *reason =
        (st == 9) ? "job-completed-successfully" :
        (st == 8) ? "job-completed-with-errors" :
        (st == 7) ? "job-canceled-by-user" : "none";
    ipp_add_string(w, IPP_TAG_KEYWORD, "job-state-reasons", reason);
}

static void send_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    while (len > 0) {
        int n = send(fd, p, len, 0);
        if (n <= 0) {
            return;
        }
        p += n;
        len -= (size_t)n;
    }
}

static void send_ipp(int fd, const uint8_t *body, size_t len)
{
    char hdr[192];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: application/ipp\r\n"
                     "Content-Length: %u\r\n"
                     "Connection: close\r\n\r\n", (unsigned)len);
    send_all(fd, hdr, (size_t)n);
    send_all(fd, body, len);
}

/* WebUI page + REST API live in webui.c; handle_client() routes GET and POST
 * API requests there and keeps /ipp/print for IPP. */

static void send_err(int fd, int code, const char *msg)
{
    char buf[192];
    int n = snprintf(buf, sizeof(buf),
                     "HTTP/1.1 %d %s\r\nContent-Length: 0\r\n"
                     "Connection: close\r\n\r\n", code, msg);
    send_all(fd, buf, (size_t)n);
}

/* Single client at a time, so one shared response buffer keeps it off the stack
 * (a 2 KB local used to overflow the task stack: "Stack protection fault"). */
typedef struct {
    uint8_t response[4096];
    uint8_t pre[PRE_MAX];     /* IPP prefix, reused as the URF feed scratch */
    uint8_t rxbuf[RD_BUF];    /* socket read buffer */
} worker_ctx_t;

static worker_ctx_t s_ctx[2];
static QueueHandle_t s_conn_q = NULL;

static void log_req_attrs(const uint8_t *buf, size_t len);

/* Read one string attribute out of the IPP attribute block. Record layout is
 * 1-byte tag, 2-byte name length, name, 2-byte value length, value; group tags
 * (<= 0x05) carry no name/value pair. */
static bool find_attr_string(const uint8_t *buf, size_t len, const char *want,
                             char *out, size_t cap)
{
    size_t off = 8;                    /* version + operation + request-id */
    size_t wl = strlen(want);
    if (out == NULL || cap == 0) {
        return false;
    }
    out[0] = '\0';
    while (off + 1 < len) {
        uint8_t tag = buf[off++];
        if (tag == IPP_TAG_END) {
            break;
        }
        if (tag <= 0x05) {
            continue;
        }
        if (off + 2 > len) {
            break;
        }
        size_t nl = ((size_t)buf[off] << 8) | buf[off + 1];
        off += 2;
        if (off + nl + 2 > len) {
            break;
        }
        const char *name = (const char *)(buf + off);
        off += nl;
        size_t vl = ((size_t)buf[off] << 8) | buf[off + 1];
        off += 2;
        if (off + vl > len) {
            break;
        }
        if (nl == wl && memcmp(name, want, nl) == 0) {
            size_t c = vl < cap - 1 ? vl : cap - 1;
            memcpy(out, buf + off, c);
            out[c] = '\0';
            return c > 0;
        }
        off += vl;
    }
    return false;
}

/* ---- one client -------------------------------------------------------- */
static void handle_client(int fd, worker_ctx_t *ctx)
{
    struct timeval rcv_tv = { .tv_sec = BRD_SHORT_TIMEOUT_S, .tv_usec = 0 };
    struct timeval snd_tv = { .tv_sec = BRD_LONG_TIMEOUT_S, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_tv, sizeof(rcv_tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd_tv, sizeof(snd_tv));
    /* Without this every small request/response pair waits on delayed ACKs
     * (~40 ms). iOS polls Get-Printer-Attributes continuously, so that latency
     * turned into a request storm that starved the actual print job. */
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    /* lwIP's delayed ACK can hold an incoming segment's ACK for up to one TCP
     * timer tick. iOS paces its HTTP chunks on those ACKs, so a late ACK adds a
     * round trip to every chunk. Ask for immediate ACKs while the body streams. */
#ifdef TCP_QUICKACK
    int quickack = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &quickack, sizeof(quickack));
#endif

    /* Polling connections get a single idle round (2 s) before we let go. */
    brd_t brd = { .fd = fd, .buf = ctx->rxbuf, .cap = sizeof(ctx->rxbuf),
                  .idle_max = 1 };
    char line[512];

    if (brd_line(&brd, line, sizeof(line)) < 0) {
        return;
    }
    char method[16] = "", path[256] = "";
    if (sscanf(line, "%15s %255s", method, path) != 2) {
        return;
    }

    size_t content_len = 0;
    bool chunked = false;
    bool expect_continue = false;
    char ua[64] = "";
    for (;;) {
        if (brd_line(&brd, line, sizeof(line)) < 0) {
            return;
        }
        if (line[0] == '\0') {
            break;
        }
        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            content_len = (size_t)strtoul(line + 15, NULL, 10);
        } else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0) {
            if (strcasestr(line, "chunked") != NULL) {
                chunked = true;
            }
        } else if (strncasecmp(line, "Expect:", 7) == 0) {
            if (strcasestr(line, "100-continue") != NULL) {
                expect_continue = true;
            }
        } else if (strncasecmp(line, "User-Agent:", 11) == 0) {
            strncpy(ua, line + 11, sizeof(ua) - 1);
        }
        if (strncasecmp(line, "Connection:", 11) == 0) {
            ESP_LOGD(TAG, "hdr %s", line);
        }
    }
    /* Peer address in the log: with a phone, a Mac's CUPS and ipptool all
     * probing the same port, "who is hammering the board" has to be answerable
     * from the serial log alone. */
    char peer[24] = "?";
    {
        struct sockaddr_in pa;
        socklen_t pl = sizeof(pa);
        if (getpeername(fd, (struct sockaddr *)&pa, &pl) == 0) {
            snprintf(peer, sizeof(peer), "%s", inet_ntoa(pa.sin_addr));
        }
    }
    ESP_LOGI(TAG, "%s %s chunked=%d len=%u expect100=%d peer=%s ua='%s'", method,
             path, (int)chunked, (unsigned)content_len, (int)expect_continue,
             peer, ua);

    /* A client that sends "Expect: 100-continue" waits for the interim response
     * before uploading; without it the job never starts and iOS keeps polling. */
    if (expect_continue) {
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        send_all(fd, cont, sizeof(cont) - 1);
    }

    if (strcmp(method, "GET") == 0) {
        webui_serve(fd, path);
        return;
    }
    if (strncmp(path, "/api/", 5) == 0) {
        /* WebUI POST: a short urlencoded form. Reuse the per-worker scratch
         * buffer so nothing large lands on the 6 KB task stack. */
        body_t body;
        body_init(&body, &brd, chunked, content_len);
        uint8_t *buf = ctx->pre;
        size_t blen = 0;
        while (blen + 1 < PRE_MAX) {
            int got = body_read(&body, buf + blen, PRE_MAX - blen - 1);
            if (got <= 0) break;
            blen += (size_t)got;
        }
        buf[blen] = '\0';
        webui_api(fd, path, (const char *)buf, blen);
        return;
    }
    if (strcmp(method, "POST") != 0 || strcmp(path, "/ipp/print") != 0) {
        send_err(fd, 404, "Not Found");
        return;
    }
    if (!chunked && content_len == 0) {
        send_err(fd, 400, "Bad Request");
        return;
    }

    /* The IPP header + job attributes are all we need to decide what to do; they
     * arrive in the first few hundred bytes. body_read() already strips chunk
     * framing, so `pre` really does start with the IPP version bytes. */
    body_t body;
    body_init(&body, &brd, chunked, content_len);

    uint8_t *pre = ctx->pre;
    size_t pre_len = 0;
    ipp_request_t msg;
    bool parsed = false;
    while (!parsed && pre_len < PRE_MAX) {
        size_t want = PRE_MAX - pre_len;
        if (want > RD_BUF) {
            want = RD_BUF;
        }
        int got = body_read(&body, pre + pre_len, want);
        if (got <= 0) {
            break;
        }
        pre_len += (size_t)got;
        parsed = (ipp_parse_request(pre, pre_len, &msg) == 0);
    }
    if (!parsed) {
        ESP_LOGE(TAG, "malformed IPP request (%u prefix bytes)", (unsigned)pre_len);
        send_err(fd, 400, "Bad IPP");
        return;
    }
    ESP_LOGI(TAG, "op=0x%04x rid=%u", msg.operation_id, (unsigned)msg.request_id);

    /* One-shot hex dump of the first request body (debug builds only): shows
     * which attributes the client asks for and whether the URF header arrived
     * intact. */
#if CONFIG_AIRPRINT_DEBUG_RASTER_DUMP
    {
        static bool dumped = false;
        if (!dumped && pre_len > 0) {
            dumped = true;
            char hex[3 * 64 + 1];
            size_t n = pre_len < 64 ? pre_len : 64;
            for (size_t i = 0; i < n; i++) {
                snprintf(hex + i * 3, 4, "%02x ", pre[i]);
            }
            hex[n * 3] = '\0';
            ESP_LOGI(TAG, "first request hex: %s", hex);
        }
    }
#endif

    if (msg.operation_id == IPP_OP_GET_PRINTER_ATTRIBUTES) {
        log_req_attrs(pre, pre_len);
    }

    if (msg.operation_id == IPP_OP_PRINT_JOB) {
        /* This is a real document upload, not a poll. Relax the receive timeout
         * and allow two idle rounds: iOS renders the URF incrementally and may
         * pause between chunks for several seconds. */
        struct timeval long_tv = { .tv_sec = BRD_LONG_TIMEOUT_S, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &long_tv, sizeof(long_tv));
        brd.idle_max = BRD_IDLE_MAX;
        const int64_t recv_start_us = esp_timer_get_time();
        int64_t eof_us = recv_start_us;
        size_t doc_bytes = 0;

        /* What paper size did the client actually choose? iOS only sends
         * `media` when it has a size it recognises; otherwise it silently falls
         * back to the job default, which is exactly what we want to see. */
        {
            char media[40];
            if (find_attr_string(pre, pre_len, "media", media, sizeof(media))) {
                ESP_LOGI(TAG, "job media=%s", media);
                ps_note_media(media);
            } else {
                ESP_LOGI(TAG, "job media: none sent (client paper not set)");
                ps_note_media("");
            }
        }

        page_buf_t *pb = ps_page_buffer();
        if (pb == NULL || pb->rows == NULL) {
            body_drain(&body);
            send_err(fd, 500, "No page buffer");
            return;
        }
        urf_stream_t us;
        if (urf_stream_init(&us, pb) != ESP_OK) {
            send_err(fd, 500, "Decoder init failed");
            return;
        }
        /* Streaming multi-page mode: for orientations without 180-degree
         * rotation, every finished page is printed immediately and the
         * buffer reused, so documents of any length print seamlessly on
         * the roll. Rotate180 reverses the order of the whole printout,
         * which is impossible while streaming — those jobs accumulate all
         * pages in the buffer first (truncated to its capacity, as before). */
        bool stream_pages = (settings_get()->orientation == ORI_NORMAL ||
                             settings_get()->orientation == ORI_MIRROR);
        uint32_t job_id = ps_next_job_id();
        if (stream_pages) {
            us.on_page_end = page_flush_cb;
            us.cb_ctx = &job_id;
            ps_job_set_hold(true);
        }
        esp_err_t err = ESP_OK;
        size_t doc_off = msg.attrs_end;
#if CONFIG_AIRPRINT_DEBUG_RASTER_DUMP
        /* The first bytes of the document as the decoder sees them: a URF
         * document must start with 'UNIRAST' 0x00. */
        {
            char hex[3 * 20 + 1] = "";
            size_t n = (pre_len > doc_off) ? (pre_len - doc_off) : 0;
            if (n > 16) {
                n = 16;
            }
            for (size_t k = 0; k < n; k++) {
                snprintf(hex + k * 3, 4, "%02x ", pre[doc_off + k]);
            }
            ESP_LOGI(TAG, "doc starts at %u, first bytes: %s", (unsigned)doc_off, hex);
        }
#endif
        if (pre_len > doc_off) {
            err = urf_stream_feed(&us, pre + doc_off, pre_len - doc_off);
        }
        if (err == ESP_OK) {
            err = body_feed_urf(&body, &us, &doc_bytes, ctx->pre,
                                sizeof(ctx->pre), recv_start_us, &eof_us);
        }
        if (err == ESP_OK) {
            err = urf_stream_finish(&us);
        }
        int64_t decode_done_us = esp_timer_get_time();
        uint32_t rows = (uint32_t)us.out_rows;
        urf_stream_free(&us);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "decode failed: %s", esp_err_to_name(err));
            if (stream_pages) {
                ps_job_abort();
            }
            send_err(fd, 500, "Decode failed");
            return;
        }

        /* Final page (or the whole document in buffer mode). A blank tail is
         * still dispatched: the handler turns it into a no-op so the job
         * reaches a proper terminal state. */
        ps_job_set_hold(false);
        ps_job_t job = {
            .job_id = job_id,
            .document = NULL,
            .document_len = 0,
            .in_flash = false,
            .flash_offset = 0,
            .page_ready = true,
            .last_page = true,
        };
        esp_err_t derr = ps_dispatch_job(&job);
        uint16_t st = (derr == ESP_OK) ? IPP_STATUS_OK : IPP_STATUS_SERVER_ERROR_BUSY;
        send_ipp(fd, ctx->response, build_job_ack(ctx->response, sizeof(ctx->response),
                                               msg.request_id, job_id, st, msg.version));
        ESP_LOGI(TAG,
                 "job %lu %s (%u rows decoded, %u doc bytes, %u ms upload, "
                 "%u ms decode, %u ms total incl. streamed pages), free heap %u",
                 (unsigned long)job_id,
                 derr == ESP_OK ? "queued" : "rejected-busy", (unsigned)rows,
                 (unsigned)doc_bytes,
                 (unsigned)((eof_us - recv_start_us) / 1000),
                 (unsigned)((decode_done_us - eof_us) / 1000),
                 (unsigned)((esp_timer_get_time() - recv_start_us) / 1000),
                 (unsigned)esp_get_free_heap_size());
        return;
    }

    /* Everything else: drain what is left of the body, then answer. */
    body_drain(&body);

    switch (msg.operation_id) {
    case IPP_OP_GET_PRINTER_ATTRIBUTES:
        send_ipp(fd, ctx->response, build_printer_attrs(ctx->response, sizeof(ctx->response),
                                                     msg.request_id, msg.version));
        break;
    case IPP_OP_VALIDATE_JOB: {
        /* Validate-Job must NOT return a job object: iOS sends it before every
         * Print-Job and used to get a fake "job-id=0, job-state=processing"
         * reply. The print center then tracked a job that never existed and
         * could consider the printer unavailable. */
        ipp_writer_t w;
        ipp_writer_init(&w, ctx->response, sizeof(ctx->response));
        ipp_resp_begin(&w, msg.version, IPP_STATUS_OK, msg.request_id);
        ipp_add_string(&w, IPP_TAG_CHARSET, "attributes-charset", "utf-8");
        ipp_add_string(&w, IPP_TAG_NATURAL_LANG, "attributes-natural-language", "en");
        ipp_resp_finish(&w);
        send_ipp(fd, ctx->response, w.len);
        break;
    }
    case IPP_OP_GET_JOB_ATTRIBUTES: {
        /* Follow the LAST job so the phone's progress UI reaches a terminal
         * state (9) with a proper reason instead of polling forever. */
        ipp_writer_t w;
        ipp_writer_init(&w, ctx->response, sizeof(ctx->response));
        ipp_resp_begin(&w, msg.version, IPP_STATUS_OK, msg.request_id);
        ipp_add_string(&w, IPP_TAG_CHARSET, "attributes-charset", "utf-8");
        ipp_add_string(&w, IPP_TAG_NATURAL_LANG, "attributes-natural-language", "en");
        uint32_t jid = ps_last_job_id();
        if (jid != 0) {
            add_job_group(&w, jid, ps_job_state());
        }
        ipp_resp_finish(&w);
        send_ipp(fd, ctx->response, w.len);
        break;
    }
    case IPP_OP_GET_JOBS: {
        /* Get-Jobs is the QUEUE. Once the handler returns, the job is complete
         * and must disappear from the list. Returning a completed job forever
         * made iOS believe the queue never drained: after the first print it
         * kept re-checking for ~3 minutes and showed "Printer Not Available". */
        ipp_writer_t w;
        ipp_writer_init(&w, ctx->response, sizeof(ctx->response));
        ipp_resp_begin(&w, msg.version, IPP_STATUS_OK, msg.request_id);
        ipp_add_string(&w, IPP_TAG_CHARSET, "attributes-charset", "utf-8");
        ipp_add_string(&w, IPP_TAG_NATURAL_LANG, "attributes-natural-language", "en");
        if (ps_job_active()) {
            uint32_t jid = ps_last_job_id();
            if (jid != 0) {
                add_job_group(&w, jid, ps_job_state());
            }
        }
        ipp_resp_finish(&w);
        send_ipp(fd, ctx->response, w.len);
        break;
    }
    case 0x0008:   /* Cancel-Job: we print synchronously, just report it */
        send_ipp(fd, ctx->response, build_job_ack(ctx->response, sizeof(ctx->response),
                                                  msg.request_id, ps_last_job_id(),
                                                  IPP_STATUS_OK, msg.version));
        break;
    default:
        ESP_LOGW(TAG, "unsupported operation 0x%04x", msg.operation_id);
        send_err(fd, 400, "Unsupported operation");
        break;
    }
}

/* Log the attribute names a client asks for (first few requests only) - tells us
 * exactly what iOS expects from Get-Printer-Attributes. */
static void log_req_attrs(const uint8_t *buf, size_t len)
{
    static int budget = 3;
    if (budget-- <= 0) {
        return;
    }
    size_t off = 8;
    char names[160] = "";
    int shown = 0;
    while (off + 1 < len && shown < 10) {
        uint8_t tag = buf[off++];
        if (tag == IPP_TAG_END) {
            break;
        }
        if (tag <= 0x05) {
            continue;
        }
        if (off + 2 > len) {
            break;
        }
        size_t nl = ((size_t)buf[off] << 8) | buf[off + 1];
        off += 2;
        if (off + nl + 2 > len) {
            break;
        }
        size_t c = nl < 39 ? nl : 39;
        char name[40];
        memcpy(name, buf + off, c);
        name[c] = '\0';
        off += nl;
        size_t vl = ((size_t)buf[off] << 8) | buf[off + 1];
        off += 2;
        if (off + vl > len) {
            break;
        }
        off += vl;
        /* Bounded append: with 10 attrs × ~39 chars the 160-byte buffer would
         * overflow, and `sizeof - strlen - 2` underflows to a huge size_t once
         * strlen passes sizeof-2 -> strncat writes past the buffer (task death). */
        size_t used = strlen(names);
        if (used + c + 2 >= sizeof(names)) {
            break;
        }
        memcpy(names + used, name, c);
        names[used + c] = ' ';
        names[used + c + 1] = '\0';
        shown++;
    }
    ESP_LOGI(TAG, "requested attrs: %s", names);
}

static void ipp_worker(void *arg)
{
    worker_ctx_t *ctx = (worker_ctx_t *)arg;
    for (;;) {
        int fd = -1;
        if (xQueueReceive(s_conn_q, &fd, portMAX_DELAY) != pdTRUE || fd < 0) {
            continue;
        }
        handle_client(fd, ctx);
        /* Half-close first: if the client sent a chunked trailer we did not
         * consume, a bare close() emits a TCP RST and iOS can discard a reply
         * it already received. SHUT_WR sends our FIN cleanly. */
        shutdown(fd, SHUT_WR);
        close(fd);
    }
}

#define WEB_PORT 80

static int make_listener(int port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        return -1;
    }
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind :%d failed: errno %d", port, errno);
        close(s);
        return -1;
    }
    if (listen(s, 8) < 0) {
        ESP_LOGE(TAG, "listen :%d failed: errno %d", port, errno);
        close(s);
        return -1;
    }
    ESP_LOGI(TAG, "HTTP listening on :%d", port);
    return s;
}

static void ipp_server_task(void *arg)
{
    (void)arg;
    /* One accept loop, two ports: 631 for AirPrint IPP, 80 for the WebUI
     * (mDNS advertises adminurl=http://catprint-c3.local/). Both feed the
     * same worker queue, so no extra task stack is needed. */
    int srv_ipp = make_listener(IPP_PORT);
    int srv_web = make_listener(WEB_PORT);
    if (srv_ipp < 0 && srv_web < 0) {
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "IPP server listening on :%d", IPP_PORT);

    TickType_t last_alive = xTaskGetTickCount();
    while (s_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        if (srv_ipp >= 0) { FD_SET(srv_ipp, &rfds); maxfd = srv_ipp; }
        if (srv_web >= 0) { FD_SET(srv_web, &rfds); if (srv_web > maxfd) maxfd = srv_web; }
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int ready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ready <= 0) {
            if ((xTaskGetTickCount() - last_alive) > pdMS_TO_TICKS(30000)) {
                last_alive = xTaskGetTickCount();
                ESP_LOGI(TAG, "alive: heap %u", (unsigned)esp_get_free_heap_size());
            }
            continue;
        }
        int listeners[2] = { srv_ipp, srv_web };
        for (int k = 0; k < 2; k++) {
            int srv = listeners[k];
            if (srv < 0 || !FD_ISSET(srv, &rfds)) {
                continue;
            }
            struct sockaddr_in peer;
            socklen_t plen = sizeof(peer);
            int fd = accept(srv, (struct sockaddr *)&peer, &plen);
            if (fd < 0) {
                continue;
            }
            /* Low-water guard: a client that keeps re-connecting while we are
             * short of heap can drive the board until lwIP has no buffers left,
             * which looks like a total network death (ping stops, mDNS stops
             * answering) - the state the user saw as "printer no longer
             * available". Drop new connections instead of grinding to a halt;
             * the poller simply retries once memory is back. */
            if (esp_get_free_heap_size() < HEAP_FLOOR) {
                ESP_LOGW(TAG, "low heap (%u), refusing connection",
                         (unsigned)esp_get_free_heap_size());
                close(fd);
                continue;
            }
            if (xQueueSend(s_conn_q, &fd, pdMS_TO_TICKS(500)) != pdTRUE) {
                ESP_LOGW(TAG, "connection queue full, dropping");
                close(fd);      /* iOS polls aggressively while the sheet is open */
            }
        }
        if ((xTaskGetTickCount() - last_alive) > pdMS_TO_TICKS(30000)) {
            last_alive = xTaskGetTickCount();
            ESP_LOGI(TAG, "alive: heap %u", (unsigned)esp_get_free_heap_size());
        }
    }

    if (srv_ipp >= 0) close(srv_ipp);
    if (srv_web >= 0) close(srv_web);
    s_task = NULL;
    vTaskDelete(NULL);
}

/* Ask our own HTTP server for /api/status over loopback. This proves that the
 * server task is still scheduling and that a socket can still be accepted and
 * answered - exactly what breaks in the "silent network death" state. */
static bool self_probe_ok(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        return false;
    }
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_port = htons(WEB_PORT),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    bool ok = false;
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) == 0) {
        static const char req[] = "GET /api/status HTTP/1.0\r\n\r\n";
        if (send(s, req, sizeof(req) - 1, 0) > 0) {
            char buf[64];
            int n = recv(s, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                ok = (strstr(buf, "200") != NULL);
            }
        }
    }
    close(s);
    return ok;
}

static void health_task(void *arg)
{
    (void)arg;
    int low = 0;
    int probe_fail = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(HEALTH_PERIOD_MS));
        if (ps_job_active()) {
            /* Printing: low heap and busy workers are expected. */
            low = 0;
            probe_fail = 0;
            continue;
        }
        unsigned heap = (unsigned)esp_get_free_heap_size();
        low = (heap < HEALTH_LOW_HEAP) ? low + 1 : 0;
        if (low >= HEALTH_LOW_LIMIT) {
            ESP_LOGE(TAG, "heap stuck at %u B for %u s - restarting to recover the network",
                     heap, (unsigned)(HEALTH_LOW_LIMIT * HEALTH_PERIOD_MS / 1000));
            vTaskDelay(pdMS_TO_TICKS(200));       /* let the log drain */
            esp_restart();
        }
        if (!self_probe_ok()) {
            probe_fail++;
            ESP_LOGW(TAG, "self-probe failed (%d/%d), heap %u",
                     probe_fail, HEALTH_PROBE_FAIL_MAX, heap);
            if (probe_fail >= HEALTH_PROBE_FAIL_MAX) {
                ESP_LOGE(TAG, "IPP server unresponsive, restarting");
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
        } else {
            probe_fail = 0;
        }
    }
}

esp_err_t ps_http_start(void)
{
    if (s_running) {
        return ESP_OK;
    }
    s_running = true;
    /* Two workers: iOS keeps polling Get-Printer-Attributes while a job uploads,
     * and a single-threaded server starves the upload (that looked like
     * "nothing happens" on the phone). */
    /* Queue depth 3: each accepted socket that waits here already owns lwIP
     * send/receive buffers (~11 KB). With depth 8 a poll burst from the phone
     * could reserve tens of KB before a worker even looked at it, which is what
     * starved the board. Three is enough slack for a Print-Job upload plus a
     * couple of polls; anything beyond that is dropped and retried by iOS. */
    s_conn_q = xQueueCreate(3, sizeof(int));
    if (s_conn_q == NULL) {
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < 2; i++) {
        if (xTaskCreate(ipp_worker, "ipp_conn", 6144, &s_ctx[i], 5, NULL) != pdPASS) {
            ESP_LOGE(TAG, "cannot start connection worker %d", i);
            s_running = false;
            return ESP_ERR_NO_MEM;
        }
    }
    if (xTaskCreate(ipp_server_task, "ipp_srv", 6144, NULL, 5, &s_task) != pdPASS) {
        s_running = false;
        ESP_LOGE(TAG, "cannot start IPP task");
        return ESP_ERR_NO_MEM;
    }
    /* Priority 2: only wakes every 15 s, must never compete with the workers. */
    if (xTaskCreate(health_task, "ipp_health", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGW(TAG, "cannot start health watchdog");
    }
    return ESP_OK;
}

esp_err_t ps_http_stop(void)
{
    s_running = false;
    return ESP_OK;
}
