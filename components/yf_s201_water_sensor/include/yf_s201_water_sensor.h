#pragma once

#include <stdint.h>
#include "esp_err.h"

#define YF_S201_DEFAULT_K_FACTOR 7.0f

typedef struct yf_s201_sensor *yf_s201_handle_t;

typedef struct
{
    int gpio_num;

    // Pulse frequency (Hz) per L/min.
    // Use 0 to select the default value of 7.
    float calibration_factor;

} yf_s201_config_t;

typedef struct
{
    uint32_t interval_pulses;
    uint64_t total_pulses;

    float flow_lpm;
    double total_liters;

} yf_s201_reading_t;

/**
 * Initialize and start the sensor's PCNT peripheral.
 */
esp_err_t yf_s201_create(
    const yf_s201_config_t *config,
    yf_s201_handle_t *out_sensor);

/**
 * Sample the sensor.
 *
 * Calculates flow using pulses accumulated since
 * the previous sample and the actual elapsed time.
 *
 * Call from one task only, typically every second.
 */
esp_err_t yf_s201_read(
    yf_s201_handle_t sensor,
    yf_s201_reading_t *reading);

/**
 * Stop the sensor and release PCNT resources.
 */
void yf_s201_delete(yf_s201_handle_t sensor);
