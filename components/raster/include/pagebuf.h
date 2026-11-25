/*
 * Page buffer: contiguous 1-bit raster rows, 384 dots = 48 bytes per row.
 * A full A4 page scaled to printer width is ~48 * 2000 = 96 KiB, which fits
 * ESP32-C3 SRAM (no PSRAM needed). Raw URF input is NEVER buffered whole;
 * it is streamed in, decoded row by row, and only the 1-bit result is kept.
 */
#ifndef PAGEBUF_H
#define PAGEBUF_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RASTER_WIDTH_PX  384u
#define RASTER_ROW_BYTES (RASTER_WIDTH_PX / 8u)
#define PAGEBUF_DEFAULT_MAX_ROWS 800u   /* ~37 KiB. One page never needs more:
                                         * an A4 raster (2480x3507 @300 dpi)
                                         * downscaled 6.46x is 543 rows, an A6
                                         * is 541, so 800 covers every page
                                         * size plus the 24-row tear gap. It
                                         * used to be 1200 (56 KiB), which ate
                                         * 19 KiB of the ~25 KiB left after
                                         * WiFi+BLE came up - and that is what
                                         * starved an iPhone A4 upload. */

typedef struct {
    uint8_t *rows;
    size_t nrows;
    size_t capacity_rows;
} page_buf_t;

/* Allocate a contiguous buffer for max_rows rows. */
esp_err_t pb_init(page_buf_t *pb, size_t max_rows);
void pb_free(page_buf_t *pb);
void pb_clear(page_buf_t *pb);

/* Append one 48-byte row; returns ESP_ERR_NO_MEM if capacity is exhausted. */
esp_err_t pb_append_row(page_buf_t *pb, const uint8_t *row48);

const uint8_t *pb_data(const page_buf_t *pb);

/* Drop trailing all-white rows (a printed dot is a set bit). Modifies
 * pb->nrows in place and returns the new count. Used to remove the page
 * margins iOS renders around every page so multi-page jobs butt together
 * seamlessly on continuous roll paper. */
uint32_t pb_trim_trailing_blank(page_buf_t *pb);

/* Transform the finished 1-bit page in place. `orientation` is one of the
 * print_orientation_t values from settings.h (0 normal, 1 rotate180,
 * 2 mirror, 3 mirror+rotate180). Works on stack/foreign buffers too: only
 * rows/nrows are touched, no allocation. */
void pb_apply_orientation(page_buf_t *pb, uint8_t orientation);

#ifdef __cplusplus
}
#endif
#endif
