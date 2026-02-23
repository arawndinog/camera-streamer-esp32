#include "app_http.h"
#include "app_uvc.h"
#include "app_wifi.h"

#include <string.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include <sys/socket.h>
#include <netinet/tcp.h>

static const char *TAG = "app_http";

// ============================================================================
// OPTIMIZED: Ping-pong frame buffers with minimal locking
// ============================================================================
typedef struct {
    uint8_t *buffer;
    size_t len;
    bool ready;
} frame_slot_t;

static frame_slot_t g_frame_buffer[2] = {0};
static uint8_t g_write_index = 0;
static uint8_t g_read_index = 0;
static SemaphoreHandle_t g_frame_mutex = NULL;  // Minimal mutex, held very briefly
static const size_t MAX_FRAME_SIZE = 512 * 1024; // 512KB buffer

// Frame notification using event group
static EventGroupHandle_t g_frame_events = NULL;
#define FRAME_READY_BIT BIT0

// Session management
static uint32_t g_active_session = 0;
static SemaphoreHandle_t g_session_mutex = NULL;

// Stream task management
typedef struct {
    int socket_fd;
    uint32_t session_id;
    bool active;
} stream_context_t;

static stream_context_t g_stream_ctx = {0};
static TaskHandle_t g_stream_task_handle = NULL;
static SemaphoreHandle_t g_stream_start_mutex = NULL;

// Statistics
static uint32_t g_frames_received = 0;
static uint32_t g_frames_sent = 0;
static uint32_t g_frames_dropped = 0;

// Minimal HTML page
static const char *index_html = 
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<title>ESP32-P4 Camera</title>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<style>"
    "body{margin:0;padding:20px;font-family:Arial;background:#f0f0f0}"
    ".container{max-width:1200px;margin:0 auto}"
    ".stats{background:white;padding:15px;border-radius:8px;margin-bottom:20px}"
    ".stats h2{margin-top:0}"
    ".stats p{margin:5px 0}"
    ".video-container{background:black;border-radius:8px;overflow:hidden}"
    "img{width:100%;height:auto;display:block}"
    "</style>"
    "<script>"
    "setInterval(async()=>{"
    "try{"
    "const r=await fetch('/stats');"
    "const d=await r.json();"
    "document.getElementById('rx').textContent=d.frames_received;"
    "document.getElementById('tx').textContent=d.frames_sent;"
    "document.getElementById('drop').textContent=d.frames_dropped;"
    "const fps=d.frames_sent-parseInt(document.getElementById('tx').dataset.prev||0);"
    "document.getElementById('tx').dataset.prev=d.frames_sent;"
    "document.getElementById('fps').textContent=fps;"
    "}catch(e){}"
    "},1000);"
    "</script>"
    "</head>"
    "<body>"
    "<div class='container'>"
    "<div class='stats'>"
    "<h2>ESP32-P4 Camera Stream</h2>"
    "<p>Frames Received: <strong id='rx'>0</strong></p>"
    "<p>Frames Sent: <strong id='tx' data-prev='0'>0</strong></p>"
    "<p>Frames Dropped: <strong id='drop'>0</strong></p>"
    "<p>FPS: <strong id='fps'>0</strong></p>"
    "</div>"
    "<div class='video-container'>"
    "<img src='/stream' alt='Camera Stream'/>"
    "</div>"
    "</div>"
    "</body>"
    "</html>";

// Generate unique session token
static uint32_t generate_session_token(void)
{
    return esp_random();
}

