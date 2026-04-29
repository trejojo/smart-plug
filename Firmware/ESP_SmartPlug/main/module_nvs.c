#include "module_nvs.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <stdbool.h>
#include <string.h>
#include "esp_err.h"


static const char *TAG = "module_nvs";

/**
 * @brief Initialize NVS module
 */
esp_err_t module_nvs_init(void)
{
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_LOGW(TAG, "NVS partition truncated, erasing...");
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "NVS initialization failed: %s", esp_err_to_name(ret));
		return ret;
	}
	
	ESP_LOGI(TAG, "NVS initialized");
	return ESP_OK;
}

/**
 * @brief Save WiFi credentials to NVS
 */
esp_err_t module_nvs_save_wifi_credentials(const char *ssid, const char *password)
{
	if (ssid == NULL || password == NULL) {
		ESP_LOGE(TAG, "Invalid credentials (NULL pointers)");
		return ESP_ERR_INVALID_ARG;
	}
	
	if (strlen(ssid) > MAX_NVS_SSID_LEN) {
		ESP_LOGE(TAG, "SSID too long (max %d)", MAX_NVS_SSID_LEN);
		return ESP_ERR_INVALID_ARG;
	}
	
	if (strlen(password) > MAX_NVS_PASSWORD_LEN) {
		ESP_LOGE(TAG, "Password too long (max %d)", MAX_NVS_PASSWORD_LEN);
		return ESP_ERR_INVALID_ARG;
	}
	
	nvs_handle_t handle;
	esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
		return ret;
	}
	
	ret = nvs_set_str(handle, NVS_KEY_SSID, ssid);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to write SSID to NVS: %s", esp_err_to_name(ret));
		nvs_close(handle);
		return ret;
	}
	
	ret = nvs_set_str(handle, NVS_KEY_PASSWORD, password);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to write password to NVS: %s", esp_err_to_name(ret));
		nvs_close(handle);
		return ret;
	}
	
	ret = nvs_commit(handle);
	nvs_close(handle);
	
	if (ret == ESP_OK) {
		ESP_LOGI(TAG, "WiFi credentials saved to NVS (SSID: %s)", ssid);
	} else {
		ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(ret));
	}
	
	return ret;
}

/**
 * @brief Load WiFi credentials from NVS
 */
esp_err_t module_nvs_load_wifi_credentials(char *ssid, char *password)
{
	if (ssid == NULL || password == NULL) {
		ESP_LOGE(TAG, "Invalid output buffers (NULL pointers)");
		return ESP_ERR_INVALID_ARG;
	}
	
	nvs_handle_t handle;
	esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "Failed to open NVS (namespace may not exist): %s", esp_err_to_name(ret));
		return ret;
	}
	
	size_t ssid_len = MAX_NVS_SSID_LEN + 1;
	ret = nvs_get_str(handle, NVS_KEY_SSID, ssid, &ssid_len);
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "Failed to read SSID from NVS: %s", esp_err_to_name(ret));
		nvs_close(handle);
		return ret;
	}
	
	size_t password_len = MAX_NVS_PASSWORD_LEN + 1;
	ret = nvs_get_str(handle, NVS_KEY_PASSWORD, password, &password_len);
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "Failed to read password from NVS: %s", esp_err_to_name(ret));
		nvs_close(handle);
		return ret;
	}
	
	nvs_close(handle);
	
	ESP_LOGI(TAG, "WiFi credentials loaded from NVS (SSID: %s)", ssid);
	return ESP_OK;
}

/**
 * @brief Check if WiFi credentials exist in NVS
 */
bool module_nvs_credentials_exist(void)
{
	char ssid[MAX_NVS_SSID_LEN + 1];
	char password[MAX_NVS_PASSWORD_LEN + 1];
	
	esp_err_t ret = module_nvs_load_wifi_credentials(ssid, password);
	return (ret == ESP_OK);
}

/**
 * @brief Erase WiFi credentials from NVS
 */
esp_err_t module_nvs_erase_wifi_credentials(void)
{
	nvs_handle_t handle;
	esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
		return ret;
	}
	
	ret = nvs_erase_key(handle, NVS_KEY_SSID);
	if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
		ESP_LOGE(TAG, "Failed to erase SSID: %s", esp_err_to_name(ret));
		nvs_close(handle);
		return ret;
	}
	
	ret = nvs_erase_key(handle, NVS_KEY_PASSWORD);
	if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
		ESP_LOGE(TAG, "Failed to erase password: %s", esp_err_to_name(ret));
		nvs_close(handle);
		return ret;
	}
	
	ret = nvs_commit(handle);
	nvs_close(handle);
	
	if (ret == ESP_OK) {
		ESP_LOGI(TAG, "WiFi credentials erased from NVS");
	}
	
	return ret;
}

/**
 * @brief Get SSID from NVS
 */
esp_err_t module_nvs_get_ssid(char *ssid)
{
	if (ssid == NULL) {
		return ESP_ERR_INVALID_ARG;
	}
	
	nvs_handle_t handle;
	esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
	if (ret != ESP_OK) {
		return ret;
	}
	
	size_t len = MAX_NVS_SSID_LEN + 1;
	ret = nvs_get_str(handle, NVS_KEY_SSID, ssid, &len);
	nvs_close(handle);
	
	return ret;
}

/**
 * @brief Get password from NVS
 */
esp_err_t module_nvs_get_password(char *password)
{
	if (password == NULL) {
		return ESP_ERR_INVALID_ARG;
	}
	
	nvs_handle_t handle;
	esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
	if (ret != ESP_OK) {
		return ret;
	}
	
	size_t len = MAX_NVS_PASSWORD_LEN + 1;
	ret = nvs_get_str(handle, NVS_KEY_PASSWORD, password, &len);
	nvs_close(handle);
	
	return ret;
}
