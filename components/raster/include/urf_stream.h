/*
 * Streaming Apple-Raster (URF) decoder.
 *
 * The IPP body arrives over WiFi and cannot be buffered (13 KB free heap) nor
 * spooled to flash (erasing while receiving starves the WiFi driver and kills
 * the TCP connection). URF/PackBits is strictly sequential, so we decode bytes
 * as they arrive and write finished 1-bit rows straight into the page buffer.
 *
 * Feed any split of the byte stream: urf_stream_feed() keeps a byte-level state
 * machine (header -> repeat-count -> opcode -> pixel payload).
 */
#ifndef URF_STREAM_H
#define URF_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dither.h"
#include "esp_err.h"
#include "pagebuf.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    US_MAGIC = 0,      /* collecting the 44-byte UNIRAST header */
    US_REPEAT,         /* waiting for the per-line repeat byte */
    US_ROW,            /* decoding one line */
    US_PAGEHDR,        /* collecting the 32-byte header of page N+1 */
} urf_stage_t;

/* Called when page N has been fully decoded AND more pages follow. Runs
 * synchronously inside urf_stream_feed(), in the caller's context: the
 * handler may print the finished page (blocking for a long time) and clear
 * the page buffer before returning; decoding of page N+1 resumes afterwards.
 * The callback mechanism is how a multi-page job prints page by page on
 * continuous roll paper without buffering the whole document. */
typedef void (*urf_page_end_cb)(void *ctx, uint32_t page_no);

typedef struct {
    page_buf_t *pb;

    /* header */
    uint8_t hdr[44];
    size_t hdr_len;
    bool header_done;
    uint32_t width;
    uint32_t height;
    uint32_t pages;
    uint32_t pages_started;    /* page headers consumed so far (page 1 included) */
    uint32_t bpp;
    uint8_t white;         /* byte value of "white" used by the 0x80 clear opcode */
    size_t row_bytes;

    /* per-page state */
    bool page_has_content;     /* false until the first non-blank row of the page;
                                * leading blank rows (the top margin iOS renders)
                                * are dropped so pages join seamlessly */
    uint32_t page_out_rows;    /* rows written to pb for the CURRENT page */

    /* line state */
    uint32_t row;          /* source rows consumed */
    uint32_t out_rows;     /* rows actually written to pb (all pages) */
    uint32_t reps;         /* duplicate lines still to emit */
    size_t col;            /* bytes filled in rowbuf */
    uint8_t op;            /* 0 none, 1 repeat-pixel, 3 literal (0x80 handled inline) */
    uint32_t op_left;      /* pixels left for the current opcode */
    uint8_t px[4];         /* pixel being assembled */
    size_t px_len;

    /* output — all accumulators are RASTER_WIDTH_PX (384) wide, NOT width wide.
     * An A4 raster is 2480 px across; keeping full-width grey/average/vertical
     * buffers cost 2480 + 2480 + 2*2480 = ~10 KB of a 23 KB heap, which is what
     * made an iPhone A4 job starve the board. Each source row is collapsed into
     * the 384 head columns as it is decoded, so the working set is constant
     * (~2.3 KB) no matter how wide the client renders. */
    uint8_t *rowbuf;
    uint16_t *acc;         /* per-source-row bucket sums (384 entries) */
    uint16_t *vacc;        /* vertical accumulation of the per-row means */
    uint16_t *acc_cnt;     /* source pixels contributing to each bucket */
    uint16_t acc_n;        /* source rows accumulated in vacc */
    uint8_t dbg[32];       /* first payload bytes, for format diagnosis */
    size_t dbg_len;
    bool dbg_done;
    dither_state_t dither;
    double row_step;
    double row_acc;

    urf_page_end_cb on_page_end;
    void *cb_ctx;

    urf_stage_t stage;
    esp_err_t err;
} urf_stream_t;

esp_err_t urf_stream_init(urf_stream_t *s, page_buf_t *pb);
void urf_stream_free(urf_stream_t *s);

/* Consume `len` bytes. Returns ESP_OK, or an error once the stream is broken. */
esp_err_t urf_stream_feed(urf_stream_t *s, const uint8_t *data, size_t len);

/* Call at end of body: flushes nothing, but validates that something was
 * decoded. */
esp_err_t urf_stream_finish(urf_stream_t *s);

#ifdef __cplusplus
}
#endif
#endif
