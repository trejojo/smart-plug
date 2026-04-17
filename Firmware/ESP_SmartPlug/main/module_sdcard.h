#pragma once

#include <stddef.h>

#include "esp_err.h"

#define MODULE_SDCARD_MOUNT_POINT "/sdcard"

esp_err_t module_sdcard_init(void);
esp_err_t module_sdcard_unmount(void);
esp_err_t module_sdcard_write_file(const char *path, const char *data);
esp_err_t module_sdcard_read_file(const char *path, char *buffer, size_t buffer_size);