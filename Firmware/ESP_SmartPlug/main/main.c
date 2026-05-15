#include "esp_log.h"
#include "esp_err.h"
#include <inttypes.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"

#include "board_profile.h"
#include "module_ade7953.h"
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

static void ade7953_startup_config(void)
{
    ESP_ERROR_CHECK(module_ade7953_set_hpf(true));

    /* CT/shunt mode: keep digital integrators disabled. Enable only for Rogowski coils. */
    ESP_ERROR_CHECK(module_ade7953_set_integrators(false, false));

    /* Safe initial gain. Increase PGA only after verifying analog input headroom. */
    ESP_ERROR_CHECK(module_ade7953_set_pga(ADE7953_PGA_GAIN_1,
                                           ADE7953_PGA_GAIN_1,
                                           ADE7953_PGA_GAIN_1));

    ESP_ERROR_CHECK(module_ade7953_set_zero_cross_timeout_ms(100.0f));
    ESP_ERROR_CHECK(module_ade7953_enable_default_power_quality_irqs());
}

static bool ade7953_service_and_should_trip(void)
{
    ade7953_measurement_t m;
    esp_err_t ret = module_ade7953_read_measurement(&m, true, true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ADE7953 read failed: %s", esp_err_to_name(ret));
        return false;
    }

    ESP_LOGI(TAG,
             "ADE raw: VRMS=%" PRIu32 " IRMSA=%" PRIu32 " IRMSB=%" PRIu32
             " AWATT=%" PRId32 " BWATT=%" PRId32 " F=%.2f Hz PFA=%.3f PFB=%.3f IRQ_A=0x%06" PRIX32 " IRQ_B=0x%06" PRIX32,
             m.raw.vrms,
             m.raw.irmsa,
             m.raw.irmsb,
             m.raw.awatt,
             m.raw.bwatt,
             m.line_frequency_hz,
             m.power_factor_a,
             m.power_factor_b,
             m.raw.irq_a,
             m.raw.irq_b);

    ade7953_safety_limits_t limits = {
        .enable_hw_critical_trip = true,
        .enable_rms_voltage_limits = false, /* enable after calibration */
        .enable_rms_current_limit = false,  /* enable after calibration */
        .enable_active_power_limit = false, /* enable after calibration */
        .min_voltage_vrms = 95.0f,
        .max_voltage_vrms = 145.0f,
        .max_current_a_arms = 5.5f,
        .max_active_power_w = 700.0f,
    };

    ade7953_safety_decision_t decision;
    module_ade7953_evaluate_safety(&m, &limits, &decision);
    if (decision.trip) {
        ESP_LOGE(TAG, "ADE7953 critical event: opening relay. hw=%d ov=%d uv=%d oc=%d op=%d",
                 decision.ade_hw_critical_event,
                 decision.overvoltage_event,
                 decision.undervoltage_event,
                 decision.overcurrent_event,
                 decision.overpower_event);
    }
    return decision.trip;
}

void app_main(void)
{
    const smartplug_board_pins_t *pins = smartplug_board_pins_get();

    esp_log_level_set("smartplug", ESP_LOG_INFO);
    ESP_LOGI(TAG, "SmartPlug bring-up boot (%s)", smartplug_board_profile_name());
    ESP_LOGI(TAG, "Relay GPIO: %d", pins->relay_gpio);
    ESP_LOGI(TAG, "RGB LED GPIO: %d", pins->rgb_led_gpio);
    ESP_LOGI(TAG, "TMP102 SDA/SCL: %d/%d", pins->tmp102_sda_gpio, pins->tmp102_scl_gpio);
    ESP_LOGI(TAG, "SD card SPI host: %d CS/MOSI/SCLK/MISO: %d/%d/%d/%d",
             pins->sd_spi_host, pins->sd_cs_gpio, pins->sd_mosi_gpio, pins->sd_sclk_gpio, pins->sd_miso_gpio);
    ESP_LOGI(TAG, "ADE7953 SPI host: %d CS/MOSI/SCLK/MISO: %d/%d/%d/%d RESET/IRQ: %d/%d",
             pins->ade_spi_host, pins->ade_cs_gpio, pins->ade_mosi_gpio, pins->ade_sclk_gpio,
             pins->ade_miso_gpio, pins->ade_reset_gpio, pins->ade_irq_gpio);
    ESP_LOGI(TAG, "Setup button GPIO: %d", pins->setup_bt_button);

    ESP_ERROR_CHECK(module_relay_init());
    ESP_ERROR_CHECK(module_ledstrip_init());
    ESP_ERROR_CHECK(module_tmp102_init());

    esp_err_t sd_init_ret = module_sdcard_init();
    if (sd_init_ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card init failed, continuing without SD logging: %s", esp_err_to_name(sd_init_ret));
    }

    esp_err_t ade_init_ret = module_ade7953_init();
    if (ade_init_ret == ESP_OK) {
        uint8_t ade_version = 0;
        if (module_ade7953_read_version(&ade_version) == ESP_OK) {
            ESP_LOGI(TAG, "ADE7953 version: 0x%02X", ade_version);
        }
        ade7953_startup_config();
    } else {
        ESP_LOGE(TAG, "ADE7953 init failed, continuing without metering: %s", esp_err_to_name(ade_init_ret));
    }

    setup_button_init(pins->setup_bt_button);

    module_relay_set(false);
    status_led_set(STATUS_IDLE);

    const char *test_path = MODULE_SDCARD_MOUNT_POINT "/test.txt";

    while (true) {
        bool ade_trip = false;

        float temperature_c = 0.0f;
        if (module_tmp102_read_celsius(&temperature_c) == ESP_OK) {
            ESP_LOGI(TAG, "TMP102 temperature: %.2f C", temperature_c);

            if (sd_init_ret == ESP_OK) {
                char write_buf[128];
                snprintf(write_buf, sizeof(write_buf), "TMP: %.2f C\n", temperature_c);
                esp_err_t sd_write_ret = module_sdcard_write_file(test_path, write_buf);
                if (sd_write_ret != ESP_OK) {
                    ESP_LOGW(TAG, "SD write failed: %s", esp_err_to_name(sd_write_ret));
                    status_led_set(STATUS_ERROR);
                }
            }
        } else {
            ESP_LOGW(TAG, "TMP102 read failed");
            status_led_set(STATUS_ERROR);
        }

        if (ade_init_ret == ESP_OK) {
            ade_trip = ade7953_service_and_should_trip();
        }

        int button_level = gpio_get_level(pins->setup_bt_button);
        ESP_LOGI(TAG, "Setup button level: %d", button_level);

        if (ade_trip) {
            module_relay_set(false);
            status_led_set(STATUS_ERROR);
        } else if (button_level == 0) {
            module_relay_set(true);
            status_led_set(STATUS_CONNECTED);
        } else {
            module_relay_set(false);
            status_led_set(STATUS_PROVISIONING);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
