#include "spool.h"

#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "spool";
static const esp_partition_t *s_part = NULL;

#define SPOOL_SECTOR 4096u

static size_t s_erased;      /* bytes erased in the current job (lazy) */
static size_t s_limit;       /* job size limit for this round */

static const esp_partition_t *part(void)
{
    if (s_part == NULL) {
        s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                          ESP_PARTITION_SUBTYPE_ANY, "storage");
        if (s_part == NULL) {
            ESP_LOGE(TAG, "no 'storage' partition - flash spool unavailable");
        }
    }
    return s_part;
}

esp_err_t spool_begin(size_t total_len)
{
    const esp_partition_t *p = part();
    if (p == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (total_len > p->size) {
        ESP_LOGE(TAG, "job too large for spool: %u > %u",
                 (unsigned)total_len, (unsigned)p->size);
        return ESP_ERR_INVALID_SIZE;
    }
    /* Erase lazily: iOS probes the printer with many small requests, and
     * erasing a whole 2 MB up front would stall each of them for seconds. */
    s_erased = 0;
    s_limit = total_len;
    return ESP_OK;
}

esp_err_t spool_write(size_t offset, const void *data, size_t len)
{
    const esp_partition_t *p = part();
    if (p == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (offset + len > p->size || offset + len > s_limit) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t need = (offset + len + SPOOL_SECTOR - 1) / SPOOL_SECTOR * SPOOL_SECTOR;
    if (need > s_erased) {
        esp_err_t err = esp_partition_erase_range(p, s_erased, need - s_erased);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "erase 0x%x+%u failed: %s", (unsigned)s_erased,
                     (unsigned)(need - s_erased), esp_err_to_name(err));
            return err;
        }
        s_erased = need;
    }
    return esp_partition_write(p, offset, data, len);
}

esp_err_t spool_read(size_t offset, void *dst, size_t len)
{
    const esp_partition_t *p = part();
    if (p == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (offset + len > p->size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return esp_partition_read(p, offset, dst, len);
}

size_t spool_capacity(void)
{
    const esp_partition_t *p = part();
    return p ? p->size : 0;
}