// ============================================================================
// FIXED: Frame callback with proper mutex protection (but held briefly)
// ============================================================================
static void frame_received_callback(const uint8_t *data, size_t len, void *user_ctx)
{
    if (len > MAX_FRAME_SIZE || len == 0) {
        g_frames_dropped++;
        return;
    }

    g_frames_received++;
    
    // Try to acquire mutex with minimal timeout (non-blocking)
    if (xSemaphoreTake(g_frame_mutex, 0) != pdTRUE) {
        g_frames_dropped++;
        return;
    }
    
    // Write to the current write buffer
    uint8_t write_slot = g_write_index;
    
    // Copy frame data
    memcpy(g_frame_buffer[write_slot].buffer, data, len);
    g_frame_buffer[write_slot].len = len;
    g_frame_buffer[write_slot].ready = true;
    
    // Swap to make this buffer available for reading
    g_read_index = write_slot;
    g_write_index = 1 - write_slot;
    
    xSemaphoreGive(g_frame_mutex);
    
    // Signal frame ready
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xEventGroupSetBitsFromISR(g_frame_events, FRAME_READY_BIT, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

// ============================================================================
// OPTIMIZED: Streaming task - direct send from frame buffer
// ============================================================================
static void stream_task(void *arg)
{
    ESP_LOGI(TAG, "Stream task started on core %d", xPortGetCoreID());
    
    int socket_fd = g_stream_ctx.socket_fd;
    uint32_t my_session = g_stream_ctx.session_id;
    
    // Allocate small header buffer only
    char *header_buf = malloc(512);
    if (header_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate header buffer");
        g_stream_ctx.active = false;
        g_stream_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    // Optimize socket for streaming
    int flag = 1;
    setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    
    int sendbuf = 256 * 1024;
    setsockopt(socket_fd, SOL_SOCKET, SO_SNDBUF, &sendbuf, sizeof(sendbuf));
    
    // Set socket timeout
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    
    // Send HTTP headers
    const char *headers = 
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        "Pragma: no-cache\r\n"
        "X-Framerate: 30\r\n"
        "\r\n";
    
    if (send(socket_fd, headers, strlen(headers), 0) < 0) {
        ESP_LOGE(TAG, "Failed to send headers");
        free(header_buf);
        g_stream_ctx.active = false;
        g_stream_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "Stream headers sent, starting frame delivery");
    
    uint32_t local_frames_sent = 0;
    uint32_t consecutive_waits = 0;
    
    // Local buffer for reading frame safely
    uint8_t *local_frame_buf = heap_caps_malloc(MAX_FRAME_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (local_frame_buf == NULL) {
        local_frame_buf = malloc(MAX_FRAME_SIZE);
        if (local_frame_buf == NULL) {
            ESP_LOGE(TAG, "Failed to allocate local frame buffer");
            free(header_buf);
            g_stream_ctx.active = false;
            g_stream_task_handle = NULL;
            vTaskDelete(NULL);
            return;
        }
    }
    
    while (g_stream_ctx.active) {
        // Check if this session is still active
        bool is_active = false;
        if (xSemaphoreTake(g_session_mutex, 0) == pdTRUE) {
            is_active = (g_active_session == my_session);
            xSemaphoreGive(g_session_mutex);
        }
        
        if (!is_active) {
            ESP_LOGI(TAG, "Session 0x%08lX terminated by newer viewer", my_session);
            break;
        }
        
        // Wait for new frame event
        EventBits_t bits = xEventGroupWaitBits(
            g_frame_events,
            FRAME_READY_BIT,
            pdTRUE,  // Clear bit on exit
            pdFALSE,
            pdMS_TO_TICKS(1000) // 1 second timeout
        );
        
        if (!(bits & FRAME_READY_BIT)) {
            consecutive_waits++;
            if (consecutive_waits >= 3) {
                ESP_LOGW(TAG, "No frames received for %lu seconds", consecutive_waits);
            }
            continue;
        }
        
        consecutive_waits = 0;
        
        // Safely copy frame data while holding mutex briefly
        size_t frame_len = 0;
        if (xSemaphoreTake(g_frame_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            uint8_t read_slot = g_read_index;
            
            if (g_frame_buffer[read_slot].ready && g_frame_buffer[read_slot].len > 0) {
                frame_len = g_frame_buffer[read_slot].len;
                memcpy(local_frame_buf, g_frame_buffer[read_slot].buffer, frame_len);
                g_frame_buffer[read_slot].ready = false;
            }
            
            xSemaphoreGive(g_frame_mutex);
        } else {
            ESP_LOGW(TAG, "Failed to acquire frame mutex");
            continue;
        }
        
        if (frame_len == 0 || frame_len > MAX_FRAME_SIZE) {
            continue;
        }
        
        // Build MJPEG frame header
        int hlen = snprintf(header_buf, 512,
            "--frame\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %zu\r\n\r\n",
            frame_len);
        
        // Send header
        if (send(socket_fd, header_buf, hlen, 0) < 0) {
            ESP_LOGI(TAG, "Client disconnected (header send failed)");
            break;
        }
        
        // Send frame data from local copy
        ssize_t sent = send(socket_fd, local_frame_buf, frame_len, 0);
        
        if (sent != (ssize_t)frame_len) {
            if (sent < 0) {
                ESP_LOGI(TAG, "Client disconnected (data send error: %d)", errno);
            } else {
                ESP_LOGI(TAG, "Client disconnected (partial send: %zd/%zu bytes)", sent, frame_len);
            }
            break;
        }
        
        // Send frame trailer
        if (send(socket_fd, "\r\n", 2, 0) < 0) {
            ESP_LOGI(TAG, "Client disconnected (trailer send failed)");
            break;
        }
        
        local_frames_sent++;
        g_frames_sent++;
        
        // Log stats every 100 frames
        if (local_frames_sent % 100 == 0) {
            ESP_LOGI(TAG, "Stats - Received: %lu, Sent: %lu, Dropped: %lu",
                     g_frames_received, g_frames_sent, g_frames_dropped);
        }
        
        // Small yield
        taskYIELD();
    }
    
    // Clean up
    free(local_frame_buf);
    free(header_buf);
    g_stream_ctx.active = false;
    
    // Clean up session if this was the active one
    if (xSemaphoreTake(g_session_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (g_active_session == my_session) {
            g_active_session = 0;
            ESP_LOGI(TAG, "Active session cleared (session: 0x%08lX)", my_session);
        }
        xSemaphoreGive(g_session_mutex);
    }
    
    ESP_LOGI(TAG, "Stream task terminated (sent %lu frames)", local_frames_sent);
    g_stream_task_handle = NULL;
    vTaskDelete(NULL);
}

// HTTP handler for root page
static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    return httpd_resp_send(req, index_html, HTTPD_RESP_USE_STRLEN);
}

// HTTP handler for statistics (JSON)
static esp_err_t stats_handler(httpd_req_t *req)
{
    char json[256];
    snprintf(json, sizeof(json),
        "{\"frames_received\":%lu,\"frames_sent\":%lu,\"frames_dropped\":%lu,\"active_session\":\"0x%08lX\"}",
        g_frames_received, g_frames_sent, g_frames_dropped, g_active_session);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, json);
}

// HTTP handler for MJPEG stream (lightweight - just launches task)
static esp_err_t stream_handler(httpd_req_t *req)
{
    uint32_t my_session = 0;
    
    // Wait for any previous stream task to finish
    if (xSemaphoreTake(g_stream_start_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire stream start mutex");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Stream busy");
        return ESP_FAIL;
    }
    
    // Stop existing stream if any
    if (g_stream_task_handle != NULL) {
        ESP_LOGI(TAG, "Stopping existing stream task");
        g_stream_ctx.active = false;
        
        // Wait for task to terminate (max 2 seconds)
        int wait_count = 0;
        while (g_stream_task_handle != NULL && wait_count < 200) {
            vTaskDelay(pdMS_TO_TICKS(10));
            wait_count++;
        }
        
        if (g_stream_task_handle != NULL) {
            ESP_LOGW(TAG, "Force deleting stuck stream task");
            vTaskDelete(g_stream_task_handle);
            g_stream_task_handle = NULL;
        }
    }
    
    // Create new session
    if (xSemaphoreTake(g_session_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        my_session = generate_session_token();
        if (g_active_session != 0) {
            ESP_LOGI(TAG, "New viewer connected, disconnecting previous viewer (session: 0x%08lX)", g_active_session);
        }
        g_active_session = my_session;
        xSemaphoreGive(g_session_mutex);
        ESP_LOGI(TAG, "Stream started for new client (session: 0x%08lX)", my_session);
    } else {
        ESP_LOGE(TAG, "Failed to acquire session mutex");
        xSemaphoreGive(g_stream_start_mutex);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Session manager busy");
        return ESP_FAIL;
    }
    
    // Get socket file descriptor
    int socket_fd = httpd_req_to_sockfd(req);
    
    // Set up stream context
    g_stream_ctx.socket_fd = socket_fd;
    g_stream_ctx.session_id = my_session;
    g_stream_ctx.active = true;
    
    // Create streaming task with larger stack
    BaseType_t ret = xTaskCreate(
        stream_task,                    // Task function
        "stream_task",                  // Task name
        16384,                          // Stack size (16KB)
        NULL,                           // Parameters
        configMAX_PRIORITIES - 2,       // High priority
        &g_stream_task_handle          // Task handle
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create stream task");
        g_stream_ctx.active = false;
        xSemaphoreGive(g_stream_start_mutex);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to start stream");
        return ESP_FAIL;
    }
    
    xSemaphoreGive(g_stream_start_mutex);
    
    ESP_LOGI(TAG, "Stream task created");
    
    // Keep HTTP connection open - the stream task owns the socket now
    // Wait for stream task to complete
    while (g_stream_task_handle != NULL) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    return ESP_OK;
}

esp_err_t app_http_init(void)
{
    ESP_LOGI(TAG, "Initializing HTTP streaming server");
    
    // Allocate ping-pong frame buffers (prefer SPIRAM)
    for (int i = 0; i < 2; i++) {
        g_frame_buffer[i].buffer = heap_caps_malloc(MAX_FRAME_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (g_frame_buffer[i].buffer == NULL) {
            g_frame_buffer[i].buffer = malloc(MAX_FRAME_SIZE);
            if (g_frame_buffer[i].buffer == NULL) {
                ESP_LOGE(TAG, "Failed to allocate frame buffer %d", i);
                for (int j = 0; j < i; j++) {
                    free(g_frame_buffer[j].buffer);
                }
                return ESP_ERR_NO_MEM;
            }
            ESP_LOGW(TAG, "Frame buffer %d allocated in internal RAM (%zu KB)", i, MAX_FRAME_SIZE / 1024);
        } else {
            ESP_LOGI(TAG, "Frame buffer %d allocated in SPIRAM (%zu KB)", i, MAX_FRAME_SIZE / 1024);
        }
        
        g_frame_buffer[i].len = 0;
        g_frame_buffer[i].ready = false;
    }
    
    // Create mutexes
    g_frame_mutex = xSemaphoreCreateMutex();
    if (g_frame_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create frame mutex");
        free(g_frame_buffer[0].buffer);
        free(g_frame_buffer[1].buffer);
        return ESP_ERR_NO_MEM;
    }
    
    g_session_mutex = xSemaphoreCreateMutex();
    if (g_session_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create session mutex");
        vSemaphoreDelete(g_frame_mutex);
        free(g_frame_buffer[0].buffer);
        free(g_frame_buffer[1].buffer);
        return ESP_ERR_NO_MEM;
    }
    
    g_stream_start_mutex = xSemaphoreCreateMutex();
    if (g_stream_start_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create stream start mutex");
        vSemaphoreDelete(g_session_mutex);
        vSemaphoreDelete(g_frame_mutex);
        free(g_frame_buffer[0].buffer);
        free(g_frame_buffer[1].buffer);
        return ESP_ERR_NO_MEM;
    }
    
    // Create event group for frame notifications
    g_frame_events = xEventGroupCreate();
    if (g_frame_events == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        vSemaphoreDelete(g_stream_start_mutex);
        vSemaphoreDelete(g_session_mutex);
        vSemaphoreDelete(g_frame_mutex);
        free(g_frame_buffer[0].buffer);
        free(g_frame_buffer[1].buffer);
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "Registering UVC frame callback");
    
    // Register frame callback with UVC module
    esp_err_t ret = app_uvc_register_frame_callback(frame_received_callback, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register frame callback: %s", esp_err_to_name(ret));
        vEventGroupDelete(g_frame_events);
        vSemaphoreDelete(g_stream_start_mutex);
        vSemaphoreDelete(g_session_mutex);
        vSemaphoreDelete(g_frame_mutex);
        free(g_frame_buffer[0].buffer);
        free(g_frame_buffer[1].buffer);
        return ret;
    }
    
    ESP_LOGI(TAG, "Starting HTTP server");
    
    // Start HTTP server
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.max_uri_handlers = 8;
    config.max_open_sockets = 5;
    config.lru_purge_enable = true;
    config.stack_size = 6144;
    config.send_wait_timeout = 5;
    config.recv_wait_timeout = 5;
    
    httpd_handle_t server = NULL;
    ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        vEventGroupDelete(g_frame_events);
        vSemaphoreDelete(g_stream_start_mutex);
        vSemaphoreDelete(g_session_mutex);
        vSemaphoreDelete(g_frame_mutex);
        free(g_frame_buffer[0].buffer);
        free(g_frame_buffer[1].buffer);
        return ret;
    }
    
    // Register URI handlers
    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &index_uri);
    
    httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &stream_uri);
    
    httpd_uri_t stats_uri = {
        .uri = "/stats",
        .method = HTTP_GET,
        .handler = stats_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &stats_uri);
    
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "HTTP server started successfully");
    ESP_LOGI(TAG, "  Web UI:    http://<device-ip>/");
    ESP_LOGI(TAG, "  Stream:    http://<device-ip>/stream");
    ESP_LOGI(TAG, "  Stats:     http://<device-ip>/stats");
    ESP_LOGI(TAG, "===========================================");
    
    return ESP_OK;
}