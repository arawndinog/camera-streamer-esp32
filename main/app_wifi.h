#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize WiFi and connect to AP
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_wifi_init(void);

/**
 * Check if WiFi is connected and has IP
 * @return true if connected with IP, false otherwise
 */
bool app_wifi_is_connected(void);

#ifdef __cplusplus
}
#endif