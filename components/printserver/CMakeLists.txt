idf_component_register(
    SRCS "ipp.c" "mdns_adv.c" "ipp_server.c" "printserver.c" "webui.c"
    INCLUDE_DIRS "include" "."
    REQUIRES mdns raster lwip esp_timer settings catprinter esp_wifi esp_netif
)
