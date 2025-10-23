/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/**
 * @brief Initialize WiFi subsystem
 * @return Queue handle for frame data or NULL on failure
 */
QueueHandle_t app_wifi_init(void);

/**
 * @brief Start WiFi connection
 */
void app_wifi_start(void);

/**
 * @brief Check if WiFi is connected
 * @return true if connected, false otherwise
 */
bool app_wifi_is_connected(void);

/**
 * @brief Get current IP address
 * @return IP address string
 */
const char* app_wifi_get_ip(void);