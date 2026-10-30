#include <stdio.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "water_tank_level.h"

static int LOW_TANK_PIN;
static int HIGH_TANK_PIN;

esp_err_t sensor_init(int low_tank_pin, int high_tank_pin)
{
    LOW_TANK_PIN = low_tank_pin;
    HIGH_TANK_PIN = high_tank_pin;

    gpio_config_t io_conf_low = {
        .pin_bit_mask = (1ULL << LOW_TANK_PIN), // Select GPIO
        .mode = GPIO_MODE_INPUT,                // Set as input
        .pull_up_en = GPIO_PULLUP_ENABLE,       // Enable internal pull-up
        .pull_down_en = GPIO_PULLDOWN_DISABLE,  // Disable pull-down
        .intr_type = GPIO_INTR_DISABLE          // Disable interrupts
    };

    gpio_config_t io_conf_high = {
        .pin_bit_mask = (1ULL << HIGH_TANK_PIN), // Select GPIO
        .mode = GPIO_MODE_INPUT,                 // Set as input
        .pull_up_en = GPIO_PULLUP_ENABLE,        // Enable internal pull-up
        .pull_down_en = GPIO_PULLDOWN_DISABLE,   // Disable pull-down
        .intr_type = GPIO_INTR_DISABLE           // Disable interrupts
    };

    ESP_ERROR_CHECK(gpio_config(&io_conf_low));
    ESP_ERROR_CHECK(gpio_config(&io_conf_high));
    return ESP_OK;
}

water_tank_level_t get_tank_level(void)
{
    int low_level = gpio_get_level(LOW_TANK_PIN);
    int high_level = gpio_get_level(HIGH_TANK_PIN);

    if (low_level == 0)
    {
        return WATER_TANK_LEVEL_LOW;
    }
    else if (high_level == 0)
    {
        return WATER_TANK_LEVEL_HIGH;
    }
    else
    {
        return WATER_TANK_LEVEL_SENSOR_ERROR;
    }
}