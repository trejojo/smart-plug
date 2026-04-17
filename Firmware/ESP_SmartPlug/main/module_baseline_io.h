#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t module_baseline_io_init(void);
void module_baseline_relay_set(bool enabled);
esp_err_t module_baseline_rgb_set(uint8_t red, uint8_t green, uint8_t blue);
