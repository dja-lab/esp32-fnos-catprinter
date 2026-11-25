idf_component_register(
    SRCS "printer_protocol.c" "catprinter.c"
    INCLUDE_DIRS "include"
    REQUIRES bt settings
)
