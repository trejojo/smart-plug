#include "module_baseline_io.h"
#include "board_profile.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "led_strip.h"

static led_strip_handle_t s_led_strip;
static const smartplug_board_pins_t *s_pins;

esp_err_t module_baseline_io_init(void)
{
    s_pins = smartplug_board_pins_get();

    ESP_RETURN_ON_ERROR(gpio_reset_pin(s_pins->relay_gpio), "module_baseline_io", "relay pin reset failed");
    ESP_RETURN_ON_ERROR(gpio_set_direction(s_pins->relay_gpio, GPIO_MODE_OUTPUT), "module_baseline_io", "relay pin mode failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(s_pins->relay_gpio, 0), "module_baseline_io", "relay default level failed");
    

    led_strip_config_t strip_config = {
        .strip_gpio_num = s_pins->rgb_led_gpio,
        .max_leds = 1,
    };
    
    gpio_config_t relay_cfg = {
        .pin_bit_mask = 1ULL << s_pins->relay_gpio,
        .mode = GPIO_MODE_OUTPUT_OD, // Modo Open-Drain para tolerar módulos de 5V
        .pull_up_en = GPIO_PULLUP_ENABLE, // Pull-up interno para ayudar al nivel lógico
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&relay_cfg), "module_baseline_io", "relay config failed");
    gpio_set_level(s_pins->relay_gpio, 1);

    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };

    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led_strip), "module_baseline_io", "led strip init failed");
    ESP_RETURN_ON_ERROR(led_strip_clear(s_led_strip), "module_baseline_io", "led clear failed");

    return ESP_OK;
}

void module_baseline_relay_set(bool enabled)
{

    if (s_pins) {
        gpio_set_level(s_pins->relay_gpio, enabled ? 1 : 0);
    }
    
}

esp_err_t module_baseline_rgb_set(uint8_t red, uint8_t green, uint8_t blue)
{
    ESP_RETURN_ON_FALSE(s_led_strip != NULL, ESP_ERR_INVALID_STATE, "module_baseline_io", "led strip not initialized");
    ESP_RETURN_ON_ERROR(led_strip_set_pixel(s_led_strip, 0, red, green, blue), "module_baseline_io", "set pixel failed");
    ESP_RETURN_ON_ERROR(led_strip_refresh(s_led_strip), "module_baseline_io", "refresh failed");
    return ESP_OK;
}
