#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the streaming module
 * 
 * This will register a callback with the UVC module to process frames
 * 
 * @return ESP_OK on success, ESP_FAIL otherwise
 */
esp_err_t app_http_init(void);

#ifdef __cplusplus
}
#endif