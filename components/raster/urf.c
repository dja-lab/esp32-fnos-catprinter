#include "urf.h"

#include <stdlib.h>
#include <string.h>

#include "dither.h"
#include "esp_log.h"
#include "spool.h"

static const char *TAG = "urf";

#define URF_FILE_HDR 12u
#define URF_PAGE_HDR 32u
#define URF_HDR_LEN  (URF_FILE_HDR + URF_PAGE_HDR)

/* ---- sequential reader over RAM or flash, with a small window ---------- */
#define WIN_SZ 2048u

typedef struct {
    const uint8_t *mem;      /* NULL => flash spool */
    size_t base;             /* absolute offset of window start */
    size_t end;              /* absolute end of document */
    size_t pos;              /* absolute read position */
    size_t wlen;             /* valid bytes in window */
    uint8_t win[WIN_SZ];
} src_t;

static void src_init(src_t *s, const urf_doc_t *doc)
{
    memset(s, 0, sizeof(*s));
    s->mem = doc->mem;
    if (doc->mem != NULL) {
        s->base = 0;
        s->pos = 0;
        s->end = doc->len;
    } else {
        /* document occupies [flash_offset, flash_offset + len) in the spool */
        s->base = doc->flash_offset;
        s->pos = doc->flash_offset;
        s->end = doc->flash_offset + doc->len;
    }
    s->wlen = 0;
}

static bool src_in_window(const src_t *s)
{
    size_t rel = s->pos - s->base;
    return s->pos >= s->base && rel < s->wlen;
}

static bool src_fill(src_t *s)
{
    if (s->pos >= s->end) {
        return false;
    }
    s->base = s->pos;
    size_t n = s->end - s->pos;
    if (n > WIN_SZ) {
        n = WIN_SZ;
    }
    if (s->mem != NULL) {
        memcpy(s->win, s->mem + s->pos, n);
        s->wlen = n;
        return true;
    }
    /* flash: read in a 512-byte-aligned window to respect partition constraints */
    size_t aligned = s->pos & ~(size_t)0x1ff;
    size_t skip = s->pos - aligned;
    size_t want = n + skip;
    if (want > WIN_SZ) {
        want = WIN_SZ;
    }
    if (spool_read(aligned, s->win, want) != ESP_OK) {
        return false;
    }
    memmove(s->win, s->win + skip, want - skip);
    s->wlen = want - skip;
    return true;
}

static int src_byte(src_t *s)
{
    if (!src_in_window(s) && !src_fill(s)) {
        return -1;
    }
    size_t rel = s->pos - s->base;
    if (rel >= s->wlen) {
        return -1;
    }
    s->pos++;
    return s->win[rel];
}

static bool src_copy(src_t *s, uint8_t *dst, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int c = src_byte(s);
        if (c < 0) {
            return false;
        }
        dst[i] = (uint8_t)c;
    }
    return true;
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* ---- decoder ----------------------------------------------------------- */

