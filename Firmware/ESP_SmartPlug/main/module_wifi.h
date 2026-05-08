#pragma once

#include "esp_err.h"
#include <stdbool.h>

#define MAX_WIFI_SSID_LEN 32
#define MAX_WIFI_PASSWORD_LEN 64

esp_err_t module_wifi_init(void);
esp_err_t module_wifi_connect(const char *ssid, const char *password);
esp_err_t module_wifi_disconnect(void);
bool module_wifi_is_connected(void);