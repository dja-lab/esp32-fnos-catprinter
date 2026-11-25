/*
 * BLE bridge HTTP server (Plan B: protocol layer lives on the NAS).
 *
 * The AirPrint/IPP protocol handling was moved to a CUPS queue on the LAN
 * NAS (see nas/ in the repo root). This firmware is only a print bridge:
 *
 *   NAS CUPS queue  --POST /print (raw URF)-->  this server  --BLE-->  printer
 *   anyone          --GET / or /api/x-------->  WebUI (unchanged)
 *
 * What survives from the AirPrint firmware and why:
 *   - the hand-rolled HTTP reader (chunked + Content-Length), streaming body
 *     reader, two-worker pool, connection-queue depth 3, HEAP_FLOOR refuse
 *     guard and the health watchdog: all field-proven against heap starvation;
 *   - the streaming URF decode with per-page flush callback: pages print
 *     seamlessly on the roll while the document is still uploading.
 * What is gone: IPP attribute encoding/decoding, port 631, mDNS, AirPrint
 * media negotiation, multi-value/collection pitfalls - none of it is needed
 * when the only client is our own NAS backend.
 */
#include "catprinter.h"
#include "printserver.h"
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

static const char *TAG = "bridge";

/* Same buffer geometry as the AirPrint build: a 175 KB page needed ~350 recv()
 * rounds with a 512-byte socket buffer; RD_BUF lives in the worker context. */
#define RD_BUF     2048u
/* Refuse new connections below this much free heap. Must stay BELOW the heap a
 * running job needs, yet high enough that lwIP can always free a PCB. */
#define HEAP_FLOOR 6144u

/* ---- health watchdog ---------------------------------------------------
 * Same failure mode as before: enough concurrent sockets can push lwIP into
 * a state where the accept loop still runs but nothing arrives any more.
 * Two detectors, both skipped while a job is printing:
 *   1. free heap parked under HEALTH_LOW_HEAP for HEALTH_LOW_LIMIT rounds;
 *   2. our own HTTP server stops answering a loopback request.
 * Either one restarts the board, which brings the network back in ~5 s. */
#define HEALTH_LOW_HEAP       10240u
#define HEALTH_LOW_LIMIT      8          /* 8 x 15 s = 120 s */
#define HEALTH_PROBE_FAIL_MAX 3
#define HEALTH_PERIOD_MS      15000
#define BRD_SHORT_TIMEOUT_S 2
#define BRD_LONG_TIMEOUT_S  60
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

/* `buf` is the worker's scratch buffer. Progress logs show whether the upload
 * is uniformly slow or stalls in bursts. */
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

#define PAGE_FLUSH_TIMEOUT_MS (300000u)

/* urf_page_end_cb: an intermediate page has been decoded into the shared page
 * buffer. Trim trailing blank rows, print WITHOUT trailing feed so the next
 * page continues seamlessly on the roll, and block until the print worker has
 * finished (the same buffer is reused; pausing the decode applies TCP
 * back-pressure to the NAS backend). */
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
    pb_clear(pb);
    ESP_LOGI(TAG, "page %u printed (%u rows), continuing", (unsigned)page_no, (unsigned)n);
}

/* ---- responses --------------------------------------------------------- */

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

static void send_json(int fd, const char *json)
{
    char hdr[160];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %u\r\n"
                     "Connection: close\r\n\r\n", (unsigned)strlen(json));
    send_all(fd, hdr, (size_t)n);
    send_all(fd, json, strlen(json));
}

static void send_err(int fd, int code, const char *msg)
{
    char buf[192];
    int n = snprintf(buf, sizeof(buf),
                     "HTTP/1.1 %d %s\r\nContent-Length: 0\r\n"
                     "Connection: close\r\n\r\n", code, msg);
    send_all(fd, buf, (size_t)n);
}

/* Single client at a time, so one shared response buffer keeps it off the stack. */
typedef struct {
    uint8_t scratch[RD_BUF];  /* URF feed scratch */
    uint8_t rxbuf[RD_BUF];    /* socket read buffer */
} worker_ctx_t;

static worker_ctx_t s_ctx[2];
static QueueHandle_t s_conn_q = NULL;

/* ---- POST /print: the whole point of this firmware ---------------------
 * Body = one raw URF document (exactly what macOS CUPS hands to a backend
 * with *cupsFilter2: "image/urf image/urf 10 -"). Decoded straight into the
 * page buffer as it streams; multi-page documents print page by page. */
