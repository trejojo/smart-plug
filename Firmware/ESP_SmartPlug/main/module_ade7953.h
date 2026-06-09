#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ADE7953 ESP-IDF driver for the Smart Plug project.
 *
 * Pure ESP-IDF C implementation. No Arduino.h, no SPIClass, no delay().
 * Target: ESP32-S3 WROOM-1U N16R8 + ADE7953 over isolated SPI.
 *
 * Six MCU-side signals expected:
 *   SCLK, MOSI, MISO, CS, RESET, IRQ.
 *
 * Scaling note:
 *   Raw ADE7953 counts are valid after communication is working.
 *   Engineering units require calibration constants for the final PCB.
 */

#define ADE7953_SPI_MAX_HZ              (5000000)
#define ADE7953_SPI_DEFAULT_HZ          (5000000)
#define ADE7953_FREQ_CLOCK_HZ           (223750.0f)
#define ADE7953_ANGLE_LSB_SECONDS       (4.47e-6f)

/* 8-bit registers */
#define ADE7953_REG_SAGCYC              0x000
#define ADE7953_REG_DISNOLOAD           0x001
#define ADE7953_REG_LCYCMODE            0x004
#define ADE7953_REG_PGA_V               0x007
#define ADE7953_REG_PGA_IA              0x008
#define ADE7953_REG_PGA_IB              0x009
#define ADE7953_REG_WRITE_PROTECT       0x040
#define ADE7953_REG_UNLOCK_120          0x0FE
#define ADE7953_REG_LAST_OP             0x0FD
#define ADE7953_REG_LAST_RWDATA8        0x0FF
#define ADE7953_REG_VERSION             0x702
#define ADE7953_REG_EX_REF              0x800

/* 16-bit registers */
#define ADE7953_REG_ZXTOUT              0x100
#define ADE7953_REG_LINECYC             0x101
#define ADE7953_REG_CONFIG              0x102
#define ADE7953_REG_CF1DEN              0x103
#define ADE7953_REG_CF2DEN              0x104
#define ADE7953_REG_CFMODE              0x107
#define ADE7953_REG_PHCALA              0x108
#define ADE7953_REG_PHCALB              0x109
#define ADE7953_REG_PFA                 0x10A
#define ADE7953_REG_PFB                 0x10B
#define ADE7953_REG_ANGLE_A             0x10C
#define ADE7953_REG_ANGLE_B             0x10D
#define ADE7953_REG_PERIOD              0x10E
#define ADE7953_REG_ALT_OUTPUT          0x110
#define ADE7953_REG_LAST_ADD            0x1FE
#define ADE7953_REG_LAST_RWDATA16       0x1FF
#define ADE7953_REG_REQUIRED_120        0x120

