
#include "ota_update_manager.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_efuse.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#define FIRMWARE_BASE_URL \
    "https://ota.abdalipaas.xyz"

#define OTA_TIMEOUT_SECONDS 600

static const char *TAG = "OTA";

// Supports versions like 1.2.3.
// Pre-release labels are intentionally not supported.
static bool parse_version(const char *text, int parts[3])
{
    if (text == NULL)
    {
        return false;
    }

    for (int i = 0; i < 3; i++)
    {
        if (!isdigit((unsigned char)*text))
        {
            return false;
        }

        int number = 0;

        while (isdigit((unsigned char)*text))
        {
            number = number * 10 + (*text - '0');

            if (number > 1000000)
            {
                return false;
            }

            text++;
        }

        parts[i] = number;

        if (i < 2)
        {
            if (*text != '.')
            {
                return false;
            }
            text++;
        }
    }

    return *text == '\0';
}

static int compare_versions(const char *new_version,
                            const char *current_version)
{
    int incoming[3];
    int current[3];

    if (!parse_version(new_version, incoming) ||
        !parse_version(current_version, current))
    {
        return -2; // Invalid version format
    }

    for (int i = 0; i < 3; i++)
    {
        if (incoming[i] > current[i])
        {
            return 1;
        }

        if (incoming[i] < current[i])
        {
            return -1;
        }
    }

    return 0;
}

// Confirm a newly booted firmware after diagnostics.
esp_err_t ota_update_confirm(bool healthy)
{
    const esp_partition_t *running =
        esp_ota_get_running_partition();

    if (running == NULL)
    {
        return ESP_FAIL;
    }

    esp_ota_img_states_t state;

    esp_err_t err = esp_ota_get_state_partition(
        running, &state);

    // No confirmation required on a normal boot.
    if (err != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY)
    {
        return ESP_OK;
    }

    if (healthy)
    {
        ESP_LOGI(TAG, "Firmware verified successfully");

        return esp_ota_mark_app_valid_cancel_rollback();
    }

    ESP_LOGE(TAG, "Firmware health check failed");
    ESP_LOGE(TAG, "Rolling back firmware");

    return esp_ota_mark_app_invalid_rollback_and_reboot();
}

