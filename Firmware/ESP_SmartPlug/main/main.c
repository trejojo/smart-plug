#include "esp_log.h"
#include "esp_err.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_profile.h"
#include "module_relay.h"
#include "module_ledstrip.h"
#include "module_pzem.h"
#include "module_tmp102.h"
#include "module_ble.h"
#include "module_wifi.h"
#include "module_nvs.h"
#include "module_mqtt.h"

static const char *TAG = "smartplug";

void app_main(void)
{
	// Get the board profile early to log pin information
	const smartplug_board_pins_t *pins = smartplug_board_pins_get();

	// Log the board profile name and pin configuration
	esp_log_level_set("smartplug", ESP_LOG_INFO);
	ESP_LOGI(TAG, "SmartPlug Module 1 boot (%s)", smartplug_board_profile_name());
	
	/* Initialize core modules */
	ESP_ERROR_CHECK(module_relay_init());
	ESP_ERROR_CHECK(module_ledstrip_init());
	ESP_ERROR_CHECK(module_tmp102_init());
	ESP_ERROR_CHECK(module_pzem_init());
	ESP_LOGI(TAG, "Relay on GPIO %d initialized", pins->relay_gpio);
	ESP_LOGI(TAG, "RGB LED on GPIO %d initialized", pins->rgb_led_gpio);
	ESP_LOGI(TAG, "PZEM-004T Modbus interface initialized");
	ESP_LOGI(TAG, "Native USB console depends on menuconfig: enable USB CDC console if required");
	
	/* Initialize NVS and BLE for credential provisioning */
	ESP_ERROR_CHECK(module_nvs_init());
	ESP_ERROR_CHECK(module_ble_init());
	ESP_ERROR_CHECK(module_wifi_init());
	ESP_ERROR_CHECK(module_mqtt_init());
	
	/* Check if WiFi credentials exist in NVS */
	if (!module_nvs_credentials_exist()) {
		ESP_LOGW(TAG, "No WiFi credentials found, starting BLE advertising for provisioning");
		ESP_ERROR_CHECK(module_ble_start_advertising());
	} else {
		ESP_LOGI(TAG, "WiFi credentials found in NVS");
		char ssid[33], password[65];
		if (module_nvs_get_ssid(ssid) == ESP_OK && module_nvs_get_password(password) == ESP_OK) {
			ESP_LOGI(TAG, "SSID: %s (will connect to WiFi)", ssid);
			ESP_ERROR_CHECK(module_wifi_connect(ssid, password));
		}
	}

	while (true) {
		/* Check if credentials were received via BLE */
		if (module_ble_credentials_received()) {
			char ssid[33], password[65];
			if (module_ble_get_ssid(ssid) == ESP_OK && module_ble_get_password(password) == ESP_OK) {
				ESP_LOGI(TAG, "Credentials received via BLE, saving to NVS (SSID: %s)", ssid);
				ESP_ERROR_CHECK(module_nvs_save_wifi_credentials(ssid, password));
				ESP_ERROR_CHECK(module_ble_stop_advertising());
				ESP_ERROR_CHECK(module_wifi_connect(ssid, password));
				module_ble_reset_credentials();
			}
		}

		/* Try to connect MQTT when WiFi is available but MQTT is not connected */
		if (!module_mqtt_is_connected() && module_wifi_is_connected()) {
			ESP_LOGI(TAG, "WiFi connected, attempting MQTT connection to 127.0.0.1:1883");
			module_mqtt_connect("127.0.0.1", 1883);
		}
		
		/* Read and publish telemetry data */
		float temperature_c;
		esp_err_t ret = module_tmp102_read_celsius(&temperature_c);
		if (ret == ESP_OK) {
			ESP_LOGI(TAG, "Current temperature: %.2f °C", temperature_c);
		} else {
			ESP_LOGE(TAG, "Failed to read temperature");
			temperature_c = 0.0f;
		}

		/* Read PZEM power measurements */
		float voltage_v = 0.0f, current_a = 0.0f, power_w = 0.0f;
		uint32_t energy_wh = 0;

		ret = module_pzem_read_voltage(0x01, &voltage_v);
		if (ret == ESP_OK) {
			ESP_LOGI(TAG, "PZEM Voltage: %.1f V", voltage_v);
		} else {
			ESP_LOGW(TAG, "PZEM voltage read failed: %s", esp_err_to_name(ret));
		}

		ret = module_pzem_read_current(0x01, &current_a);
		if (ret == ESP_OK) {
			ESP_LOGI(TAG, "PZEM Current: %.3f A", current_a);
		} else {
			ESP_LOGW(TAG, "PZEM current read failed: %s", esp_err_to_name(ret));
		}

		ret = module_pzem_read_power(0x01, &power_w);
		if (ret == ESP_OK) {
			ESP_LOGI(TAG, "PZEM Power: %.1f W", power_w);
		} else {
			ESP_LOGW(TAG, "PZEM power read failed: %s", esp_err_to_name(ret));
		}

		ret = module_pzem_read_energy(0x01, &energy_wh);
		if (ret == ESP_OK) {
			ESP_LOGI(TAG, "PZEM Energy: %lu Wh", (unsigned long)energy_wh);
		} else {
			ESP_LOGW(TAG, "PZEM energy read failed: %s", esp_err_to_name(ret));
		}

		/* Publish telemetry via MQTT if connected */
		if (module_mqtt_is_connected()) {
			module_mqtt_publish_status(temperature_c, voltage_v, current_a, power_w, energy_wh, true);
		}

		// Cycle through some example states to demonstrate functionality
		ESP_LOGI(TAG, "Status: relay OFF, LED blue");
		module_relay_set(false);
		ESP_ERROR_CHECK(module_ledstrip_set(0, 0, 32));
		vTaskDelay(pdMS_TO_TICKS(5000));

		ESP_LOGI(TAG, "Status: relay ON, LED green");
		module_relay_set(true);
		ESP_ERROR_CHECK(module_ledstrip_set(0, 32, 0));
		vTaskDelay(pdMS_TO_TICKS(5000));

		ESP_LOGI(TAG, "Status: relay OFF, LED red");
		module_relay_set(false);
		ESP_ERROR_CHECK(module_ledstrip_set(32, 0, 0));
		vTaskDelay(pdMS_TO_TICKS(5000));
	}
}