/* 32-bit mirror registers. Lower 24 bits contain the valid 24-bit data; signed registers are sign-extended. */
#define ADE7953_REG_SAGLVL_32           0x300
#define ADE7953_REG_ACCMODE_32          0x301
#define ADE7953_REG_AP_NOLOAD_32        0x303
#define ADE7953_REG_VAR_NOLOAD_32       0x304
#define ADE7953_REG_VA_NOLOAD_32        0x305
#define ADE7953_REG_AVA_32              0x310
#define ADE7953_REG_BVA_32              0x311
#define ADE7953_REG_AWATT_32            0x312
#define ADE7953_REG_BWATT_32            0x313
#define ADE7953_REG_AVAR_32             0x314
#define ADE7953_REG_BVAR_32             0x315
#define ADE7953_REG_IA_32               0x316
#define ADE7953_REG_IB_32               0x317
#define ADE7953_REG_V_32                0x318
#define ADE7953_REG_IRMSA_32            0x31A
#define ADE7953_REG_IRMSB_32            0x31B
#define ADE7953_REG_VRMS_32             0x31C
#define ADE7953_REG_AENERGYA_32         0x31E
#define ADE7953_REG_AENERGYB_32         0x31F
#define ADE7953_REG_RENERGYA_32         0x320
#define ADE7953_REG_RENERGYB_32         0x321
#define ADE7953_REG_APENERGYA_32        0x322
#define ADE7953_REG_APENERGYB_32        0x323
#define ADE7953_REG_OVLVL_32            0x324
#define ADE7953_REG_OILVL_32            0x325
#define ADE7953_REG_VPEAK_32            0x326
#define ADE7953_REG_RSTVPEAK_32         0x327
#define ADE7953_REG_IAPEAK_32           0x328
#define ADE7953_REG_RSTIAPEAK_32        0x329
#define ADE7953_REG_IBPEAK_32           0x32A
#define ADE7953_REG_RSTIBPEAK_32        0x32B
#define ADE7953_REG_IRQENA_32           0x32C
#define ADE7953_REG_IRQSTATA_32         0x32D
#define ADE7953_REG_RSTIRQSTATA_32      0x32E
#define ADE7953_REG_IRQENB_32           0x32F
#define ADE7953_REG_IRQSTATB_32         0x330
#define ADE7953_REG_RSTIRQSTATB_32      0x331
#define ADE7953_REG_CRC_32              0x37F
#define ADE7953_REG_AIGAIN_32           0x380
#define ADE7953_REG_AVGAIN_32           0x381
#define ADE7953_REG_AWGAIN_32           0x382
#define ADE7953_REG_AVARGAIN_32         0x383
#define ADE7953_REG_AVAGAIN_32          0x384
#define ADE7953_REG_AIRMSOS_32          0x386
#define ADE7953_REG_VRMSOS_32           0x388
#define ADE7953_REG_AWATTOS_32          0x389
#define ADE7953_REG_AVAROS_32           0x38A
#define ADE7953_REG_AVAOS_32            0x38B
#define ADE7953_REG_BIGAIN_32           0x38C
#define ADE7953_REG_BWGAIN_32           0x38E
#define ADE7953_REG_BVARGAIN_32         0x38F
#define ADE7953_REG_BVAGAIN_32          0x390
#define ADE7953_REG_BIRMSOS_32          0x392
#define ADE7953_REG_BWATTOS_32          0x395
#define ADE7953_REG_BVAROS_32           0x396
#define ADE7953_REG_BVAOS_32            0x397
#define ADE7953_REG_LAST_RWDATA32       0x3FF

/* CONFIG register bits */
#define ADE7953_CONFIG_INTENA           (1U << 0)
#define ADE7953_CONFIG_INTENB           (1U << 1)
#define ADE7953_CONFIG_HPFEN            (1U << 2)
#define ADE7953_CONFIG_PFMODE           (1U << 3)
#define ADE7953_CONFIG_REVP_CF          (1U << 4)
#define ADE7953_CONFIG_REVP_PULSE       (1U << 5)
#define ADE7953_CONFIG_ZXLPF            (1U << 6)
#define ADE7953_CONFIG_SWRST            (1U << 7)
#define ADE7953_CONFIG_CRC_ENABLE       (1U << 8)
#define ADE7953_CONFIG_ZX_I_B           (1U << 11)
#define ADE7953_CONFIG_COMM_LOCK        (1U << 15)

/* LCYCMODE register bits */
#define ADE7953_LCYCMODE_ALWATT         (1U << 0)
#define ADE7953_LCYCMODE_BLWATT         (1U << 1)
#define ADE7953_LCYCMODE_ALVAR          (1U << 2)
#define ADE7953_LCYCMODE_BLVAR          (1U << 3)
#define ADE7953_LCYCMODE_ALVA           (1U << 4)
#define ADE7953_LCYCMODE_BLVA           (1U << 5)
#define ADE7953_LCYCMODE_RSTREAD        (1U << 6)

