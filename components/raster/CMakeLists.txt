idf_component_register(
    SRCS "urf.c" "urf_stream.c" "dither.c" "pagebuf.c" "spool.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_partition settings
)
