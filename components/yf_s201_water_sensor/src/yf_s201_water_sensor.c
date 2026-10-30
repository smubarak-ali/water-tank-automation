#include <stdio.h>
#include "yf_s201_water_sensor.h"
#include <stdbool.h>
#include <stdlib.h>
#include "driver/pulse_cnt.h"
#include "esp_timer.h"

#define PCNT_HIGH_LIMIT 30000
#define PCNT_LOW_LIMIT -30000
#define PCNT_GLITCH_NS 1000

struct yf_s201_sensor
{
    pcnt_unit_handle_t unit;
    pcnt_channel_handle_t channel;

    float calibration_factor;

    int previous_count;
    int64_t previous_time_us;

    uint64_t total_pulses;

    bool enabled;
    bool started;
    bool high_watch_added;
    bool low_watch_added;
};

void yf_s201_delete(yf_s201_handle_t sensor)
{
    if (!sensor)
    {
        return;
    }

    if (sensor->unit)
    {
        if (sensor->started)
        {
            pcnt_unit_stop(sensor->unit);
        }

        if (sensor->enabled)
        {
            pcnt_unit_disable(sensor->unit);
        }

        if (sensor->high_watch_added)
        {
            pcnt_unit_remove_watch_point(
                sensor->unit, PCNT_HIGH_LIMIT);
        }

        if (sensor->low_watch_added)
        {
            pcnt_unit_remove_watch_point(
                sensor->unit, PCNT_LOW_LIMIT);
        }
    }

    if (sensor->channel)
    {
        pcnt_del_channel(sensor->channel);
    }

    if (sensor->unit)
    {
        pcnt_del_unit(sensor->unit);
    }

    free(sensor);
}

esp_err_t yf_s201_create(
    const yf_s201_config_t *config,
    yf_s201_handle_t *out_sensor)
{
    if (!config || !out_sensor ||
        config->gpio_num < 0 ||
        config->calibration_factor < 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_sensor = NULL;

    yf_s201_handle_t sensor =
        calloc(1, sizeof(*sensor));

    if (!sensor)
    {
        return ESP_ERR_NO_MEM;
    }

    sensor->calibration_factor =
        config->calibration_factor > 0
            ? config->calibration_factor
            : YF_S201_DEFAULT_K_FACTOR;

    esp_err_t err;

#define TRY(expr)          \
    do                     \
    {                      \
        err = (expr);      \
        if (err != ESP_OK) \
            goto fail;     \
    } while (0)

    // 1. Create the PCNT unit.
    pcnt_unit_config_t unit_config = {
        .high_limit = PCNT_HIGH_LIMIT,
        .low_limit = PCNT_LOW_LIMIT,
        .flags.accum_count = true,
    };

    TRY(pcnt_new_unit(
        &unit_config, &sensor->unit));

    // 2. Configure the signal input.
    pcnt_chan_config_t channel_config = {
        .edge_gpio_num = config->gpio_num,
        .level_gpio_num = -1,
    };

    TRY(pcnt_new_channel(
        sensor->unit,
        &channel_config,
        &sensor->channel));

    // 3. Count rising edges only.
    TRY(pcnt_channel_set_edge_action(
        sensor->channel,
        PCNT_CHANNEL_EDGE_ACTION_INCREASE,
        PCNT_CHANNEL_EDGE_ACTION_HOLD));

    // 4. Filter short electrical glitches.
    pcnt_glitch_filter_config_t filter_config = {
        .max_glitch_ns = PCNT_GLITCH_NS,
    };

    TRY(pcnt_unit_set_glitch_filter(
        sensor->unit, &filter_config));

    // 5. Enable overflow accumulation.
    TRY(pcnt_unit_add_watch_point(
        sensor->unit, PCNT_HIGH_LIMIT));
    sensor->high_watch_added = true;

    TRY(pcnt_unit_add_watch_point(
        sensor->unit, PCNT_LOW_LIMIT));
    sensor->low_watch_added = true;

    // 6. Enable PCNT.
    TRY(pcnt_unit_enable(sensor->unit));
    sensor->enabled = true;

    // Clear count to activate watch points.
    TRY(pcnt_unit_clear_count(sensor->unit));

    // 7. Start counting continuously.
    TRY(pcnt_unit_start(sensor->unit));
    sensor->started = true;

    sensor->previous_count = 0;
    sensor->previous_time_us = esp_timer_get_time();
    sensor->total_pulses = 0;

    *out_sensor = sensor;
    return ESP_OK;

fail:
    yf_s201_delete(sensor);
    return err;

#undef TRY
}

esp_err_t yf_s201_read(
    yf_s201_handle_t sensor,
    yf_s201_reading_t *reading)
{
    if (!sensor || !reading)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // Read the PCNT hardware count.
    int current_count = 0;

    esp_err_t err = pcnt_unit_get_count(
        sensor->unit, &current_count);

    if (err != ESP_OK)
    {
        return err;
    }

    const int64_t now_us = esp_timer_get_time();

    const int64_t elapsed_us =
        now_us - sensor->previous_time_us;

    if (elapsed_us <= 0)
    {
        return ESP_ERR_INVALID_STATE;
    }

    // Unsigned subtraction also handles 32-bit rollover.
    const uint32_t pulse_delta =
        (uint32_t)current_count -
        (uint32_t)sensor->previous_count;

    // Actual sample duration in seconds.
    const double elapsed_seconds =
        (double)elapsed_us / 1000000.0;

    // Hz = pulses / seconds.
    const double frequency =
        pulse_delta / elapsed_seconds;

    // Flow in liters per minute.
    const float flow_lpm =
        (float)(frequency / sensor->calibration_factor);

    // Accumulate the measured pulses.
    sensor->total_pulses += pulse_delta;

    // Pulses per liter = K * 60.
    const double total_liters =
        (double)sensor->total_pulses /
        (sensor->calibration_factor * 60.0);

    reading->interval_pulses = pulse_delta;
    reading->total_pulses = sensor->total_pulses;
    reading->flow_lpm = flow_lpm;
    reading->total_liters = total_liters;

    // Save reference values for the next read.
    sensor->previous_count = current_count;
    sensor->previous_time_us = now_us;

    return ESP_OK;
}