/* IRQ A / voltage bits */
#define ADE7953_IRQ_A_AEHFA             (1UL << 0)
#define ADE7953_IRQ_A_VAREHFA           (1UL << 1)
#define ADE7953_IRQ_A_VAEHFA            (1UL << 2)
#define ADE7953_IRQ_A_AEOFA             (1UL << 3)
#define ADE7953_IRQ_A_VAREOFA           (1UL << 4)
#define ADE7953_IRQ_A_VAEOFA            (1UL << 5)
#define ADE7953_IRQ_A_AP_NOLOADA        (1UL << 6)
#define ADE7953_IRQ_A_VAR_NOLOADA       (1UL << 7)
#define ADE7953_IRQ_A_VA_NOLOADA        (1UL << 8)
#define ADE7953_IRQ_A_APSIGN_A          (1UL << 9)
#define ADE7953_IRQ_A_VARSIGN_A         (1UL << 10)
#define ADE7953_IRQ_A_ZXTO_IA           (1UL << 11)
#define ADE7953_IRQ_A_ZXIA              (1UL << 12)
#define ADE7953_IRQ_A_OIA               (1UL << 13)
#define ADE7953_IRQ_A_ZXTO              (1UL << 14)
#define ADE7953_IRQ_A_ZXV               (1UL << 15)
#define ADE7953_IRQ_A_OV                (1UL << 16)
#define ADE7953_IRQ_A_WSMP              (1UL << 17)
#define ADE7953_IRQ_A_CYCEND            (1UL << 18)
#define ADE7953_IRQ_A_SAG               (1UL << 19)
#define ADE7953_IRQ_A_RESET             (1UL << 20)
#define ADE7953_IRQ_A_CRC               (1UL << 21)

/* IRQ B bits */
#define ADE7953_IRQ_B_AEHFB             (1UL << 0)
#define ADE7953_IRQ_B_VAREHFB           (1UL << 1)
#define ADE7953_IRQ_B_VAEHFB            (1UL << 2)
#define ADE7953_IRQ_B_AEOFB             (1UL << 3)
#define ADE7953_IRQ_B_VAREOFB           (1UL << 4)
#define ADE7953_IRQ_B_VAEOFB            (1UL << 5)
#define ADE7953_IRQ_B_AP_NOLOADB        (1UL << 6)
#define ADE7953_IRQ_B_VAR_NOLOADB       (1UL << 7)
#define ADE7953_IRQ_B_VA_NOLOADB        (1UL << 8)
#define ADE7953_IRQ_B_APSIGN_B          (1UL << 9)
#define ADE7953_IRQ_B_VARSIGN_B         (1UL << 10)
#define ADE7953_IRQ_B_ZXTO_IB           (1UL << 11)
#define ADE7953_IRQ_B_ZXIB              (1UL << 12)
#define ADE7953_IRQ_B_OIB               (1UL << 13)

/* ACCMODE read-only sign / no-load status bits */
#define ADE7953_ACCMODE_APSIGN_A        (1UL << 10)
#define ADE7953_ACCMODE_APSIGN_B        (1UL << 11)
#define ADE7953_ACCMODE_VARSIGN_A       (1UL << 12)
#define ADE7953_ACCMODE_VARSIGN_B       (1UL << 13)
#define ADE7953_ACCMODE_ACTNLOAD_A      (1UL << 16)
#define ADE7953_ACCMODE_VANLOAD_A       (1UL << 17)
#define ADE7953_ACCMODE_VARNLOAD_A      (1UL << 18)
#define ADE7953_ACCMODE_ACTNLOAD_B      (1UL << 19)
#define ADE7953_ACCMODE_VANLOAD_B       (1UL << 20)
#define ADE7953_ACCMODE_VARNLOAD_B      (1UL << 21)

typedef enum {
    ADE7953_PGA_GAIN_1  = 0x00,
    ADE7953_PGA_GAIN_2  = 0x01,
    ADE7953_PGA_GAIN_4  = 0x02,
    ADE7953_PGA_GAIN_8  = 0x03,
    ADE7953_PGA_GAIN_16 = 0x04,
    ADE7953_PGA_GAIN_22 = 0x05, /* Current Channel A only */
} ade7953_pga_gain_t;

