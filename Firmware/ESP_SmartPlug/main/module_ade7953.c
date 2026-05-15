#include "module_ade7953.h"

#include <math.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_profile.h"

#define ADE7953_SPI_MODE                (0)
#define ADE7953_SPI_TRANS_MAX_BYTES     (7)  /* 2 addr + 1 R/W + 4 data */
#define ADE7953_REQUIRED_REG_UNLOCK     (0xAD)
#define ADE7953_REQUIRED_REG_VALUE      (0x0030)
#define ADE7953_DEFAULT_READY_WAIT_MS   (120)
#define ADE7953_RESET_LOW_US            (20)
#define ADE7953_ENERGY_DEFAULT_RSTREAD  (ADE7953_LCYCMODE_RSTREAD)
#define ADE7953_RAW24_MASK              (0x00FFFFFFUL)
#define ADE7953_GAIN_REG_NOMINAL        (0x400000UL)
#define ADE7953_GAIN_REG_MIN            (0x200000UL)
#define ADE7953_GAIN_REG_MAX            (0x600000UL)

static const char *TAG = "ade7953";

static spi_device_handle_t s_spi = NULL;
static ade7953_config_t s_cfg = {0};
static ade7953_calibration_t s_cal = {0};
static bool s_initialized = false;

static float s_total_active_energy_a_wh = 0.0f;
static float s_total_active_energy_b_wh = 0.0f;
static float s_total_reactive_energy_a_varh = 0.0f;
static float s_total_reactive_energy_b_varh = 0.0f;
static float s_total_apparent_energy_a_vah = 0.0f;
static float s_total_apparent_energy_b_vah = 0.0f;

static const uint32_t ADE7953_SAFE_IRQ_A_MASK =
    ADE7953_IRQ_A_SAG |
    ADE7953_IRQ_A_OV |
    ADE7953_IRQ_A_OIA |
    ADE7953_IRQ_A_CRC;

static const uint32_t ADE7953_SAFE_IRQ_B_MASK =
    ADE7953_IRQ_B_OIB;

static esp_err_t ensure_initialized(void)
{
    return s_initialized && s_spi ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t spi_read_bytes(uint16_t reg, uint8_t *data, size_t len)
{
    esp_err_t err = ensure_initialized();
    if (err != ESP_OK) {
        return err;
    }
    if (data == NULL || len == 0 || len > 4) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t tx[ADE7953_SPI_TRANS_MAX_BYTES] = {0};
    uint8_t rx[ADE7953_SPI_TRANS_MAX_BYTES] = {0};

    tx[0] = (uint8_t)((reg >> 8) & 0xFF);
    tx[1] = (uint8_t)(reg & 0xFF);
    tx[2] = 0x80; /* Read command: MSB = 1 */

    spi_transaction_t transaction = {0};
    transaction.length = (3 + len) * 8;
    transaction.tx_buffer = tx;
    transaction.rx_buffer = rx;

    gpio_set_level(s_cfg.cs_gpio, 0);
    err = spi_device_polling_transmit(s_spi, &transaction);
    esp_rom_delay_us(2);
    gpio_set_level(s_cfg.cs_gpio, 1);
    if (err != ESP_OK) {
        return err;
    }

    memcpy(data, &rx[3], len);
    return ESP_OK;
}

static esp_err_t spi_write_bytes(uint16_t reg, const uint8_t *data, size_t len)
{
    esp_err_t err = ensure_initialized();
    if (err != ESP_OK) {
        return err;
    }
    if (data == NULL || len == 0 || len > 4) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t tx[ADE7953_SPI_TRANS_MAX_BYTES] = {0};
    tx[0] = (uint8_t)((reg >> 8) & 0xFF);
    tx[1] = (uint8_t)(reg & 0xFF);
    tx[2] = 0x00; /* Write command: MSB = 0 */
    memcpy(&tx[3], data, len);

    spi_transaction_t transaction = {0};
    transaction.length = (3 + len) * 8;
    transaction.tx_buffer = tx;

    gpio_set_level(s_cfg.cs_gpio, 0);
    err = spi_device_polling_transmit(s_spi, &transaction);
    esp_rom_delay_us(2);
    gpio_set_level(s_cfg.cs_gpio, 1);
    return err;
}

static uint32_t clamp_u32(uint32_t value, uint32_t lo, uint32_t hi)
{
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

static uint32_t scale_gain_register(uint32_t current_reg, float reference, float measured)
{
    if (measured <= 0.0f || reference <= 0.0f) {
        return current_reg;
    }
    double next = (double)current_reg * ((double)reference / (double)measured);
    if (next < (double)ADE7953_GAIN_REG_MIN) {
        next = (double)ADE7953_GAIN_REG_MIN;
    }
    if (next > (double)ADE7953_GAIN_REG_MAX) {
        next = (double)ADE7953_GAIN_REG_MAX;
    }
    return (uint32_t)(next + 0.5);
}

esp_err_t module_ade7953_get_default_config(ade7953_config_t *out_config)
{
    if (out_config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const smartplug_board_pins_t *pins = smartplug_board_pins_get();
    memset(out_config, 0, sizeof(*out_config));

    out_config->spi_host = pins->ade_spi_host;
    out_config->cs_gpio = pins->ade_cs_gpio;
    out_config->mosi_gpio = pins->ade_mosi_gpio;
    out_config->miso_gpio = pins->ade_miso_gpio;
    out_config->sclk_gpio = pins->ade_sclk_gpio;
    out_config->reset_gpio = pins->ade_reset_gpio;
    out_config->irq_gpio = pins->ade_irq_gpio;
    out_config->spi_clock_hz = ADE7953_SPI_DEFAULT_HZ;
    out_config->use_hw_reset = true;
    out_config->wait_for_reset_irq = true;
    out_config->apply_required_settings = true;
    out_config->lock_spi_interface = true;

    return ESP_OK;
}

esp_err_t module_ade7953_init(void)
{
    ade7953_config_t config;
    ESP_RETURN_ON_ERROR(module_ade7953_get_default_config(&config), TAG, "default config failed");
    return module_ade7953_init_with_config(&config);
}

esp_err_t module_ade7953_init_with_config(const ade7953_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->cs_gpio == GPIO_NUM_NC || config->mosi_gpio == GPIO_NUM_NC ||
        config->miso_gpio == GPIO_NUM_NC || config->sclk_gpio == GPIO_NUM_NC) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_initialized) {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }

    s_cfg = *config;
    if (s_cfg.spi_clock_hz <= 0) {
        s_cfg.spi_clock_hz = ADE7953_SPI_DEFAULT_HZ;
    }
    if (s_cfg.spi_clock_hz > ADE7953_SPI_MAX_HZ) {
        ESP_LOGW(TAG, "SPI clock limited from %d Hz to %d Hz", s_cfg.spi_clock_hz, ADE7953_SPI_MAX_HZ);
        s_cfg.spi_clock_hz = ADE7953_SPI_MAX_HZ;
    }

    gpio_config_t cs_cfg = {
        .pin_bit_mask = 1ULL << s_cfg.cs_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cs_cfg), TAG, "cs gpio config failed");
    gpio_set_level(s_cfg.cs_gpio, 1);

    if (s_cfg.reset_gpio != GPIO_NUM_NC) {
        gpio_config_t reset_cfg = {
            .pin_bit_mask = 1ULL << s_cfg.reset_gpio,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&reset_cfg), TAG, "reset gpio config failed");
        gpio_set_level(s_cfg.reset_gpio, 1);
    }

    if (s_cfg.irq_gpio != GPIO_NUM_NC) {
        gpio_config_t irq_cfg = {
            .pin_bit_mask = 1ULL << s_cfg.irq_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&irq_cfg), TAG, "irq gpio config failed");
    }

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = s_cfg.mosi_gpio,
        .miso_io_num = s_cfg.miso_gpio,
        .sclk_io_num = s_cfg.sclk_gpio,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = ADE7953_SPI_TRANS_MAX_BYTES,
    };

    esp_err_t err = spi_bus_initialize(s_cfg.spi_host, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SPI bus already initialized; continuing");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = s_cfg.spi_clock_hz,
        .mode = ADE7953_SPI_MODE,
        .spics_io_num = -1, /* CS is controlled manually to satisfy COMM_LOCK timing. */
        .queue_size = 1,
    };

    ESP_RETURN_ON_ERROR(spi_bus_add_device(s_cfg.spi_host, &dev_cfg, &s_spi), TAG, "spi_bus_add_device failed");
    s_initialized = true;

    if (s_cfg.use_hw_reset) {
        ESP_RETURN_ON_ERROR(module_ade7953_hw_reset(), TAG, "hardware reset failed");
    } else {
        ESP_RETURN_ON_ERROR(module_ade7953_wait_ready(ADE7953_DEFAULT_READY_WAIT_MS), TAG, "ready wait failed");
    }

    if (s_cfg.apply_required_settings) {
        ESP_RETURN_ON_ERROR(module_ade7953_apply_required_settings(), TAG, "required settings failed");
    }

    /* Clear the reset interrupt latched during power-up/reset on both channels. */
    ade7953_events_t startup_events;
    if (module_ade7953_read_events(&startup_events, true) == ESP_OK && startup_events.reset_done) {
        ESP_LOGI(TAG, "Cleared ADE7953 power-up reset IRQ");
    }

    if (s_cfg.lock_spi_interface) {
        ESP_RETURN_ON_ERROR(module_ade7953_lock_spi_interface(), TAG, "SPI lock failed");
    }

    ESP_LOGI(TAG, "ADE7953 initialized on SPI host %d, CS=%d, SCLK=%d, MOSI=%d, MISO=%d, RESET=%d, IRQ=%d",
             s_cfg.spi_host, s_cfg.cs_gpio, s_cfg.sclk_gpio, s_cfg.mosi_gpio, s_cfg.miso_gpio,
             s_cfg.reset_gpio, s_cfg.irq_gpio);

    return ESP_OK;
}

