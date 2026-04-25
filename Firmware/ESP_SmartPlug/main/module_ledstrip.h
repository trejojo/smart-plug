#pragma once

#include <stdint.h>
#include "esp_err.h"

/**
 * @brief Initialize RGB LED strip module
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t module_ledstrip_init(void);

/**
 * @brief Set RGB LED color
 * @param red Red component (0-255)
 * @param green Green component (0-255)
 * @param blue Blue component (0-255)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t module_ledstrip_set(uint8_t red, uint8_t green, uint8_t blue);
