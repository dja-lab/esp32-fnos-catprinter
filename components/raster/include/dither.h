/*
 * Downscale + Floyd-Steinberg dithering.
 * Input rows may be any width; they are box-filtered to 384 dots first,
 * then error-diffused to 1 bit. Output is MSB-first, matching PBM P4 and
 * the bit reversal done later by the BLE protocol layer.
 */
#ifndef DITHER_H
#define DITHER_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "pagebuf.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t *err_cur;
    int16_t *err_next;
    size_t width;
} dither_state_t;

esp_err_t dither_init(dither_state_t *st, size_t width);
void dither_free(dither_state_t *st);

/* gray: 8-bit samples, gray_len samples. out48: 48 bytes. */
esp_err_t dither_row(dither_state_t *st, const uint8_t *gray, size_t gray_len,
                     uint8_t *out48);

#ifdef __cplusplus
}
#endif
#endif
