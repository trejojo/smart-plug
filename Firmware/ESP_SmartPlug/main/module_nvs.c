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

esp_err_t module_nvs_get_pzem_enabled(bool *enabled)
{
	if (enabled == NULL) {
		return ESP_ERR_INVALID_ARG;
	}

	nvs_handle_t handle;
	esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
	if (ret != ESP_OK) {
		// Namespace may not exist yet; default to enabled
		*enabled = true;
		return ESP_OK;
	}

	uint8_t val = 1;
	ret = nvs_get_u8(handle, NVS_KEY_PZEM_ENABLED, &val);
	nvs_close(handle);

	if (ret == ESP_ERR_NVS_NOT_FOUND) {
		*enabled = true; // default
		return ESP_OK;
	} else if (ret != ESP_OK) {
		return ret;
	}

	*enabled = (val != 0);
	return ESP_OK;
}

/* Calibration persistence ------------------------------------------------- */
esp_err_t module_nvs_save_calibration(float kv, float ki, float wh_lsb)
{
	nvs_handle_t handle;
	esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to open NVS for calibration: %s", esp_err_to_name(ret));
		return ret;
	}

	ret = nvs_set_blob(handle, NVS_KEY_KV, &kv, sizeof(kv));
	if (ret != ESP_OK) goto done;
	ret = nvs_set_blob(handle, NVS_KEY_KI, &ki, sizeof(ki));
	if (ret != ESP_OK) goto done;
	ret = nvs_set_blob(handle, NVS_KEY_WH_LSB, &wh_lsb, sizeof(wh_lsb));
	if (ret != ESP_OK) goto done;

	ret = nvs_commit(handle);
	if (ret == ESP_OK) {
		ESP_LOGI(TAG, "Calibration saved to NVS (kv=%f, ki=%f, wh_lsb=%f)", kv, ki, wh_lsb);
	} else {
		ESP_LOGE(TAG, "Failed to commit calibration to NVS: %s", esp_err_to_name(ret));
	}

done:
	nvs_close(handle);
	return ret;
}

esp_err_t module_nvs_load_calibration(float *kv, float *ki, float *wh_lsb)
{
	if (kv == NULL || ki == NULL || wh_lsb == NULL) {
		return ESP_ERR_INVALID_ARG;
	}

	nvs_handle_t handle;
	esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "NVS namespace not present for calibration: %s", esp_err_to_name(ret));
		*kv = *ki = *wh_lsb = 0.0f;
		return ESP_OK;
	}

	size_t required = 0;
	required = sizeof(float);
	ret = nvs_get_blob(handle, NVS_KEY_KV, kv, &required);
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "KV not found in NVS: %s", esp_err_to_name(ret));
		*kv = 0.0f;
	}

	required = sizeof(float);
	ret = nvs_get_blob(handle, NVS_KEY_KI, ki, &required);
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "KI not found in NVS: %s", esp_err_to_name(ret));
		*ki = 0.0f;
	}

	required = sizeof(float);
	ret = nvs_get_blob(handle, NVS_KEY_WH_LSB, wh_lsb, &required);
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "WH_LSB not found in NVS: %s", esp_err_to_name(ret));
		*wh_lsb = 0.0f;
	}

	nvs_close(handle);
	ESP_LOGI(TAG, "Calibration loaded from NVS (kv=%f, ki=%f, wh_lsb=%f)", *kv, *ki, *wh_lsb);
	return ESP_OK;
}

/* AWGAIN persistence ----------------------------------------------------- */
esp_err_t module_nvs_save_awgain(uint32_t awgain_raw24)
{
	nvs_handle_t handle;
	esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to open NVS for AWGAIN: %s", esp_err_to_name(ret));
		return ret;
	}

	ret = nvs_set_u32(handle, NVS_KEY_AWGAIN, awgain_raw24 & 0x00FFFFFFU);
	if (ret == ESP_OK) {
		ret = nvs_commit(handle);
	}
	if (ret == ESP_OK) {
		ESP_LOGI(TAG, "AWGAIN saved to NVS: 0x%06X", awgain_raw24 & 0x00FFFFFFU);
	} else {
		ESP_LOGE(TAG, "Failed to save AWGAIN to NVS: %s", esp_err_to_name(ret));
	}

	nvs_close(handle);
	return ret;
}

esp_err_t module_nvs_load_awgain(uint32_t *awgain_raw24)
{
	if (awgain_raw24 == NULL) return ESP_ERR_INVALID_ARG;
	nvs_handle_t handle;
	esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "NVS namespace not present for AWGAIN: %s", esp_err_to_name(ret));
		*awgain_raw24 = 0;
		return ESP_OK;
	}

	uint32_t val = 0;
	ret = nvs_get_u32(handle, NVS_KEY_AWGAIN, &val);
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "AWGAIN not found in NVS: %s", esp_err_to_name(ret));
		*awgain_raw24 = 0;
	} else {
		*awgain_raw24 = val & 0x00FFFFFFU;
		ESP_LOGI(TAG, "AWGAIN loaded from NVS: 0x%06X", *awgain_raw24);
	}
	nvs_close(handle);
	return ESP_OK;
}
