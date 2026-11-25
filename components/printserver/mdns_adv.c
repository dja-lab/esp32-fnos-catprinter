/*
 * Bonjour/mDNS advertisement for AirPrint.
 * Critical: `_universal` subtype + `pdl=image/urf` + `URF=` TXT keyword.
 * Without these, iOS will not list the printer or will send PDF instead.
 *
 * The instance name IS what iOS shows in the print sheet, and every TXT field
 * that carries a human-readable name must follow the same setting - otherwise
 * iOS keeps showing a stale name ("CatPrint-C3") next to the new one.
 */
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "mdns.h"

#include "printserver.h"
#include "settings.h"

static const char *TAG = "mdns_adv";

#define PRINT_HOSTNAME  "catprint-c3"
#define IPP_PORT        631

static bool s_mdns_up = false;

esp_err_t ps_mdns_rename(const char *instance_name);

/* mdns copies TXT keys/values internally, but keeping these static removes any
 * doubt about lifetime while a rename re-uses the same buffers. */
static char s_ty[SETTINGS_NAME_MAX + 8];
static char s_product[SETTINGS_NAME_MAX + 10];

static esp_err_t add_service(const char *name)
{
    const char *n = (name != NULL && name[0] != '\0') ? name : "CatPrint-C3";
    snprintf(s_ty, sizeof(s_ty), "%s", n);
    snprintf(s_product, sizeof(s_product), "(%s)", n);

    mdns_txt_item_t txt_items[] = {
        {"txtvers", "1"},
        {"qtotal", "1"},
        {"rp",      "ipp/print"},
        /* Only URF: iOS renders to raster itself; no PDF parsing on MCU. */
        {"pdl",     "image/urf"},
        /* Real printers publish a full capability string; iOS parses this to
         * decide the raster it may send. "kind" must be a legal value
         * (document/photo/envelope/label) - an unknown value makes iOS ignore
         * the advertisement for job submission. */
        {"URF",     "CP1,CR1,SRGB24,RS300,FB8,W8,DM1"},
        {"kind",    "document"},
        {"ty",      s_ty},
        {"product", s_product},
        {"Color",   "F"},
        {"Duplex",  "F"},
        {"priority", "0"},
        {"usb_MFG", "CatPrint"},
        {"usb_MDL", "C3"},
        {"note",    s_ty},
        {"adminurl", "http://catprint-c3.local/"},
        {"uuid",    "6b1d0000-0000-1000-8000-e83dc19ddc38"},
    };

    /* IMPORTANT: service_type is "_ipp" and proto is "_tcp" — NOT "_ipp._tcp".
     * Passing "_ipp._tcp" here publishes
     *   catprinter._ipp._tcp._tcp.local
     * which no AirPrint client ever looks at (iOS silently shows nothing, while
     * the A record still resolves, which makes it look like a network problem). */
    esp_err_t err = mdns_service_add(NULL, "_ipp", "_tcp", IPP_PORT,
                                     txt_items, sizeof(txt_items) / sizeof(txt_items[0]));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_service_add failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Required by AirPrint clients to recognise a universal printer. */
    err = mdns_service_subtype_add_for_host(NULL, "_ipp", "_tcp", NULL,
                                            "_universal");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "subtype add failed: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "advertising '%s' (_ipp._tcp) at %s.local:%d",
             n, PRINT_HOSTNAME, IPP_PORT);
    return ESP_OK;
}

esp_err_t ps_mdns_start(const char *instance_name)
{
    if (s_mdns_up) {
        return ps_mdns_rename(instance_name);
    }
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_mdns_up = true;

    err = mdns_hostname_set(PRINT_HOSTNAME);
    if (err != ESP_OK) {
        return err;
    }
    err = mdns_instance_name_set(instance_name ? instance_name : "CatPrint-C3");
    if (err != ESP_OK) {
        return err;
    }
    return add_service(instance_name);
}

/* Rename the advertised queue without a reboot: removing the service makes
 * esp-mdns send a goodbye packet for the OLD instance name (otherwise iOS keeps
 * the stale entry until the record TTL expires), then the new name is probed
 * and announced ~250 ms later. */
esp_err_t ps_mdns_rename(const char *instance_name)
{
    if (!s_mdns_up) {
        return ESP_ERR_INVALID_STATE;
    }
    mdns_instance_name_set(instance_name);
    mdns_service_remove("_ipp", "_tcp");
    return add_service(instance_name);
}

esp_err_t ps_mdns_stop(void)
{
    if (!s_mdns_up) {
        return ESP_OK;
    }
    mdns_service_remove("_ipp", "_tcp");
    mdns_free();
    s_mdns_up = false;
    return ESP_OK;
}
