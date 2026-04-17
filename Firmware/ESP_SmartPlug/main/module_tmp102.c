#include "module_tmp102.h"
#include "tmp102.h"
#include "board_profile.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "TMP102_MODULE";
static bool s_initialized = false;
static const smartplug_board_pins_t *s_pins;

esp_err_t module_tmp102_init(void)
{
    // Avoid re-initialization if already done
    if (s_initialized) {
        return ESP_OK;
    }

    // Get pin configuration from board profile
    s_pins = smartplug_board_pins_get();
    
    // Config the structure that library askss for, 
    // using the pins defined in board_profile and the I2C address
    tmp102_config_t config = {
        .addr = 0x48,
        .pin_sda = s_pins->tmp102_sda_gpio,
        .pin_scl= s_pins->tmp102_scl_gpio,
    };

    // Init the library with the config, it will handle the I2C driver and the sensor setup
    esp_err_t ret = tmp102_init(&config);
    
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to initialize TMP102 library");

    s_initialized = true;
    ESP_LOGI(TAG, "TMP102 initialized (SDA:%d, SCL:%d, Addr:0x%02X)", 
             s_pins->tmp102_sda_gpio, s_pins->tmp102_scl_gpio, s_pins->tmp102_i2c_addr);

    return ESP_OK;
}

esp_err_t module_tmp102_read_celsius(float *temperature_c)
{
    // Security checks: valid pointer and module initialized
    ESP_RETURN_ON_FALSE(temperature_c != NULL, ESP_ERR_INVALID_ARG, TAG, "Null pointer for temperature output");
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "MModule not initialized");

    // Use the library function to read the temperature in Celsius, it will handle the I2C communication and data conversion
    *temperature_c = tmp102_read_temp_c();

    // The TMP102 returns -128.0°C if there was an error reading the sensor (e.g., not connected), so we can check for that as a simple error detection mechanism
    if (*temperature_c == -128.0f) {
        ESP_LOGE(TAG, "Error reading sensor (¿is it connected?)");
        return ESP_FAIL;
    }

    return ESP_OK;
}