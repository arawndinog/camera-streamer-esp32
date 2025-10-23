#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "app_wifi.h"

static const char *TAG = "app_wifi";

// WiFi configuration - modify these for your network
#define WIFI_SSID      "Owl Nest"
#define WIFI_PASS      "maumaus0cute"
#define WIFI_MAX_RETRY 5

// Server configuration
#define SERVER_PORT 8080

static httpd_handle_t server = NULL;
static QueueHandle_t frame_queue = NULL;
static bool wifi_connected = false;
static char ip_addr[16] = "0.0.0.0";

// MJPEG stream handler
static esp_err_t stream_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Stream client connected");
    
    // Send MJPEG multipart header
    const char *header = 
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "\r\n";
    httpd_send(req, header, strlen(header));

    while (true) {
        uvc_host_frame_t *frame = NULL;
        
        // Wait for a frame from the queue (with timeout)
        if (xQueueReceive(frame_queue, &frame, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (frame && frame->data && frame->data_len > 0) {
                char frame_header[128];
                int header_len = snprintf(frame_header, sizeof(frame_header),
                    "--frame\r\n"
                    "Content-Type: image/jpeg\r\n"
                    "Content-Length: %d\r\n"
                    "\r\n", frame->data_len);
                
                // Send frame header
                if (httpd_send(req, frame_header, header_len) < 0) {
                    ESP_LOGI(TAG, "Client disconnected during header");
                    break;
                }
                
                // Send frame data
                if (httpd_send(req, (const char *)frame->data, frame->data_len) < 0) {
                    ESP_LOGI(TAG, "Client disconnected during frame");
                    break;
                }
                
                // Send frame separator
                httpd_send(req, "\r\n", 2);
                
                // Return frame to UVC driver (important!)
                uvc_host_frame_return(NULL, frame); // Note: stream handle not available here
            }
        }
        
        // Check if client disconnected
        if (httpd_req_get_hdr_value_len(req, "Host") == 0) {
            ESP_LOGI(TAG, "Client disconnected");
            break;
        }
    }
    
    ESP_LOGI(TAG, "Stream client disconnected");
    return ESP_OK;
}

// HTTP URI handlers
static const httpd_uri_t stream_uri = {
    .uri       = "/stream",
    .method    = HTTP_GET,
    .handler   = stream_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t index_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = stream_handler, // Redirect root to stream for simplicity
    .user_ctx  = NULL
};

// Start web server
static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = SERVER_PORT;
    config.ctrl_port = SERVER_PORT;
    config.max_open_sockets = 3;

    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_register_uri_handler(server, &stream_uri);
        httpd_register_uri_handler(server, &index_uri);
        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}

// Stop web server
static void stop_webserver(httpd_handle_t server)
{
    if (server) {
        httpd_stop(server);
        ESP_LOGI(TAG, "Server stopped");
    }
}

// WiFi event handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    static int retry_count = 0;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "WiFi STA starting...");
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected = false;
        stop_webserver(server);
        server = NULL;
        
        if (retry_count < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            retry_count++;
            ESP_LOGI(TAG, "Retry to connect to the AP (%d/%d)", retry_count, WIFI_MAX_RETRY);
        } else {
            ESP_LOGI(TAG, "Failed to connect after %d retries", WIFI_MAX_RETRY);
        }
    } 
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        snprintf(ip_addr, sizeof(ip_addr), IPSTR, IP2STR(&event->ip_info.ip));
        
        wifi_connected = true;
        retry_count = 0;
        
        ESP_LOGI(TAG, "Got IP: %s", ip_addr);
        
        // Start web server when we get an IP
        server = start_webserver();
    }
}

// Initialize WiFi
QueueHandle_t app_wifi_init(void)
{
    ESP_LOGI(TAG, "Initializing WiFi");
    
    // Create frame queue
    frame_queue = xQueueCreate(3, sizeof(uvc_host_frame_t *));
    if (!frame_queue) {
        ESP_LOGE(TAG, "Failed to create frame queue");
        return NULL;
    }

    // Initialize TCP/IP stack and event loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Create default STA interface
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    // Initialize WiFi with default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    // Configure WiFi as STA
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    
    ESP_LOGI(TAG, "WiFi initialization complete");
    return frame_queue;
}

// Start WiFi
void app_wifi_start(void)
{
    ESP_LOGI(TAG, "Starting WiFi");
    ESP_ERROR_CHECK(esp_wifi_start());
}

// Check if WiFi is connected
bool app_wifi_is_connected(void)
{
    return wifi_connected;
}

// Get IP address
const char* app_wifi_get_ip(void)
{
    return ip_addr;
}