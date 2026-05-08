#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief BLE Credentials Service UUIDs (16-bit UUIDs for simplicity)
 * 
 * Service: Credentials Service (0x180A is Device Info, but we'll use a custom UUID)
 * We'll use: 0xA500 (custom)
 * 
 * Characteristics:
 *   - SSID: 0xA501
 *   - Password: 0xA502
 * 
 * Full UUID format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
 * Base UUID: 12345678-1234-5678-1234-56789abcdef0
 * For simplicity, using 128-bit UUIDs:
 * Service:    12345678-1234-5678-1234-56789abcdef0 (or custom)
 * SSID char:  12345678-1234-5678-1234-56789abcdef1
 * Password:   12345678-1234-5678-1234-56789abcdef2
 */

/* Service and Characteristic UUIDs (128-bit) */
#define CREDS_SERVICE_UUID \
    0x12, 0x34, 0x56, 0x78, 0x12, 0x34, 0x56, 0x78, \
    0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0

#define CREDS_JSON_CHAR_UUID \
    0x12, 0x34, 0x56, 0x78, 0x12, 0x34, 0x56, 0x78, \
    0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf1

/* Maximum credential lengths and JSON buffer */
#define MAX_SSID_LEN 32
#define MAX_PASSWORD_LEN 64
#define MAX_JSON_LEN 256

/**
 * @brief Initialize BLE module
 * 
 * Sets up NimBLE stack and initializes the Credentials Service
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t module_ble_init(void);

/**
 * @brief Start BLE advertising
 * 
 * Begins advertising the Credentials Service, making the device discoverable
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t module_ble_start_advertising(void);

/**
 * @brief Stop BLE advertising
 * 
 * Stops advertising but keeps the BLE stack running
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t module_ble_stop_advertising(void);

/**
 * @brief Check if credentials were received via BLE (JSON parsed successfully)
 * 
 * @return true if JSON credentials have been received and parsed, false otherwise
 */
bool module_ble_credentials_received(void);

/**
 * @brief Get the received SSID credential
 * 
 * @param ssid Output buffer for SSID (must be at least MAX_SSID_LEN)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t module_ble_get_ssid(char *ssid);

/**
 * @brief Get the received password credential
 * 
 * @param password Output buffer for password (must be at least MAX_PASSWORD_LEN)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t module_ble_get_password(char *password);

/**
 * @brief Reset received credentials (clear the BLE buffers)
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t module_ble_reset_credentials(void);
