#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include <inttypes.h>
#include <stdlib.h>
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

static const char *TAG = "ayce_smartplug"; // Updated TAG to AYCE
static bool g_relay_on = false;
static bool g_mqtt_connect_requested = false;
static TickType_t g_mqtt_connect_requested_tick = 0;
static bool g_gui_relay_explicit_cmd_pending = false;
static bool g_gui_relay_explicit_cmd_target = false;
static bool g_gui_relay_toggle_requested = false;

static const char *MQTT_BROKER_IP = "192.168.137.1";
static const uint16_t MQTT_BROKER_PORT = 1883;

static TaskHandle_t s_main_task_handle = NULL;
static gpio_num_t s_relay_gpio = GPIO_NUM_NC;
static gpio_num_t s_ade_irq_gpio = GPIO_NUM_NC;
static portMUX_TYPE g_gui_relay_request_mux = portMUX_INITIALIZER_UNLOCKED;

#ifndef ENABLE_WAVEFORM_STREAM
#define ENABLE_WAVEFORM_STREAM 1
#endif

#if ENABLE_WAVEFORM_STREAM
#define WAVEFORM_CHUNK_SIZE 512
#define WAVEFORM_JSON_SCRATCHPAD_SIZE 16384

static int32_t s_raw_v_buf0[WAVEFORM_CHUNK_SIZE];
static int32_t s_raw_i_buf0[WAVEFORM_CHUNK_SIZE];
static int32_t s_raw_v_buf1[WAVEFORM_CHUNK_SIZE];
static int32_t s_raw_i_buf1[WAVEFORM_CHUNK_SIZE];

static volatile uint8_t s_active_buffer = 0;
static volatile uint16_t s_chunk_index = 0;
static volatile uint8_t s_ready_buffer = 0;
static volatile uint16_t s_ready_count = 0;
static volatile bool s_chunk_ready = false;
static volatile bool s_snapshot_request_pending = false;
static volatile bool s_snapshot_active = false;
static TaskHandle_t s_waveform_capture_task_handle = NULL;
static TaskHandle_t s_waveform_pub_task_handle = NULL;
static portMUX_TYPE s_waveform_mux = portMUX_INITIALIZER_UNLOCKED;

// --- DEBUG SILENT COUNTERS ---
static volatile uint32_t dbg_irq_wakeups = 0;
static volatile uint32_t dbg_spi_failures = 0;
static volatile uint32_t dbg_last_sample_index = 0;
static volatile uint64_t dbg_last_wakeup_time_us = 0;

#endif

#ifndef ENABLE_IRQ_DEBUG_READBACK
#define ENABLE_IRQ_DEBUG_READBACK 0
#endif

#if ENABLE_WAVEFORM_STREAM


/**
 * @brief Unified ISR handler for the ADE7953 interrupt pin.
 * Acts as a traffic cop routing the hardware interrupt to the appropriate FreeRTOS task.
 * If a waveform capture is active, it wakes up the high-priority capture task.
 * Otherwise, it wakes up the standard main task for routine telemetry.
 */

