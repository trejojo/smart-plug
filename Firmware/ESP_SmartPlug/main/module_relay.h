#pragma once

#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief Initialize relay module
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t module_relay_init(void);

/**
 * @brief Set relay state
 * @param enabled true to turn relay ON, false to turn relay OFF
 */
void module_relay_set(bool enabled);