esp_err_t module_ade7953_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    if (s_spi != NULL) {
        ESP_RETURN_ON_ERROR(spi_bus_remove_device(s_spi), TAG, "remove device failed");
        s_spi = NULL;
    }

    /* Do not call spi_bus_free() here because other modules may share the host in some variants. */
    s_initialized = false;
    return ESP_OK;
}

bool module_ade7953_is_initialized(void)
{
    return s_initialized;
}

esp_err_t module_ade7953_read8(uint16_t reg, uint8_t *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return spi_read_bytes(reg, value, 1);
}

esp_err_t module_ade7953_read16(uint16_t reg, uint16_t *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t data[2] = {0};
    ESP_RETURN_ON_ERROR(spi_read_bytes(reg, data, sizeof(data)), TAG, "read16 failed");
    *value = ((uint16_t)data[0] << 8) | (uint16_t)data[1];
    return ESP_OK;
}

esp_err_t module_ade7953_read24(uint16_t reg, uint32_t *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t data[3] = {0};
    ESP_RETURN_ON_ERROR(spi_read_bytes(reg, data, sizeof(data)), TAG, "read24 failed");
    *value = ((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 8) | (uint32_t)data[2];
    return ESP_OK;
}

esp_err_t module_ade7953_read32(uint16_t reg, uint32_t *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t data[4] = {0};
    ESP_RETURN_ON_ERROR(spi_read_bytes(reg, data, sizeof(data)), TAG, "read32 failed");
    *value = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
             ((uint32_t)data[2] << 8) | (uint32_t)data[3];
    return ESP_OK;
}

esp_err_t module_ade7953_read_s24(uint16_t reg, int32_t *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t raw = 0;
    ESP_RETURN_ON_ERROR(module_ade7953_read24(reg, &raw), TAG, "read_s24 failed");
    *value = module_ade7953_sign_extend(raw, 24);
    return ESP_OK;
}

esp_err_t module_ade7953_read_s32(uint16_t reg, int32_t *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t raw = 0;
    ESP_RETURN_ON_ERROR(module_ade7953_read32(reg, &raw), TAG, "read_s32 failed");
    *value = (int32_t)raw;
    return ESP_OK;
}

esp_err_t module_ade7953_write8(uint16_t reg, uint8_t value)
{
    return spi_write_bytes(reg, &value, 1);
}

esp_err_t module_ade7953_write16(uint16_t reg, uint16_t value)
{
    uint8_t data[2] = {
        (uint8_t)((value >> 8) & 0xFF),
        (uint8_t)(value & 0xFF),
    };
    return spi_write_bytes(reg, data, sizeof(data));
}

esp_err_t module_ade7953_write24(uint16_t reg, uint32_t value)
{
    value &= ADE7953_RAW24_MASK;
    uint8_t data[3] = {
        (uint8_t)((value >> 16) & 0xFF),
        (uint8_t)((value >> 8) & 0xFF),
        (uint8_t)(value & 0xFF),
    };
    return spi_write_bytes(reg, data, sizeof(data));
}

esp_err_t module_ade7953_write32(uint16_t reg, uint32_t value)
{
    uint8_t data[4] = {
        (uint8_t)((value >> 24) & 0xFF),
        (uint8_t)((value >> 16) & 0xFF),
        (uint8_t)((value >> 8) & 0xFF),
        (uint8_t)(value & 0xFF),
    };
    return spi_write_bytes(reg, data, sizeof(data));
}

esp_err_t module_ade7953_hw_reset(void)
{
    if (s_cfg.reset_gpio == GPIO_NUM_NC) {
        return module_ade7953_wait_ready(ADE7953_DEFAULT_READY_WAIT_MS);
    }

    gpio_set_level(s_cfg.reset_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(s_cfg.reset_gpio, 0);
    esp_rom_delay_us(ADE7953_RESET_LOW_US);
    gpio_set_level(s_cfg.reset_gpio, 1);

    return module_ade7953_wait_ready(ADE7953_DEFAULT_READY_WAIT_MS);
}

esp_err_t module_ade7953_wait_ready(uint32_t timeout_ms)
{
    if (!s_cfg.wait_for_reset_irq || s_cfg.irq_gpio == GPIO_NUM_NC) {
        vTaskDelay(pdMS_TO_TICKS(timeout_ms > 0 ? timeout_ms : ADE7953_DEFAULT_READY_WAIT_MS));
        return ESP_OK;
    }

    int64_t start_ms = esp_timer_get_time() / 1000;
    int64_t timeout = timeout_ms > 0 ? (int64_t)timeout_ms : ADE7953_DEFAULT_READY_WAIT_MS;
    while (((esp_timer_get_time() / 1000) - start_ms) < timeout) {
        if (gpio_get_level(s_cfg.irq_gpio) == 0) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    /* Fallback: the datasheet allows timeout-based first communication if IRQ is not used. */
    ESP_LOGW(TAG, "IRQ ready timeout; continuing after timeout-based wait");
    return ESP_OK;
}

esp_err_t module_ade7953_apply_required_settings(void)
{
    ESP_RETURN_ON_ERROR(module_ade7953_write8(ADE7953_REG_UNLOCK_120, ADE7953_REQUIRED_REG_UNLOCK), TAG, "unlock 0x120 failed");
    ESP_RETURN_ON_ERROR(module_ade7953_write16(ADE7953_REG_REQUIRED_120, ADE7953_REQUIRED_REG_VALUE), TAG, "write 0x120 failed");

    uint16_t verify = 0;
    ESP_RETURN_ON_ERROR(module_ade7953_read16(ADE7953_REG_REQUIRED_120, &verify), TAG, "verify 0x120 failed");
    if (verify != ADE7953_REQUIRED_REG_VALUE) {
        ESP_LOGE(TAG, "0x120 verify failed: got 0x%04X", verify);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t module_ade7953_lock_spi_interface(void)
{
    uint16_t config = 0;
    ESP_RETURN_ON_ERROR(module_ade7953_read16(ADE7953_REG_CONFIG, &config), TAG, "read CONFIG failed");
    config &= (uint16_t)~ADE7953_CONFIG_COMM_LOCK; /* 0 = lock enabled */
    ESP_RETURN_ON_ERROR(module_ade7953_write16(ADE7953_REG_CONFIG, config), TAG, "write CONFIG lock failed");

    uint16_t verify = 0;
    ESP_RETURN_ON_ERROR(module_ade7953_read16(ADE7953_REG_CONFIG, &verify), TAG, "verify CONFIG failed");
    return ((verify & ADE7953_CONFIG_COMM_LOCK) == 0U) ? ESP_OK : ESP_FAIL;
}

esp_err_t module_ade7953_software_reset(void)
{
    uint16_t config = 0;
    ESP_RETURN_ON_ERROR(module_ade7953_read16(ADE7953_REG_CONFIG, &config), TAG, "read CONFIG failed");
    config |= ADE7953_CONFIG_SWRST;
    ESP_RETURN_ON_ERROR(module_ade7953_write16(ADE7953_REG_CONFIG, config), TAG, "write SWRST failed");
    ESP_RETURN_ON_ERROR(module_ade7953_wait_ready(ADE7953_DEFAULT_READY_WAIT_MS), TAG, "wait SWRST failed");
    return module_ade7953_apply_required_settings();
}

esp_err_t module_ade7953_read_version(uint8_t *version)
{
    return module_ade7953_read8(ADE7953_REG_VERSION, version);
}

esp_err_t module_ade7953_read_crc(uint32_t *crc)
{
    return module_ade7953_read32(ADE7953_REG_CRC_32, crc);
}

void module_ade7953_set_calibration(const ade7953_calibration_t *cal)
{
    if (cal != NULL) {
        s_cal = *cal;
    }
}

void module_ade7953_get_calibration(ade7953_calibration_t *cal)
{
    if (cal != NULL) {
        *cal = s_cal;
    }
}

void module_ade7953_reset_energy_totals(void)
{
    s_total_active_energy_a_wh = 0.0f;
    s_total_active_energy_b_wh = 0.0f;
    s_total_reactive_energy_a_varh = 0.0f;
    s_total_reactive_energy_b_varh = 0.0f;
    s_total_apparent_energy_a_vah = 0.0f;
    s_total_apparent_energy_b_vah = 0.0f;
}

esp_err_t module_ade7953_calibrate_rms_from_reference(float reference_vrms,
                                                       float reference_iarms,
                                                       float reference_ibarms)
{
    ade7953_raw_measurement_t raw;
    ESP_RETURN_ON_ERROR(module_ade7953_read_raw_measurement(&raw, false, false), TAG, "read raw rms cal failed");

    if (reference_vrms > 0.0f && raw.vrms > 0U) {
        s_cal.volts_per_vrms_lsb = reference_vrms / (float)raw.vrms;
    }
    if (reference_iarms > 0.0f && raw.irmsa > 0U) {
        s_cal.amps_per_irmsa_lsb = reference_iarms / (float)raw.irmsa;
    }
    if (reference_ibarms > 0.0f && raw.irmsb > 0U) {
        s_cal.amps_per_irmsb_lsb = reference_ibarms / (float)raw.irmsb;
    }
    return ESP_OK;
}

esp_err_t module_ade7953_calibrate_power_from_reference(float reference_w_a,
                                                         float reference_w_b,
                                                         float reference_var_a,
                                                         float reference_var_b,
                                                         float reference_va_a,
                                                         float reference_va_b)
{
    ade7953_raw_measurement_t raw;
    ESP_RETURN_ON_ERROR(module_ade7953_read_raw_measurement(&raw, false, false), TAG, "read raw power cal failed");

    if (reference_w_a != 0.0f && raw.awatt != 0) {
        s_cal.watts_per_awatt_lsb = reference_w_a / (float)raw.awatt;
    }
    if (reference_w_b != 0.0f && raw.bwatt != 0) {
        s_cal.watts_per_bwatt_lsb = reference_w_b / (float)raw.bwatt;
    }
    if (reference_var_a != 0.0f && raw.avar != 0) {
        s_cal.vars_per_avar_lsb = reference_var_a / (float)raw.avar;
    }
    if (reference_var_b != 0.0f && raw.bvar != 0) {
        s_cal.vars_per_bvar_lsb = reference_var_b / (float)raw.bvar;
    }
    if (reference_va_a > 0.0f && raw.ava != 0) {
        s_cal.va_per_ava_lsb = reference_va_a / (float)raw.ava;
    }
    if (reference_va_b > 0.0f && raw.bva != 0) {
        s_cal.va_per_bva_lsb = reference_va_b / (float)raw.bva;
    }
    return ESP_OK;
}

esp_err_t module_ade7953_read_raw_measurement(ade7953_raw_measurement_t *out_raw,
                                              bool read_energy_deltas,
                                              bool clear_irq_status)
{
    if (out_raw == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_raw, 0, sizeof(*out_raw));

    uint32_t u32 = 0;
    int32_t s32 = 0;
    uint16_t u16 = 0;

    ESP_RETURN_ON_ERROR(module_ade7953_read32(ADE7953_REG_VRMS_32, &u32), TAG, "VRMS read failed");
    out_raw->vrms = u32 & ADE7953_RAW24_MASK;
    ESP_RETURN_ON_ERROR(module_ade7953_read32(ADE7953_REG_IRMSA_32, &u32), TAG, "IRMSA read failed");
    out_raw->irmsa = u32 & ADE7953_RAW24_MASK;
    ESP_RETURN_ON_ERROR(module_ade7953_read32(ADE7953_REG_IRMSB_32, &u32), TAG, "IRMSB read failed");
    out_raw->irmsb = u32 & ADE7953_RAW24_MASK;

    ESP_RETURN_ON_ERROR(module_ade7953_read_s32(ADE7953_REG_V_32, &s32), TAG, "V waveform read failed");
    out_raw->v_waveform = s32;
    ESP_RETURN_ON_ERROR(module_ade7953_read_s32(ADE7953_REG_IA_32, &s32), TAG, "IA waveform read failed");
    out_raw->ia_waveform = s32;
    ESP_RETURN_ON_ERROR(module_ade7953_read_s32(ADE7953_REG_IB_32, &s32), TAG, "IB waveform read failed");
    out_raw->ib_waveform = s32;

    ESP_RETURN_ON_ERROR(module_ade7953_read_s32(ADE7953_REG_AWATT_32, &s32), TAG, "AWATT read failed");
    out_raw->awatt = s32;
    ESP_RETURN_ON_ERROR(module_ade7953_read_s32(ADE7953_REG_BWATT_32, &s32), TAG, "BWATT read failed");
    out_raw->bwatt = s32;
    ESP_RETURN_ON_ERROR(module_ade7953_read_s32(ADE7953_REG_AVAR_32, &s32), TAG, "AVAR read failed");
    out_raw->avar = s32;
    ESP_RETURN_ON_ERROR(module_ade7953_read_s32(ADE7953_REG_BVAR_32, &s32), TAG, "BVAR read failed");
    out_raw->bvar = s32;
    ESP_RETURN_ON_ERROR(module_ade7953_read_s32(ADE7953_REG_AVA_32, &s32), TAG, "AVA read failed");
    out_raw->ava = s32;
    ESP_RETURN_ON_ERROR(module_ade7953_read_s32(ADE7953_REG_BVA_32, &s32), TAG, "BVA read failed");
    out_raw->bva = s32;

    if (read_energy_deltas) {
        ESP_RETURN_ON_ERROR(module_ade7953_read_s32(ADE7953_REG_AENERGYA_32, &s32), TAG, "AENERGYA read failed");
        out_raw->aenergy_a_delta = s32;
        ESP_RETURN_ON_ERROR(module_ade7953_read_s32(ADE7953_REG_AENERGYB_32, &s32), TAG, "AENERGYB read failed");
        out_raw->aenergy_b_delta = s32;
        ESP_RETURN_ON_ERROR(module_ade7953_read_s32(ADE7953_REG_RENERGYA_32, &s32), TAG, "RENERGYA read failed");
        out_raw->renergy_a_delta = s32;
        ESP_RETURN_ON_ERROR(module_ade7953_read_s32(ADE7953_REG_RENERGYB_32, &s32), TAG, "RENERGYB read failed");
        out_raw->renergy_b_delta = s32;
        ESP_RETURN_ON_ERROR(module_ade7953_read_s32(ADE7953_REG_APENERGYA_32, &s32), TAG, "APENERGYA read failed");
        out_raw->apenergy_a_delta = s32;
        ESP_RETURN_ON_ERROR(module_ade7953_read_s32(ADE7953_REG_APENERGYB_32, &s32), TAG, "APENERGYB read failed");
        out_raw->apenergy_b_delta = s32;
    }

    ESP_RETURN_ON_ERROR(module_ade7953_read32(ADE7953_REG_VPEAK_32, &u32), TAG, "VPEAK read failed");
    out_raw->vpeak = u32 & ADE7953_RAW24_MASK;
    ESP_RETURN_ON_ERROR(module_ade7953_read32(ADE7953_REG_IAPEAK_32, &u32), TAG, "IAPEAK read failed");
    out_raw->iapeak = u32 & ADE7953_RAW24_MASK;
    ESP_RETURN_ON_ERROR(module_ade7953_read32(ADE7953_REG_IBPEAK_32, &u32), TAG, "IBPEAK read failed");
    out_raw->ibpeak = u32 & ADE7953_RAW24_MASK;

    ESP_RETURN_ON_ERROR(module_ade7953_read16(ADE7953_REG_PERIOD, &u16), TAG, "PERIOD read failed");
    out_raw->period = u16;
    ESP_RETURN_ON_ERROR(module_ade7953_read16(ADE7953_REG_PFA, &u16), TAG, "PFA read failed");
    out_raw->pfa = (int16_t)u16;
    ESP_RETURN_ON_ERROR(module_ade7953_read16(ADE7953_REG_PFB, &u16), TAG, "PFB read failed");
    out_raw->pfb = (int16_t)u16;
    ESP_RETURN_ON_ERROR(module_ade7953_read16(ADE7953_REG_ANGLE_A, &u16), TAG, "ANGLE_A read failed");
    out_raw->angle_a = (int16_t)u16;
    ESP_RETURN_ON_ERROR(module_ade7953_read16(ADE7953_REG_ANGLE_B, &u16), TAG, "ANGLE_B read failed");
    out_raw->angle_b = (int16_t)u16;

    ESP_RETURN_ON_ERROR(module_ade7953_read32(clear_irq_status ? ADE7953_REG_RSTIRQSTATA_32 : ADE7953_REG_IRQSTATA_32, &u32), TAG, "IRQ A read failed");
    out_raw->irq_a = u32 & ADE7953_RAW24_MASK;
    ESP_RETURN_ON_ERROR(module_ade7953_read32(clear_irq_status ? ADE7953_REG_RSTIRQSTATB_32 : ADE7953_REG_IRQSTATB_32, &u32), TAG, "IRQ B read failed");
    out_raw->irq_b = u32 & ADE7953_RAW24_MASK;
    ESP_RETURN_ON_ERROR(module_ade7953_read32(ADE7953_REG_ACCMODE_32, &u32), TAG, "ACCMODE read failed");
    out_raw->accmode = u32 & ADE7953_RAW24_MASK;

    return ESP_OK;
}

esp_err_t module_ade7953_read_measurement(ade7953_measurement_t *out_measurement,
                                          bool read_energy_deltas,
                                          bool clear_irq_status)
{
    if (out_measurement == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_measurement, 0, sizeof(*out_measurement));

    out_measurement->timestamp_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    ESP_RETURN_ON_ERROR(module_ade7953_read_raw_measurement(&out_measurement->raw, read_energy_deltas, clear_irq_status),
                        TAG, "raw measurement failed");

    const ade7953_raw_measurement_t *r = &out_measurement->raw;

    out_measurement->voltage_vrms = (float)r->vrms * s_cal.volts_per_vrms_lsb;
    out_measurement->current_a_arms = (float)r->irmsa * s_cal.amps_per_irmsa_lsb;
    out_measurement->current_b_arms = (float)r->irmsb * s_cal.amps_per_irmsb_lsb;
    out_measurement->active_power_a_w = (float)r->awatt * s_cal.watts_per_awatt_lsb;
    out_measurement->active_power_b_w = (float)r->bwatt * s_cal.watts_per_bwatt_lsb;
    out_measurement->reactive_power_a_var = (float)r->avar * s_cal.vars_per_avar_lsb;
    out_measurement->reactive_power_b_var = (float)r->bvar * s_cal.vars_per_bvar_lsb;
    out_measurement->apparent_power_a_va = (float)r->ava * s_cal.va_per_ava_lsb;
    out_measurement->apparent_power_b_va = (float)r->bva * s_cal.va_per_bva_lsb;
    out_measurement->power_factor_a = module_ade7953_pf_raw_to_float(r->pfa);
    out_measurement->power_factor_b = module_ade7953_pf_raw_to_float(r->pfb);
    out_measurement->line_frequency_hz = module_ade7953_period_raw_to_hz(r->period);
    out_measurement->angle_a_deg = module_ade7953_angle_raw_to_degrees(r->angle_a, out_measurement->line_frequency_hz);
    out_measurement->angle_b_deg = module_ade7953_angle_raw_to_degrees(r->angle_b, out_measurement->line_frequency_hz);

    out_measurement->active_energy_a_wh_delta = (float)r->aenergy_a_delta * s_cal.wh_per_aenergy_a_lsb;
    out_measurement->active_energy_b_wh_delta = (float)r->aenergy_b_delta * s_cal.wh_per_aenergy_b_lsb;
    out_measurement->reactive_energy_a_varh_delta = (float)r->renergy_a_delta * s_cal.varh_per_renergy_a_lsb;
    out_measurement->reactive_energy_b_varh_delta = (float)r->renergy_b_delta * s_cal.varh_per_renergy_b_lsb;
    out_measurement->apparent_energy_a_vah_delta = (float)r->apenergy_a_delta * s_cal.vah_per_apenergy_a_lsb;
    out_measurement->apparent_energy_b_vah_delta = (float)r->apenergy_b_delta * s_cal.vah_per_apenergy_b_lsb;

    s_total_active_energy_a_wh += out_measurement->active_energy_a_wh_delta;
    s_total_active_energy_b_wh += out_measurement->active_energy_b_wh_delta;
    s_total_reactive_energy_a_varh += out_measurement->reactive_energy_a_varh_delta;
    s_total_reactive_energy_b_varh += out_measurement->reactive_energy_b_varh_delta;
    s_total_apparent_energy_a_vah += out_measurement->apparent_energy_a_vah_delta;
    s_total_apparent_energy_b_vah += out_measurement->apparent_energy_b_vah_delta;

    out_measurement->active_energy_a_wh_total = s_total_active_energy_a_wh;
    out_measurement->active_energy_b_wh_total = s_total_active_energy_b_wh;
    out_measurement->reactive_energy_a_varh_total = s_total_reactive_energy_a_varh;
    out_measurement->reactive_energy_b_varh_total = s_total_reactive_energy_b_varh;
    out_measurement->apparent_energy_a_vah_total = s_total_apparent_energy_a_vah;
    out_measurement->apparent_energy_b_vah_total = s_total_apparent_energy_b_vah;

    out_measurement->reverse_active_a = (r->accmode & ADE7953_ACCMODE_APSIGN_A) != 0;
    out_measurement->reverse_active_b = (r->accmode & ADE7953_ACCMODE_APSIGN_B) != 0;
    out_measurement->reverse_reactive_a = (r->accmode & ADE7953_ACCMODE_VARSIGN_A) != 0;
    out_measurement->reverse_reactive_b = (r->accmode & ADE7953_ACCMODE_VARSIGN_B) != 0;
    out_measurement->no_load_active_a = (r->accmode & ADE7953_ACCMODE_ACTNLOAD_A) != 0;
    out_measurement->no_load_active_b = (r->accmode & ADE7953_ACCMODE_ACTNLOAD_B) != 0;
    out_measurement->no_load_reactive_a = (r->accmode & ADE7953_ACCMODE_VARNLOAD_A) != 0;
    out_measurement->no_load_reactive_b = (r->accmode & ADE7953_ACCMODE_VARNLOAD_B) != 0;
    out_measurement->no_load_apparent_a = (r->accmode & ADE7953_ACCMODE_VANLOAD_A) != 0;
    out_measurement->no_load_apparent_b = (r->accmode & ADE7953_ACCMODE_VANLOAD_B) != 0;

    return ESP_OK;
}

esp_err_t module_ade7953_read_events(ade7953_events_t *out_events, bool clear_latched)
{
    if (out_events == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_events, 0, sizeof(*out_events));

    uint32_t irq_a = 0;
    uint32_t irq_b = 0;
    uint32_t accmode = 0;

    ESP_RETURN_ON_ERROR(module_ade7953_read32(clear_latched ? ADE7953_REG_RSTIRQSTATA_32 : ADE7953_REG_IRQSTATA_32, &irq_a), TAG, "read IRQ A failed");
    ESP_RETURN_ON_ERROR(module_ade7953_read32(clear_latched ? ADE7953_REG_RSTIRQSTATB_32 : ADE7953_REG_IRQSTATB_32, &irq_b), TAG, "read IRQ B failed");
    ESP_RETURN_ON_ERROR(module_ade7953_read32(ADE7953_REG_ACCMODE_32, &accmode), TAG, "read ACCMODE failed");

    irq_a &= ADE7953_RAW24_MASK;
    irq_b &= ADE7953_RAW24_MASK;
    accmode &= ADE7953_RAW24_MASK;

    out_events->irq_a = irq_a;
    out_events->irq_b = irq_b;
    out_events->accmode = accmode;

    out_events->reset_done = (irq_a & ADE7953_IRQ_A_RESET) != 0;
    out_events->sag = (irq_a & ADE7953_IRQ_A_SAG) != 0;
    out_events->overvoltage = (irq_a & ADE7953_IRQ_A_OV) != 0;
    out_events->overcurrent_a = (irq_a & ADE7953_IRQ_A_OIA) != 0;
    out_events->overcurrent_b = (irq_b & ADE7953_IRQ_B_OIB) != 0;
    out_events->zero_cross_timeout_voltage = (irq_a & ADE7953_IRQ_A_ZXTO) != 0;
    out_events->zero_cross_timeout_current_a = (irq_a & ADE7953_IRQ_A_ZXTO_IA) != 0;
    out_events->zero_cross_timeout_current_b = (irq_b & ADE7953_IRQ_B_ZXTO_IB) != 0;
    out_events->zero_cross_voltage = (irq_a & ADE7953_IRQ_A_ZXV) != 0;
    out_events->zero_cross_current_a = (irq_a & ADE7953_IRQ_A_ZXIA) != 0;
    out_events->zero_cross_current_b = (irq_b & ADE7953_IRQ_B_ZXIB) != 0;
    out_events->waveform_sample_ready = (irq_a & ADE7953_IRQ_A_WSMP) != 0;
    out_events->line_cycle_end = (irq_a & ADE7953_IRQ_A_CYCEND) != 0;
    out_events->crc_changed = (irq_a & ADE7953_IRQ_A_CRC) != 0;
    out_events->active_noload_a = (irq_a & ADE7953_IRQ_A_AP_NOLOADA) != 0;
    out_events->active_noload_b = (irq_b & ADE7953_IRQ_B_AP_NOLOADB) != 0;
    out_events->reactive_noload_a = (irq_a & ADE7953_IRQ_A_VAR_NOLOADA) != 0;
    out_events->reactive_noload_b = (irq_b & ADE7953_IRQ_B_VAR_NOLOADB) != 0;
    out_events->apparent_noload_a = (irq_a & ADE7953_IRQ_A_VA_NOLOADA) != 0;
    out_events->apparent_noload_b = (irq_b & ADE7953_IRQ_B_VA_NOLOADB) != 0;
    out_events->active_sign_changed_a = (irq_a & ADE7953_IRQ_A_APSIGN_A) != 0;
    out_events->active_sign_changed_b = (irq_b & ADE7953_IRQ_B_APSIGN_B) != 0;
    out_events->reactive_sign_changed_a = (irq_a & ADE7953_IRQ_A_VARSIGN_A) != 0;
    out_events->reactive_sign_changed_b = (irq_b & ADE7953_IRQ_B_VARSIGN_B) != 0;
    out_events->reverse_active_a = (accmode & ADE7953_ACCMODE_APSIGN_A) != 0;
    out_events->reverse_active_b = (accmode & ADE7953_ACCMODE_APSIGN_B) != 0;
    out_events->reverse_reactive_a = (accmode & ADE7953_ACCMODE_VARSIGN_A) != 0;
    out_events->reverse_reactive_b = (accmode & ADE7953_ACCMODE_VARSIGN_B) != 0;

    return ESP_OK;
}

esp_err_t module_ade7953_read_peaks(uint32_t *vpeak, uint32_t *iapeak, uint32_t *ibpeak,
                                    bool reset_after_read)
{
    uint32_t raw = 0;
    if (vpeak != NULL) {
        ESP_RETURN_ON_ERROR(module_ade7953_read32(reset_after_read ? ADE7953_REG_RSTVPEAK_32 : ADE7953_REG_VPEAK_32, &raw), TAG, "read V peak failed");
        *vpeak = raw & ADE7953_RAW24_MASK;
    }
    if (iapeak != NULL) {
        ESP_RETURN_ON_ERROR(module_ade7953_read32(reset_after_read ? ADE7953_REG_RSTIAPEAK_32 : ADE7953_REG_IAPEAK_32, &raw), TAG, "read IA peak failed");
        *iapeak = raw & ADE7953_RAW24_MASK;
    }
    if (ibpeak != NULL) {
        ESP_RETURN_ON_ERROR(module_ade7953_read32(reset_after_read ? ADE7953_REG_RSTIBPEAK_32 : ADE7953_REG_IBPEAK_32, &raw), TAG, "read IB peak failed");
        *ibpeak = raw & ADE7953_RAW24_MASK;
    }
    return ESP_OK;
}

esp_err_t module_ade7953_set_pga(ade7953_pga_gain_t voltage_gain,
                                 ade7953_pga_gain_t current_a_gain,
                                 ade7953_pga_gain_t current_b_gain)
{
    if (voltage_gain == ADE7953_PGA_GAIN_22 || current_b_gain == ADE7953_PGA_GAIN_22) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(module_ade7953_write8(ADE7953_REG_PGA_V, (uint8_t)voltage_gain), TAG, "write PGA_V failed");
    ESP_RETURN_ON_ERROR(module_ade7953_write8(ADE7953_REG_PGA_IA, (uint8_t)current_a_gain), TAG, "write PGA_IA failed");
    ESP_RETURN_ON_ERROR(module_ade7953_write8(ADE7953_REG_PGA_IB, (uint8_t)current_b_gain), TAG, "write PGA_IB failed");
    return ESP_OK;
}

esp_err_t module_ade7953_set_integrators(bool enable_current_a, bool enable_current_b)
{
    uint16_t config = 0;
    ESP_RETURN_ON_ERROR(module_ade7953_read16(ADE7953_REG_CONFIG, &config), TAG, "read CONFIG failed");
    if (enable_current_a) {
        config |= ADE7953_CONFIG_INTENA;
    } else {
        config &= (uint16_t)~ADE7953_CONFIG_INTENA;
    }
    if (enable_current_b) {
        config |= ADE7953_CONFIG_INTENB;
    } else {
        config &= (uint16_t)~ADE7953_CONFIG_INTENB;
    }
    return module_ade7953_write16(ADE7953_REG_CONFIG, config);
}

esp_err_t module_ade7953_set_hpf(bool enable)
{
    uint16_t config = 0;
    ESP_RETURN_ON_ERROR(module_ade7953_read16(ADE7953_REG_CONFIG, &config), TAG, "read CONFIG failed");
    if (enable) {
        config |= ADE7953_CONFIG_HPFEN;
    } else {
        config &= (uint16_t)~ADE7953_CONFIG_HPFEN;
    }
    return module_ade7953_write16(ADE7953_REG_CONFIG, config);
}

esp_err_t module_ade7953_set_current_zx_source_b(bool use_current_b)
{
    uint16_t config = 0;
    ESP_RETURN_ON_ERROR(module_ade7953_read16(ADE7953_REG_CONFIG, &config), TAG, "read CONFIG failed");
    if (use_current_b) {
        config |= ADE7953_CONFIG_ZX_I_B;
    } else {
        config &= (uint16_t)~ADE7953_CONFIG_ZX_I_B;
    }
    return module_ade7953_write16(ADE7953_REG_CONFIG, config);
}

esp_err_t module_ade7953_set_zero_cross_timeout_ms(float timeout_ms)
{
    if (timeout_ms <= 0.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    /* ZXTOUT decrements at 14 kHz: 14 counts per ms. */
    uint32_t counts = (uint32_t)(timeout_ms * 14.0f + 0.5f);
    counts = clamp_u32(counts, 1, 0xFFFF);
    return module_ade7953_write16(ADE7953_REG_ZXTOUT, (uint16_t)counts);
}

esp_err_t module_ade7953_set_sag_threshold_raw(uint8_t half_cycles, uint32_t sag_level_raw)
{
    /* Disable sag before changing threshold/cycles. Sag is disabled when SAGCYC or SAGLVL is zero. */
    ESP_RETURN_ON_ERROR(module_ade7953_write32(ADE7953_REG_SAGLVL_32, 0), TAG, "disable sag failed");
    ESP_RETURN_ON_ERROR(module_ade7953_write8(ADE7953_REG_SAGCYC, half_cycles), TAG, "write SAGCYC failed");
    return module_ade7953_write32(ADE7953_REG_SAGLVL_32, sag_level_raw & ADE7953_RAW24_MASK);
}

esp_err_t module_ade7953_set_overvoltage_overcurrent_raw(uint32_t ov_level_raw, uint32_t oi_level_raw)
{
    ESP_RETURN_ON_ERROR(module_ade7953_write32(ADE7953_REG_OVLVL_32, ov_level_raw & ADE7953_RAW24_MASK), TAG, "write OVLVL failed");
    ESP_RETURN_ON_ERROR(module_ade7953_write32(ADE7953_REG_OILVL_32, oi_level_raw & ADE7953_RAW24_MASK), TAG, "write OILVL failed");
    return ESP_OK;
}

esp_err_t module_ade7953_set_no_load_thresholds_raw(uint32_t active_noload_raw,
                                                    uint32_t reactive_noload_raw,
                                                    uint32_t apparent_noload_raw)
{
    /* Recommended sequence: disable, write thresholds, re-enable with module_ade7953_enable_no_load_detection(). */
    ESP_RETURN_ON_ERROR(module_ade7953_enable_no_load_detection(false, false, false), TAG, "disable no-load failed");
    ESP_RETURN_ON_ERROR(module_ade7953_write32(ADE7953_REG_AP_NOLOAD_32, active_noload_raw & ADE7953_RAW24_MASK), TAG, "write AP_NOLOAD failed");
    ESP_RETURN_ON_ERROR(module_ade7953_write32(ADE7953_REG_VAR_NOLOAD_32, reactive_noload_raw & ADE7953_RAW24_MASK), TAG, "write VAR_NOLOAD failed");
    ESP_RETURN_ON_ERROR(module_ade7953_write32(ADE7953_REG_VA_NOLOAD_32, apparent_noload_raw & ADE7953_RAW24_MASK), TAG, "write VA_NOLOAD failed");
    return ESP_OK;
}

esp_err_t module_ade7953_enable_no_load_detection(bool active, bool reactive, bool apparent)
{
    uint8_t dis = 0;
    if (!active) {
        dis |= (1U << 0);
    }
    if (!reactive) {
        dis |= (1U << 1);
    }
    if (!apparent) {
        dis |= (1U << 2);
    }
    return module_ade7953_write8(ADE7953_REG_DISNOLOAD, dis);
}

esp_err_t module_ade7953_configure_irq_masks(uint32_t irqena_mask, uint32_t irqenb_mask)
{
    ESP_RETURN_ON_ERROR(module_ade7953_write32(ADE7953_REG_IRQENA_32, irqena_mask & ADE7953_RAW24_MASK), TAG, "write IRQENA failed");
    ESP_RETURN_ON_ERROR(module_ade7953_write32(ADE7953_REG_IRQENB_32, irqenb_mask & ADE7953_RAW24_MASK), TAG, "write IRQENB failed");
    return ESP_OK;
}

esp_err_t module_ade7953_enable_default_power_quality_irqs(void)
{
    const uint32_t irq_a = ADE7953_IRQ_A_SAG |
                           ADE7953_IRQ_A_OV |
                           ADE7953_IRQ_A_OIA |
                           ADE7953_IRQ_A_ZXTO |
                           ADE7953_IRQ_A_ZXTO_IA |
                           ADE7953_IRQ_A_CYCEND |
                           ADE7953_IRQ_A_CRC;
    const uint32_t irq_b = ADE7953_IRQ_B_OIB |
                           ADE7953_IRQ_B_ZXTO_IB;
    return module_ade7953_configure_irq_masks(irq_a, irq_b);
}

esp_err_t module_ade7953_configure_line_cycle_mode(uint16_t half_line_cycles, uint8_t lcycmode_mask)
{
    ESP_RETURN_ON_ERROR(module_ade7953_write16(ADE7953_REG_LINECYC, half_line_cycles), TAG, "write LINECYC failed");
    return module_ade7953_write8(ADE7953_REG_LCYCMODE, lcycmode_mask);
}

esp_err_t module_ade7953_configure_cf_outputs(ade7953_cf_source_t cf1_source,
                                              ade7953_cf_source_t cf2_source,
                                              bool enable_cf1,
                                              bool enable_cf2,
                                              uint16_t cf1den,
                                              uint16_t cf2den)
{
    uint16_t cfmode = ((uint16_t)cf1_source & 0x000F) | (((uint16_t)cf2_source & 0x000F) << 4);
    if (!enable_cf1) {
        cfmode |= (1U << 8);
    }
    if (!enable_cf2) {
        cfmode |= (1U << 9);
    }

    /* Datasheet recommends two sequential writes when modifying CFxDEN. */
    ESP_RETURN_ON_ERROR(module_ade7953_write16(ADE7953_REG_CF1DEN, cf1den), TAG, "write CF1DEN 1 failed");
    ESP_RETURN_ON_ERROR(module_ade7953_write16(ADE7953_REG_CF1DEN, cf1den), TAG, "write CF1DEN 2 failed");
    ESP_RETURN_ON_ERROR(module_ade7953_write16(ADE7953_REG_CF2DEN, cf2den), TAG, "write CF2DEN 1 failed");
    ESP_RETURN_ON_ERROR(module_ade7953_write16(ADE7953_REG_CF2DEN, cf2den), TAG, "write CF2DEN 2 failed");
    return module_ade7953_write16(ADE7953_REG_CFMODE, cfmode);
}

void module_ade7953_evaluate_safety(const ade7953_measurement_t *measurement,
                                    const ade7953_safety_limits_t *limits,
                                    ade7953_safety_decision_t *out_decision)
{
    if (out_decision == NULL) {
        return;
    }
    memset(out_decision, 0, sizeof(*out_decision));
    if (measurement == NULL || limits == NULL) {
        return;
    }

    if (limits->enable_hw_critical_trip) {
        out_decision->ade_hw_critical_event =
            ((measurement->raw.irq_a & ADE7953_SAFE_IRQ_A_MASK) != 0) ||
            ((measurement->raw.irq_b & ADE7953_SAFE_IRQ_B_MASK) != 0);
    }

    if (limits->enable_rms_voltage_limits && s_cal.volts_per_vrms_lsb > 0.0f) {
        out_decision->undervoltage_event = measurement->voltage_vrms < limits->min_voltage_vrms;
        out_decision->overvoltage_event = measurement->voltage_vrms > limits->max_voltage_vrms;
    }

    if (limits->enable_rms_current_limit) {
        bool a_over = (s_cal.amps_per_irmsa_lsb > 0.0f) && (measurement->current_a_arms > limits->max_current_a_arms);
        bool b_over = (s_cal.amps_per_irmsb_lsb > 0.0f) && (measurement->current_b_arms > limits->max_current_a_arms);
        out_decision->overcurrent_event = a_over || b_over;
    }

    if (limits->enable_active_power_limit) {
        bool a_over = (s_cal.watts_per_awatt_lsb > 0.0f) && (fabsf(measurement->active_power_a_w) > limits->max_active_power_w);
        bool b_over = (s_cal.watts_per_bwatt_lsb > 0.0f) && (fabsf(measurement->active_power_b_w) > limits->max_active_power_w);
        out_decision->overpower_event = a_over || b_over;
    }

    out_decision->trip = out_decision->ade_hw_critical_event ||
                         out_decision->overvoltage_event ||
                         out_decision->undervoltage_event ||
                         out_decision->overcurrent_event ||
                         out_decision->overpower_event;
}

float module_ade7953_period_raw_to_hz(uint16_t period_raw)
{
    if (period_raw == 0U) {
        return 0.0f;
    }
    return ADE7953_FREQ_CLOCK_HZ / ((float)period_raw + 1.0f);
}

float module_ade7953_pf_raw_to_float(int16_t pf_raw)
{
    return (float)pf_raw / 32768.0f;
}

float module_ade7953_angle_raw_to_degrees(int16_t angle_raw, float line_frequency_hz)
{
    if (line_frequency_hz <= 0.0f) {
        return 0.0f;
    }
    return (float)angle_raw * ADE7953_ANGLE_LSB_SECONDS * line_frequency_hz * 360.0f;
}

int32_t module_ade7953_sign_extend(uint32_t value, uint8_t bits)
{
    if (bits == 0U || bits > 32U) {
        return 0;
    }
    if (bits == 32U) {
        return (int32_t)value;
    }
    const uint32_t mask = 1UL << (bits - 1U);
    const uint32_t full = (1UL << bits) - 1UL;
    value &= full;
    return (int32_t)((value ^ mask) - mask);
}

/* Optional internal gain-register helper left private for later production calibration workflow. */
__attribute__((unused)) static esp_err_t tune_gain_register_from_reference(uint16_t gain_reg,
                                                                  float reference_value,
                                                                  float measured_value)
{
    uint32_t current = 0;
    ESP_RETURN_ON_ERROR(module_ade7953_read32(gain_reg, &current), TAG, "read gain failed");
    current &= ADE7953_RAW24_MASK;
    uint32_t next = scale_gain_register(current, reference_value, measured_value);
    return module_ade7953_write32(gain_reg, next);
}
