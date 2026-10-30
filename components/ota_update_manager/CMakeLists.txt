idf_component_register(
    SRCS "src/ota_update_manager.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        esp_https_ota
        esp_http_client
        esp-tls
        app_update
        esp_partition
        esp_app_format
        efuse
        esp_timer
        esp_system
)
