#include "esp_log.h"
#include "esp_err.h"
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"

#include "board_profile.h"
#include "module_ledstrip.h"
#include "module_relay.h"
#include "module_sdcard.h"
#include "module_tmp102.h"

static const char *TAG = "smartplug";

typedef enum {
	STATUS_IDLE = 0,
	STATUS_PROVISIONING,
	STATUS_CONNECTED,
	STATUS_ERROR,
} status_t;

static void status_led_set(status_t s)
{
	switch (s) {
		case STATUS_PROVISIONING:
			module_ledstrip_set(0, 0, 32); /* blue */
			break;
		case STATUS_CONNECTED:
			module_ledstrip_set(0, 32, 0); /* green */
			break;
		case STATUS_ERROR:
			module_ledstrip_set(32, 0, 0); /* red */
			break;
		case STATUS_IDLE:
		default:
			module_ledstrip_set(0, 0, 0); /* off */
			break;
	}
}

static void setup_button_init(gpio_num_t gpio_num)
{
	gpio_config_t button_cfg = {
		.pin_bit_mask = 1ULL << gpio_num,
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_ENABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	ESP_ERROR_CHECK(gpio_config(&button_cfg));
}

void app_main(void)
{
	const smartplug_board_pins_t *pins = smartplug_board_pins_get();

	esp_log_level_set("smartplug", ESP_LOG_INFO);
	ESP_LOGI(TAG, "SmartPlug bring-up boot (%s)", smartplug_board_profile_name());
	ESP_LOGI(TAG, "Relay GPIO: %d", pins->relay_gpio);
	ESP_LOGI(TAG, "RGB LED GPIO: %d", pins->rgb_led_gpio);
	ESP_LOGI(TAG, "TMP102 SDA/SCL: %d/%d", pins->tmp102_sda_gpio, pins->tmp102_scl_gpio);
	ESP_LOGI(TAG, "SD card SPI host: %d CS/MOSI/SCLK/MISO: %d/%d/%d/%d", pins->sd_spi_host, pins->sd_cs_gpio, pins->sd_mosi_gpio, pins->sd_sclk_gpio, pins->sd_miso_gpio);
	ESP_LOGI(TAG, "Setup button GPIO: %d", pins->setup_bt_button);

	ESP_ERROR_CHECK(module_relay_init());
	ESP_ERROR_CHECK(module_ledstrip_init());
	ESP_ERROR_CHECK(module_tmp102_init());
	esp_err_t sd_init_ret = module_sdcard_init();
	if (sd_init_ret != ESP_OK) {
		ESP_LOGW(TAG, "SD card init failed, continuing without SD logging: %s", esp_err_to_name(sd_init_ret));
	}
	setup_button_init(pins->setup_bt_button);

	module_relay_set(false);
	status_led_set(STATUS_IDLE);

	/* LFN is disabled in sdkconfig, so keep this 8.3-safe */
	const char *test_path = MODULE_SDCARD_MOUNT_POINT "/test.txt";
	/* Write initial TMP102 reading to SD and verify by reading it back */
	if (sd_init_ret == ESP_OK) {
		float start_temp = 0.0f;
		char write_buf[128];
		if (module_tmp102_read_celsius(&start_temp) == ESP_OK) {
			snprintf(write_buf, sizeof(write_buf), "TMP: %.2f C\n", start_temp);
		} else {
			snprintf(write_buf, sizeof(write_buf), "TMP: read_failed\n");
		}

		esp_err_t sd_write_ret = module_sdcard_write_file(test_path, write_buf);
		if (sd_write_ret == ESP_OK) {
			ESP_LOGI(TAG, "Wrote TMP reading to SD: %s", write_buf);
			status_led_set(STATUS_CONNECTED);
			char read_buf[256] = {0};
			esp_err_t sd_read_ret = module_sdcard_read_file(test_path, read_buf, sizeof(read_buf));
			if (sd_read_ret == ESP_OK) {
				ESP_LOGI(TAG, "SD file contents: %s", read_buf);
			} else {
				ESP_LOGW(TAG, "SD read failed: %s", esp_err_to_name(sd_read_ret));
			}
		} else {
			ESP_LOGW(TAG, "SD card write failed: %s", esp_err_to_name(sd_write_ret));
			status_led_set(STATUS_ERROR);
		}
	}

	while (true) {
		float temperature_c = 0.0f;
		if (module_tmp102_read_celsius(&temperature_c) == ESP_OK) {
			ESP_LOGI(TAG, "TMP102 temperature: %.2f C", temperature_c);

			if (sd_init_ret == ESP_OK) {
				/* write the latest reading to SD and read it back to verify */
				char write_buf[128];
				snprintf(write_buf, sizeof(write_buf), "TMP: %.2f C\n", temperature_c);
				esp_err_t sd_write_ret = module_sdcard_write_file(test_path, write_buf);
				if (sd_write_ret == ESP_OK) {
					char read_buf[256] = {0};
					esp_err_t sd_read_ret = module_sdcard_read_file(test_path, read_buf, sizeof(read_buf));
					if (sd_read_ret == ESP_OK) {
						ESP_LOGI(TAG, "SD file contents: %s", read_buf);
					} else {
						ESP_LOGW(TAG, "SD read failed: %s", esp_err_to_name(sd_read_ret));
					}
					status_led_set(STATUS_CONNECTED);
				} else {
					ESP_LOGW(TAG, "SD write failed: %s", esp_err_to_name(sd_write_ret));
					status_led_set(STATUS_ERROR);
				}
			}
		} else {
			ESP_LOGW(TAG, "TMP102 read failed");
			status_led_set(STATUS_ERROR);
		}

		int button_level = gpio_get_level(pins->setup_bt_button);
		ESP_LOGI(TAG, "Setup button level: %d", button_level);

		if (button_level == 0) {
			module_relay_set(true);
			status_led_set(STATUS_CONNECTED);
		} else {
			module_relay_set(false);
			status_led_set(STATUS_PROVISIONING);
		}

		vTaskDelay(pdMS_TO_TICKS(1000));
	}
}