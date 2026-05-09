#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Initialize MQTT module
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t module_mqtt_init(void);

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
 * @brief Publish combined status (temperature + energy readings)
 *
 * Publishes to topic: smartplug/status with JSON payload
 *
 * @param temp_celsius Temperature in Celsius
 * @param voltage_v Voltage in volts
 * @param current_a Current in amperes
 * @param power_w Power in watts
 * @param energy_wh Energy in watt-hours
 * @param relay_on Relay status
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t module_mqtt_publish_status(float temp_celsius, float voltage_v, float current_a, float power_w, uint32_t energy_wh, bool relay_on);

/**
 * @brief Disconnect from MQTT broker
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t module_mqtt_disconnect(void);
