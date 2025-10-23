/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>

#include "sdkconfig.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "usb/usb_host.h"
#include "usb/uvc_host.h"

#include "app_wifi.h"

#define EXAMPLE_USB_HOST_PRIORITY   (15)

// Private function prototypes
static bool frame_callback(const uvc_host_frame_t *frame, void *user_ctx);
static void stream_callback(const uvc_host_stream_event_data_t *event, void *user_ctx);

// Private variables
static QueueHandle_t rx_frames_queue;
static bool dev_connected = false;
static const char *TAG = "app_uvc";
static uvc_host_stream_hdl_t uvc_stream = NULL;
static QueueHandle_t wifi_frame_queue = NULL;

static const uvc_host_stream_config_t stream_config = {
    .event_cb = stream_callback,
    .frame_cb = frame_callback,
    .user_ctx = &rx_frames_queue,
    .usb = {
        .vid = UVC_HOST_ANY_VID, // Set to 0 to match any VID
        .pid = UVC_HOST_ANY_PID, // Set to 0 to match any PID
        .uvc_stream_index = 0,   /* Index of UVC function you want to use. Set to 0 to use first available UVC function.
                                    Setting this to >= 1 will only work if the camera has multiple UVC functions (eg. multiple image sensors in one USB device) */
    },
    .vs_format = {
        .h_res = 1920,
        .v_res = 1080,
        .fps = 20,
        .format = UVC_VS_FORMAT_MJPEG,
    },
    .advanced = {
        .frame_size = 512 * 1024,      // Pre-allocate ~1MB buffers for 1080p MJPEG
        .number_of_frame_buffers = 6,   // Quad buffering for smooth 30fps
        .number_of_urbs = 8,            // More URBs for high data rate
        .urb_size = 16 * 1024,          // 64KB URBs for efficient transfers
        .frame_heap_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
    },
};

static bool frame_callback(const uvc_host_frame_t *frame, void *user_ctx)
{
    assert(frame);
    QueueHandle_t frame_q = *((QueueHandle_t *)user_ctx);
    BaseType_t result = xQueueSendToBack(frame_q, &frame, 0);
    if (wifi_frame_queue) {
        xQueueSendToBack(wifi_frame_queue, &frame, 0);
    }
    if (pdPASS != result) {
        ESP_LOGW(TAG, "Local queue full, losing frame");
        return true;
    }
    return false;
}

static void stream_callback(const uvc_host_stream_event_data_t *event, void *user_ctx)
{
    switch (event->type) {
    case UVC_HOST_TRANSFER_ERROR:
        ESP_LOGE(TAG, "USB error has occurred, err_no = %i", event->transfer_error.error);
        break;
    case UVC_HOST_DEVICE_DISCONNECTED:
        ESP_LOGI(TAG, "Device suddenly disconnected");
        dev_connected = false;
        ESP_ERROR_CHECK(uvc_host_stream_close(event->device_disconnected.stream_hdl));
        break;
    case UVC_HOST_FRAME_BUFFER_OVERFLOW:
        // The Frame was discarded because it exceeded the available frame buffer size.
        // To resolve this, increase the `frame_size` parameter in `uvc_host_stream_config_t.advanced` to allocate a larger buffer.
        ESP_LOGW(TAG, "Frame buffer overflow");
        break;
    case UVC_HOST_FRAME_BUFFER_UNDERFLOW:
        // The Frame was discarded because no available buffer was free for storage.
        // To resolve this, either optimize your processing speed or increase the `number_of_frame_buffers` parameter in
        // `uvc_host_stream_config_t.advanced` to allocate additional buffers.
        ESP_LOGW(TAG, "Frame buffer underflow");
        break;
    default:
        abort();
        break;
    }
}

static void usb_lib_task(void *arg)
{
    while (1) {
        // Start handling system events
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "USB: All devices freed");
            // Continue handling USB events to allow device reconnection
        }
    }
}

static void frame_handling_task(void *arg)
{
    const uvc_host_stream_config_t *stream_cfg = (const uvc_host_stream_config_t *)arg;
    
    while (true) {
        ESP_LOGI(TAG, "Looking for UVC camera...");
        esp_err_t err = uvc_host_stream_open(stream_cfg, pdMS_TO_TICKS(5000), &uvc_stream);
        if (ESP_OK != err) {
            ESP_LOGI(TAG, "No camera found, retrying...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        
        dev_connected = true;
        ESP_LOGI(TAG, "Camera connected! Starting stream...");
        
        uvc_host_stream_start(uvc_stream);
        
        // Main streaming loop
        while (dev_connected) {
            uvc_host_frame_t *frame;
            if (xQueueReceive(rx_frames_queue, &frame, pdMS_TO_TICKS(1000)) == pdPASS) {
                // Optional: Add USB-specific frame processing here
                ESP_LOGD(TAG, "Frame: %dx%d, %d bytes", frame->data_len);
                
                // Return frame to UVC driver
                uvc_host_frame_return(uvc_stream, frame);
            }
        }
        
        ESP_LOGI(TAG, "Camera disconnected, cleaning up...");
        uvc_host_stream_close(uvc_stream);
        uvc_stream = NULL;
    }
}

/**
 * @brief Main application
 */

void app_main(void)
{
    ESP_LOGI(TAG, "Starting ESP32 Camera System");
    
    // Initialize local frame queue
    rx_frames_queue = xQueueCreate(3, sizeof(uvc_host_frame_t *));
    assert(rx_frames_queue);
    
    // Initialize WiFi streamer
    wifi_frame_queue = app_wifi_init();
    
    if (wifi_frame_queue) {
        app_wifi_start();
        ESP_LOGI(TAG, "WiFi streaming enabled - waiting for connection...");
    } else {
        ESP_LOGW(TAG, "WiFi initialization failed");
    }
    
    // Initialize USB subsystem
    ESP_LOGI(TAG, "Initializing USB subsystem");
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    
    // Create USB tasks
    BaseType_t task_created = xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 
                                                     4096, NULL, 
                                                     EXAMPLE_USB_HOST_PRIORITY, NULL, tskNO_AFFINITY);
    assert(task_created == pdTRUE);
    
    // Install UVC driver
    ESP_LOGI(TAG, "Installing UVC driver");
    const uvc_host_driver_config_t uvc_driver_config = {
        .driver_task_stack_size = 4 * 1024,
        .driver_task_priority = EXAMPLE_USB_HOST_PRIORITY + 1,
        .xCoreID = tskNO_AFFINITY,
        .create_background_task = true,
    };
    ESP_ERROR_CHECK(uvc_host_install(&uvc_driver_config));
    
    // Start frame handling task
    task_created = xTaskCreatePinnedToCore(frame_handling_task, "frame_hdl", 
                                          8192, (void *)&stream_config, 
                                          EXAMPLE_USB_HOST_PRIORITY - 2, NULL, tskNO_AFFINITY);
    assert(task_created == pdTRUE);
    
    ESP_LOGI(TAG, "Camera system started successfully!");

    while (1) {
        if (app_wifi_is_connected()) {
            ESP_LOGI(TAG, "WiFi Connected! Stream: http://%s:8080/stream", app_wifi_get_ip());
        } else {
            ESP_LOGI(TAG, "WiFi: Waiting for connection...");
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}