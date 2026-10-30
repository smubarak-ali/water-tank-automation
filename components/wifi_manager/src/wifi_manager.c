
#include "wifi_manager.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#define WIFI_CONNECTED_BIT BIT0
#define MAX_RETRY_DELAY_MS 30000

static const char *TAG = "wifi_manager";

static EventGroupHandle_t wifi_events = NULL;
static esp_netif_t *wifi_netif = NULL;
static esp_timer_handle_t retry_timer = NULL;

static esp_event_handler_instance_t wifi_handler = NULL;
static esp_event_handler_instance_t ip_handler = NULL;

static bool wifi_initialized = false;
static bool wifi_started = false;
static volatile bool running = false;

static uint32_t retry_delay_ms = 1000;

// Schedule another connection attempt.
static void schedule_reconnect(void)
{
    if (!running || retry_timer == NULL)
    {
        return;
    }

    ESP_LOGW(TAG, "Reconnecting in %lu ms",
             (unsigned long)retry_delay_ms);

    esp_err_t err = esp_timer_start_once(
        retry_timer,
        (uint64_t)retry_delay_ms * 1000);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Could not schedule retry: %s",
                 esp_err_to_name(err));
        return;
    }

    // Increase delay up to 30 seconds.
    retry_delay_ms *= 2;

    if (retry_delay_ms > MAX_RETRY_DELAY_MS)
    {
        retry_delay_ms = MAX_RETRY_DELAY_MS;
    }
}

// Called when the retry timer expires.
static void retry_callback(void *arg)
{
    if (!running)
    {
        return;
    }

    esp_err_t err = esp_wifi_connect();

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Connection attempt failed: %s",
                 esp_err_to_name(err));
    }
}

// Process Wi-Fi and IP events.
static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    if (!running)
    {
        return;
    }

    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START)
    {

        ESP_LOGI(TAG, "Connecting to Wi-Fi...");

        esp_err_t err = esp_wifi_connect();

        if (err != ESP_OK)
        {
            schedule_reconnect();
        }
    }

    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED)
    {

        xEventGroupClearBits(
            wifi_events,
            WIFI_CONNECTED_BIT);

        wifi_event_sta_disconnected_t *event = event_data;

        ESP_LOGW(TAG, "Wi-Fi disconnected. Reason: %d",
                 event->reason);

        schedule_reconnect();
    }

    else if (event_base == IP_EVENT &&
             event_id == IP_EVENT_STA_GOT_IP)
    {

        ip_event_got_ip_t *event = event_data;

        ESP_LOGI(TAG, "Connected! IP: " IPSTR,
                 IP2STR(&event->ip_info.ip));

        // Reset retry delay after success.
        retry_delay_ms = 1000;

        if (esp_timer_is_active(retry_timer))
        {
            esp_timer_stop(retry_timer);
        }

        xEventGroupSetBits(
            wifi_events,
            WIFI_CONNECTED_BIT);
    }
}

// Initialize and start Wi-Fi.
esp_err_t wifi_manager_start(
    const char *ssid,
    const char *password)
{
    if (ssid == NULL || password == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (wifi_events != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    size_t ssid_len = strlen(ssid);
    size_t password_len = strlen(password);

    // This component requires WPA2/WPA3-Personal.
    if (ssid_len == 0 || ssid_len > 32 ||
        password_len < 8 || password_len > 63)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err;

    wifi_events = xEventGroupCreate();

    if (wifi_events == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    wifi_netif = esp_netif_create_default_wifi_sta();

    // Initialize Wi-Fi driver.
    wifi_init_config_t init_config =
        WIFI_INIT_CONFIG_DEFAULT();

    err = esp_wifi_init(&init_config);
    if (err != ESP_OK)
    {
        goto cleanup;
    }

    wifi_initialized = true;

    // Keep Wi-Fi credentials in RAM instead of
    // writing them to the Wi-Fi driver's NVS storage.
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK)
    {
        goto cleanup;
    }

    // Create reconnect timer.
    esp_timer_create_args_t timer_config = {
        .callback = retry_callback,
        .name = "wifi_retry"};

    err = esp_timer_create(&timer_config, &retry_timer);
    if (err != ESP_OK)
    {
        goto cleanup;
    }

    // Register Wi-Fi event handler.
    err = esp_event_handler_instance_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        wifi_event_handler,
        NULL,
        &wifi_handler);

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    // Register IP event handler.
    err = esp_event_handler_instance_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        wifi_event_handler,
        NULL,
        &ip_handler);

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    // Configure Wi-Fi credentials.
    wifi_config_t wifi_config = {0};

    memcpy(wifi_config.sta.ssid, ssid, ssid_len);
    memcpy(wifi_config.sta.password,
           password, password_len);

    wifi_config.sta.threshold.authmode =
        WIFI_AUTH_WPA2_PSK;

    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    wifi_config.sta.sae_pwe_h2e =
        WPA3_SAE_PWE_BOTH;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK)
    {
        goto cleanup;
    }

    err = esp_wifi_set_config(
        WIFI_IF_STA,
        &wifi_config);

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    retry_delay_ms = 1000;
    running = true;

    err = esp_wifi_start();

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    wifi_started = true;

    ESP_LOGI(TAG, "Wi-Fi manager started");
    return ESP_OK;

cleanup:
    wifi_manager_stop();
    return err;
}

// Wait for an IP address.
esp_err_t wifi_manager_wait_for_connection(
    uint32_t timeout_ms)
{
    if (!running || wifi_events == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    EventBits_t bits = xEventGroupWaitBits(
        wifi_events,
        WIFI_CONNECTED_BIT,
        pdFALSE,
        pdTRUE,
        pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT)
    {
        return ESP_OK;
    }

    return ESP_ERR_TIMEOUT;
}

// Return true when connected with an IP.
bool wifi_manager_is_connected(void)
{
    if (wifi_events == NULL)
    {
        return false;
    }

    EventBits_t bits = xEventGroupGetBits(wifi_events);

    return (bits & WIFI_CONNECTED_BIT) != 0;
}

// Stop Wi-Fi and release resources.
void wifi_manager_stop(void)
{
    running = false;

    if (retry_timer != NULL)
    {
        esp_timer_stop(retry_timer);
        esp_timer_delete(retry_timer);
        retry_timer = NULL;
    }

    if (wifi_handler != NULL)
    {
        esp_event_handler_instance_unregister(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            wifi_handler);
        wifi_handler = NULL;
    }

    if (ip_handler != NULL)
    {
        esp_event_handler_instance_unregister(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            ip_handler);
        ip_handler = NULL;
    }

    if (wifi_started)
    {
        esp_wifi_stop();
        wifi_started = false;
    }

    if (wifi_initialized)
    {
        esp_wifi_deinit();
        wifi_initialized = false;
    }

    if (wifi_netif != NULL)
    {
        esp_netif_destroy_default_wifi(wifi_netif);
        wifi_netif = NULL;
    }

    if (wifi_events != NULL)
    {
        vEventGroupDelete(wifi_events);
        wifi_events = NULL;
    }

    retry_delay_ms = 1000;
}
