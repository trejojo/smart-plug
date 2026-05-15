#include "board_profile.h"

#include "sdkconfig.h"

__attribute__((unused)) static const smartplug_board_pins_t s_devkit_pins = {
    .relay_gpio = GPIO_NUM_2,
    .rgb_led_gpio = GPIO_NUM_38,
    .tmp102_i2c_port = I2C_NUM_0,
    .tmp102_sda_gpio = GPIO_NUM_17,
    .tmp102_scl_gpio = GPIO_NUM_18,
    .tmp102_i2c_addr = 0x48,
    .sd_spi_host = SPI3_HOST,
    .sd_cs_gpio = GPIO_NUM_4,
    .sd_mosi_gpio = GPIO_NUM_5,
    .sd_sclk_gpio = GPIO_NUM_6,
    .sd_miso_gpio = GPIO_NUM_7,
    .pzem_uart_port = UART_NUM_1,
    .pzem_tx_gpio = GPIO_NUM_16,
    .pzem_rx_gpio = GPIO_NUM_15,
    .pzem_dir_gpio = GPIO_NUM_NC,
    .pzem_uart_baud_rate = 9600,
    .pzem_default_slave_addr = 0x01,
};

static const smartplug_board_pins_t s_custom_pcb_pins = {
    .relay_gpio = GPIO_NUM_15,
    .rgb_led_gpio = GPIO_NUM_16,
    .tmp102_i2c_port = I2C_NUM_0,
    .tmp102_sda_gpio = GPIO_NUM_17,
    .tmp102_scl_gpio = GPIO_NUM_18,
    .tmp102_i2c_addr = 0x48,
    .sd_spi_host = SPI3_HOST,
    .sd_cs_gpio = GPIO_NUM_4,
    .sd_mosi_gpio = GPIO_NUM_5,
    .sd_sclk_gpio = GPIO_NUM_6,
    .sd_miso_gpio = GPIO_NUM_7,
    .setup_bt_button = GPIO_NUM_8,
    // Ade library is not yet integrated, so these pins are reserved for future use and not currently defined in the board profile
    /*
    .ade_spi_host = SPI2_HOST,
    .ade_cs_gpio = GPIO_NUM_10,
    .ade_mosi_gpio = GPIO_NUM_11,
    .ade_sclk_gpio = GPIO_NUM_12,
    .ade_miso_gpio = GPIO_NUM_13,
    .ade_reset_gpio = GPIO_NUM_14,
    .ade_irq_gpio = GPIO_NUM_21,
    */
};

const smartplug_board_pins_t *smartplug_board_pins_get(void)
{
#if CONFIG_SMARTPLUG_BOARD_PROFILE_DEVKITC1
    return &s_devkit_pins;
#elif CONFIG_SMARTPLUG_BOARD_PROFILE_CUSTOM_PCB
    return &s_custom_pcb_pins;
#else
    return &s_devkit_pins;
#endif
}

const char *smartplug_board_profile_name(void)
{
#if CONFIG_SMARTPLUG_BOARD_PROFILE_DEVKITC1
    return "ESP32-S3-DevKitC-1";
#elif CONFIG_SMARTPLUG_BOARD_PROFILE_CUSTOM_PCB
    return "SmartPlug Custom PCB";
#else
    return "Unknown";
#endif
}
