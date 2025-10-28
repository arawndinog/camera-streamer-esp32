#include "app_http.h"
#include "app_uvc.h"
#include "esp_log.h"

static const char *TAG = "app_http";

/**
 * @brief Frame processing callback
 * Called by UVC module when a new frame is ready
 */
static void process_frame(const uint8_t *data, size_t len, void *user_ctx)
{
    // ESP_LOGI(TAG, "Frame received - Length: %d bytes", len);
    
    // TODO: Add your streaming logic here
    // - data points to MJPEG frame
    // - len is the size in bytes
    // - Process as needed (send over WiFi, save to SD, etc.)
}

esp_err_t app_http_init(void)
{
    ESP_LOGI(TAG, "Initializing streaming module");
    
    // Register our frame processing callback with UVC module
    esp_err_t ret = app_uvc_register_frame_callback(process_frame, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register frame callback");
        return ret;
    }
    
    ESP_LOGI(TAG, "Streaming module initialized successfully");
    return ESP_OK;
}