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

static const char *TAG = "smartplug";

void app_main(void)
{
	// Get the board profile early to log pin information
	const smartplug_board_pins_t *pins = smartplug_board_pins_get();

	// Log the board profile name and pin configuration
	esp_log_level_set("smartplug", ESP_LOG_INFO);
	ESP_LOGI(TAG, "SmartPlug Module 1 boot (%s)", smartplug_board_profile_name());
	ESP_ERROR_CHECK(module_relay_init());
	ESP_ERROR_CHECK(module_ledstrip_init());
	ESP_ERROR_CHECK(module_tmp102_init());
	ESP_ERROR_CHECK(module_pzem_init());
	ESP_LOGI(TAG, "Relay on GPIO %d initialized", pins->relay_gpio);
	ESP_LOGI(TAG, "RGB LED on GPIO %d initialized", pins->rgb_led_gpio);
	ESP_LOGI(TAG, "PZEM-004T Modbus interface initialized");
	ESP_LOGI(TAG, "Native USB console depends on menuconfig: enable USB CDC console if required");

	while (true) {
		float temperature_c;
		esp_err_t ret = module_tmp102_read_celsius(&temperature_c);
		if (ret == ESP_OK) {
			ESP_LOGI(TAG, "Current temperature: %.2f °C", temperature_c);
		} else {
			ESP_LOGE(TAG, "Failed to read temperature");
		}

		// Read PZEM power measurements
		float voltage_v, current_a, power_w;
		uint32_t energy_wh;

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