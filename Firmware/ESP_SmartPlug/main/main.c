#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"

#include "board_profile.h"
#include "module_ade7953.h"
#include "module_ble.h"
#include "module_ledstrip.h"
#include "module_mqtt.h"
#include "module_nvs.h"
#include "module_relay.h"
#include "module_sdcard.h"
#include "module_tmp102.h"
#include "module_wifi.h"

static const char *TAG = "aice_smartplug"; // Updated TAG to AICE
static bool g_relay_on = false;
static bool g_mqtt_connect_requested = false;
static TickType_t g_mqtt_connect_requested_tick = 0;

static const char *MQTT_BROKER_IP = "192.168.137.1";
static const uint16_t MQTT_BROKER_PORT = 1883;

static TaskHandle_t s_main_task_handle = NULL;
static gpio_num_t s_relay_gpio = GPIO_NUM_NC;
static gpio_num_t s_ade_irq_gpio = GPIO_NUM_NC;

#ifndef ENABLE_WAVEFORM_STREAM
#define ENABLE_WAVEFORM_STREAM 0
#endif

#ifndef ENABLE_IRQ_DEBUG_READBACK
#define ENABLE_IRQ_DEBUG_READBACK 0
#endif

#if ENABLE_WAVEFORM_STREAM
static TaskHandle_t s_wave_task = NULL;

