/*
 * Print-job spool: the IPP body is written to the `storage` partition instead
 * of being kept in RAM. On a C3 with WiFi+BLE up there are only ~13 KB of free
 * heap, so even a 40 KB document cannot be buffered in memory.
 */
#ifndef SPOOL_H
#define SPOOL_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Erase (in 4 KB sectors) and prepare `total_len` bytes at offset 0. */
esp_err_t spool_begin(size_t total_len);

/* Append at `offset` (must be sequential). */
esp_err_t spool_write(size_t offset, const void *data, size_t len);

/* Read back, 4-byte-alignment safe. Returns ESP_ERR_INVALID_STATE if the
 * storage partition is missing. */
esp_err_t spool_read(size_t offset, void *dst, size_t len);

size_t spool_capacity(void);

#ifdef __cplusplus
}
#endif
#endif
