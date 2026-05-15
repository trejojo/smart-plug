# ADE7953 ESP-IDF remake for Smart Plug

This package is a pure ESP-IDF C remake of the ADE7953 library for an ESP32-S3 WROOM-1U N16R8. It does not use Arduino libraries.

## Files

```text
main/module_ade7953.h      Public API, register map, structures and helpers
main/module_ade7953.c      SPI driver, init, measurements, events, calibration and safety helper
main/board_profile.h       Board profile extended with ADE7953 pins
main/board_profile.c       Custom PCB and DevKit ADE7953 pin assignment
main/CMakeLists.txt        Adds module_ade7953.c and ESP-IDF dependencies
examples/main_ade7953_integration.c
                           Example of how to integrate with your current app_main()
docs/ADE7953_calibration_notes.md
                           Practical calibration workflow
```

## ADE7953 pins added to board_profile

```c
spi_host_device_t ade_spi_host;
gpio_num_t ade_cs_gpio;
gpio_num_t ade_mosi_gpio;
gpio_num_t ade_sclk_gpio;
gpio_num_t ade_miso_gpio;
gpio_num_t ade_reset_gpio;
gpio_num_t ade_irq_gpio;
```

Default custom PCB assignment included:

```c
.ade_spi_host  = SPI2_HOST,
.ade_cs_gpio   = GPIO_NUM_10,
.ade_mosi_gpio = GPIO_NUM_11,
.ade_sclk_gpio = GPIO_NUM_12,
.ade_miso_gpio = GPIO_NUM_13,
.ade_reset_gpio = GPIO_NUM_14,
.ade_irq_gpio   = GPIO_NUM_21,
```

## Minimal app_main integration

```c
#include "module_ade7953.h"

ESP_ERROR_CHECK(module_ade7953_init());
ESP_ERROR_CHECK(module_ade7953_set_hpf(true));
ESP_ERROR_CHECK(module_ade7953_set_integrators(false, false));
ESP_ERROR_CHECK(module_ade7953_set_pga(ADE7953_PGA_GAIN_1,
                                       ADE7953_PGA_GAIN_1,
                                       ADE7953_PGA_GAIN_1));
ESP_ERROR_CHECK(module_ade7953_set_zero_cross_timeout_ms(100.0f));
ESP_ERROR_CHECK(module_ade7953_enable_default_power_quality_irqs());

while (true) {
    ade7953_measurement_t m;
    if (module_ade7953_read_measurement(&m, true, true) == ESP_OK) {
        ESP_LOGI("app", "VRMS raw=%lu IRMSA raw=%lu AWATT raw=%ld f=%.2f Hz PF=%.3f",
                 (unsigned long)m.raw.vrms,
                 (unsigned long)m.raw.irmsa,
                 (long)m.raw.awatt,
                 m.line_frequency_hz,
                 m.power_factor_a);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
}
```

## Important behavior

- SPI mode is 0.
- Default SPI speed is 1 MHz for reliable bring-up through digital isolators.
- The driver applies the mandatory `0xFE = 0xAD` then `0x120 = 0x0030` sequence.
- The driver can lock the ADE7953 communication interface to SPI.
- Raw measurements work before calibration.
- Engineering-unit fields remain zero until calibration constants are set.
- Energy deltas are read from ADE7953 energy registers. With default `LCYCMODE.RSTREAD = 1`, reading these registers resets them, and the driver accumulates totals in RAM.

## Main APIs

- `module_ade7953_init()`
- `module_ade7953_read_measurement()`
- `module_ade7953_read_events()`
- `module_ade7953_set_pga()`
- `module_ade7953_set_zero_cross_timeout_ms()`
- `module_ade7953_set_sag_threshold_raw()`
- `module_ade7953_set_overvoltage_overcurrent_raw()`
- `module_ade7953_enable_default_power_quality_irqs()`
- `module_ade7953_evaluate_safety()`

## Product-level note

For a real smart plug, do not rely only on firmware logic. Use appropriate fuse, relay rating, creepage/clearance, thermal design, enclosure safety, isolation validation and production calibration. The ADE7953 can detect power-quality events, but the relay-opening decision must be validated under actual load types, including inductive and nonlinear loads.
