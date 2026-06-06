#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef esp_err_t (*module_mqtt_safety_limits_handler_t)(float max_vrms, float max_iarms, void *user_data);
typedef esp_err_t (*module_mqtt_relay_command_handler_t)(const char *action, void *user_data);

/**
 * @brief Initialize MQTT module
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t module_mqtt_init(void);

/**
 * @brief Register a handler for MQTT safety-limit commands on topic ayce/cmd
 */
esp_err_t module_mqtt_set_safety_limits_handler(module_mqtt_safety_limits_handler_t handler, void *user_data);

/**
 * @brief Register a handler for relay commands on topic ayce/cmd
 */
esp_err_t module_mqtt_set_relay_command_handler(module_mqtt_relay_command_handler_t handler, void *user_data);

/**
 * @brief Connect to MQTT broker
 *
 * Should be called after WiFi is connected
 *
 * @param broker_ip IP address of MQTT broker (e.g., "192.168.1.100")
 * @param broker_port Port number of MQTT broker (typically 1883)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t module_mqtt_connect(const char *broker_ip, uint16_t broker_port);

/**
 * @brief Connect to MQTT broker using full broker URI
 *
 * Example: "mqtt://127.0.0.1:1883" or "mqtts://example.com:8883"
 */
esp_err_t module_mqtt_connect_uri(const char *broker_uri);

/**
 * @brief Check if MQTT is connected
 *
 * @return true if connected, false otherwise
 */
bool module_mqtt_is_connected(void);

/**
 * @brief Publish relay status
 *
 * Publishes to topic: smartplug/relay
 *
 * @param relay_on true if relay is ON, false if OFF
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t module_mqtt_publish_relay(bool relay_on);

/**
 * @brief Publish LED status (RGB values)
 *
 * Publishes to topic: smartplug/led
 *
 * @param red Red component (0-255)
 * @param green Green component (0-255)
 * @param blue Blue component (0-255)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t module_mqtt_publish_led(uint8_t red, uint8_t green, uint8_t blue);

/**
 * @brief Publish temperature reading
 *
 * Publishes to topic: smartplug/temperature
 *
 * @param temp_celsius Temperature in Celsius
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t module_mqtt_publish_temperature(float temp_celsius);

/**
 * @brief Publish energy readings
 *
 * Publishes to topic: smartplug/energy
 *
 * @param voltage_v Voltage in volts
 * @param current_a Current in amperes
 * @param power_w Power in watts
 * @param energy_wh Energy in watt-hours
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t module_mqtt_publish_energy(float voltage_v, float current_a, float power_w, uint32_t energy_wh);

/**
 * @brief Publish a critical-protection event
 *
 * Publishes to topic: smartplug/events
 */
esp_err_t module_mqtt_publish_critical_protection(const char *cause,
												  uint64_t timestamp_ms,
										  float voltage_vrms,
										  float current_a_arms,
										  float current_b_arms,
												  uint32_t duration_cycles,
												  const char *action_taken,
												  const char *system_status);

/**
 * @brief Publish combined status (temperature + ADE telemetry)
 *
 * Publishes to topic: smartplug/status with JSON payload
 *
 * @param temperature_c Temperature in Celsius
 * @param vrms Voltage in volts
 * @param irms Current in amperes
 * @param pf Power factor
 * @param active_power Active power in watts
 * @param reactive_power Reactive power in vars
 * @param frequency Line frequency in hertz
 * @param no_load No-load flag
 * @param energy_wh Energy in watt-hours
 * @param relay_state Relay status
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t module_mqtt_publish_status(float temperature_c,
											 float vrms,
											 float irms,
											 float pf,
											 float active_power,
											 float reactive_power,
											 float frequency,
											 bool no_load,
											 float energy_wh,
											 bool relay_state);

/**
 * @brief Publish a waveform chunk payload
 *
 * Publishes to topic: smartplug/waveform/chunk
 */
esp_err_t module_mqtt_publish_waveform_chunk(const char *payload);

/**
 * @brief Disconnect from MQTT broker
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t module_mqtt_disconnect(void);
