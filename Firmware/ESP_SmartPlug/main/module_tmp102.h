#pragma once

#include "esp_err.h"

// Inicializa el bus I2C y el sensor
esp_err_t module_tmp102_init(void);

// Lee la temperatura y la guarda en el puntero que le pases
esp_err_t module_tmp102_read_celsius(float *temperature_c);