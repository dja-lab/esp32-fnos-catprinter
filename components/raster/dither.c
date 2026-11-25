#include "dither.h"
#include "settings.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static int16_t clamp_err(int v)
{
    if (v > 512) return 512;
    if (v < -512) return -512;
    return (int16_t)v;
}

esp_err_t dither_init(dither_state_t *st, size_t width)
{
    if (st == NULL || width == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    st->width = width;
    st->err_cur = (int16_t *)calloc(width, sizeof(int16_t));
    st->err_next = (int16_t *)calloc(width, sizeof(int16_t));
    if (st->err_cur == NULL || st->err_next == NULL) {
        dither_free(st);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void dither_free(dither_state_t *st)
{
    if (st == NULL) {
        return;
    }
    free(st->err_cur);
    free(st->err_next);
    st->err_cur = NULL;
    st->err_next = NULL;
    st->width = 0;
}

esp_err_t dither_row(dither_state_t *st, const uint8_t *gray, size_t gray_len,
                     uint8_t *out48)
{
    if (st == NULL || gray == NULL || out48 == NULL ||
        st->err_cur == NULL || gray_len == 0 || st->width != RASTER_WIDTH_PX) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Area average (box filter): the honest resample for a 6.5x downscale of a
     * whole page. Taking the darkest sample instead made every stroke touching a
     * 6.5x6.5 cell a full black dot -> a page of small text printed as a black
     * smear. No error diffusion either: Floyd-Steinberg turned antialiased text
     * into speckle on a 1-bit head (pdprint.swift just thresholds too). */
    uint8_t line[RASTER_WIDTH_PX];
    for (size_t x = 0; x < RASTER_WIDTH_PX; x++) {
        size_t s0 = (x * gray_len) / RASTER_WIDTH_PX;
        size_t s1 = ((x + 1) * gray_len + RASTER_WIDTH_PX - 1) / RASTER_WIDTH_PX;
        if (s1 <= s0) {
            s1 = s0 + 1;
        }
        if (s1 > gray_len) {
            s1 = gray_len;
        }
        uint32_t sum = 0;
        for (size_t i = s0; i < s1; i++) {
            sum += gray[i];
        }
        line[x] = (uint8_t)(sum / (s1 - s0));
    }

    memset(out48, 0, RASTER_ROW_BYTES);
    /* Runtime-tunable from the WebUI (default = CONFIG_AIRPRINT_THRESHOLD). */
    uint8_t thr = settings_get()->threshold;
    for (size_t x = 0; x < RASTER_WIDTH_PX; x++) {
        /* Plain threshold, no error diffusion (pdprint.swift does the same).
         * The threshold is tunable because area-averaging a 6.5x downscale
         * lightens strokes: raising it keeps more of the text. */
        if (line[x] < thr) {
            out48[x >> 3] |= (uint8_t)(0x80u >> (x & 7));
        }
    }
    return ESP_OK;
}
