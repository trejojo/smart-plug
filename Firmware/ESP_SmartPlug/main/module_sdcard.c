#include "module_sdcard.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>

#include "board_profile.h"
#include "driver/spi_common.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "SDCARD_MODULE";
static const smartplug_board_pins_t *s_pins;
static sdmmc_card_t *s_card;
static spi_host_device_t s_spi_host;
static bool s_initialized;

esp_err_t module_sdcard_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_pins = smartplug_board_pins_get();
    ESP_RETURN_ON_FALSE(s_pins != NULL, ESP_ERR_INVALID_STATE, TAG, "board profile not available");

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = s_pins->sd_spi_host;
    s_spi_host = host.slot;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = s_pins->sd_mosi_gpio,
        .miso_io_num = s_pins->sd_miso_gpio,
        .sclk_io_num = s_pins->sd_sclk_gpio,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    esp_err_t ret = spi_bus_initialize(s_spi_host, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return ret;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = s_pins->sd_cs_gpio;
    slot_config.host_id = host.slot;

    ret = esp_vfs_fat_sdspi_mount(MODULE_SDCARD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_fat_sdspi_mount failed: %s", esp_err_to_name(ret));
        spi_bus_free(s_spi_host);
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "SD card mounted on host %d (CS:%d MOSI:%d MISO:%d SCLK:%d)",
             s_spi_host,
             s_pins->sd_cs_gpio,
             s_pins->sd_mosi_gpio,
             s_pins->sd_miso_gpio,
             s_pins->sd_sclk_gpio);
    sdmmc_card_print_info(stdout, s_card);

    return ESP_OK;
}

esp_err_t module_sdcard_unmount(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    esp_vfs_fat_sdcard_unmount(MODULE_SDCARD_MOUNT_POINT, s_card);
    spi_bus_free(s_spi_host);

    s_card = NULL;
    s_pins = NULL;
    s_initialized = false;

    return ESP_OK;
}

esp_err_t module_sdcard_write_file(const char *path, const char *data)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "SD card not initialized");
    ESP_RETURN_ON_FALSE(path != NULL, ESP_ERR_INVALID_ARG, TAG, "path is null");
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_INVALID_ARG, TAG, "data is null");

    FILE *file = fopen(path, "w");
    ESP_RETURN_ON_FALSE(file != NULL, ESP_FAIL, TAG, "failed to open %s for writing", path);

    int written = fprintf(file, "%s", data);
    fclose(file);

    ESP_RETURN_ON_FALSE(written >= 0, ESP_FAIL, TAG, "failed to write %s", path);
    return ESP_OK;
}

esp_err_t module_sdcard_read_file(const char *path, char *buffer, size_t buffer_size)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "SD card not initialized");
    ESP_RETURN_ON_FALSE(path != NULL, ESP_ERR_INVALID_ARG, TAG, "path is null");
    ESP_RETURN_ON_FALSE(buffer != NULL, ESP_ERR_INVALID_ARG, TAG, "buffer is null");
    ESP_RETURN_ON_FALSE(buffer_size > 0, ESP_ERR_INVALID_ARG, TAG, "buffer too small");

    FILE *file = fopen(path, "r");
    ESP_RETURN_ON_FALSE(file != NULL, ESP_FAIL, TAG, "failed to open %s for reading", path);

    size_t bytes_read = fread(buffer, 1, buffer_size - 1, file);
    bool read_error = ferror(file);
    fclose(file);

    ESP_RETURN_ON_FALSE(!read_error, ESP_FAIL, TAG, "failed to read %s", path);

    buffer[bytes_read] = '\0';
    return ESP_OK;
}