typedef enum {
    ADE7953_CF_ACTIVE_A      = 0x0,
    ADE7953_CF_REACTIVE_A    = 0x1,
    ADE7953_CF_APPARENT_A    = 0x2,
    ADE7953_CF_IRMS_A        = 0x3,
    ADE7953_CF_ACTIVE_B      = 0x4,
    ADE7953_CF_REACTIVE_B    = 0x5,
    ADE7953_CF_APPARENT_B    = 0x6,
    ADE7953_CF_IRMS_B        = 0x7,
    ADE7953_CF_IRMS_A_PLUS_B = 0x8,
    ADE7953_CF_ACTIVE_A_PLUS_B = 0x9,
} ade7953_cf_source_t;

typedef struct {
    spi_host_device_t spi_host;
    gpio_num_t cs_gpio;
    gpio_num_t mosi_gpio;
    gpio_num_t miso_gpio;
    gpio_num_t sclk_gpio;
    gpio_num_t reset_gpio;
    gpio_num_t irq_gpio;
    int spi_clock_hz;
    bool use_hw_reset;
    bool wait_for_reset_irq;
    bool apply_required_settings;
    bool lock_spi_interface;
} ade7953_config_t;

typedef struct {
    /* Scale factors: engineering unit = raw_count * scale. Set through calibration. */
    float volts_per_vrms_lsb;
    float amps_per_irmsa_lsb;
    float amps_per_irmsb_lsb;

    float watts_per_awatt_lsb;
    float watts_per_bwatt_lsb;
    float vars_per_avar_lsb;
    float vars_per_bvar_lsb;
    float va_per_ava_lsb;
    float va_per_bva_lsb;

    /* Energy registers are signed read-with-reset by default. */
    float wh_per_aenergy_a_lsb;
    float wh_per_aenergy_b_lsb;
    float varh_per_renergy_a_lsb;
    float varh_per_renergy_b_lsb;
    float vah_per_apenergy_a_lsb;
    float vah_per_apenergy_b_lsb;
} ade7953_calibration_t;

typedef struct {
    uint32_t vrms;
    uint32_t irmsa;
    uint32_t irmsb;
    int32_t v_waveform;
    int32_t ia_waveform;
    int32_t ib_waveform;
    int32_t awatt;
    int32_t bwatt;
    int32_t avar;
    int32_t bvar;
    int32_t ava;
    int32_t bva;
    int32_t aenergy_a_delta;
    int32_t aenergy_b_delta;
    int32_t renergy_a_delta;
    int32_t renergy_b_delta;
    int32_t apenergy_a_delta;
    int32_t apenergy_b_delta;
    uint32_t vpeak;
    uint32_t iapeak;
    uint32_t ibpeak;
    uint16_t period;
    int16_t pfa;
    int16_t pfb;
    int16_t angle_a;
    int16_t angle_b;
    uint32_t irq_a;
    uint32_t irq_b;
    uint32_t accmode;
} ade7953_raw_measurement_t;

typedef struct {
    uint64_t timestamp_ms;
    ade7953_raw_measurement_t raw;

    float voltage_vrms;
    float current_a_arms;
    float current_b_arms;
    float active_power_a_w;
    float active_power_b_w;
    float reactive_power_a_var;
    float reactive_power_b_var;
    float apparent_power_a_va;
    float apparent_power_b_va;
    float power_factor_a;
    float power_factor_b;
    float line_frequency_hz;
    float angle_a_deg;
    float angle_b_deg;

    float active_energy_a_wh_delta;
    float active_energy_b_wh_delta;
    float reactive_energy_a_varh_delta;
    float reactive_energy_b_varh_delta;
    float apparent_energy_a_vah_delta;
    float apparent_energy_b_vah_delta;

    float active_energy_a_wh_total;
    float active_energy_b_wh_total;
    float reactive_energy_a_varh_total;
    float reactive_energy_b_varh_total;
    float apparent_energy_a_vah_total;
    float apparent_energy_b_vah_total;

    bool reverse_active_a;
    bool reverse_active_b;
    bool reverse_reactive_a;
    bool reverse_reactive_b;
    bool no_load_active_a;
    bool no_load_active_b;
    bool no_load_reactive_a;
    bool no_load_reactive_b;
    bool no_load_apparent_a;
    bool no_load_apparent_b;
} ade7953_measurement_t;

