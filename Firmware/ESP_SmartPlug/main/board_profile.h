#pragma once

#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/spi_common.h"
#include "driver/uart.h"

typedef struct {
    gpio_num_t relay_gpio;
    gpio_num_t rgb_led_gpio;

    i2c_port_t tmp102_i2c_port;
    gpio_num_t tmp102_sda_gpio;
    gpio_num_t tmp102_scl_gpio;
    uint8_t tmp102_i2c_addr;

    spi_host_device_t sd_spi_host;
    gpio_num_t sd_cs_gpio;
    gpio_num_t sd_mosi_gpio;
    gpio_num_t sd_sclk_gpio;
    gpio_num_t sd_miso_gpio;

    uart_port_t pzem_uart_port;
    gpio_num_t pzem_tx_gpio;
    gpio_num_t pzem_rx_gpio;
    gpio_num_t pzem_dir_gpio;
    uint32_t pzem_uart_baud_rate;
    uint8_t pzem_default_slave_addr;
} smartplug_board_pins_t;

const smartplug_board_pins_t *smartplug_board_pins_get(void);
const char *smartplug_board_profile_name(void);