static void handle_print(int fd, worker_ctx_t *ctx, body_t *body,
                         bool chunked, size_t content_len, const char *peer)
{
    (void)chunked;
    (void)content_len;
    page_buf_t *pb = ps_page_buffer();
    if (pb == NULL || pb->rows == NULL) {
        body_drain(body);
        send_err(fd, 500, "No page buffer");
        return;
    }
    urf_stream_t us;
    if (urf_stream_init(&us, pb) != ESP_OK) {
        send_err(fd, 500, "Decoder init failed");
        return;
    }
    /* Streaming multi-page mode for orientations without 180-degree rotation;
     * Rotate180 accumulates pages first (truncated to buffer capacity). */
    bool stream_pages = (settings_get()->orientation == ORI_NORMAL ||
                         settings_get()->orientation == ORI_MIRROR);
    uint32_t job_id = ps_next_job_id();
    if (stream_pages) {
        us.on_page_end = page_flush_cb;
        us.cb_ctx = &job_id;
        ps_job_set_hold(true);
    }
    esp_err_t err = ESP_OK;
    size_t doc_bytes = 0;
    int64_t recv_start_us = esp_timer_get_time();
    int64_t eof_us = recv_start_us;
    err = body_feed_urf(body, &us, &doc_bytes, ctx->scratch,
                        sizeof(ctx->scratch), recv_start_us, &eof_us);
    if (err == ESP_OK) {
        err = urf_stream_finish(&us);
    }
    uint32_t rows = (uint32_t)us.out_rows;
    uint32_t pages = (uint32_t)us.pages_started;
    urf_stream_free(&us);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "decode failed: %s (%u doc bytes)", esp_err_to_name(err),
                 (unsigned)doc_bytes);
        if (stream_pages) {
            ps_job_abort();
        }
        send_err(fd, 500, "Decode failed");
        return;
    }

    /* Final page (or the whole document in buffer mode). A blank tail is
     * still dispatched: the handler turns it into a no-op so the job reaches
     * a proper terminal state. */
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
    ESP_LOGI(TAG, "job %lu %s (%u rows, %u doc bytes, %u ms total), peer=%s, heap %u",
             (unsigned long)job_id,
             derr == ESP_OK ? "queued" : "rejected-busy", (unsigned)rows,
             (unsigned)doc_bytes,
             (unsigned)((esp_timer_get_time() - recv_start_us) / 1000),
             peer, (unsigned)esp_get_free_heap_size());

    char json[128];
    snprintf(json, sizeof(json),
             "{\"ok\":%s,\"job\":%lu,\"rows\":%u,\"pages\":%u}",
             derr == ESP_OK ? "true" : "false",
             (unsigned long)job_id, (unsigned)rows, (unsigned)pages);
    send_json(fd, json);
}

/* ---- POST /raw: pure byte relay -----------------------------------------
 * Body = a fully-formed printer command stream (frames + CRC8 built by the
 * NAS backend). Pushed to 0xAE01 verbatim through cp_send_raw(), which
 * handles MTU chunking and the printer's flow-control notifications; each
 * relayed chunk applies TCP back-pressure to the sender. No decode, no
 * buffering, no local settings - the NAS decides everything. */
static volatile bool s_raw_busy = false;

static void handle_raw(int fd, worker_ctx_t *ctx, body_t *body, const char *peer)
{
    if (s_raw_busy) {
        body_drain(body);
        send_err(fd, 503, "Busy");
        return;
    }
    if (!cp_is_ready()) {
        body_drain(body);
        send_err(fd, 503, "Printer not connected");
        return;
    }
    s_raw_busy = true;
    size_t total = 0;
    esp_err_t err = ESP_OK;
    int64_t start_us = esp_timer_get_time();
    for (;;) {
        int got = body_read(body, ctx->scratch, sizeof(ctx->scratch));
        if (got < 0) {
            err = ESP_FAIL;
            break;
        }
        if (got == 0) {
            break;
        }
        err = cp_send_raw(ctx->scratch, (size_t)got, 60000);
        if (err != ESP_OK) {
            break;
        }
        total += (size_t)got;
    }
    s_raw_busy = false;
    ESP_LOGI(TAG, "raw relay %s: %u B in %u ms, peer=%s, heap %u",
             err == ESP_OK ? "ok" : "FAILED", (unsigned)total,
             (unsigned)((esp_timer_get_time() - start_us) / 1000),
             peer, (unsigned)esp_get_free_heap_size());
    if (err != ESP_OK) {
        send_err(fd, 500, "BLE relay failed");
        return;
    }
    char json[96];
    snprintf(json, sizeof(json), "{\"ok\":true,\"bytes\":%u}", (unsigned)total);
    send_json(fd, json);
}