typedef struct {
    uint32_t irq_a;
    uint32_t irq_b;
    uint32_t accmode;

    bool reset_done;
    bool sag;
    bool overvoltage;
    bool overcurrent_a;
    bool overcurrent_b;
    bool zero_cross_timeout_voltage;
    bool zero_cross_timeout_current_a;
    bool zero_cross_timeout_current_b;
    bool zero_cross_voltage;
    bool zero_cross_current_a;
    bool zero_cross_current_b;
    bool waveform_sample_ready;
    bool line_cycle_end;
    bool crc_changed;

    bool active_noload_a;
    bool active_noload_b;
    bool reactive_noload_a;
    bool reactive_noload_b;
    bool apparent_noload_a;
    bool apparent_noload_b;

    bool active_sign_changed_a;
    bool active_sign_changed_b;
    bool reactive_sign_changed_a;
    bool reactive_sign_changed_b;
    bool reverse_active_a;
    bool reverse_active_b;
    bool reverse_reactive_a;
    bool reverse_reactive_b;
} ade7953_events_t;

typedef struct {
    bool trip;
    bool overvoltage_event;
    bool undervoltage_event;
    bool overcurrent_event;
    bool overpower_event;
    bool ade_hw_critical_event;
} ade7953_safety_decision_t;

typedef struct {
    bool enable_hw_critical_trip;
    bool enable_rms_voltage_limits;
    bool enable_rms_current_limit;
    bool enable_active_power_limit;
    float min_voltage_vrms;
    float max_voltage_vrms;
    float max_current_a_arms;
    float max_active_power_w;
} ade7953_safety_limits_t;

typedef struct {
    ade7953_safety_limits_t safety_limits;
} ade7953_smartplug_policy_t;

/* Init / deinit */
esp_err_t module_ade7953_get_default_config(ade7953_config_t *out_config);
esp_err_t module_ade7953_init(void);
esp_err_t module_ade7953_init_with_config(const ade7953_config_t *config);
esp_err_t module_ade7953_deinit(void);
bool module_ade7953_is_initialized(void);
bool module_ade7953_is_waveform_capturing(void);

/* Low-level register access */
esp_err_t module_ade7953_read8(uint16_t reg, uint8_t *value);
esp_err_t module_ade7953_read16(uint16_t reg, uint16_t *value);
esp_err_t module_ade7953_read24(uint16_t reg, uint32_t *value);
esp_err_t module_ade7953_read32(uint16_t reg, uint32_t *value);
esp_err_t module_ade7953_read_s24(uint16_t reg, int32_t *value);
esp_err_t module_ade7953_read_s32(uint16_t reg, int32_t *value);
esp_err_t module_ade7953_read_waveform_samples(int32_t *v_waveform, int32_t *ia_waveform);
esp_err_t module_ade7953_set_waveform_capture_mode(bool enabled);
esp_err_t module_ade7953_write8(uint16_t reg, uint8_t value);
esp_err_t module_ade7953_write16(uint16_t reg, uint16_t value);
esp_err_t module_ade7953_write24(uint16_t reg, uint32_t value);
esp_err_t module_ade7953_write32(uint16_t reg, uint32_t value);

/* Bring-up helpers */
esp_err_t module_ade7953_hw_reset(void);
esp_err_t module_ade7953_wait_ready(uint32_t timeout_ms);
esp_err_t module_ade7953_apply_required_settings(void);
esp_err_t module_ade7953_lock_spi_interface(void);
esp_err_t module_ade7953_software_reset(void);
esp_err_t module_ade7953_read_version(uint8_t *version);
esp_err_t module_ade7953_read_crc(uint32_t *crc);

