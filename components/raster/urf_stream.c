#include "urf_stream.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "urf";

#define URF_HDR_LEN 44u
#define URF_PAGE_HDR_LEN 32u

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* File header: "UNIRAST\0" (8 B) + page count (4 B BE). Page headers follow
 * immediately and are 32 bytes each. */
static esp_err_t parse_file_header(urf_stream_t *s)
{
    if (memcmp(s->hdr, "UNIRAST", 7) != 0) {
        ESP_LOGE(TAG, "bad URF magic (%02x %02x %02x %02x)",
                 s->hdr[0], s->hdr[1], s->hdr[2], s->hdr[3]);
        return ESP_ERR_INVALID_ARG;
    }
    s->pages = be32(s->hdr + 8);
    if (s->pages == 0) {
        s->pages = 1;              /* defensive: some clients under-report */
    }
    if (s->pages > 1) {
        ESP_LOGI(TAG, "multi-page job: %u pages (printed seamlessly "
                 "one page at a time)", (unsigned)s->pages);
    }
    return ESP_OK;
}

/* Per-page header (32 B): same layout as bytes 12..43 of the file block.
 * bits = ph[0] (bits per PIXEL), cs = ph[1], width = be32(ph+12),
 * height = be32(ph+16). Allocates / resizes the line buffers when the page
 * geometry differs from the previous page. */
