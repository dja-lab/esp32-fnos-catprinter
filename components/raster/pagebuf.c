#include "pagebuf.h"
#include "settings.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

esp_err_t pb_init(page_buf_t *pb, size_t max_rows)
{
    if (pb == NULL || max_rows == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    pb->rows = (uint8_t *)malloc(max_rows * RASTER_ROW_BYTES);
    if (pb->rows == NULL) {
        return ESP_ERR_NO_MEM;
    }
    pb->capacity_rows = max_rows;
    pb->nrows = 0;
    return ESP_OK;
}

void pb_free(page_buf_t *pb)
{
    if (pb == NULL) {
        return;
    }
    free(pb->rows);
    pb->rows = NULL;
    pb->nrows = 0;
    pb->capacity_rows = 0;
}

void pb_clear(page_buf_t *pb)
{
    if (pb != NULL) {
        pb->nrows = 0;
    }
}

esp_err_t pb_append_row(page_buf_t *pb, const uint8_t *row48)
{
    if (pb == NULL || row48 == NULL || pb->rows == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pb->nrows >= pb->capacity_rows) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(&pb->rows[pb->nrows * RASTER_ROW_BYTES], row48, RASTER_ROW_BYTES);
    pb->nrows++;
    return ESP_OK;
}

const uint8_t *pb_data(const page_buf_t *pb)
{
    return pb != NULL ? pb->rows : NULL;
}

static bool pb_row_blank(const uint8_t *row)
{
    for (size_t k = 0; k < RASTER_ROW_BYTES; k++) {
        if (row[k] != 0) {
            return false;
        }
    }
    return true;
}

uint32_t pb_trim_trailing_blank(page_buf_t *pb)
{
    if (pb == NULL || pb->rows == NULL) {
        return 0;
    }
    size_t n = pb->nrows;
    while (n > 0 && pb_row_blank(pb->rows + (n - 1) * RASTER_ROW_BYTES)) {
        n--;
    }
    pb->nrows = n;
    return (uint32_t)n;
}

static uint8_t rev8(uint8_t b)
{
    b = (uint8_t)(((b & 0xaa) >> 1) | ((b & 0x55) << 1));
    b = (uint8_t)(((b & 0xcc) >> 2) | ((b & 0x33) << 2));
    return (uint8_t)(((b & 0xf0) >> 4) | ((b & 0x0f) << 4));
}

void pb_apply_orientation(page_buf_t *pb, uint8_t orientation)
{
    if (pb == NULL || pb->rows == NULL || orientation == ORI_NORMAL) {
        return;
    }
    uint8_t *rows = pb->rows;
    size_t n = pb->nrows;
    bool mirror = (orientation == ORI_MIRROR || orientation == ORI_MIRROR_ROT180);
    bool rotate = (orientation == ORI_ROTATE180 || orientation == ORI_MIRROR_ROT180);

    if (mirror) {
        for (size_t r = 0; r < n; r++) {
            uint8_t *row = rows + r * RASTER_ROW_BYTES;
            for (size_t k = 0; k < RASTER_ROW_BYTES; k++) {
                row[k] = rev8(row[k]);
            }
        }
    }
    if (rotate) {
        uint8_t tmp[RASTER_ROW_BYTES];
        for (size_t r = 0; r < n / 2; r++) {
            uint8_t *a = rows + r * RASTER_ROW_BYTES;
            uint8_t *b = rows + (n - 1 - r) * RASTER_ROW_BYTES;
            memcpy(tmp, a, RASTER_ROW_BYTES);
            for (size_t k = 0; k < RASTER_ROW_BYTES; k++) {
                a[k] = rev8(b[RASTER_ROW_BYTES - 1 - k]);
                b[k] = rev8(tmp[RASTER_ROW_BYTES - 1 - k]);
            }
        }
        if (n % 2) {
            uint8_t *m = rows + (n / 2) * RASTER_ROW_BYTES;
            memcpy(tmp, m, RASTER_ROW_BYTES);
            for (size_t k = 0; k < RASTER_ROW_BYTES; k++) {
                m[k] = rev8(tmp[RASTER_ROW_BYTES - 1 - k]);
            }
        }
    }
}