/* Calibration / scaling */
esp_err_t module_ade7953_get_default_calibration(ade7953_calibration_t *out_cal);
void module_ade7953_set_calibration(const ade7953_calibration_t *cal);
void module_ade7953_get_calibration(ade7953_calibration_t *cal);
void module_ade7953_reset_energy_totals(void);
esp_err_t module_ade7953_apply_smartplug_startup(const ade7953_smartplug_policy_t *policy);
esp_err_t module_ade7953_calibrate_rms_from_reference(float reference_vrms,
                                                       float reference_iarms,
                                                       float reference_ibarms);
esp_err_t module_ade7953_calibrate_power_from_reference(float reference_w_a,
                                                         float reference_w_b,
                                                         float reference_var_a,
                                                         float reference_var_b,
                                                         float reference_va_a,
                                                         float reference_va_b);

/* Measurement API */
esp_err_t module_ade7953_read_raw_measurement(ade7953_raw_measurement_t *out_raw,
                                              bool read_energy_deltas,
                                              bool clear_irq_status);
esp_err_t module_ade7953_read_measurement(ade7953_measurement_t *out_measurement,
                                          bool read_energy_deltas,
                                          bool clear_irq_status);
esp_err_t module_ade7953_start_snapshot_capture(void);
esp_err_t module_ade7953_read_events(ade7953_events_t *out_events, bool clear_latched);
esp_err_t module_ade7953_read_peaks(uint32_t *vpeak, uint32_t *iapeak, uint32_t *ibpeak,
                                    bool reset_after_read);

/* Configuration helpers */
esp_err_t module_ade7953_set_pga(ade7953_pga_gain_t voltage_gain,
                                 ade7953_pga_gain_t current_a_gain,
                                 ade7953_pga_gain_t current_b_gain);
esp_err_t module_ade7953_set_integrators(bool enable_current_a, bool enable_current_b);
esp_err_t module_ade7953_set_hpf(bool enable);
esp_err_t module_ade7953_set_current_zx_source_b(bool use_current_b);
esp_err_t module_ade7953_set_zero_cross_timeout_ms(float timeout_ms);
esp_err_t module_ade7953_set_sag_threshold_raw(uint8_t half_cycles, uint32_t sag_level_raw);
esp_err_t module_ade7953_set_overvoltage_overcurrent_raw(uint32_t ov_level_raw, uint32_t oi_level_raw);
esp_err_t module_ade7953_set_overvoltage_overcurrent_from_rms(float max_vrms,
                                                              float max_iarms);
esp_err_t module_ade7953_set_no_load_thresholds_raw(uint32_t active_noload_raw,
                                                    uint32_t reactive_noload_raw,
                                                    uint32_t apparent_noload_raw);
esp_err_t module_ade7953_enable_no_load_detection(bool active, bool reactive, bool apparent);
esp_err_t module_ade7953_configure_irq_masks(uint32_t irqena_mask, uint32_t irqenb_mask);
esp_err_t module_ade7953_enable_default_power_quality_irqs(void);
esp_err_t module_ade7953_configure_line_cycle_mode(uint16_t half_line_cycles, uint8_t lcycmode_mask);
esp_err_t module_ade7953_configure_cf_outputs(ade7953_cf_source_t cf1_source,
                                              ade7953_cf_source_t cf2_source,
                                              bool enable_cf1,
                                              bool enable_cf2,
                                              uint16_t cf1den,
                                              uint16_t cf2den);

/* Safety helper: use this after module_ade7953_read_measurement() and open relay outside this module. */
void module_ade7953_evaluate_safety(const ade7953_measurement_t *measurement,
                                    const ade7953_safety_limits_t *limits,
                                    ade7953_safety_decision_t *out_decision);

/* Utility */
float module_ade7953_period_raw_to_hz(uint16_t period_raw);
float module_ade7953_pf_raw_to_float(int16_t pf_raw);
float module_ade7953_angle_raw_to_degrees(int16_t angle_raw, float line_frequency_hz);
int32_t module_ade7953_sign_extend(uint32_t value, uint8_t bits);

#ifdef __cplusplus
}
#endif
