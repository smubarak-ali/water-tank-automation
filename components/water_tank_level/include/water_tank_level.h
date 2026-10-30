#pragma once

#include "esp_err.h"

typedef enum
{
    WATER_TANK_LEVEL_LOW,
    WATER_TANK_LEVEL_HIGH,
    WATER_TANK_LEVEL_SENSOR_ERROR
} water_tank_level_t;

esp_err_t sensor_init(int low_tank_pin, int high_tank_pin);

water_tank_level_t get_tank_level(void);
