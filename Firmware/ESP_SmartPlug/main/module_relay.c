#include "module_relay.h"

#include "board_profile.h"
#include "driver/gpio.h"
#include "esp_check.h"

static const smartplug_board_pins_t *s_pins;

esp_err_t module_relay_init(void)
{
    s_pins = smartplug_board_pins_get();

    ESP_RETURN_ON_ERROR(gpio_reset_pin(s_pins->relay_gpio), "module_relay", "relay pin reset failed");
    ESP_RETURN_ON_ERROR(gpio_set_direction(s_pins->relay_gpio, GPIO_MODE_OUTPUT), "module_relay", "relay pin mode failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(s_pins->relay_gpio, 1), "module_relay", "relay default level failed");

    gpio_config_t relay_cfg = {
        .pin_bit_mask = 1ULL << s_pins->relay_gpio,
        .mode = GPIO_MODE_OUTPUT_OD, // Open-Drain for 5V tolerance
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&relay_cfg), "module_relay", "relay config failed");
    gpio_set_level(s_pins->relay_gpio, 1);

    return ESP_OK;
}

void module_relay_set(bool enabled)
{
    if (s_pins) {
        gpio_set_level(s_pins->relay_gpio, enabled ? 1 : 0);
    }
}