// Download and install firmware.
// Call this from a dedicated FreeRTOS task.
esp_err_t ota_update_install(const char *target_version)
{
    int version_parts[3];

    if (!parse_version(target_version, version_parts))
    {
        ESP_LOGE(TAG, "Invalid requested version");
        return ESP_ERR_INVALID_ARG;
    }

    const esp_app_desc_t *current =
        esp_app_get_description();

    int comparison = compare_versions(
        target_version, current->version);

    if (comparison == -2)
    {
        ESP_LOGE(TAG, "Current version format invalid");
        return ESP_ERR_INVALID_STATE;
    }

    if (comparison <= 0)
    {
        ESP_LOGW(TAG, "Firmware is not newer");
        return ESP_ERR_INVALID_VERSION;
    }

    // Do not overwrite an unconfirmed application.
    const esp_partition_t *running =
        esp_ota_get_running_partition();

    esp_ota_img_states_t state;

    if (running == NULL)
    {
        return ESP_FAIL;
    }

    if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY)
    {

        ESP_LOGE(TAG, "Current firmware unconfirmed");
        return ESP_ERR_INVALID_STATE;
    }

    // Check that another application slot exists.
    const esp_partition_t *next =
        esp_ota_get_next_update_partition(NULL);

    if (next == NULL || next == running)
    {
        ESP_LOGE(TAG, "No available OTA partition");
        return ESP_ERR_NOT_FOUND;
    }

    // URL is constructed only from validated version digits.
    char url[192];

    int url_length = snprintf(
        url, sizeof(url),
        "%s/%s.bin",
        FIRMWARE_BASE_URL,
        target_version);

    if (url_length < 0 ||
        url_length >= sizeof(url))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_config_t http_config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .keep_alive_enable = true,
        .disable_auto_redirect = true,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_https_ota_handle_t handle = NULL;

    ESP_LOGI(TAG, "Starting OTA: %s", target_version);

    esp_err_t err = esp_https_ota_begin(
        &ota_config, &handle);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "OTA connection failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    // Reject unexpected HTTP responses.
    if (esp_https_ota_get_status_code(handle) != 200)
    {
        ESP_LOGE(TAG, "Unexpected HTTP status");
        err = ESP_ERR_INVALID_RESPONSE;
        goto abort_update;
    }

    // Check firmware size before writing.
    int firmware_size =
        esp_https_ota_get_image_size(handle);

    // This implementation requires Content-Length.
    if (firmware_size <= 0 ||
        (size_t)firmware_size > next->size)
    {

        ESP_LOGE(TAG,
                 "Firmware size invalid or too large");

        err = ESP_ERR_INVALID_SIZE;
        goto abort_update;
    }

    ESP_LOGI(TAG, "Firmware size: %d", firmware_size);
    ESP_LOGI(TAG, "Partition size: %lu",
             (unsigned long)next->size);

    // Read firmware metadata before installation.
    esp_app_desc_t incoming = {0};

    err = esp_https_ota_get_img_desc(
        handle, &incoming);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Cannot read firmware metadata");
        goto abort_update;
    }

    // Ensure firmware belongs to the same project.
    if (strncmp(incoming.project_name,
                current->project_name,
                sizeof(incoming.project_name)) != 0)
    {

        ESP_LOGE(TAG, "Wrong firmware project");
        err = ESP_ERR_INVALID_VERSION;
        goto abort_update;
    }

    // Verify the downloaded firmware version.
    if (strcmp(incoming.version, target_version) != 0 ||
        compare_versions(incoming.version,
                         current->version) != 1)
    {

        ESP_LOGE(TAG, "Firmware version mismatch");
        err = ESP_ERR_INVALID_VERSION;
        goto abort_update;
    }

    // Conservative policy: keep security version equal
    // so the previous app can remain rollback-eligible.
    if (incoming.secure_version !=
        current->secure_version)
    {

        ESP_LOGE(TAG, "Security version mismatch");
        err = ESP_ERR_INVALID_VERSION;
        goto abort_update;
    }

#ifdef CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK
    if (!esp_efuse_check_secure_version(
            incoming.secure_version))
    {

        ESP_LOGE(TAG, "Firmware blocked by eFuse");
        err = ESP_ERR_INVALID_VERSION;
        goto abort_update;
    }
#endif

    // Download and write to inactive OTA partition.
    int64_t started = esp_timer_get_time();
    int last_progress = -1;

    do
    {
        err = esp_https_ota_perform(handle);

        int received =
            esp_https_ota_get_image_len_read(handle);

        if (received >= 0)
        {
            int progress =
                (int)((100LL * received) / firmware_size);

            if (progress > 100)
            {
                progress = 100;
            }

            if (progress / 10 > last_progress / 10)
            {
                ESP_LOGI(TAG, "Downloaded: %d%%", progress);
                last_progress = progress;
            }
        }

        int64_t elapsed = esp_timer_get_time() - started;

        if (elapsed > (int64_t)OTA_TIMEOUT_SECONDS *
                          1000000LL)
        {

            ESP_LOGE(TAG, "OTA timed out");
            err = ESP_ERR_TIMEOUT;
            break;
        }

    } while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Download failed: %s",
                 esp_err_to_name(err));
        goto abort_update;
    }

    // Confirm the complete image was received.
    if (!esp_https_ota_is_complete_data_received(handle))
    {
        ESP_LOGE(TAG, "Incomplete firmware download");
        err = ESP_FAIL;
        goto abort_update;
    }

    // Finish performs image validation and changes
    // the next boot partition only on success.
    err = esp_https_ota_finish(handle);

    // Do not abort after calling finish.
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Firmware validation failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Firmware installed successfully");
    ESP_LOGI(TAG, "Rebooting into new firmware");

    esp_restart();
    return ESP_OK;

abort_update:
    esp_https_ota_abort(handle);
    return err;
}
