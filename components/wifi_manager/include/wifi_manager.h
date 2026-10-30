
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Start Wi-Fi connection and automatic reconnection.
esp_err_t wifi_manager_start(const char *ssid, const char *password);

// Wait until an IP address is received, or timeout.
esp_err_t wifi_manager_wait_for_connection(uint32_t timeout_ms);

// Check the current connection status.
bool wifi_manager_is_connected(void);

// Stop Wi-Fi and release component resources.
void wifi_manager_stop(void);