static void IRAM_ATTR ade7953_waveform_irq_handler(void *arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (s_wave_task != NULL) {
        vTaskNotifyGiveFromISR(s_wave_task, &xHigherPriorityTaskWoken);
    }
    if (xHigherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void waveform_stream_task(void *pvParameters)
{
    while (true) {
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 0) {
#if ENABLE_IRQ_DEBUG_READBACK
            ade7953_events_t events = {0};
            if (module_ade7953_read_events(&events, false) == ESP_OK) {
                ESP_LOGW(TAG, "ADE IRQ debug: irq_a=0x%06" PRIX32 " irq_b=0x%06" PRIX32 " wsmp=%d cycend=%d",
                         events.irq_a,
                         events.irq_b,
                         events.waveform_sample_ready,
                         events.line_cycle_end);
            }
#endif
            continue;
        }

        ade7953_raw_measurement_t raw = {0};
        if (module_ade7953_read_raw_measurement(&raw, false, true) == ESP_OK) {
            printf("WAVE,%" PRId32 ",%" PRId32 "\n", raw.v_waveform, raw.ia_waveform);
        }
    }
}
#endif

typedef enum {
    STATUS_IDLE = 0,
    STATUS_BLE_WAITING,         // Blinking Blue (Waiting for credentials)
    STATUS_WIFI_CONNECTING,     // Solid Blue (Credentials received, connecting to Wi-Fi)
    STATUS_MQTT_CONNECTING,     // Blinking Green (Wi-Fi connected, connecting to MQTT)
    STATUS_MQTT_CONNECTED,      // Solid Green (MQTT connected)
    STATUS_ERROR,               // Solid Red (ADE trip or sensor error)
} aice_status_t;

// Global variable to hold the current status
static volatile aice_status_t g_current_status = STATUS_IDLE;

// Helper to update the system status
static void aice_set_status(aice_status_t s)
{
    g_current_status = s;
}

static ade7953_smartplug_policy_t g_ade_policy = {
    .safety_limits = {
        .enable_hw_critical_trip = false,
        .enable_rms_voltage_limits = false, /* enable after calibration */
        .enable_rms_current_limit = false,  /* enable after calibration */
        .enable_active_power_limit = false, /* enable after calibration */
        .min_voltage_vrms = 95.0f,
        .max_voltage_vrms = 145.0f,
        .max_current_a_arms = 5.5f,
        .max_active_power_w = 700.0f,
    },
};

typedef struct {
    gpio_int_type_t intr_type;
    gpio_pullup_t pull_up_en;
    gpio_pulldown_t pull_down_en;
    uint32_t isr_service_flags;
} ade_irq_policy_t;

static const ade_irq_policy_t g_ade_irq_policy = {
    .intr_type = GPIO_INTR_NEGEDGE,
    .pull_up_en = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .isr_service_flags = 0,
};

static void aice_refresh_network_state(bool credentials_in_nvs)
{
    const bool wifi_connected = module_wifi_is_connected();
    const bool mqtt_connected = module_mqtt_is_connected();

    if (mqtt_connected) {
        g_mqtt_connect_requested = false;
        aice_set_status(STATUS_MQTT_CONNECTED);
        return;
    }

    if (!wifi_connected) {
        g_mqtt_connect_requested = false;
        if (credentials_in_nvs) {
            aice_set_status(STATUS_WIFI_CONNECTING);
        } else {
            aice_set_status(STATUS_BLE_WAITING);
        }
        return;
    }

    aice_set_status(STATUS_MQTT_CONNECTING);

    if (!g_mqtt_connect_requested) {
        ESP_LOGI(TAG, "WiFi connected, attempting MQTT connection to %s:%u", MQTT_BROKER_IP, MQTT_BROKER_PORT);
        if (module_mqtt_connect(MQTT_BROKER_IP, MQTT_BROKER_PORT) == ESP_OK) {
            g_mqtt_connect_requested = true;
            g_mqtt_connect_requested_tick = xTaskGetTickCount();
        }
        return;
    }

    if ((xTaskGetTickCount() - g_mqtt_connect_requested_tick) > pdMS_TO_TICKS(30000)) {
        ESP_LOGW(TAG, "MQTT connection attempt timed out; allowing retry");
        g_mqtt_connect_requested = false;
    }
}

static esp_err_t aice_apply_safety_limits_update(float max_vrms, float max_iarms, void *user_data)
{
    (void)user_data;

    if (max_vrms <= 0.0f || max_iarms <= 0.0f) {
        ESP_LOGW(TAG, "Ignoring invalid safety-limit update: max_vrms=%.2f max_iarms=%.3f", max_vrms, max_iarms);
        return ESP_ERR_INVALID_ARG;
    }

    g_ade_policy.safety_limits.max_voltage_vrms = max_vrms;
    g_ade_policy.safety_limits.max_current_a_arms = max_iarms;

    ESP_LOGI(TAG, "Updating ADE safety limits from MQTT: max_vrms=%.2f max_iarms=%.3f", max_vrms, max_iarms);
    if (module_ade7953_set_overvoltage_overcurrent_from_rms(max_vrms, max_iarms) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to apply updated ADE hardware thresholds");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static bool ade7953_service_and_should_trip(const ade7953_measurement_t *measurement,
                                            const ade7953_safety_limits_t *limits,
                                            ade7953_safety_decision_t *out_decision)
{
    if (measurement == NULL || limits == NULL) {
        if (out_decision != NULL) {
            memset(out_decision, 0, sizeof(*out_decision));
        }
        return false;
    }

    ESP_LOGI(TAG,
             "ADE cal: V=%.2fV IA=%.3fA IB=%.3fA PA=%.2fW PB=%.2fW "
             "F=%.2fHz PFA=%.3f PFB=%.3f E_A=%.4fWh IRQ_A=0x%06" PRIX32 " IRQ_B=0x%06" PRIX32
             " | raw VRMS=%" PRIu32 " IRMSA=%" PRIu32 " IRMSB=%" PRIu32 " AWATT=%" PRId32 " BWATT=%" PRId32,
             measurement->voltage_vrms,
             measurement->current_a_arms,
             measurement->current_b_arms,
             measurement->active_power_a_w,
             measurement->active_power_b_w,
             measurement->line_frequency_hz,
             measurement->power_factor_a,
             measurement->power_factor_b,
             measurement->active_energy_a_wh_total,
             measurement->raw.irq_a,
             measurement->raw.irq_b,
             measurement->raw.vrms,
             measurement->raw.irmsa,
             measurement->raw.irmsb,
             measurement->raw.awatt,
             measurement->raw.bwatt,
             measurement->raw.awatt,
             measurement->raw.bwatt);

    ade7953_safety_decision_t decision;
    module_ade7953_evaluate_safety(measurement, limits, &decision);
    if (out_decision != NULL) {
        *out_decision = decision;
    }
    if (decision.trip) {
        ESP_LOGE(TAG, "ADE7953 critical event: opening relay. hw=%d ov=%d uv=%d oc=%d op=%d",
                 decision.ade_hw_critical_event,
                 decision.overvoltage_event,
                 decision.undervoltage_event,
                 decision.overcurrent_event,
                 decision.overpower_event);
    }

    return decision.trip;
}

static bool ade7953_measurement_has_no_load(const ade7953_measurement_t *measurement)
{
    if (measurement == NULL) {
        return false;
    }

    return (measurement->raw.accmode & ADE7953_ACCMODE_ACTNLOAD_A) != 0;
}

static const char *critical_protection_cause_from_events(const ade7953_events_t *events,
                                                         const ade7953_measurement_t *measurement,
                                                         const ade7953_safety_decision_t *decision)
{
    if (decision != NULL && decision->ade_hw_critical_event) {
        return "HW_CRITICAL";
    }

    if (events != NULL) {
        if (events->overvoltage) {
            return "OVERVOLTAGE";
        }

        if (events->overcurrent_a || events->overcurrent_b) {
            if (measurement != NULL && measurement->voltage_vrms < g_ade_policy.safety_limits.min_voltage_vrms) {
                return "OVERCURRENT_SAG";
            }

            return "OVERCURRENT";
        }
    }

    if (decision != NULL) {
        if (decision->overvoltage_event) {
            return "OVERVOLTAGE";
        }

        if (decision->overcurrent_event) {
            if (measurement != NULL && measurement->voltage_vrms < g_ade_policy.safety_limits.min_voltage_vrms) {
                return "OVERCURRENT_SAG";
            }

            return "OVERCURRENT";
        }
    }

    return "UNKNOWN";
}

static uint32_t critical_protection_duration_cycles(const ade7953_measurement_t *measurement,
                                                    uint64_t start_ms)
{
    if (measurement == NULL || measurement->line_frequency_hz <= 0.0f) {
        return 1U;
    }

    uint64_t elapsed_ms = 0;
    if (measurement->timestamp_ms > start_ms) {
        elapsed_ms = measurement->timestamp_ms - start_ms;
    }

    float cycles = ((float)elapsed_ms * measurement->line_frequency_hz) / 1000.0f;
    if (cycles < 1.0f) {
        cycles = 1.0f;
    }

    return (uint32_t)(cycles + 0.5f);
}

static void IRAM_ATTR ade7953_safety_irq_handler(void *arg)
{
    if (s_relay_gpio != GPIO_NUM_NC) {
        gpio_set_level(s_relay_gpio, 0);
    }

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (s_main_task_handle != NULL) {
        vTaskNotifyGiveFromISR(s_main_task_handle, &xHigherPriorityTaskWoken);
    }
    if (xHigherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void ade_irq_isr_setup(const smartplug_board_pins_t *pins, const ade_irq_policy_t *policy)
{
    if (pins == NULL || policy == NULL) {
        return;
    }

    s_ade_irq_gpio = pins->ade_irq_gpio;

    gpio_config_t irq_pin_cfg = {
        .pin_bit_mask = 1ULL << pins->ade_irq_gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = policy->pull_up_en,
        .pull_down_en = policy->pull_down_en,
        .intr_type = policy->intr_type,
    };
    ESP_ERROR_CHECK(gpio_config(&irq_pin_cfg));

    esp_err_t isr_ret = gpio_install_isr_service(policy->isr_service_flags);
    if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(isr_ret);
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(pins->ade_irq_gpio, ade7953_safety_irq_handler, NULL));
}

// Dedicated FreeRTOS task to handle LED colors and blinking asynchronously (GRB Hardware Mapping)
static void led_control_task(void *pvParameters)
{
    bool toggle = false;
    while (1) {
        aice_status_t current = g_current_status;
        uint8_t r = 0, g = 0, b = 0;

        switch (current) {
            case STATUS_BLE_WAITING:
                // Blue channel is unaffected by GRB inversion
                b = toggle ? 32 : 0; // Blink Blue
                break;
                
            case STATUS_WIFI_CONNECTING:
                b = 32;              // Solid Blue
                break;
                
            case STATUS_MQTT_CONNECTING:
                // Hardware expects GRB, so writing to r lights up physical Green
                r = toggle ? 32 : 0; // Blink Green
                g = 0;
                break;
                
            case STATUS_MQTT_CONNECTED:
                // Hardware expects GRB, so writing to r lights up physical Green
                r = 32;              // Solid Green
                g = 0; 
                break;
                

            case STATUS_ERROR:
                // Hardware expects GRB, so writing to g lights up physical Red
                r = 0;
                g = 32;              // Solid Red
                break;  
            case STATUS_IDLE:
            default:
                // Off
                break;
        }

        module_ledstrip_set(r, g, b);
        toggle = !toggle;
        
        // Blink interval (e.g., 500ms). Adjust pdMS_TO_TICKS for faster/slower blinking.
        vTaskDelay(pdMS_TO_TICKS(500)); 
    }
}

static void setup_button_init(gpio_num_t gpio_num)
{
    gpio_config_t button_cfg = {
        .pin_bit_mask = 1ULL << gpio_num,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button_cfg));
}

void app_main(void)
{
    const smartplug_board_pins_t *pins = smartplug_board_pins_get();
    s_main_task_handle = xTaskGetCurrentTaskHandle();
    s_relay_gpio = pins->relay_gpio;

    esp_log_level_set("aice_smartplug", ESP_LOG_INFO);
    ESP_LOGI(TAG, "AICE bring-up boot (%s)", smartplug_board_profile_name());
    ESP_LOGI(TAG, "Relay GPIO: %d", pins->relay_gpio);
    ESP_LOGI(TAG, "RGB LED GPIO: %d", pins->rgb_led_gpio);
    ESP_LOGI(TAG, "TMP102 SDA/SCL: %d/%d", pins->tmp102_sda_gpio, pins->tmp102_scl_gpio);
    ESP_LOGI(TAG, "SD card SPI host: %d CS/MOSI/SCLK/MISO: %d/%d/%d/%d",
             pins->sd_spi_host, pins->sd_cs_gpio, pins->sd_mosi_gpio, pins->sd_sclk_gpio, pins->sd_miso_gpio);
    ESP_LOGI(TAG, "ADE7953 SPI host: %d CS/MOSI/SCLK/MISO: %d/%d/%d/%d RESET/IRQ: %d/%d",
             pins->ade_spi_host, pins->ade_cs_gpio, pins->ade_mosi_gpio, pins->ade_sclk_gpio,
             pins->ade_miso_gpio, pins->ade_reset_gpio, pins->ade_irq_gpio);
    ESP_LOGI(TAG, "Setup button GPIO: %d", pins->setup_bt_button);

    ESP_ERROR_CHECK(module_relay_init());
    ESP_ERROR_CHECK(module_ledstrip_init());
    ESP_ERROR_CHECK(module_tmp102_init());
    ESP_ERROR_CHECK(module_nvs_init());
    ESP_ERROR_CHECK(module_ble_init());
    ESP_ERROR_CHECK(module_wifi_init());
    ESP_ERROR_CHECK(module_mqtt_init());
    ESP_ERROR_CHECK(module_mqtt_set_safety_limits_handler(aice_apply_safety_limits_update, NULL));

    bool credentials_in_nvs = module_nvs_credentials_exist();
    if (credentials_in_nvs) {
        char ssid[MAX_NVS_SSID_LEN + 1] = {0};
        char password[MAX_NVS_PASSWORD_LEN + 1] = {0};

        if (module_nvs_get_ssid(ssid) == ESP_OK && module_nvs_get_password(password) == ESP_OK) {
            ESP_LOGI(TAG, "WiFi credentials found in NVS (SSID: %s)", ssid);
            ESP_ERROR_CHECK(module_wifi_connect(ssid, password));
        } else {
            ESP_LOGW(TAG, "WiFi credentials exist in NVS but could not be loaded");
            credentials_in_nvs = false;
        }
    }

    if (!credentials_in_nvs) {
        ESP_LOGW(TAG, "No WiFi credentials found, starting BLE advertising for provisioning");
        ESP_ERROR_CHECK(module_ble_start_advertising());
    }

    aice_refresh_network_state(credentials_in_nvs);

    // Start the asynchronous LED handling task
    xTaskCreate(led_control_task, "led_control_task", 2048, NULL, 5, NULL);

    esp_err_t sd_init_ret = module_sdcard_init();
    if (sd_init_ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card init failed, continuing without SD logging: %s", esp_err_to_name(sd_init_ret));
    }

    esp_err_t ade_init_ret = module_ade7953_init();
    if (ade_init_ret == ESP_OK) {
        uint8_t ade_version = 0;
        if (module_ade7953_read_version(&ade_version) == ESP_OK) {
            ESP_LOGI(TAG, "ADE7953 version: 0x%02X", ade_version);
        }
        ade7953_calibration_t ade_cal = {0};
        if (module_ade7953_get_default_calibration(&ade_cal) == ESP_OK) {
            module_ade7953_set_calibration(&ade_cal);
            ESP_LOGI(TAG,
                     "Applied ADE scale calibration: kv=%.8f ki=%.8f kw=%.8e wh=%.8e",
                     ade_cal.volts_per_vrms_lsb,
                     ade_cal.amps_per_irmsa_lsb,
                     ade_cal.watts_per_awatt_lsb,
                     ade_cal.wh_per_aenergy_a_lsb);
        }
        ESP_ERROR_CHECK(module_ade7953_apply_smartplug_startup(&g_ade_policy));
        ade_irq_isr_setup(pins, &g_ade_irq_policy);
    } else {
        ESP_LOGE(TAG, "ADE7953 init failed, continuing without metering: %s", esp_err_to_name(ade_init_ret));
    }

    setup_button_init(pins->setup_bt_button);

    module_relay_set(true);
    g_relay_on = true;

    const char *test_path = MODULE_SDCARD_MOUNT_POINT "/test.txt";
    uint32_t button_press_ticks = 0;
    uint32_t sensor_timer_ticks = 0;
    bool button_was_pressed = false;
    bool critical_protection_active = false;
    bool critical_protection_report_pending = false;
    bool critical_protection_irq_events_valid = false;
    uint64_t critical_protection_started_ms = 0;
    ade7953_measurement_t critical_protection_measurement = {0};
    ade7953_safety_decision_t critical_protection_decision = {0};
    ade7953_events_t critical_protection_irq_events = {0};
    
    while (true) {
        bool ade_irq_pending = false;
        while (ulTaskNotifyTake(pdTRUE, 0) > 0) {
            ade_irq_pending = true;
        }
        if (ade_irq_pending == true) {
            g_relay_on = false;            // Sync the MQTT payload state
            aice_set_status(STATUS_ERROR); // Turn the LED Red

            ESP_LOGE(TAG, "Hardware Safety Trip! Relay physically opened by ISR.");
            
            // Read events to log the exact cause
            ade7953_events_t events = {0};
            if (module_ade7953_read_events(&events, true) == ESP_OK) {
                if (events.overcurrent_a || events.overcurrent_b) {
                    ESP_LOGE(TAG, "Cause: OVERCURRENT");
                }
                if (events.overvoltage) {
                    ESP_LOGE(TAG, "Cause: OVERVOLTAGE");
                }
                critical_protection_irq_events = events;
                critical_protection_irq_events_valid = true;
            } else {
                critical_protection_irq_events_valid = false;
            }

            critical_protection_active = true;
            critical_protection_report_pending = true;
            if (critical_protection_started_ms == 0) {
                critical_protection_started_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
            }
        }

        int button_level = gpio_get_level(pins->setup_bt_button);

        // -------------------------------------------------------------
        // FAST LOGIC: BUTTON HANDLING (Every 100ms)
        // -------------------------------------------------------------
        if (button_level == 0) { // Button is physically pressed
            button_press_ticks++;
            button_was_pressed = true;

            // If held for 4 seconds (40 ticks * 100ms)
            if (button_press_ticks == 40) {
                ESP_LOGW(TAG, "Button held > 4s! Erasing NVS credentials and restarting...");

                module_relay_set(false);
                g_relay_on = false;
                ESP_ERROR_CHECK_WITHOUT_ABORT(module_mqtt_disconnect());
                ESP_ERROR_CHECK_WITHOUT_ABORT(module_wifi_disconnect());
                ESP_ERROR_CHECK_WITHOUT_ABORT(module_ble_stop_advertising());
                ESP_ERROR_CHECK_WITHOUT_ABORT(module_nvs_erase_wifi_credentials());

                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart(); // Reboot the plug to return to BLE mode
            }
        } else { // Button is released
            if (button_was_pressed) {
                // If it was a short press (less than 4 seconds)
                if (button_press_ticks > 0 && button_press_ticks < 40) {
                    if (critical_protection_active) {
                        ade7953_measurement_t recovery_measurement = {0};
                        ade7953_safety_decision_t recovery_decision = {0};
                        esp_err_t recovery_ret = module_ade7953_read_measurement(&recovery_measurement, true, true);
                        if (recovery_ret == ESP_OK) {
                            module_ade7953_evaluate_safety(&recovery_measurement, &g_ade_policy.safety_limits, &recovery_decision);
                            if (!recovery_decision.trip) {
                                critical_protection_active = false;
                                critical_protection_started_ms = 0;
                                g_relay_on = true;
                                module_relay_set(true);
                                ESP_LOGI(TAG, "Button short press: critical protection cleared, relay closed");
                                aice_refresh_network_state(credentials_in_nvs);
                            } else {
                                module_relay_set(false);
                                g_relay_on = false;
                                aice_set_status(STATUS_ERROR);
                                ESP_LOGW(TAG, "Button short press ignored: protection condition still present");
                            }
                        } else {
                            module_relay_set(false);
                            g_relay_on = false;
                            aice_set_status(STATUS_ERROR);
                            ESP_LOGW(TAG, "Button short press could not verify recovery: %s", esp_err_to_name(recovery_ret));
                        }
                    } else {
                        g_relay_on = !g_relay_on; // Toggle relay state
                        module_relay_set(g_relay_on);
                        ESP_LOGI(TAG, "Button short press: Relay toggled %s", g_relay_on ? "ON" : "OFF");
                    }
                }
                button_was_pressed = false;
                button_press_ticks = 0;
            }
        }

        // -------------------------------------------------------------
        // SLOW LOGIC: SENSORS & NETWORK (Every 1000ms)
        // -------------------------------------------------------------
        sensor_timer_ticks++;
        if (sensor_timer_ticks >= 20) {
            sensor_timer_ticks = 0; // Reset 1-second timer

            bool ade_trip = false;
            bool sensor_error = false;
            float temperature_c = 0.0f;
            ade7953_measurement_t measurement = {0};
            bool ade_measurement_ok = false;

            // BLE Credential Check
            if (module_ble_credentials_received()) {
                char ssid[MAX_NVS_SSID_LEN + 1] = {0};
                char password[MAX_NVS_PASSWORD_LEN + 1] = {0};

                if (module_ble_get_ssid(ssid) == ESP_OK && module_ble_get_password(password) == ESP_OK) {
                    ESP_LOGI(TAG, "Credentials received via BLE, saving to NVS (SSID: %s)", ssid);
                    ESP_ERROR_CHECK(module_nvs_save_wifi_credentials(ssid, password));
                    ESP_ERROR_CHECK(module_ble_stop_advertising());
                    ESP_ERROR_CHECK(module_wifi_connect(ssid, password));
                    credentials_in_nvs = true;
                    g_mqtt_connect_requested = false;
                    module_ble_reset_credentials();
                }
            }

            aice_refresh_network_state(credentials_in_nvs);

            if (module_mqtt_is_connected()) {
                g_mqtt_connect_requested = false;
            }

            // TMP102 Reading
            if (module_tmp102_read_celsius(&temperature_c) == ESP_OK) {
                ESP_LOGI(TAG, "TMP102 temperature: %.2f C", temperature_c);

                if (sd_init_ret == ESP_OK) {
                    char write_buf[128];
                    snprintf(write_buf, sizeof(write_buf), "TMP: %.2f C\n", temperature_c);
                    esp_err_t sd_write_ret = module_sdcard_write_file(test_path, write_buf);
                    if (sd_write_ret != ESP_OK) {
                        ESP_LOGW(TAG, "SD write failed: %s", esp_err_to_name(sd_write_ret));
                        sensor_error = true;
                    }
                }
            } else {
                ESP_LOGW(TAG, "TMP102 read failed");
                sensor_error = true;
            }

            // ADE7953 Reading
            if (ade_init_ret == ESP_OK) {
                if (ade_irq_pending) {
                    ade7953_events_t safety_events = {0};
                    if (module_ade7953_read_events(&safety_events, true) == ESP_OK) {
                        ESP_LOGE(TAG, "ADE safety IRQ: irq_a=0x%06" PRIX32 " irq_b=0x%06" PRIX32 " ov=%d oia=%d oib=%d accmode=0x%06" PRIX32,
                                 safety_events.irq_a,
                                 safety_events.irq_b,
                                 safety_events.overvoltage,
                                 safety_events.overcurrent_a,
                                 safety_events.overcurrent_b,
                                 safety_events.accmode);

                        critical_protection_irq_events = safety_events;
                        critical_protection_irq_events_valid = true;
                    } else {
                        ESP_LOGW(TAG, "ADE safety IRQ occurred, but SPI diagnostics failed");
                        critical_protection_irq_events_valid = false;
                    }
                    critical_protection_active = true;
                    critical_protection_report_pending = true;
                    if (critical_protection_started_ms == 0) {
                        critical_protection_started_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
                    }
                    ade_trip = true;
                }
#if ENABLE_IRQ_DEBUG_READBACK && !ENABLE_WAVEFORM_STREAM
                ade7953_events_t ade_events = {0};
                if (module_ade7953_read_events(&ade_events, false) == ESP_OK) {
                    if (ade_events.waveform_sample_ready || ade_events.line_cycle_end || ade_events.sag ||
                        ade_events.overvoltage || ade_events.overcurrent_a || ade_events.overcurrent_b) {
                        ESP_LOGI(TAG, "ADE IRQ debug: irq_a=0x%06" PRIX32 " irq_b=0x%06" PRIX32,
                                 ade_events.irq_a, ade_events.irq_b);
                    }
                }
#endif
                esp_err_t ade_ret = module_ade7953_read_measurement(&measurement, true, true);
                if (ade_ret == ESP_OK) {
                    ade_measurement_ok = true;
                    ade_trip = ade7953_service_and_should_trip(&measurement, &g_ade_policy.safety_limits, &critical_protection_decision);
                    if (ade_trip && !critical_protection_active) {
                        critical_protection_active = true;
                        critical_protection_report_pending = true;
                        critical_protection_started_ms = measurement.timestamp_ms;
                        critical_protection_measurement = (ade7953_measurement_t){0};
                        critical_protection_irq_events_valid = false;
                        memset(&critical_protection_irq_events, 0, sizeof(critical_protection_irq_events));
                    }

                    if (critical_protection_report_pending && critical_protection_measurement.timestamp_ms == 0) {
                        critical_protection_measurement = measurement;
                        if (!critical_protection_irq_events_valid) {
                            memset(&critical_protection_irq_events, 0, sizeof(critical_protection_irq_events));
                        }
                    }
                } else {
                    ESP_LOGW(TAG, "ADE7953 read failed: %s", esp_err_to_name(ade_ret));
                    sensor_error = true;
                }
            } else {
                sensor_error = true;
            }

            // State Machine & LED Handling
            if (ade_trip || sensor_error || critical_protection_active) {
                module_relay_set(false);
                g_relay_on = false;
                aice_set_status(STATUS_ERROR);
            } 

            if (critical_protection_report_pending && ade_measurement_ok && module_mqtt_is_connected()) {
                const char *cause = critical_protection_cause_from_events(
                    critical_protection_irq_events_valid ? &critical_protection_irq_events : NULL,
                    &critical_protection_measurement,
                    &critical_protection_decision);
                uint32_t duration_cycles = critical_protection_duration_cycles(&critical_protection_measurement,
                                                                               critical_protection_started_ms);
                esp_err_t mqtt_ret = module_mqtt_publish_critical_protection(
                    cause,
                    critical_protection_measurement.timestamp_ms,
                    critical_protection_measurement.voltage_vrms,
                    critical_protection_measurement.current_a_arms,
                    critical_protection_measurement.current_b_arms,
                    duration_cycles,
                    "RELAY_OPEN",
                    "LOCKED_AWAITING_ACK");
                if (mqtt_ret == ESP_OK) {
                    critical_protection_report_pending = false;
                } else {
                    ESP_LOGW(TAG, "MQTT critical-protection publish failed: %s", esp_err_to_name(mqtt_ret));
                }
            }

            // MQTT Publish
            if (module_mqtt_is_connected() && ade_measurement_ok && !ade_trip && !sensor_error && !critical_protection_active) {
                esp_err_t mqtt_ret = module_mqtt_publish_status(
                    temperature_c,
                    measurement.voltage_vrms,
                    measurement.current_a_arms,
                    measurement.power_factor_a,
                    measurement.active_power_a_w,
                    measurement.reactive_power_a_var,
                    measurement.line_frequency_hz,
                    ade7953_measurement_has_no_load(&measurement),
                    (uint32_t)(measurement.active_energy_a_wh_total + 0.5f),
                    g_relay_on);
                if (mqtt_ret != ESP_OK) {
                    ESP_LOGW(TAG, "MQTT status publish failed: %s", esp_err_to_name(mqtt_ret));
                }
            }
        } // End 1000ms block

        // 100ms base delay allows the button to be extremely responsive
        vTaskDelay(pdMS_TO_TICKS(100)); 
    }
}