#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Frame ready callback function type
 * 
 * @param data Pointer to frame data (MJPEG)
 * @param len Length of frame data in bytes
 * @param user_ctx User context passed during registration
 */
typedef void (*uvc_frame_ready_cb_t)(const uint8_t *data, size_t len, void *user_ctx);

/**
 * @brief Initialize UVC module
 * 
 * @return ESP_OK on success
 */
esp_err_t app_uvc_init(void);

/**
 * @brief Register a callback for frame processing
 * 
 * @param frame_cb Callback function to call when frame is ready
 * @param user_ctx User context to pass to callback
 * @return ESP_OK on success
 */
esp_err_t app_uvc_register_frame_callback(uvc_frame_ready_cb_t frame_cb, void *user_ctx);

#ifdef __cplusplus
}
#endif
