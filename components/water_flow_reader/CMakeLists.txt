idf_component_register(
    SRCS "src/water_flow_reader.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_adc yf_s201_water_sensor
)
