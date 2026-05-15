#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include <inttypes.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#include "board_profile.h"
#include "module_ade7953.h"
#include "module_relay.h"

static const char *TAG = "aice_cal";

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
    ESP_ERROR_CHECK(module_ade7953_set_integrators(false, false));
    ESP_ERROR_CHECK(module_ade7953_set_pga(ADE7953_PGA_GAIN_1,
                                           ADE7953_PGA_GAIN_1,
                                           ADE7953_PGA_GAIN_1));
}

void app_main(void)
{
    const smartplug_board_pins_t *pins = smartplug_board_pins_get();
    
    /* Disable default ESP logging to keep the serial stream clean for Python */
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set("aice_cal", ESP_LOG_INFO);

    ESP_LOGI(TAG, "AICE Calibration Mode Started");

    ESP_ERROR_CHECK(module_relay_init());
    setup_button_init(pins->setup_bt_button);

    esp_err_t ade_init_ret = module_ade7953_init();
    if (ade_init_ret == ESP_OK) {
        ade7953_startup_config();
        ESP_LOGI(TAG, "ADE7953 Initialized Successfully.");
    } else {
        ESP_LOGE(TAG, "ADE7953 Init Failed! Halting.");
        while(1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    bool relay_state = false;
    module_relay_set(relay_state);
    
    int last_button_level = 1;

    /* Print the CSV header once for reference (Python will ignore it or parse it) */
    printf("t_ms,vrms_raw,irmsa_raw,irmsb_raw,awatt_raw,avar_raw,ava_raw,aenergy_raw,pf_raw,period_raw\n");

    while (true) {
        /* Simple button debounce and toggle for the relay */
        int current_button_level = gpio_get_level(pins->setup_bt_button);
        if (current_button_level == 0 && last_button_level == 1) {
            relay_state = !relay_state;
            module_relay_set(relay_state);
            ESP_LOGW(TAG, "Relay toggled: %s", relay_state ? "ON" : "OFF");
        }
        last_button_level = current_button_level;

        ade7953_measurement_t m = {0};
        /* read_energy_deltas = true, clear_irq_status = true */
        if (module_ade7953_read_measurement(&m, true, true) == ESP_OK) {
            
            uint64_t t_ms = esp_timer_get_time() / 1000ULL;
            
            /* Print the comma-separated data stream */
            printf("%llu,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRId32 ",%" PRId32 ",%" PRId32 ",%" PRId32 ",%" PRId16 ",%u\n",
                   t_ms,
                   m.raw.vrms,
                   m.raw.irmsa,
                   m.raw.irmsb,
                   m.raw.awatt,
                   m.raw.avar,
                   m.raw.ava,
                   m.raw.aenergy_a_delta,
                   m.raw.pfa,
                   m.raw.period);
        }

        /* 100ms delay yields approx 10Hz sampling rate, good for Python parsing */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}