static void IRAM_ATTR unified_ade_gpio_isr_handler(void *arg) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    // Traffic Cop Logic
    if (module_ade7953_is_waveform_capturing()) {
        
        // 1. WAVEFORM MODE: Route to the high-speed background task
        if (s_waveform_capture_task_handle != NULL) {
            vTaskNotifyGiveFromISR(s_waveform_capture_task_handle, &xHigherPriorityTaskWoken);
        }
        
    } else {
        
        // 2. NORMAL MODE: Route to main task
        if (s_main_task_handle != NULL) {
            vTaskNotifyGiveFromISR(s_main_task_handle, &xHigherPriorityTaskWoken);
        }
        
    }

    // Force a context switch if a higher priority task just woke up
    if (xHigherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/**
 * @brief Initiates a high-speed waveform snapshot capture.
 * * Verifies system state, clears pending events, initializes buffers, sets the 
 * ADE7953 into waveform mode, configures the IRQ masks to fire on new samples (WSMP), 
 * and wakes up the waveform capture task.
 * * @return ESP_OK if successfully started, or an error code if already busy/invalid state.
 */

esp_err_t module_ade7953_start_snapshot_capture(void)
{
    // Ensure the necessary FreeRTOS tasks are actually running
    if (s_waveform_capture_task_handle == NULL || s_waveform_pub_task_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    // Purge any lingering ADE7953 events before starting fresh
    ade7953_events_t dummy_events = {0};
    module_ade7953_read_events(&dummy_events, true);

    // Reset ping-pong buffer indices and state flags
    portENTER_CRITICAL(&s_waveform_mux);
    if (s_snapshot_request_pending || s_snapshot_active) {
        portEXIT_CRITICAL(&s_waveform_mux);
        return ESP_ERR_INVALID_STATE;
    }

    // Tell the ADE driver to enter waveform streaming mode
    s_active_buffer = 0;
    s_chunk_index = 0;
    s_ready_buffer = 0;
    s_ready_count = 0;
    s_chunk_ready = false;
    s_snapshot_request_pending = true;
    ESP_ERROR_CHECK_WITHOUT_ABORT(module_ade7953_set_waveform_capture_mode(true));
    portEXIT_CRITICAL(&s_waveform_mux);

    // Configure hardware interrupts for Overvoltage, Overcurrent, and Waveform Sample Ready (WSMP)
    esp_err_t irq_ret = module_ade7953_configure_irq_masks(ADE7953_IRQ_A_OV | ADE7953_IRQ_A_OIA | ADE7953_IRQ_A_WSMP, 0);
    if (irq_ret != ESP_OK) {
        portENTER_CRITICAL(&s_waveform_mux);
        s_snapshot_request_pending = false;
        portEXIT_CRITICAL(&s_waveform_mux);
        return irq_ret;
    }
    // Write 0x00 to WAVMODE register to select Voltage and Current streaming
    module_ade7953_write32(0x018, 0x00); // 0x018 is the WAVMODE register

    // Wake up the background capture task
    xTaskNotifyGive(s_waveform_capture_task_handle);
    ESP_LOGI(TAG, "Waveform snapshot requested: %d samples", WAVEFORM_CHUNK_SIZE);
    return ESP_OK;
}

/**
 * @brief FreeRTOS task responsible for formatting and publishing waveform data via MQTT.
 * Waits for a complete chunk of data from the high-speed capture task, applies 
 * calibration scaling to convert raw LSBs into real-world Volts and Amps, 
 * packages it into a JSON array, and sends it out over MQTT.
 */
static void waveform_stream_task(void *pvParameters)
{
    s_waveform_pub_task_handle = xTaskGetCurrentTaskHandle();
    // Allocate a large buffer for the JSON string on the heap to avoid stack overflow
    char *json_scratchpad = malloc(WAVEFORM_JSON_SCRATCHPAD_SIZE);
    if (json_scratchpad == NULL) {
        ESP_LOGE(TAG, "Failed to allocate waveform JSON scratchpad");
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        // 1. WAIT FOREVER until the fast task gives us the 512 samples!
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // Lock in the buffer that is ready for processing (ping-pong logic)
        int32_t *v_ptr = NULL;
        int32_t *i_ptr = NULL;
        uint16_t sample_count = 0;

        portENTER_CRITICAL(&s_waveform_mux);
        if (s_chunk_ready) {
            const uint8_t ready_buffer = s_ready_buffer;
            sample_count = s_ready_count;
            v_ptr = (ready_buffer == 0) ? s_raw_v_buf0 : s_raw_v_buf1;
            i_ptr = (ready_buffer == 0) ? s_raw_i_buf0 : s_raw_i_buf1;
            s_chunk_ready = false;
        }
        portEXIT_CRITICAL(&s_waveform_mux);

        if (v_ptr == NULL || i_ptr == NULL || sample_count == 0) {
            continue;
        }
        // Retrieve current calibration constants to convert LSBs to floats
        ade7953_calibration_t cal = {0};
        module_ade7953_get_calibration(&cal);
        const float v_scale = cal.volts_per_vrms_lsb > 0.0f ? cal.volts_per_vrms_lsb : 0.00001903f;
        const float i_scale = cal.amps_per_irmsa_lsb > 0.0f ? cal.amps_per_irmsa_lsb : 0.00000076f;
        // Build the JSON Payload
        int offset = snprintf(json_scratchpad, WAVEFORM_JSON_SCRATCHPAD_SIZE,
                              "{\"event_type\":\"WAVEFORM_CHUNK\",\"count\":%" PRIu16 ",\"v\":[",
                              sample_count);
        // Append Voltage samples
        for (uint16_t i = 0; i < sample_count && offset < WAVEFORM_JSON_SCRATCHPAD_SIZE; ++i) {
            offset += snprintf(json_scratchpad + offset, WAVEFORM_JSON_SCRATCHPAD_SIZE - offset,
                               "%.2f%s", (float)v_ptr[i] * v_scale, (i + 1U == sample_count) ? "" : ",");
        }

        offset += snprintf(json_scratchpad + offset, WAVEFORM_JSON_SCRATCHPAD_SIZE - offset, "],\"i\":[");
        // Append Current samples
        for (uint16_t i = 0; i < sample_count && offset < WAVEFORM_JSON_SCRATCHPAD_SIZE; ++i) {
            offset += snprintf(json_scratchpad + offset, WAVEFORM_JSON_SCRATCHPAD_SIZE - offset,
                               "%.3f%s", (float)i_ptr[i] * i_scale, (i + 1U == sample_count) ? "" : ",");
        }
        // Publish to MQTT
        snprintf(json_scratchpad + offset, WAVEFORM_JSON_SCRATCHPAD_SIZE - offset, "]}");

        esp_err_t publish_ret = module_mqtt_publish_waveform_chunk(json_scratchpad);
        if (publish_ret != ESP_OK) {
            ESP_LOGW(TAG, "Waveform chunk publish failed: %s", esp_err_to_name(publish_ret));
        } else {
            ESP_LOGI(TAG, "Waveform snapshot sent successfully");
        }

        // 2. CLEANUP: Lower the shields and turn off the hardware stream
        ESP_ERROR_CHECK_WITHOUT_ABORT(module_ade7953_set_waveform_capture_mode(false));
        ESP_ERROR_CHECK_WITHOUT_ABORT(module_ade7953_configure_irq_masks(ADE7953_IRQ_A_OV | ADE7953_IRQ_A_OIA, 0));
    }

    free(json_scratchpad);
}
#endif

/**
 * @brief Represents the high-level connection and operational state of the AYCE SmartPlug.
 */
typedef enum {
    STATUS_IDLE = 0,
    STATUS_BLE_WAITING,         // Blinking Blue (Waiting for credentials)
    STATUS_WIFI_CONNECTING,     // Solid Blue (Credentials received, connecting to Wi-Fi)
    STATUS_MQTT_CONNECTING,     // Blinking Green (Wi-Fi connected, connecting to MQTT)
    STATUS_MQTT_CONNECTED,      // Solid Green (MQTT connected)
    STATUS_ERROR,               // Solid Red (ADE trip or sensor error)
} ayce_status_t;

/**
 * @brief Helper to atomically update the global system status.
 */
static volatile ayce_status_t g_current_status = STATUS_IDLE;

// Helper to update the system status
static void ayce_set_status(ayce_status_t s)
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
/**
 * @brief Evaluates current network flags (WiFi/MQTT) and updates the global device state.
 * * Also handles MQTT connection initiation and timeout retries.
 * * @param credentials_in_nvs Boolean indicating if WiFi credentials exist in flash memory.
 */
static void ayce_refresh_network_state(bool credentials_in_nvs)
{
    const bool wifi_connected = module_wifi_is_connected();
    const bool mqtt_connected = module_mqtt_is_connected();

    if (mqtt_connected) {
        g_mqtt_connect_requested = false;
        ayce_set_status(STATUS_MQTT_CONNECTED);
        return;
    }

    if (!wifi_connected) {
        g_mqtt_connect_requested = false;
        if (credentials_in_nvs) {
            ayce_set_status(STATUS_WIFI_CONNECTING);
        } else {
            ayce_set_status(STATUS_BLE_WAITING);
        }
        return;
    }

    ayce_set_status(STATUS_MQTT_CONNECTING);

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

/**
 * @brief Callback to dynamically update the ADE hardware safety limits received from MQTT.
 */
static esp_err_t ayce_apply_safety_limits_update(float max_vrms, float max_iarms, void *user_data)
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

/**
 * @brief MQTT Callback function to parse and queue relay control commands safely.
 * * Uses a Mutex to protect global flags that will be consumed by the main loop.
 */
static esp_err_t ayce_queue_relay_command(const char *action, void *user_data)
{
    (void)user_data;

    if (action == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "MQTT Callback evaluating relay action: '%s'", action);

    portENTER_CRITICAL(&g_gui_relay_request_mux);

    // 1. Explicit commands from the GUI
    if (strcmp(action, "RELAY_ON") == 0 || strcmp(action, "ON") == 0) {
        g_gui_relay_explicit_cmd_pending = true;
        g_gui_relay_explicit_cmd_target = true;  // Target = ON
        portEXIT_CRITICAL(&g_gui_relay_request_mux);
        return ESP_OK; // Prevents the 258 Error!
    } 
    else if (strcmp(action, "RELAY_OFF") == 0 || strcmp(action, "OFF") == 0) {
        g_gui_relay_explicit_cmd_pending = true;
        g_gui_relay_explicit_cmd_target = false; // Target = OFF
        portEXIT_CRITICAL(&g_gui_relay_request_mux);
        return ESP_OK; // Prevents the 258 Error!
    }
    // 2. Legacy Toggle command (Keeps old functionality intact)
    else if (strcmp(action, "toggle_relay") == 0 || strcmp(action, "relay_toggle") == 0 || strcmp(action, "RELAY_TOGGLE") == 0) {
        g_gui_relay_toggle_requested = true;
        portEXIT_CRITICAL(&g_gui_relay_request_mux);
        return ESP_OK;
    }

    // 3. If it receives garbage text, reject it
    portEXIT_CRITICAL(&g_gui_relay_request_mux);
    ESP_LOGE(TAG, "Unrecognized relay action received via MQTT: '%s'", action);
    return ESP_ERR_INVALID_ARG;
}

/**
 * @brief Evaluates grid recovery and processes relay state modification requests.
 *
 * Checks if the system is currently recovering from a critical safety trip (e.g., overvoltage). 
 * If a critical condition was active, it forces a real-time readout from the ADE7953 to 
 * verify if line parameters have stabilized back within safe limits before re-enabling the 
 * relay. If conditions are safe or if the system was in a normal state, it safely toggles 
 * the relay state and publishes an updated notification packet over MQTT.
 */
static void ayce_handle_relay_toggle_request(const char *source,
                                             bool credentials_in_nvs,
                                             bool *critical_protection_active,
                                             uint64_t *critical_protection_started_ms)
{
    if (critical_protection_active == NULL || critical_protection_started_ms == NULL) {
        return;
    }

    if (*critical_protection_active) {
        ade7953_measurement_t recovery_measurement = {0};
        ade7953_safety_decision_t recovery_decision = {0};
        esp_err_t recovery_ret = module_ade7953_read_measurement(&recovery_measurement, true, true);
        if (recovery_ret == ESP_OK) {
            module_ade7953_evaluate_safety(&recovery_measurement, &g_ade_policy.safety_limits, &recovery_decision);
            if (!recovery_decision.trip) {
                *critical_protection_active = false;
                *critical_protection_started_ms = 0;
                g_relay_on = true;
                module_relay_set(true);
                ESP_ERROR_CHECK_WITHOUT_ABORT(module_mqtt_publish_relay(g_relay_on));
                ESP_LOGI(TAG, "%s: critical protection cleared, relay closed", source != NULL ? source : "Relay request");
                ayce_refresh_network_state(credentials_in_nvs);
            } else {
                module_relay_set(false);
                g_relay_on = false;
                ESP_ERROR_CHECK_WITHOUT_ABORT(module_mqtt_publish_relay(g_relay_on));
                ayce_set_status(STATUS_ERROR);
                ESP_LOGW(TAG, "%s ignored: protection condition still present", source != NULL ? source : "Relay request");
            }
        } else {
            module_relay_set(false);
            g_relay_on = false;
            ESP_ERROR_CHECK_WITHOUT_ABORT(module_mqtt_publish_relay(g_relay_on));
            ayce_set_status(STATUS_ERROR);
            ESP_LOGW(TAG, "%s could not verify recovery: %s", source != NULL ? source : "Relay request", esp_err_to_name(recovery_ret));
        }
    } else {
        g_relay_on = !g_relay_on;
        module_relay_set(g_relay_on);
        ESP_ERROR_CHECK_WITHOUT_ABORT(module_mqtt_publish_relay(g_relay_on));
        ESP_LOGI(TAG, "%s: Relay toggled %s", source != NULL ? source : "Relay request", g_relay_on ? "ON" : "OFF");
    }
}
/**
 * @brief Pulls real-time telemetry from the ADE7953 and verifies electrical safety limits.
 *
 * Logs extensive metrics—including VRMS, IRMS, active/apparent power, line frequency, 
 * power factor, total energy accumulation, and internal raw register states—to the serial 
 * interface. Following diagnostic telemetry output, it assesses measurements against 
 * software policy boundaries to determine if a hardware safety cutoff condition has occurred.
 *
 */
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
/**
 * @brief Assesses whether the active load on the smart plug drops below operational thresholds.
 *
 * Queries the raw Accumulation Mode (ACCMODE) flags mapped out from the ADE7953 energy metrics 
 * to parse whether the silicon has flagged an active channel 'no-load' status bit.
 *
 * @param[in] measurement Pointer to the evaluation measurement structure.
 * * @return true if the chip reports no-load status on Channel A, false if a load is detected or argument is NULL.
 */
static bool ade7953_measurement_has_no_load(const ade7953_measurement_t *measurement)
{
    if (measurement == NULL) {
        return false;
    }

    return (measurement->raw.accmode & ADE7953_ACCMODE_ACTNLOAD_A) != 0;
}

/**
 * @brief Resolves active hardware event flags and software decisions into a descriptive string literal.
 *
 * Inspects a combination of raw hardware interrupt events and downstream software trip decisions 
 * to isolate the precise root cause triggering an emergency relay isolation (e.g., pure Overcurrent, 
 * an Overcurrent sag under low voltage bounds, or Overvoltage).
 */
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

/**
 * @brief Quantifies the temporal duration of a critical fault state translated into line cycles.
 *
 * Calculates the delta time elapsed from the initial ignition of a safety fault condition 
 * to the terminal snapshot timestamp, converting milliseconds into physical line cycles 
 * proportional to the active grid frequency.
 */
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
/**
 * @brief Hard-wired IRAM interrupt service handler executing rapid safety fault isolation.
 * This function handles safety-critical interrupts from the energy monitoring chip. To achieve 
 * sub-millisecond response loops, it bypasses the standard operating system scheduler queues by 
 * interacting directly with the peripheral registers to clear the relay driver GPIO line. It then 
 * triggers non-blocking context switch notifications to unblock background handler tasks.
 */
static void IRAM_ATTR ade7953_safety_irq_handler(void *arg)
{
    if (s_relay_gpio != GPIO_NUM_NC) {
        gpio_set_level(s_relay_gpio, 0);
    }

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (s_main_task_handle != NULL) {
        vTaskNotifyGiveFromISR(s_main_task_handle, &xHigherPriorityTaskWoken);
    }
#if ENABLE_WAVEFORM_STREAM
    if (s_waveform_capture_task_handle != NULL) {
        vTaskNotifyGiveFromISR(s_waveform_capture_task_handle, &xHigherPriorityTaskWoken);
    }
#endif
    if (xHigherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

#if ENABLE_WAVEFORM_STREAM
/**
 * @brief High-priority, microsecond-accurate task executing bounded polling waveform capture.
 *
 * Implements a time-critical tracking block utilizing hardware micro-delays (`esp_rom_delay_us`) 
 * to sample raw voltage and current waveforms out of the ADE7953 over SPI at exactly ~2400 Hz. 
 * Bypasses tick-based RTOS context yielding to eliminate scheduling jitter during the sampling window. 
 * Employs a fallback retry configuration and a strict execution watchdog limit to protect the 
 * processor from watchdog timeouts and background thread starvation.
 *
 * @param[in] pvParameters Generic FreeRTOS parameter block context pointer.
 */
static void waveform_capture_task(void *pvParameters)
{
    (void)pvParameters;
    s_waveform_capture_task_handle = xTaskGetCurrentTaskHandle();

    while (true) {
        // Sleep permanently until the MQTT handler triggers a new snapshot request
        if (ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 0) {
            continue;
        }

        int attempts_left = 5;
        bool capture_successful = false;
        
        uint64_t profile_start_us = 0;
        uint64_t profile_end_us = 0;

        while (attempts_left > 0 && !capture_successful) {
            
            if (attempts_left < 5) {
                ESP_LOGW(TAG, "Retrying waveform capture... Attempts remaining: %d", attempts_left);
                
                ade7953_events_t cleanup_events;
                module_ade7953_read_events(&cleanup_events, true);
                
                portENTER_CRITICAL(&s_waveform_mux);
                s_snapshot_request_pending = false;
                s_snapshot_active = true;
                s_chunk_index = 0;
                portEXIT_CRITICAL(&s_waveform_mux);

                // NOTE: We no longer re-arm the high-speed WSMP interrupt here
                // because we are using software polling!
            } 
            else if (s_snapshot_request_pending) {
                s_snapshot_request_pending = false;
                s_snapshot_active = true;
                s_chunk_index = 0;
                
                dbg_irq_wakeups = 0; 
                dbg_spi_failures = 0;
            }

            if (!s_snapshot_active) {
                break; 
            }

            uint64_t capture_start_time_us = (uint64_t)esp_timer_get_time();
            profile_start_us = capture_start_time_us; 
            bool snapshot_timeout_tripped = false;

            // ==============================================================
            // SOFTWARE POLLING TARGET: 2400 Hz (416 microseconds)
            // ==============================================================
            uint64_t target_period_us = 416; 
            uint64_t next_sample_time_us = capture_start_time_us + target_period_us;

            // Purge any lingering FreeRTOS notifications before we enter the fast loop
            // to ensure no leftover hardware interrupts mess with our timing.
            ulTaskNotifyTake(pdTRUE, 0);

            // --- HIGH SPEED SOFTWARE-DRIVEN SAMPLING WINDOW ---
            while (s_snapshot_active) {
                
                // Watchdog: 1 full second limit
                if (((uint64_t)esp_timer_get_time() - capture_start_time_us) > 1000000ULL) {
                    snapshot_timeout_tripped = true;
                    break; 
                }

                // 1. PRECISION MICROSECOND WAIT LOOP 
                // This completely replaces ulTaskNotifyTake!
                while ((uint64_t)esp_timer_get_time() < next_sample_time_us) {
                    esp_rom_delay_us(5); // Tiny micro-sleep prevents OS crashes
                }

                // Lock in target for the next loop to prevent drift
                next_sample_time_us += target_period_us;

                dbg_last_wakeup_time_us = (uint64_t)esp_timer_get_time(); 

                // 2. Read latest data from ADE7953 over SPI
                int32_t v_sample = 0;
                int32_t i_sample = 0;
                if (module_ade7953_read_waveform_samples(&v_sample, &i_sample) != ESP_OK) {
                    dbg_spi_failures++;
                    continue;
                }

                portENTER_CRITICAL(&s_waveform_mux);
                int32_t *v_buf = (s_active_buffer == 0) ? s_raw_v_buf0 : s_raw_v_buf1;
                int32_t *i_buf = (s_active_buffer == 0) ? s_raw_i_buf0 : s_raw_i_buf1;
                
                v_buf[s_chunk_index] = v_sample;
                i_buf[s_chunk_index] = i_sample;
                s_chunk_index++;
                dbg_last_sample_index = s_chunk_index;

                if (s_chunk_index >= WAVEFORM_CHUNK_SIZE) {
                    profile_end_us = (uint64_t)esp_timer_get_time(); 
                    s_ready_buffer = s_active_buffer;
                    s_ready_count = s_chunk_index;
                    s_chunk_ready = true;
                    s_snapshot_active = false; 
                    s_active_buffer = (s_active_buffer == 0) ? 1 : 0; 
                    s_chunk_index = 0;
                    capture_successful = true; 
                }
                portEXIT_CRITICAL(&s_waveform_mux);
            }

            if (snapshot_timeout_tripped) {
                ESP_LOGE(TAG, "Attempt failed! Watchdog tripped at sample %" PRIu32 "/512.", dbg_last_sample_index);
                attempts_left--;
                
                portENTER_CRITICAL(&s_waveform_mux);
                s_snapshot_active = false;
                portEXIT_CRITICAL(&s_waveform_mux);
                
                vTaskDelay(pdMS_TO_TICKS(10)); 
            }
        }

        if (!capture_successful) {
            ESP_LOGE(TAG, "Fatal: All 5 capture attempts failed.");
            
            portENTER_CRITICAL(&s_waveform_mux);
            s_snapshot_active = false;
            s_chunk_ready = false;
            s_chunk_index = 0;
            portEXIT_CRITICAL(&s_waveform_mux);

            ESP_ERROR_CHECK_WITHOUT_ABORT(module_ade7953_set_waveform_capture_mode(false));
            ESP_ERROR_CHECK_WITHOUT_ABORT(module_ade7953_configure_irq_masks(ADE7953_IRQ_A_OV | ADE7953_IRQ_A_OIA, 0));
            
            ade7953_events_t final_cleanup;
            module_ade7953_read_events(&final_cleanup, true);
            continue; 
        }

        // --- SUCCESS PATH: Profile and Publish ---
        uint64_t total_duration_us = profile_end_us - profile_start_us;
        float actual_sampling_rate_hz = 0.0f;
        if (total_duration_us > 0) {
            actual_sampling_rate_hz = ((float)WAVEFORM_CHUNK_SIZE / (float)total_duration_us) * 1000000.0f;
        }
        
        ESP_LOGI(TAG, "Profile: Captured %d samples safely. Elapsed time: %llu us. Measured Frequency: %.2f Hz", 
                 WAVEFORM_CHUNK_SIZE, total_duration_us, actual_sampling_rate_hz);

        if (s_waveform_pub_task_handle != NULL) {
            xTaskNotifyGive(s_waveform_pub_task_handle);
        }
    }
}
#endif

/**
 * @brief Initializes and registers the low-level GPIO interrupt service for the ADE7953.
 *
 * Configures the specified physical IRQ pin as a digital input using the electrical policy's 
 * resistor pull settings and hardware edge trigger properties. It installs the ESP32 global 
 * ISR service framework (safely ignoring any errors if it was previously instantiated by 
 * another module) and attaches the unified callback driver to handle immediate runtime events.
 */
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
    ESP_ERROR_CHECK(gpio_isr_handler_add(pins->ade_irq_gpio, unified_ade_gpio_isr_handler, NULL));
}

/**
 * @brief Asynchronous FreeRTOS execution task managing device status signaling via RGB LED.
 * 
 * Runs continuously in an infinite background loop to read the global system connection state 
 * machine (`g_current_status`). It drives custom pulse and solid illumination indicators to visually 
 * represent BLE provisioning, Wi-Fi transitions, and MQTT health. 
 * * @note This handler explicitly handles inverted GRB (Green-Red-Blue) physical hardware matrices 
 * by software-mapping target Red requests onto the physical Green lane register, and vice versa.
 * @param[in] pvParameters Generic FreeRTOS parameter block context pointer (unused).
 */
static void led_control_task(void *pvParameters)
{
    bool toggle = false;
    while (1) {
        ayce_status_t current = g_current_status;
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
/**
 * @brief Configures the physical user-button GPIO line for digital input polling.
 *
 * Allocates explicit hardware properties to a specified push-button pin—forcing an internal 
 * pull-up electrical resistor profile while entirely suppressing edge-triggered interrupts to 
 * ensure safe state evaluation via standard main loop software debouncing routines.
 */
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

    esp_log_level_set("ayce_smartplug", ESP_LOG_INFO);
    ESP_LOGI(TAG, "AYCE bring-up boot (%s)", smartplug_board_profile_name());
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
    ESP_ERROR_CHECK(module_mqtt_set_safety_limits_handler(ayce_apply_safety_limits_update, NULL));
    ESP_ERROR_CHECK(module_mqtt_set_relay_command_handler(ayce_queue_relay_command, NULL));

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

    ayce_refresh_network_state(credentials_in_nvs);

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
            const float kw_lsb = ade_cal.volts_per_vrms_lsb * ade_cal.amps_per_irmsa_lsb * 16777216.0f;
            ade_cal.watts_per_awatt_lsb = kw_lsb;
            ade_cal.watts_per_bwatt_lsb = kw_lsb;
            ade_cal.vars_per_avar_lsb = kw_lsb;
            ade_cal.vars_per_bvar_lsb = kw_lsb;
            ade_cal.va_per_ava_lsb = kw_lsb;
            ade_cal.va_per_bva_lsb = kw_lsb;

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

#if ENABLE_WAVEFORM_STREAM
        xTaskCreate(waveform_stream_task, "waveform_stream_task", 4096, NULL, 5, NULL);
        xTaskCreatePinnedToCore(waveform_capture_task, 
                       "waveform_capture_task", 
                       4096, 
                       NULL, 
                       20,         // Keep priority high
                       NULL, 
                       1);
#endif
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
            if (module_ade7953_is_waveform_capturing()) {
                // Do absolutely nothing! 
            }
            
            else if (!critical_protection_active) {
                g_relay_on = false;            // Sync the MQTT payload state
                ayce_set_status(STATUS_ERROR); // Turn the LED Red

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
                        ayce_handle_relay_toggle_request("Button short press", credentials_in_nvs, &critical_protection_active, &critical_protection_started_ms);
                    } else {
                        ayce_handle_relay_toggle_request("Button short press", credentials_in_nvs, &critical_protection_active, &critical_protection_started_ms);
                    }
                }
                button_was_pressed = false;
                button_press_ticks = 0;
            }
        }

        bool gui_relay_toggle_requested = false;
        bool explicit_cmd_pending = false;
        bool explicit_cmd_target = false;

        // Safely extract ALL the flags
        portENTER_CRITICAL(&g_gui_relay_request_mux);
        
        if (g_gui_relay_toggle_requested) {
            gui_relay_toggle_requested = true;
            g_gui_relay_toggle_requested = false;
        }
        if (g_gui_relay_explicit_cmd_pending) {
            explicit_cmd_pending = true;
            explicit_cmd_target = g_gui_relay_explicit_cmd_target;
            g_gui_relay_explicit_cmd_pending = false;
        }
        
        portEXIT_CRITICAL(&g_gui_relay_request_mux);

        if (gui_relay_toggle_requested) {
            ayce_handle_relay_toggle_request("GUI relay command", credentials_in_nvs, &critical_protection_active, &critical_protection_started_ms);
        }

        // 2. Process NEW Explicit "ON/OFF" logic (from our new GUI)
        if (explicit_cmd_pending) {
            ESP_LOGI(TAG, "Executing explicit MQTT command: %s", explicit_cmd_target ? "ON" : "OFF");
            
            // Physically toggle the hardware relay pin
            module_relay_set(explicit_cmd_target);
            
            // Update the global telemetry variable so the GUI receives the right state
            g_relay_on = explicit_cmd_target;
            
            // If the user forces the plug back ON manually after a trip, clear the alarm
            if (explicit_cmd_target == true && critical_protection_active) {
                ESP_LOGI(TAG, "Clearing critical protection state due to explicit user ON command");
                critical_protection_active = false;
            }
        }

        // -------------------------------------------------------------
        // SLOW LOGIC: SENSORS & NETWORK (Every 1000ms)
        // -------------------------------------------------------------
        sensor_timer_ticks++;
        if (sensor_timer_ticks >= 10) {
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

            ayce_refresh_network_state(credentials_in_nvs);

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
                if (module_ade7953_is_waveform_capturing()) {
                ESP_LOGW(TAG, "Waveform STUCK! Debug -> Wakeups: %" PRIu32 " | Index: %" PRIu32 "/512 | SPI Fails: %" PRIu32 " | Time since last IRQ: %llu us",
                     dbg_irq_wakeups,
                     dbg_last_sample_index,
                     dbg_spi_failures,
                     (esp_timer_get_time() - dbg_last_wakeup_time_us));
                } else {
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
            }} else {
                sensor_error = true;
            }

            // State Machine & LED Handling
            if (ade_trip || sensor_error || critical_protection_active) {
                module_relay_set(false);
                g_relay_on = false;
                ayce_set_status(STATUS_ERROR);
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
                    measurement.active_energy_a_wh_total,
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