/*
 * Apple Raster (URF / image/urf) decoding — real on-wire format.
 *
 * File header  : "UNIRAST\0" (8 B) + page count (u32 BE)          = 12 B
 * Page header  : 32 B, big-endian
 *                [0] bits per pixel, [1] colorspace (1=RGB, 2=gray),
 *                [2] duplex, [3] quality,
 *                [4..11] unknown, [12..15] width px, [16..19] height px,
 *                [20..23] dpi, [24..31] unknown
 * Raster data  : per line group, a "repeat" byte (n+1 identical lines) then
 *                PackBits-style data for ONE line:
 *                  0x00..0x7F  repeat the next pixel (c+1) times
 *                  0x80        fill the rest of the line with the next pixel
 *                  0x81..0xFF  copy the next (257-c) pixels literally
 *
 * The document is read through a small window (RAM buffer or flash spool), so a
 * 2 MB job never needs 2 MB of heap.
 */
#ifndef URF_H
#define URF_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "pagebuf.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t *mem;     /* non-NULL: document already in RAM */
    size_t flash_offset;    /* used when mem == NULL: offset in spool partition */
    size_t len;             /* document length in bytes */
} urf_doc_t;

/* Stream a URF document into a 1-bit page buffer:
 *   parse header -> PackBits-decode one line -> gray -> downscale 384 -> dither.
 * Only page 1 of a multi-page job is printed (logged). */
esp_err_t raster_urf_to_pagebuf(const urf_doc_t *doc, page_buf_t *pb);

#ifdef __cplusplus
}
#endif
#endif
