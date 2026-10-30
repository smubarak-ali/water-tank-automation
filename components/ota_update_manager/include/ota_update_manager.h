#pragma once

#include <stdbool.h>
#include "esp_err.h"

// Downloads and installs the requested version.
// On success, the ESP32 restarts.
esp_err_t ota_update_install(const char *target_version);

// Called after boot-time diagnostics.
// true: accept new firmware.
// false: roll back to previous firmware.
esp_err_t ota_update_confirm(bool healthy);
