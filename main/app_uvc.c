#include "app_uvc.h"

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

#define USB_HOST_PRIORITY   (15)

// Private function prototypes
static bool frame_callback(const uvc_host_frame_t *frame, void *user_ctx);
static void stream_callback(const uvc_host_stream_event_data_t *event, void *user_ctx);

// Private variables
static QueueHandle_t rx_frames_queue;
static bool dev_connected = false;
static const char *TAG = "app_uvc";
static uvc_frame_ready_cb_t g_user_frame_callback = NULL;
static void *g_user_callback_ctx = NULL;

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
        .frame_size = 0,                // == 0: Use dwMaxVideoFrameSize from format negotiation result (might be too large)
        .number_of_frame_buffers = 3,   // Use triple buffering scheme if SPIRAM is available
        .number_of_urbs = 3,            // 3x 10kB URBs is usually enough, even for higher resolutions
        .urb_size = 10 * 1024,          // Larger values result in less frequent interrupts at the cost of memory consumption
        .frame_heap_caps = MALLOC_CAP_SPIRAM, // Use SPIRAM for frame buffers if available
    },
};

static bool frame_callback(const uvc_host_frame_t *frame, void *user_ctx)
{
    assert(frame);
    assert(user_ctx);
    QueueHandle_t frame_q = *((QueueHandle_t *)user_ctx);   //user_ctx is provided library callback

    // Send the received frame to queue for further processing
    ESP_LOGD(TAG, "Frame callback! data len: %d", frame->data_len);
    BaseType_t result = xQueueSendToBack(frame_q, &frame, 0);
    if (pdPASS != result) {
        ESP_LOGW(TAG, "Queue full, losing frame"); // This should never happen
        return true; // We will not process this frame, return it immediately
    }
    return false; // We only passed the frame to Queue, so we must return false and call uvc_host_frame_return() later
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
    const uvc_host_stream_config_t *stream_config = (const uvc_host_stream_config_t *)arg;
    QueueHandle_t frame_q = *((QueueHandle_t *)(stream_config->user_ctx));  
    // rx_frames_queue, as defined in stream_config.user_ctx
    // creates local value frame_q to store a copy of rx_frames_queue
    
    while (true) {
        uvc_host_stream_hdl_t uvc_stream = NULL;
        ESP_LOGI(TAG, "Looking for UVC camera...");
        esp_err_t err = uvc_host_stream_open(stream_config, pdMS_TO_TICKS(5000), &uvc_stream);
        if (ESP_OK != err) {
            ESP_LOGI(TAG, "Failed to open device");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        
        dev_connected = true;
        ESP_LOGI(TAG, "Camera connected! Starting stream...");
        vTaskDelay(pdMS_TO_TICKS(100));
        
        uvc_host_stream_start(uvc_stream);
        
        // Main streaming loop
        while (dev_connected) {
            uvc_host_frame_t *frame;
            if (xQueueReceive(frame_q, &frame, pdMS_TO_TICKS(5000)) == pdPASS) {
                // ESP_LOGI(TAG, "New frame! Len: %d", frame->data_len);
                if (g_user_frame_callback != NULL) {
                    g_user_frame_callback(frame->data, frame->data_len, g_user_callback_ctx);
                }
                uvc_host_frame_return(uvc_stream, frame);
            }
        }
        if (dev_connected) {
            ESP_LOGI(TAG, "Stream stop");
            uvc_host_stream_stop(uvc_stream);
            vTaskDelay(pdMS_TO_TICKS(2000));
        } else {
            ESP_LOGI(TAG, "Device disconnected");
        }
    }
}

esp_err_t app_uvc_init(void)
{
    rx_frames_queue = xQueueCreate(3, sizeof(uvc_host_frame_t *));  //frame pointers
    assert(rx_frames_queue);
    
    ESP_LOGI(TAG, "Installing USB Host");
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    
    // Create a FreeRTOS task that will handle USB library events
    BaseType_t task_created = xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, NULL, USB_HOST_PRIORITY, NULL, tskNO_AFFINITY);
    assert(task_created == pdTRUE);
    
    ESP_LOGI(TAG, "Installing UVC driver");
    const uvc_host_driver_config_t uvc_driver_config = {
        .driver_task_stack_size = 4 * 1024,
        .driver_task_priority = USB_HOST_PRIORITY + 1,
        .xCoreID = tskNO_AFFINITY,
        .create_background_task = true,
    };
    ESP_ERROR_CHECK(uvc_host_install(&uvc_driver_config));
    
    task_created = xTaskCreatePinnedToCore(frame_handling_task, "frame_hdl", 4096, (void *)&stream_config, USB_HOST_PRIORITY - 2, NULL, tskNO_AFFINITY);
    assert(task_created == pdTRUE);

    return ESP_OK;
}

esp_err_t app_uvc_register_frame_callback(uvc_frame_ready_cb_t frame_cb, void *user_ctx)
{
    if (frame_cb == NULL) {
        ESP_LOGE(TAG, "Frame callback cannot be NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    g_user_frame_callback = frame_cb;
    g_user_callback_ctx = user_ctx;
    
    ESP_LOGI(TAG, "Frame callback registered");
    return ESP_OK;
}