static esp_err_t start_page(urf_stream_t *s, const uint8_t *ph)
{
    uint8_t bits = ph[0];
    uint8_t cs = ph[1];
    uint32_t width = be32(ph + 12);
    uint32_t height = be32(ph + 16);

    /* Apple Raster colour spaces, from CUPS raster-stream.c (rawcspace /
     * rawnumcolors): 0 SW(1), 1 sRGB(3), 2 CIELab(3), 3 AdobeRGB(3),
     * 4 W(1), 5 RGB(3), 6 CMYK(4).  `bits` is bits per PIXEL, so bits/8 is
     * bytes per pixel; a few clients report bits per channel instead, in which
     * case multiply by the channel count. */
    static const uint8_t k_num_colors[7] = {1, 3, 3, 3, 1, 3, 4};
    /* 0x80 "clear to end of line" fills with white: 0xff for W/RGB/SW/sRGB/
     * AdobeRGB, 0x00 for K-like spaces (CIELab/CMYK). */
    static const uint8_t k_white_is_ff[7] = {1, 1, 0, 1, 1, 1, 0};
    uint8_t nc = (cs < 7u) ? k_num_colors[cs] : 1u;
    uint32_t bpp = bits / 8u;
    if (bpp == 0) {
        bpp = 1;
    }
    if (bpp < nc) {
        bpp = bpp * nc;            /* client reported bits per channel */
    }
    uint8_t white = (cs < 7u && k_white_is_ff[cs]) ? 0xff : 0x00;
    if (width == 0 || height == 0 || width > 4096) {
        ESP_LOGE(TAG, "implausible page %ux%u", (unsigned)width, (unsigned)height);
        return ESP_ERR_INVALID_ARG;
    }

    size_t row_bytes = (size_t)width * bpp;
    if (row_bytes != s->row_bytes) {
        free(s->rowbuf);
        s->rowbuf = (uint8_t *)malloc(row_bytes);
        if (s->rowbuf == NULL) {
            s->row_bytes = 0;
            return ESP_ERR_NO_MEM;
        }
        s->row_bytes = row_bytes;
    }
    if (s->acc == NULL) {
        /* Fixed-size working set: one entry per head dot, independent of the
         * page width the client chose. */
        s->acc = (uint16_t *)calloc(RASTER_WIDTH_PX, sizeof(uint16_t));
        s->vacc = (uint16_t *)calloc(RASTER_WIDTH_PX, sizeof(uint16_t));
        s->acc_cnt = (uint16_t *)calloc(RASTER_WIDTH_PX, sizeof(uint16_t));
        if (s->acc == NULL || s->vacc == NULL || s->acc_cnt == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    s->width = width;
    s->height = height;
    s->bpp = bpp;
    s->white = white;
    memset(s->rowbuf, 0xff, s->row_bytes);

    /* How many source pixels fall into each of the 384 output columns. Computed
     * once per page; used to turn the per-row bucket sums into means. */
    memset(s->acc_cnt, 0, RASTER_WIDTH_PX * sizeof(uint16_t));
    for (uint32_t x = 0; x < width; x++) {
        uint32_t b = (uint32_t)((uint64_t)x * RASTER_WIDTH_PX / width);
        if (b >= RASTER_WIDTH_PX) {
            b = RASTER_WIDTH_PX - 1;
        }
        if (s->acc_cnt[b] < 0xffffu) {
            s->acc_cnt[b]++;
        }
    }

    /* Horizontal scale is handled by dither_row(); drop source rows at the same
     * ratio so a 2480 px wide A4 raster is not printed 6x too tall. */
    s->row_step = (double)width / (double)RASTER_WIDTH_PX;
    if (s->row_step < 1.0) {
        s->row_step = 1.0;
    }

    s->row = 0;
    s->page_out_rows = 0;
    s->page_has_content = false;
    s->acc_n = 0;
    s->row_acc = 0.0;
    s->reps = 0;
    s->col = 0;
    s->op = 0;
    s->op_left = 0;
    s->px_len = 0;

    ESP_LOGI(TAG, "page %u: %ux%u bits=%u cs=%u bpp=%u row_bytes=%u "
             "(row_step %.2f)", (unsigned)s->pages_started + 1u,
             (unsigned)width, (unsigned)height, (unsigned)bits, (unsigned)cs,
             (unsigned)bpp, (unsigned)row_bytes, s->row_step);
    return ESP_OK;
}

esp_err_t urf_stream_init(urf_stream_t *s, page_buf_t *pb)
{
    if (s == NULL || pb == NULL || pb->rows == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(s, 0, sizeof(*s));
    s->pb = pb;
    s->stage = US_MAGIC;
    pb_clear(pb);
    return ESP_OK;
}

void urf_stream_free(urf_stream_t *s)
{
    if (s == NULL) {
        return;
    }
    free(s->rowbuf);
    free(s->acc);
    free(s->vacc);
    free(s->acc_cnt);
    s->rowbuf = NULL;
    s->acc = NULL;
    s->vacc = NULL;
    s->acc_cnt = NULL;
    if (s->header_done) {
        dither_free(&s->dither);
        s->header_done = false;
    }
}

/* Emit the just-finished source line, honouring line repetition and the
 * vertical scale factor. */
static esp_err_t emit_line(urf_stream_t *s)
{
    /* Collapse this source line into the 384 head columns immediately. Doing it
     * here (instead of keeping full-width grey/average buffers) is what keeps
     * an A4 job inside the heap: the working set is 384 entries, not 2480. */
    memset(s->acc, 0, RASTER_WIDTH_PX * sizeof(uint16_t));
    for (uint32_t x = 0; x < s->width; x++) {
        const uint8_t *p = s->rowbuf + (size_t)x * s->bpp;
        uint8_t g;
        if (s->bpp >= 3) {
            g = (uint8_t)(((uint32_t)p[0] * 77u + (uint32_t)p[1] * 150u +
                           (uint32_t)p[2] * 29u) >> 8);
        } else {
            g = p[0];
        }
        uint32_t b = (uint32_t)((uint64_t)x * RASTER_WIDTH_PX / s->width);
        if (b >= RASTER_WIDTH_PX) {
            b = RASTER_WIDTH_PX - 1;
        }
        s->acc[b] = (uint16_t)(s->acc[b] + g);     /* <= 255 * 7, no overflow */
    }
    /* Per-row column means. Computed once per decoded line, then added to the
     * vertical accumulator once per repetition (URF repeat counts duplicate the
     * same line, so they must weigh in). */
    uint8_t rm[RASTER_WIDTH_PX];
    for (uint32_t x = 0; x < RASTER_WIDTH_PX; x++) {
        uint16_t c = s->acc_cnt[x] ? s->acc_cnt[x] : 1;
        rm[x] = (uint8_t)(s->acc[x] / c);
    }

    for (uint32_t r = 0; r < s->reps; r++) {
        if (s->row >= s->height) {
            break;
        }
        for (uint32_t x = 0; x < RASTER_WIDTH_PX; x++) {
            uint32_t v = (uint32_t)s->vacc[x] + rm[x];
            s->vacc[x] = (uint16_t)(v > 0xfffeu ? 0xfffeu : v);   /* saturate */
        }
        s->acc_n++;
        s->row++;
        s->row_acc += 1.0;
        if (s->row_acc + 1e-9 < s->row_step) {
            continue;
        }
        s->row_acc -= s->row_step;

        uint8_t avg[RASTER_WIDTH_PX];
        for (uint32_t x = 0; x < RASTER_WIDTH_PX; x++) {
            avg[x] = (uint8_t)(s->vacc[x] / (s->acc_n ? s->acc_n : 1));
        }
        memset(s->vacc, 0, RASTER_WIDTH_PX * sizeof(uint16_t));
        s->acc_n = 0;

        uint8_t row48[RASTER_ROW_BYTES];
        /* 384 in, 384 out: dither_row() then applies no further horizontal
         * scaling (its box filter is a pass-through when lengths match). */
        if (dither_row(&s->dither, avg, RASTER_WIDTH_PX, row48) != ESP_OK) {
            return ESP_FAIL;
        }
        /* Drop the leading blank rows of every page (the top margin iOS
         * renders): pages then butt together seamlessly on the roll. Interior
         * blank rows are preserved once the page has content. */
        if (!s->page_has_content) {
            bool blank = true;
            for (size_t k = 0; k < RASTER_ROW_BYTES; k++) {
                if (row48[k] != 0) {
                    blank = false;
                    break;
                }
            }
            if (blank) {
                continue;
            }
            s->page_has_content = true;
        }
        if (pb_append_row(s->pb, row48) != ESP_OK) {
            ESP_LOGW(TAG, "page buffer full at row %u", (unsigned)s->out_rows);
            s->row = s->height;
            break;
        }
        s->out_rows++;
        s->page_out_rows++;
    }
    s->reps = 0;
    s->col = 0;
    s->op = 0;
    s->op_left = 0;
    s->px_len = 0;
    s->stage = US_REPEAT;
    return ESP_OK;
}

esp_err_t urf_stream_feed(urf_stream_t *s, const uint8_t *data, size_t len)
{
    if (s == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s->err != ESP_OK) {
        return s->err;
    }

    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];

        if (s->stage == US_MAGIC) {
            s->hdr[s->hdr_len++] = c;
            if (s->hdr_len == URF_HDR_LEN) {
                esp_err_t err = parse_file_header(s);
                if (err == ESP_OK) {
                    err = start_page(s, s->hdr + 12);
                }
                if (err == ESP_OK) {
                    err = dither_init(&s->dither, RASTER_WIDTH_PX);
                    if (err == ESP_OK) {
                        s->header_done = true;
                    }
                }
                if (err != ESP_OK) {
                    s->err = err;
                    return err;
                }
                s->stage = US_REPEAT;
            }
            continue;
        }

        /* Diagnostic: remember the first payload bytes so the URF variant a
         * client sends (row repeat byte + PackBits opcodes) can be inspected. */
        if (!s->dbg_done) {
            s->dbg[s->dbg_len++] = c;
            if (s->dbg_len >= sizeof(s->dbg)) {
                s->dbg_done = true;
                char hex[3 * 33 + 1];
                for (size_t k = 0; k < sizeof(s->dbg); k++) {
                    snprintf(hex + k * 3, 4, "%02x ", s->dbg[k]);
                }
                hex[sizeof(s->dbg) * 3] = '\0';
                ESP_LOGI(TAG, "first payload bytes: %s", hex);
            }
        }

        if (s->stage == US_PAGEHDR) {
            /* Header of page N+1. The page-end callback (which printed page N
             * and cleared the buffer) already ran before we got here. */
            s->hdr[s->hdr_len++] = c;
            if (s->hdr_len == URF_PAGE_HDR_LEN) {
                esp_err_t err = start_page(s, s->hdr);
                if (err != ESP_OK) {
                    s->err = err;
                    return err;
                }
                s->stage = US_REPEAT;
            }
            continue;
        }

        if (s->row >= s->height) {
            /* Current page fully decoded. If more pages follow, hand the
             * finished page to the callback (typically: trim + print + clear
             * the buffer), then expect the next 32-byte page header. Without
             * more pages, ignore trailing bytes (old page-1-only behaviour).
             * NOTE: this byte itself is the first byte of the next page
             * header — it must be re-processed, not discarded. */
            s->pages_started++;
            if (s->pages_started < s->pages) {
                ESP_LOGI(TAG, "page %u/%u complete (%u rows)", (unsigned)s->pages_started,
                         (unsigned)s->pages, (unsigned)s->page_out_rows);
                if (s->on_page_end != NULL) {
                    s->on_page_end(s->cb_ctx, s->pages_started);
                    if (s->err != ESP_OK) {
                        return s->err;
                    }
                }
                s->stage = US_PAGEHDR;
                s->hdr_len = 0;
                s->hdr[s->hdr_len++] = c;
                continue;
            }
            continue;
        }

        if (s->stage == US_REPEAT) {
            s->reps = (uint32_t)c + 1u;
            s->col = 0;
            s->op = 0;
            s->op_left = 0;
            s->px_len = 0;
            s->stage = US_ROW;
            continue;
        }

        /* US_ROW: either assembling a pixel, or executing an opcode. */
        if (s->op_left > 0) {
            s->px[s->px_len++] = c;
            if (s->px_len < s->bpp) {
                continue;
            }
            s->px_len = 0;

            if (s->op == 1) {
                /* 0x00..0x7F: ONE pixel byte followed by a repeat count. Write
                 * that pixel op_left times - do NOT read a byte per dot (that
                 * was the bug: it swallowed the following opcodes and turned the
                 * whole page into stripes). */
                while (s->op_left > 0 && s->col + s->bpp <= s->row_bytes) {
                    memcpy(s->rowbuf + s->col, s->px, s->bpp);
                    s->col += s->bpp;
                    s->op_left--;
                }
                s->op_left = 0;
                s->op = 0;
            } else {
                /* 0x81..0xFF: literal run - one pixel byte per dot. */
                if (s->col + s->bpp <= s->row_bytes) {
                    memcpy(s->rowbuf + s->col, s->px, s->bpp);
                    s->col += s->bpp;
                }
                s->op_left--;
            }
            if (s->col >= s->row_bytes) {
                esp_err_t err = emit_line(s);
                if (err != ESP_OK) {
                    s->err = err;
                    return err;
                }
            }
            continue;
        }

        /* new opcode */
        if (c == 0x80) {
            /* Apple Raster "clear to end of line": fill the rest of this line
             * with WHITE and finish it. This opcode does NOT consume a following
             * pixel byte - see cupsRasterReadPixels() in CUPS raster-stream.c.
             * Reading a pixel here (the old behaviour) shifted the entire
             * PackBits stream by one byte and painted the right two thirds of
             * every page solid black. */
            while (s->col + s->bpp <= s->row_bytes) {
                memset(s->rowbuf + s->col, s->white, s->bpp);
                s->col += s->bpp;
            }
            esp_err_t err = emit_line(s);
            if (err != ESP_OK) {
                s->err = err;
                return err;
            }
            continue;
        }
        if (c <= 0x7f) {
            s->op = 1;
            s->op_left = (uint32_t)c + 1u;
        } else {
            s->op = 3;
            s->op_left = 257u - (uint32_t)c;
        }
    }
    return ESP_OK;
}

esp_err_t urf_stream_finish(urf_stream_t *s)
{
    if (s == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s->err != ESP_OK) {
        return s->err;
    }
    if (!s->header_done) {
        ESP_LOGE(TAG, "document ended before URF header was complete");
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "decoded %u of %u rows across %u page header(s)", (unsigned)s->out_rows,
             (unsigned)s->height, (unsigned)s->pages_started);
    return s->out_rows > 0 ? ESP_OK : ESP_ERR_INVALID_ARG;
}
