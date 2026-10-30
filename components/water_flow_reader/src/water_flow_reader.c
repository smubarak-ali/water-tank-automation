#include <stdio.h>
#include "water_flow_reader.h"
#include <math.h>
#include <string.h>

esp_err_t water_flow_monitor_init(
    water_flow_monitor_t *m,
    const water_flow_monitor_config_t *cfg)
{
    if (!m || !cfg ||
        !isfinite(cfg->flow_on_lpm) ||
        !isfinite(cfg->flow_off_lpm) ||
        cfg->flow_on_lpm <= 0 ||
        cfg->flow_off_lpm < 0 ||
        cfg->flow_off_lpm >= cfg->flow_on_lpm ||
        cfg->startup_grace_ms == 0 ||
        cfg->flow_confirm_ms == 0 ||
        cfg->no_flow_confirm_ms == 0 ||
        cfg->max_sample_gap_ms == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(m, 0, sizeof(*m));

    m->config = *cfg;
    m->state = WATER_FLOW_IDLE;

    return ESP_OK;
}

void water_flow_monitor_start(
    water_flow_monitor_t *m,
    int64_t now_ms)
{
    if (!m)
        return;

    m->state = WATER_FLOW_STARTING;
    m->started_ms = now_ms;
    m->last_sample_ms = now_ms;

    m->above_pending = false;
    m->below_pending = false;
    m->last_flow_lpm = 0;
}

void water_flow_monitor_stop(
    water_flow_monitor_t *m)
{
    if (!m)
        return;

    m->state = WATER_FLOW_IDLE;
    m->above_pending = false;
    m->below_pending = false;
}

void water_flow_monitor_sensor_error(
    water_flow_monitor_t *m)
{
    if (!m)
        return;

    m->state = WATER_FLOW_SENSOR_ERROR;
    m->above_pending = false;
    m->below_pending = false;
}

esp_err_t water_flow_monitor_update(
    water_flow_monitor_t *m,
    const yf_s201_reading_t *reading,
    int64_t now_ms)
{
    if (!m || !reading)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (m->state == WATER_FLOW_IDLE)
    {
        return ESP_OK;
    }

    if (m->state == WATER_FLOW_SENSOR_ERROR)
    {
        return ESP_ERR_INVALID_STATE;
    }

    // Detect missed or excessively delayed samples.
    int64_t gap_ms = now_ms - m->last_sample_ms;

    if (gap_ms < 0 ||
        gap_ms > m->config.max_sample_gap_ms)
    {
        water_flow_monitor_sensor_error(m);
        return ESP_ERR_TIMEOUT;
    }

    m->last_sample_ms = now_ms;

    float flow = reading->flow_lpm;

    if (!isfinite(flow) || flow < 0)
    {
        water_flow_monitor_sensor_error(m);
        return ESP_ERR_INVALID_ARG;
    }

    m->last_flow_lpm = flow;

    // --------------------------------------
    // 1. Track sustained sufficient flow.
    // --------------------------------------
    if (flow >= m->config.flow_on_lpm)
    {
        if (!m->above_pending)
        {
            m->above_since_ms = now_ms;
            m->above_pending = true;
        }
    }
    else
    {
        m->above_pending = false;
    }

    // --------------------------------------
    // 2. Track sustained low/absent flow.
    // --------------------------------------
    if (flow <= m->config.flow_off_lpm)
    {
        if (!m->below_pending)
        {
            m->below_since_ms = now_ms;
            m->below_pending = true;
        }
    }
    else
    {
        m->below_pending = false;
    }

    // --------------------------------------
    // 3. Confirm FLOWING.
    // --------------------------------------
    if (m->state == WATER_FLOW_STARTING ||
        m->state == WATER_FLOW_NO_FLOW)
    {

        if (m->above_pending &&
            now_ms - m->above_since_ms >=
                m->config.flow_confirm_ms)
        {

            m->state = WATER_FLOW_FLOWING;
            m->below_pending = false;

            return ESP_OK;
        }
    }

    // --------------------------------------
    // 4. Check startup grace timeout.
    // --------------------------------------
    if (m->state == WATER_FLOW_STARTING)
    {

        if (now_ms - m->started_ms >=
            m->config.startup_grace_ms)
        {

            m->state = WATER_FLOW_NO_FLOW;
        }

        return ESP_OK;
    }

    // --------------------------------------
    // 5. Detect loss of established flow.
    // --------------------------------------
    if (m->state == WATER_FLOW_FLOWING)
    {

        if (m->below_pending &&
            now_ms - m->below_since_ms >=
                m->config.no_flow_confirm_ms)
        {

            m->state = WATER_FLOW_NO_FLOW;
            m->above_pending = false;
        }
    }

    return ESP_OK;
}

const char *water_flow_monitor_state_name(
    water_flow_state_t state)
{
    switch (state)
    {
    case WATER_FLOW_IDLE:
        return "IDLE";

    case WATER_FLOW_STARTING:
        return "STARTING";

    case WATER_FLOW_FLOWING:
        return "FLOWING";

    case WATER_FLOW_NO_FLOW:
        return "NO_FLOW";

    case WATER_FLOW_SENSOR_ERROR:
        return "SENSOR_ERROR";

    default:
        return "UNKNOWN";
    }
}
