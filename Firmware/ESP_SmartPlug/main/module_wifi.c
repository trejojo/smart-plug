#include "module_wifi.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include <string.h>

static const char *TAG = "module_wifi";

static bool wifi_initialized = false;
static bool wifi_started = false;
static bool wifi_connected = false;
static bool wifi_connect_requested = false;
static uint8_t wifi_retry_count = 0;

static const int WIFI_MAX_RETRY = 5;

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
	(void)arg;

	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
		ESP_LOGI(TAG, "Wi-Fi started, connecting to AP...");
		if (wifi_connect_requested) {
			esp_wifi_connect();
		}
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
		wifi_connected = false;
		if (wifi_retry_count < WIFI_MAX_RETRY) {
			wifi_retry_count++;
			ESP_LOGW(TAG, "Disconnected from AP, retrying (%u/%d)", wifi_retry_count, WIFI_MAX_RETRY);
			esp_wifi_connect();
		} else {
			wifi_connect_requested = false;
			ESP_LOGE(TAG, "Failed to connect after %d retries", WIFI_MAX_RETRY);
		}
	} else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
		wifi_connected = true;
		wifi_connect_requested = false;
		wifi_retry_count = 0;
		ESP_LOGI(TAG, "Connected to Wi-Fi, IP: " IPSTR, IP2STR(&event->ip_info.ip));
	}
}

static esp_err_t wifi_ensure_initialized(void)
{
	if (wifi_initialized) {
		return ESP_OK;
	}

	esp_err_t ret = esp_netif_init();
	if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
		ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
		return ret;
	}

	ret = esp_event_loop_create_default();
	if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
		ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(ret));
		return ret;
	}

	if (esp_netif_create_default_wifi_sta() == NULL) {
		ESP_LOGE(TAG, "Failed to create default Wi-Fi STA netif");
		return ESP_FAIL;
	}

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ret = esp_wifi_init(&cfg);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
		return ret;
	}

	ret = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to register Wi-Fi event handler: %s", esp_err_to_name(ret));
		return ret;
	}

	ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to register IP event handler: %s", esp_err_to_name(ret));
		return ret;
	}

	ret = esp_wifi_set_mode(WIFI_MODE_STA);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(ret));
		return ret;
	}

	wifi_initialized = true;
	ESP_LOGI(TAG, "Wi-Fi module initialized in STA mode");
	return ESP_OK;
}

esp_err_t module_wifi_init(void)
{
	return wifi_ensure_initialized();
}

esp_err_t module_wifi_connect(const char *ssid, const char *password)
{
	if (ssid == NULL || password == NULL) {
		return ESP_ERR_INVALID_ARG;
	}

	if (strlen(ssid) == 0 || strlen(ssid) > MAX_WIFI_SSID_LEN) {
		return ESP_ERR_INVALID_ARG;
	}

	if (strlen(password) > MAX_WIFI_PASSWORD_LEN) {
		return ESP_ERR_INVALID_ARG;
	}

	esp_err_t ret = wifi_ensure_initialized();
	if (ret != ESP_OK) {
		return ret;
	}

	wifi_config_t wifi_config = {0};
	memset(&wifi_config, 0, sizeof(wifi_config));
	memcpy(wifi_config.sta.ssid, ssid, strlen(ssid));
	memcpy(wifi_config.sta.password, password, strlen(password));
	wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

	ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(ret));
		return ret;
	}

	wifi_connect_requested = true;
	wifi_connected = false;
	wifi_retry_count = 0;

	if (!wifi_started) {
		ret = esp_wifi_start();
		if (ret != ESP_OK) {
			ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
			wifi_connect_requested = false;
			return ret;
		}

		wifi_started = true;
		ESP_LOGI(TAG, "Wi-Fi radio started, waiting for connection attempt");
	} else {
		ret = esp_wifi_connect();
		if (ret != ESP_OK) {
			ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(ret));
			wifi_connect_requested = false;
			return ret;
		}
	}

	ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);
	return ESP_OK;
}

esp_err_t module_wifi_disconnect(void)
{
	if (!wifi_started) {
		return ESP_OK;
	}

	wifi_connect_requested = false;
	wifi_connected = false;
	wifi_retry_count = 0;

	esp_err_t ret = esp_wifi_disconnect();
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "esp_wifi_disconnect returned: %s", esp_err_to_name(ret));
	}

	return ESP_OK;
}

bool module_wifi_is_connected(void)
{
	return wifi_connected;
}