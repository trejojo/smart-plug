#include "module_pzem.h"

#include <stdbool.h>
#include <stdint.h>

#include "board_profile.h"
#include "pzem-driver.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "PZEM_MODULE";
static const smartplug_board_pins_t *s_pins;
static bool s_initialized;

esp_err_t module_pzem_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_pins = smartplug_board_pins_get();
    ESP_RETURN_ON_FALSE(s_pins != NULL, ESP_ERR_INVALID_STATE, TAG, "board profile not available");

    // Initialize PZEM driver with TX/RX pins from board profile
    initialize_pzem((uint8_t)s_pins->pzem_tx_gpio, (uint8_t)s_pins->pzem_rx_gpio);

    s_initialized = true;

    ESP_LOGI(TAG,
             "PZEM initialized (TX:%d RX:%d Addr:0x%02X)",
             s_pins->pzem_tx_gpio,
             s_pins->pzem_rx_gpio,
             s_pins->pzem_default_slave_addr);

    return ESP_OK;
}

esp_err_t module_pzem_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    s_pins = NULL;
    s_initialized = false;
    return ESP_OK;
}

esp_err_t module_pzem_read_voltage(uint8_t slave_addr, float *voltage_v)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "PZEM module not initialized");
    ESP_RETURN_ON_FALSE(voltage_v != NULL, ESP_ERR_INVALID_ARG, TAG, "voltage_v is null");

    if (!updateValues(slave_addr)) {
        return ESP_FAIL;
    }

    *voltage_v = getVoltage();
    return ESP_OK;
}

esp_err_t module_pzem_read_current(uint8_t slave_addr, float *current_a)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "PZEM module not initialized");
    ESP_RETURN_ON_FALSE(current_a != NULL, ESP_ERR_INVALID_ARG, TAG, "current_a is null");

    if (!updateValues(slave_addr)) {
        return ESP_FAIL;
    }

    *current_a = getCurrent();
    return ESP_OK;
}

esp_err_t module_pzem_read_power(uint8_t slave_addr, float *power_w)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "PZEM module not initialized");
    ESP_RETURN_ON_FALSE(power_w != NULL, ESP_ERR_INVALID_ARG, TAG, "power_w is null");

    if (!updateValues(slave_addr)) {
        return ESP_FAIL;
    }

    *power_w = getPower();
    return ESP_OK;
}

esp_err_t module_pzem_read_energy(uint8_t slave_addr, uint32_t *energy_wh)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "PZEM module not initialized");
    ESP_RETURN_ON_FALSE(energy_wh != NULL, ESP_ERR_INVALID_ARG, TAG, "energy_wh is null");

    if (!updateValues(slave_addr)) {
        return ESP_FAIL;
    }

    *energy_wh = (uint32_t)getEnergy();
    return ESP_OK;
}