esp_err_t raster_urf_to_pagebuf(const urf_doc_t *doc, page_buf_t *pb)
{
    if (doc == NULL || pb == NULL || pb->rows == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (doc->mem == NULL && doc->flash_offset == 0 && spool_capacity() == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    src_t src;
    src_init(&src, doc);

    uint8_t fh[URF_FILE_HDR + URF_PAGE_HDR];
    if (!src_copy(&src, fh, sizeof(fh))) {
        ESP_LOGE(TAG, "document shorter than URF headers (%u bytes)", (unsigned)doc->len);
        return ESP_ERR_INVALID_ARG;
    }
    if (memcmp(fh, "UNIRAST", 7) != 0) {
        ESP_LOGE(TAG, "bad URF magic (first bytes %02x %02x %02x %02x)",
                 fh[0], fh[1], fh[2], fh[3]);
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t pages = be32(fh + 8);
    const uint8_t *ph = fh + URF_FILE_HDR;
    uint8_t bits = ph[0];
    uint8_t cs = ph[1];
    uint32_t width = be32(ph + 12);
    uint32_t height = be32(ph + 16);
    uint32_t dpi = be32(ph + 20);
    uint8_t bpp = (uint8_t)(bits / 8u);
    if (bpp == 0) {
        bpp = 1;
    }

    ESP_LOGI(TAG, "URF %ux%u dpi=%u bits=%u cs=%u pages=%u",
             (unsigned)width, (unsigned)height, (unsigned)dpi,
             (unsigned)bits, (unsigned)cs, (unsigned)pages);

    if (width == 0 || height == 0 || width > 4096) {
        ESP_LOGE(TAG, "implausible page size");
        return ESP_ERR_INVALID_ARG;
    }
    if (pages > 1) {
        ESP_LOGW(TAG, "multi-page job: only page 1 printed (pages=%u)", (unsigned)pages);
    }

    uint32_t rows_to_decode = height;
    if (rows_to_decode > pb->capacity_rows) {
        ESP_LOGW(TAG, "page taller than buffer, truncating %u -> %u rows",
                 (unsigned)height, (unsigned)pb->capacity_rows);
        rows_to_decode = (uint32_t)pb->capacity_rows;
    }

    size_t row_bytes = (size_t)width * bpp;
    uint8_t *rowbuf = (uint8_t *)malloc(row_bytes);
    uint8_t *gray = (uint8_t *)malloc(width);
    dither_state_t dither;
    esp_err_t err = dither_init(&dither, RASTER_WIDTH_PX);
    if (rowbuf == NULL || gray == NULL || err != ESP_OK) {
        free(rowbuf);
        free(gray);
        if (err == ESP_OK) {
            dither_free(&dither);
        }
        ESP_LOGE(TAG, "no memory for row buffers");
        return ESP_ERR_NO_MEM;
    }

    uint8_t row48[RASTER_ROW_BYTES];
    pb_clear(pb);

    /* Aspect correction: the print head is fixed at 384 dots, so a wide source
     * row gets squashed horizontally by dither_row(). Drop source rows at the
     * same ratio, otherwise a 2480 px A4 raster prints 6x too tall. */
    double row_step = (double)width / (double)RASTER_WIDTH_PX;
    if (row_step < 1.0) {
        row_step = 1.0;
    }
    double row_acc = 0.0;

    uint32_t row = 0;
    bool truncated = false;
    while (row < rows_to_decode && !truncated) {
        int c = src_byte(&src);
        if (c < 0) {
            break;                                  /* clean end of document */
        }
        uint32_t reps = (uint32_t)c + 1u;           /* lines sharing this data */
        size_t col = 0;

        while (col < row_bytes) {
            int cc = src_byte(&src);
            if (cc < 0) {
                truncated = true;
                break;
            }
            uint8_t cb = (uint8_t)cc;
            if (cb <= 0x7f) {                        /* one pixel, repeated */
                uint32_t n = (uint32_t)cb + 1u;
                if (!src_copy(&src, rowbuf, bpp)) {
                    truncated = true;
                    break;
                }
                uint8_t px[4];
                memcpy(px, rowbuf, bpp < sizeof(px) ? bpp : sizeof(px));
                for (uint32_t k = 0; k < n && col + bpp <= row_bytes; k++) {
                    memcpy(rowbuf + col, px, bpp);
                    col += bpp;
                }
            } else if (cb == 0x80) {                 /* fill to end of line */
                uint8_t px[4];
                if (!src_copy(&src, px, bpp)) {
                    truncated = true;
                    break;
                }
                while (col + bpp <= row_bytes) {
                    memcpy(rowbuf + col, px, bpp);
                    col += bpp;
                }
            } else {                                 /* literal run */
                uint32_t n = 257u - (uint32_t)cb;
                for (uint32_t k = 0; k < n && col + bpp <= row_bytes; k++) {
                    if (!src_copy(&src, rowbuf + col, bpp)) {
                        truncated = true;
                        break;
                    }
                    col += bpp;
                }
            }
        }
        if (truncated) {
            break;
        }
        memset(rowbuf + col, 0xff, row_bytes - col);

        for (uint32_t r = 0; r < reps && row < rows_to_decode; r++, row++) {
            row_acc += 1.0;
            if (row_acc + 1e-9 < row_step) {
                continue;               /* drop this row to keep the aspect */
            }
            row_acc -= row_step;
            for (uint32_t x = 0; x < width; x++) {
                const uint8_t *p = rowbuf + (size_t)x * bpp;
                if (bpp >= 3) {
                    gray[x] = (uint8_t)(((uint32_t)p[0] * 77u +
                                         (uint32_t)p[1] * 150u +
                                         (uint32_t)p[2] * 29u) >> 8);
                } else {
                    gray[x] = p[0];
                }
            }
            if (dither_row(&dither, gray, width, row48) != ESP_OK) {
                row = rows_to_decode;
                break;
            }
            if (pb_append_row(pb, row48) != ESP_OK) {
                ESP_LOGW(TAG, "page buffer full at row %u", (unsigned)row);
                row = rows_to_decode;
                break;
            }
        }
    }

    free(rowbuf);
    free(gray);
    dither_free(&dither);

    ESP_LOGI(TAG, "decoded %u rows into page buffer%s",
             (unsigned)pb->nrows, truncated ? " (truncated)" : "");
    return pb->nrows > 0 ? ESP_OK : ESP_ERR_INVALID_ARG;
}
