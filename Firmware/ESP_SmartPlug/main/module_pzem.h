#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @brief Initialize PZEM module with board profile configuration
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t module_pzem_init(void);

/**
 * @brief Deinitialize PZEM module
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t module_pzem_deinit(void);

/**
 * @brief Read voltage from PZEM device
 * @param slave_addr Modbus slave address of PZEM device
 * @param voltage_v Output parameter for voltage in Volts
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t module_pzem_read_voltage(uint8_t slave_addr, float *voltage_v);

/**
 * @brief Read current from PZEM device
 * @param slave_addr Modbus slave address of PZEM device
 * @param current_a Output parameter for current in Amperes
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t module_pzem_read_current(uint8_t slave_addr, float *current_a);

/**
 * @brief Read power from PZEM device
 * @param slave_addr Modbus slave address of PZEM device
 * @param power_w Output parameter for power in Watts
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t module_pzem_read_power(uint8_t slave_addr, float *power_w);

/**
 * @brief Read energy from PZEM device
 * @param slave_addr Modbus slave address of PZEM device
 * @param energy_wh Output parameter for energy in Wh
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t module_pzem_read_energy(uint8_t slave_addr, uint32_t *energy_wh);
