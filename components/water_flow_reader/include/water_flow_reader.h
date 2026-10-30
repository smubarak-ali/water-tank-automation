#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "yf_s201_water_sensor.h"

typedef enum
{
    WATER_FLOW_IDLE,
    WATER_FLOW_STARTING,
    WATER_FLOW_FLOWING,
    WATER_FLOW_NO_FLOW,
    WATER_FLOW_SENSOR_ERROR
} water_flow_state_t;

typedef struct
{
    float flow_on_lpm;
    float flow_off_lpm;

    uint32_t startup_grace_ms;
    uint32_t flow_confirm_ms;
    uint32_t no_flow_confirm_ms;
    uint32_t max_sample_gap_ms;
} water_flow_monitor_config_t;

typedef struct
{
    water_flow_monitor_config_t config;
    water_flow_state_t state;

    int64_t started_ms;
    int64_t last_sample_ms;
    int64_t above_since_ms;
    int64_t below_since_ms;

    bool above_pending;
    bool below_pending;

    float last_flow_lpm;
} water_flow_monitor_t;

// Initialize the monitor.
esp_err_t water_flow_monitor_init(
    water_flow_monitor_t *monitor,
    const water_flow_monitor_config_t *config);

// Call when water flow becomes expected.
void water_flow_monitor_start(
    water_flow_monitor_t *monitor,
    int64_t now_ms);

// Call when water flow is no longer expected.
void water_flow_monitor_stop(
    water_flow_monitor_t *monitor);

// Process a reading from YF-S201.
esp_err_t water_flow_monitor_update(
    water_flow_monitor_t *monitor,
    const yf_s201_reading_t *reading,
    int64_t now_ms);

// Mark the sensor as faulty or unavailable.
void water_flow_monitor_sensor_error(
    water_flow_monitor_t *monitor);

// Convert state to a readable string.
const char *water_flow_monitor_state_name(
    water_flow_state_t state);
