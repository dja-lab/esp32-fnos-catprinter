idf_component_register(
    SRCS "bridge_server.c" "printserver.c" "webui.c"
    INCLUDE_DIRS "include" "."
    REQUIRES raster lwip esp_timer settings catprinter esp_wifi esp_netif
)
