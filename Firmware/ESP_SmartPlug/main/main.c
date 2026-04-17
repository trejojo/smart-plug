#include "esp_log.h"
#include "esp_err.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_profile.h"
#include "module_baseline_io.h"
#include "module_sdcard.h"
#include "module_tmp102.h"

static const char *TAG = "smartplug";
static uint32_t s_sd_test_cycle = 0;

static void sdcard_quick_state_test(float temperature_c, bool relay_enabled)
{
	char write_buf[96];
	char read_buf[96];
	uint32_t cycle = ++s_sd_test_cycle;
	const char *relay_state = relay_enabled ? "ON" : "OFF";

	snprintf(write_buf, sizeof(write_buf), "cycle=%lu,temp=%.2f,relay=%s\n", (unsigned long)cycle, temperature_c, relay_state);
	ESP_LOGW(TAG, "[SD TEST] cycle=%lu write: %s", (unsigned long)cycle, write_buf);

	esp_err_t ret = module_sdcard_write_file(MODULE_SDCARD_MOUNT_POINT "/status.txt", write_buf);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "[SD TEST] cycle=%lu write failed: %s", (unsigned long)cycle, esp_err_to_name(ret));
		return;
	}

	ret = module_sdcard_read_file(MODULE_SDCARD_MOUNT_POINT "/status.txt", read_buf, sizeof(read_buf));
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "[SD TEST] cycle=%lu read failed: %s", (unsigned long)cycle, esp_err_to_name(ret));
		return;
	}

	ESP_LOGW(TAG, "[SD TEST] cycle=%lu read: %s", (unsigned long)cycle, read_buf);
}

void app_main(void)
{
	// Get the board profile early to log pin information
	const smartplug_board_pins_t *pins = smartplug_board_pins_get(); 

	// Log the board profile name and pin configuration
	esp_log_level_set("smartplug", ESP_LOG_INFO);
	esp_log_level_set("SDCARD_MODULE", ESP_LOG_INFO);
	ESP_LOGI(TAG, "SmartPlug Module 1 boot (%s)", smartplug_board_profile_name());
	ESP_ERROR_CHECK(module_baseline_io_init());
	ESP_ERROR_CHECK(module_sdcard_init());
	ESP_ERROR_CHECK(module_tmp102_init());
	ESP_LOGI(TAG, "Relay on GPIO %d initialized", pins->relay_gpio);
	ESP_LOGI(TAG, "RGB LED on GPIO %d initialized", pins->rgb_led_gpio);
	ESP_LOGI(TAG, "SD card mounted at %s", MODULE_SDCARD_MOUNT_POINT);

	char sdcard_status[96];
	snprintf(sdcard_status, sizeof(sdcard_status), "Booted on %s\n", smartplug_board_profile_name());
	ESP_ERROR_CHECK(module_sdcard_write_file(MODULE_SDCARD_MOUNT_POINT "/boot.txt", sdcard_status));
	ESP_LOGI(TAG, "Native USB console depends on menuconfig: enable USB CDC console if required");

	while (true) {
		float temperature_c;
		esp_err_t ret = module_tmp102_read_celsius(&temperature_c);
		if (ret == ESP_OK) {
			ESP_LOGI(TAG, "Current temperature: %.2f °C", temperature_c);
		} else {
			ESP_LOGE(TAG, "Failed to read temperature");
			temperature_c = -999.0f;
		}
		
		// Cycle through some example states to demonstrate functionality
		ESP_LOGI(TAG, "Status: relay OFF, LED blue");
		module_baseline_relay_set(false);
		sdcard_quick_state_test(temperature_c, false);
		ESP_ERROR_CHECK(module_baseline_rgb_set(0, 0, 32));
		// FreeRTOS to pause execution for a while to see the effect
		vTaskDelay(pdMS_TO_TICKS(5000));

		ESP_LOGI(TAG, "Status: relay ON, LED green");
		module_baseline_relay_set(true);
		sdcard_quick_state_test(temperature_c, true);
		ESP_ERROR_CHECK(module_baseline_rgb_set(0, 32, 0));
		vTaskDelay(pdMS_TO_TICKS(5000));

		ESP_LOGI(TAG, "Status: relay OFF, LED red");
		module_baseline_relay_set(false);
		sdcard_quick_state_test(temperature_c, false);
		ESP_ERROR_CHECK(module_baseline_rgb_set(32, 0, 0));
		vTaskDelay(pdMS_TO_TICKS(5000));
	}
}