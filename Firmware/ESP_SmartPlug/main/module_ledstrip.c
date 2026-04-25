#include "module_ledstrip.h"

#include "board_profile.h"
#include "esp_check.h"
#include "led_strip.h"

static led_strip_handle_t s_led_strip;
static const smartplug_board_pins_t *s_pins;

esp_err_t module_ledstrip_init(void)
{
    s_pins = smartplug_board_pins_get();

    led_strip_config_t strip_config = {
        .strip_gpio_num = s_pins->rgb_led_gpio,
        .max_leds = 1,
    };

    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };

    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led_strip), "module_ledstrip", "led strip init failed");
    ESP_RETURN_ON_ERROR(led_strip_clear(s_led_strip), "module_ledstrip", "led clear failed");

    return ESP_OK;
}

esp_err_t module_ledstrip_set(uint8_t red, uint8_t green, uint8_t blue)
{
    ESP_RETURN_ON_FALSE(s_led_strip != NULL, ESP_ERR_INVALID_STATE, "module_ledstrip", "led strip not initialized");
    ESP_RETURN_ON_ERROR(led_strip_set_pixel(s_led_strip, 0, red, green, blue), "module_ledstrip", "set pixel failed");
    ESP_RETURN_ON_ERROR(led_strip_refresh(s_led_strip), "module_ledstrip", "refresh failed");
    return ESP_OK;
}
