idf_component_register(
    SRCS "src/mqtt_service.c"
    INCLUDE_DIRS "include"
    REQUIRES
        mqtt
        esp_event
        mbedtls
    )
