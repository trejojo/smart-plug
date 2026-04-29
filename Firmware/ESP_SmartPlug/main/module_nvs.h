#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief NVS storage for WiFi credentials
 * 
 * Uses ESP32 NVS (Non-Volatile Storage) to persist credentials across reboots
 * Namespace: "smartplug_wifi"
 * Keys:
 *   - "ssid": WiFi network name
 *   - "password": WiFi password
 */

#define NVS_NAMESPACE "smartplug_wifi"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASSWORD "password"
#define MAX_NVS_SSID_LEN 32
#define MAX_NVS_PASSWORD_LEN 64

/**
 * @brief Initialize NVS module
 * 
 * Initializes the NVS flash and opens the storage namespace
 * Must be called once at startup
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t module_nvs_init(void);

/**
 * @brief Save WiFi credentials to NVS
 * 
 * @param ssid Network SSID (max 32 characters)
 * @param password Network password (max 64 characters)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t module_nvs_save_wifi_credentials(const char *ssid, const char *password);

/**
 * @brief Load WiFi credentials from NVS
 * 
 * @param ssid Output buffer for SSID (must be at least 33 bytes: 32 chars + null terminator)
 * @param password Output buffer for password (must be at least 65 bytes: 64 chars + null terminator)
 * @return ESP_OK on success, ESP_ERR_NVS_NOT_FOUND if credentials not found
 */
esp_err_t module_nvs_load_wifi_credentials(char *ssid, char *password);

/**
 * @brief Check if WiFi credentials exist in NVS
 * 
 * @return true if both SSID and password are stored, false otherwise
 */
bool module_nvs_credentials_exist(void);

/**
 * @brief Erase WiFi credentials from NVS
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t module_nvs_erase_wifi_credentials(void);

/**
 * @brief Get SSID from NVS
 * 
 * @param ssid Output buffer for SSID (must be at least 33 bytes)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t module_nvs_get_ssid(char *ssid);

/**
 * @brief Get password from NVS
 * 
 * @param password Output buffer for password (must be at least 65 bytes)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t module_nvs_get_password(char *password);