/* ---- one client -------------------------------------------------------- */
static void handle_client(int fd, worker_ctx_t *ctx)
{
    struct timeval rcv_tv = { .tv_sec = BRD_SHORT_TIMEOUT_S, .tv_usec = 0 };
    struct timeval snd_tv = { .tv_sec = BRD_LONG_TIMEOUT_S, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_tv, sizeof(rcv_tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd_tv, sizeof(snd_tv));
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
#ifdef TCP_QUICKACK
    int quickack = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &quickack, sizeof(quickack));
#endif

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
    char peer[24] = "?";
    {
        struct sockaddr_in pa;
        socklen_t pl = sizeof(pa);
        if (getpeername(fd, (struct sockaddr *)&pa, &pl) == 0) {
            snprintf(peer, sizeof(peer), "%s", inet_ntoa(pa.sin_addr));
        }
    }
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
        }
    }
    ESP_LOGI(TAG, "%s %s chunked=%d len=%u peer=%s", method, path,
             (int)chunked, (unsigned)content_len, peer);

    if (strcmp(method, "GET") == 0) {
        webui_serve(fd, path);
        return;
    }
    if (strncmp(path, "/api/", 5) == 0) {
        /* WebUI POST: a short urlencoded form. Reuse the worker scratch. */
        body_t body;
        body_init(&body, &brd, chunked, content_len);
        uint8_t *buf = ctx->scratch;
        size_t blen = 0;
        while (blen + 1 < RD_BUF) {
            int got = body_read(&body, buf + blen, RD_BUF - blen - 1);
            if (got <= 0) break;
            blen += (size_t)got;
        }
        buf[blen] = '\0';
        webui_api(fd, path, (const char *)buf, blen);
        return;
    }
    bool is_print = (strcmp(path, "/print") == 0);
    bool is_raw = (strcmp(path, "/raw") == 0);
    if (strcmp(method, "POST") != 0 || (!is_print && !is_raw)) {
        send_err(fd, 404, "Not Found");
        return;
    }
    if (!chunked && content_len == 0) {
        send_err(fd, 400, "Bad Request");
        return;
    }

    /* A print upload may legitimately pause between chunks (NAS filters then
     * streams). Relax the receive timeout for the body phase. */
    {
        struct timeval long_tv = { .tv_sec = BRD_LONG_TIMEOUT_S, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &long_tv, sizeof(long_tv));
        brd.idle_max = BRD_IDLE_MAX;
    }
    body_t body;
    body_init(&body, &brd, chunked, content_len);
    if (strcmp(method, "POST") == 0 && strcmp(path, "/raw") == 0) {
        handle_raw(fd, ctx, &body, peer);
        return;
    }
    handle_print(fd, ctx, &body, chunked, content_len, peer);
}

static void bridge_worker(void *arg)
{
    worker_ctx_t *ctx = (worker_ctx_t *)arg;
    for (;;) {
        int fd = -1;
        if (xQueueReceive(s_conn_q, &fd, portMAX_DELAY) != pdTRUE || fd < 0) {
            continue;
        }
        handle_client(fd, ctx);
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
    if (listen(s, 3) < 0) {
        ESP_LOGE(TAG, "listen :%d failed: errno %d", port, errno);
        close(s);
        return -1;
    }
    ESP_LOGI(TAG, "HTTP listening on :%d", port);
    return s;
}

static void bridge_server_task(void *arg)
{
    (void)arg;
    int srv = make_listener(WEB_PORT);
    if (srv < 0) {
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    TickType_t last_alive = xTaskGetTickCount();
    while (s_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(srv, &rfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int ready = select(srv + 1, &rfds, NULL, NULL, &tv);
        if (ready <= 0) {
            if ((xTaskGetTickCount() - last_alive) > pdMS_TO_TICKS(30000)) {
                last_alive = xTaskGetTickCount();
                ESP_LOGI(TAG, "alive: heap %u", (unsigned)esp_get_free_heap_size());
            }
            continue;
        }
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int fd = accept(srv, (struct sockaddr *)&peer, &plen);
        if (fd < 0) {
            continue;
        }
        if (esp_get_free_heap_size() < HEAP_FLOOR) {
            ESP_LOGW(TAG, "low heap (%u), refusing connection",
                     (unsigned)esp_get_free_heap_size());
            close(fd);
            continue;
        }
        if (xQueueSend(s_conn_q, &fd, pdMS_TO_TICKS(500)) != pdTRUE) {
            ESP_LOGW(TAG, "connection queue full, dropping");
            close(fd);
        }
        if ((xTaskGetTickCount() - last_alive) > pdMS_TO_TICKS(30000)) {
            last_alive = xTaskGetTickCount();
            ESP_LOGI(TAG, "alive: heap %u", (unsigned)esp_get_free_heap_size());
        }
    }

    close(srv);
    s_task = NULL;
    vTaskDelete(NULL);
}

/* Ask our own HTTP server for /api/status over loopback: proves the server
 * task is still scheduling and a socket can still be accepted and answered. */
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
                ESP_LOGE(TAG, "bridge server unresponsive, restarting");
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
    s_conn_q = xQueueCreate(3, sizeof(int));
    if (s_conn_q == NULL) {
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < 2; i++) {
        if (xTaskCreate(bridge_worker, "bridge_conn", 6144, &s_ctx[i], 5, NULL) != pdPASS) {
            ESP_LOGE(TAG, "cannot start connection worker %d", i);
            s_running = false;
            return ESP_ERR_NO_MEM;
        }
    }
    if (xTaskCreate(bridge_server_task, "bridge_srv", 6144, NULL, 5, &s_task) != pdPASS) {
        s_running = false;
        ESP_LOGE(TAG, "cannot start bridge task");
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(health_task, "bridge_health", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGW(TAG, "cannot start health watchdog");
    }
    return ESP_OK;
}

esp_err_t ps_http_stop(void)
{
    s_running = false;
    return ESP_OK;
}
