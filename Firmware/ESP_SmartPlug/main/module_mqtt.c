#include "module_mqtt.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "module_ade7953.h"
#include "module_relay.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "CJson.h"

static const char *TAG = "module_mqtt";

/* Standardized MQTT topic contract */
#define TOPIC_TELEMETRY_STATUS       "smartplug/telemetry/status"
#define TOPIC_TELEMETRY_TEMPERATURE  "smartplug/telemetry/temperature"
#define TOPIC_TELEMETRY_ENERGY       "smartplug/telemetry/energy"
#define TOPIC_EVENTS_PROTECTION      "smartplug/events/protection"
#define TOPIC_STATE_RELAY            "smartplug/state/relay"
#define TOPIC_STATE_LED              "smartplug/state/led"
#define TOPIC_COMMAND_ACK            "smartplug/commands/ack"
#define TOPIC_COMMAND_RELAY          "smartplug/commands/relay"
#define TOPIC_COMMAND_CONFIG         "smartplug/commands/config"
#define TOPIC_WAVEFORM_REQUEST       "smartplug/waveform/request"
#define TOPIC_WAVEFORM_DATA          "smartplug/waveform/data"

/* Legacy development topics accepted during transition only. */
#define LEGACY_TOPIC_STATUS          "smartplug/status"
#define LEGACY_TOPIC_EVENTS          "smartplug/events"
#define LEGACY_TOPIC_RELAY           "smartplug/relay"
#define LEGACY_TOPIC_LED             "smartplug/led"
#define LEGACY_TOPIC_TEMPERATURE     "smartplug/temperature"
#define LEGACY_TOPIC_ENERGY          "smartplug/energy"
#define LEGACY_TOPIC_RELAY_CMD       "smartplug/cmd"
#define LEGACY_TOPIC_AICE_CMD        "aice/cmd"
#define LEGACY_TOPIC_AICE_STATUS     "aice/status"
#define LEGACY_TOPIC_WAVEFORM_CHUNK  "smartplug/waveform/chunk"
#define LEGACY_TOPIC_WAVEFORM_CMD    "smartplug/waveform/cmd"

/* MQTT client handle */
static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_connected = false;
static module_mqtt_safety_limits_handler_t s_safety_limits_handler = NULL;
static void *s_safety_limits_handler_user_data = NULL;
static module_mqtt_relay_command_handler_t s_relay_command_handler = NULL;
static void *s_relay_command_handler_user_data = NULL;

static bool mqtt_topic_matches(const esp_mqtt_event_handle_t event, const char *topic)
{
	if (event == NULL || topic == NULL) {
		return false;
	}

	size_t topic_len = strlen(topic);
	return event->topic_len == (int)topic_len && strncmp(event->topic, topic, topic_len) == 0;
}


static bool mqtt_payload_equals(const esp_mqtt_event_handle_t event, const char *expected)
{
	if (event == NULL || event->data == NULL || expected == NULL) {
		return false;
	}

	size_t expected_len = strlen(expected);
	return event->data_len == (int)expected_len && strncmp(event->data, expected, expected_len) == 0;
}

static bool command_string_is_waveform_request(const char *command)
{
	return command != NULL &&
	       (strcmp(command, "REQUEST_WAVEFORM") == 0 ||
	        strcmp(command, "send_waveform") == 0 ||
	        strcmp(command, "capture_wave") == 0);
}

static void publish_safety_limits_ack(float max_vrms, float max_iarms, bool accepted, const char *reason)
{
	char ack_payload[256];
	snprintf(ack_payload, sizeof(ack_payload),
	         "{"
	         "\"event_type\":\"SAFETY_LIMITS_UPDATE\","
	         "\"accepted\":%s,"
	         "\"max_vrms\":%.2f,"
	         "\"max_iarms\":%.3f,"
	         "\"reason\":\"%s\""
	         "}",
	         accepted ? "true" : "false",
	         max_vrms,
	         max_iarms,
	         reason != NULL ? reason : "unknown");

	int ack_msg_id = esp_mqtt_client_publish(mqtt_client, TOPIC_COMMAND_ACK, ack_payload, 0, 1, 0);
	if (ack_msg_id == -1) {
		ESP_LOGE(TAG, "Failed to publish safety-limit acknowledgment");
	} else {
		ESP_LOGI(TAG, "Published safety-limit acknowledgment: %s (msg_id: %d)", ack_payload, ack_msg_id);
	}
}

static void publish_waveform_request_ack(bool accepted, const char *reason)
{
	char ack_payload[256];
	snprintf(ack_payload, sizeof(ack_payload),
	         "{"
	         "\"event_type\":\"WAVEFORM_REQUEST\"," 
	         "\"accepted\":%s,"
	         "\"reason\":\"%s\""
	         "}",
	         accepted ? "true" : "false",
	         reason != NULL ? reason : "unknown");

	int ack_msg_id = esp_mqtt_client_publish(mqtt_client, TOPIC_COMMAND_ACK, ack_payload, 0, 1, 0);
	if (ack_msg_id == -1) {
		ESP_LOGE(TAG, "Failed to publish waveform-request acknowledgment");
	} else {
		ESP_LOGI(TAG, "Published waveform-request acknowledgment: %s (msg_id: %d)", ack_payload, ack_msg_id);
	}
}

static void publish_relay_command_ack(const char *action, bool accepted, const char *reason)
{
	char ack_payload[256];
	snprintf(ack_payload, sizeof(ack_payload),
	         "{"
	         "\"event_type\":\"RELAY_COMMAND\"," 
	         "\"action\":\"%s\"," 
	         "\"accepted\":%s,"
	         "\"reason\":\"%s\""
	         "}",
	         action != NULL ? action : "unknown",
	         accepted ? "true" : "false",
	         reason != NULL ? reason : "unknown");

	int ack_msg_id = esp_mqtt_client_publish(mqtt_client, TOPIC_COMMAND_ACK, ack_payload, 0, 1, 0);
	if (ack_msg_id == -1) {
		ESP_LOGE(TAG, "Failed to publish relay-command acknowledgment");
	} else {
		ESP_LOGI(TAG, "Published relay-command acknowledgment: %s (msg_id: %d)", ack_payload, ack_msg_id);
	}
}

esp_err_t module_mqtt_publish_critical_protection(const char *cause,
                                                  uint64_t timestamp_ms,
                                                  float voltage_vrms,
                                                  float current_a_arms,
                                                  float current_b_arms,
                                                  uint32_t duration_cycles,
                                                  const char *action_taken,
                                                  const char *system_status)
{
	if (!module_mqtt_is_connected()) {
		return ESP_ERR_INVALID_STATE;
	}

	cJSON *root = cJSON_CreateObject();
	if (root == NULL) {
		return ESP_ERR_NO_MEM;
	}

	cJSON_AddStringToObject(root, "event_type", "CRITICAL_PROTECTION");
	cJSON_AddStringToObject(root, "cause", cause != NULL ? cause : "UNKNOWN");
	cJSON_AddNumberToObject(root, "timestamp", (double)timestamp_ms);

	cJSON *data = cJSON_CreateObject();
	if (data == NULL) {
		cJSON_Delete(root);
		return ESP_ERR_NO_MEM;
	}

	cJSON_AddItemToObject(root, "data", data);
	cJSON_AddNumberToObject(data, "voltage_vrms", voltage_vrms);
	cJSON_AddNumberToObject(data, "current_a_arms", current_a_arms);
	cJSON_AddNumberToObject(data, "current_b_arms", current_b_arms);
	cJSON_AddNumberToObject(data, "duration_cycles", duration_cycles);
	cJSON_AddStringToObject(root, "action_taken", action_taken != NULL ? action_taken : "RELAY_OPEN");
	cJSON_AddStringToObject(root, "system_status", system_status != NULL ? system_status : "LOCKED_AWAITING_ACK");

	char *payload = cJSON_PrintUnformatted(root);
	if (payload == NULL) {
		cJSON_Delete(root);
		return ESP_ERR_NO_MEM;
	}

	int msg_id = esp_mqtt_client_publish(mqtt_client, TOPIC_EVENTS_PROTECTION, payload, 0, 1, 0);

	cJSON_free(payload);
	cJSON_Delete(root);

	if (msg_id == -1) {
		ESP_LOGE(TAG, "Failed to publish critical-protection event");
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "Published critical-protection event (msg_id: %d)", msg_id);
	return ESP_OK;
}

static void handle_safety_limits_command(const esp_mqtt_event_handle_t event)
{
	if (event == NULL || event->data == NULL || event->data_len <= 0) {
		ESP_LOGW(TAG, "Ignoring empty safety-limit command");
		publish_safety_limits_ack(0.0f, 0.0f, false, "empty payload");
		return;
	}

	if (event->data_len >= 256) {
		ESP_LOGW(TAG, "Safety-limit command too large: %d bytes", event->data_len);
		publish_safety_limits_ack(0.0f, 0.0f, false, "payload too large");
		return;
	}

	char payload[256];
	memcpy(payload, event->data, (size_t)event->data_len);
	payload[event->data_len] = '\0';

	cJSON *root = cJSON_Parse(payload);
	if (root == NULL) {
		ESP_LOGW(TAG, "Invalid safety-limit JSON: %s", payload);
		publish_safety_limits_ack(0.0f, 0.0f, false, "invalid json");
		return;
	}

	cJSON *action_item = cJSON_GetObjectItemCaseSensitive(root, "action");
	cJSON *command_item = cJSON_GetObjectItemCaseSensitive(root, "command");
	const char *action_value = cJSON_IsString(action_item) ? action_item->valuestring : NULL;
	const char *command_value = cJSON_IsString(command_item) ? command_item->valuestring : NULL;
	if (command_string_is_waveform_request(action_value) || command_string_is_waveform_request(command_value)) {
		esp_err_t waveform_ret = module_ade7953_start_snapshot_capture();
		if (waveform_ret == ESP_OK) {
			ESP_LOGI(TAG, "Waveform request received from GUI: starting 512-sample snapshot");
			publish_waveform_request_ack(true, "snapshot started");
		} else {
			ESP_LOGW(TAG, "Waveform request rejected: %s", esp_err_to_name(waveform_ret));
			publish_waveform_request_ack(false, esp_err_to_name(waveform_ret));
		}
		cJSON_Delete(root);
		return;
	}

	if (cJSON_IsString(action_item) && action_item->valuestring != NULL &&
	    (strcmp(action_item->valuestring, "toggle_relay") == 0 ||
	     strcmp(action_item->valuestring, "relay_toggle") == 0 ||
	     strcmp(action_item->valuestring, "RELAY_TOGGLE") == 0)) {
		esp_err_t relay_ret = ESP_ERR_INVALID_STATE;
		if (s_relay_command_handler != NULL) {
			relay_ret = s_relay_command_handler(action_item->valuestring, s_relay_command_handler_user_data);
		}
		if (relay_ret == ESP_OK) {
			ESP_LOGI(TAG, "Relay command received from GUI: %s", action_item->valuestring);
			publish_relay_command_ack(action_item->valuestring, true, "queued");
		} else {
			ESP_LOGW(TAG, "Relay command rejected: %s", esp_err_to_name(relay_ret));
			publish_relay_command_ack(action_item->valuestring, false, esp_err_to_name(relay_ret));
		}
		cJSON_Delete(root);
		return;
	}

	cJSON *max_vrms_item = cJSON_GetObjectItemCaseSensitive(root, "max_vrms");
	cJSON *max_iarms_item = cJSON_GetObjectItemCaseSensitive(root, "max_iarms");
	if (!cJSON_IsNumber(max_vrms_item) || !cJSON_IsNumber(max_iarms_item)) {
		ESP_LOGW(TAG, "Safety-limit JSON missing numeric max_vrms/max_iarms");
		publish_safety_limits_ack(0.0f, 0.0f, false, "missing numeric fields");
		cJSON_Delete(root);
		return;
	}

	float max_vrms = (float)max_vrms_item->valuedouble;
	float max_iarms = (float)max_iarms_item->valuedouble;
	ESP_LOGI(TAG, "Received safety-limit command: max_vrms=%.2f max_iarms=%.3f", max_vrms, max_iarms);

	esp_err_t apply_ret = ESP_OK;
	if (s_safety_limits_handler != NULL) {
		apply_ret = s_safety_limits_handler(max_vrms, max_iarms, s_safety_limits_handler_user_data);
	} else {
		apply_ret = ESP_ERR_INVALID_STATE;
	}
	publish_safety_limits_ack(max_vrms, max_iarms, apply_ret == ESP_OK, apply_ret == ESP_OK ? "applied" : esp_err_to_name(apply_ret));

	cJSON_Delete(root);
}


static void handle_relay_command(const esp_mqtt_event_handle_t event)
{
	if (event == NULL || event->data == NULL || event->data_len <= 0) {
		ESP_LOGW(TAG, "Ignoring empty relay command");
		publish_relay_command_ack("unknown", false, "empty payload");
		return;
	}

	if (event->data_len >= 256) {
		ESP_LOGW(TAG, "Relay command too large: %d bytes", event->data_len);
		publish_relay_command_ack("unknown", false, "payload too large");
		return;
	}

	char payload[256];
	memcpy(payload, event->data, (size_t)event->data_len);
	payload[event->data_len] = '\0';

	bool relay_set = false;
	bool relay_on = false;
	const char *action = "unknown";

	cJSON *root = cJSON_Parse(payload);
	if (root != NULL) {
		cJSON *command_item = cJSON_GetObjectItemCaseSensitive(root, "command");
		cJSON *relay_item = cJSON_GetObjectItemCaseSensitive(root, "relay");

		if (cJSON_IsString(command_item) && command_item->valuestring != NULL) {
			action = command_item->valuestring;
			if (strcmp(command_item->valuestring, "RELAY_ON") == 0 || strcmp(command_item->valuestring, "ON") == 0) {
				relay_on = true;
				relay_set = true;
			} else if (strcmp(command_item->valuestring, "RELAY_OFF") == 0 || strcmp(command_item->valuestring, "OFF") == 0) {
				relay_on = false;
				relay_set = true;
			}
		}

		if (!relay_set && cJSON_IsBool(relay_item)) {
			relay_on = cJSON_IsTrue(relay_item);
			relay_set = true;
			action = relay_on ? "RELAY_ON" : "RELAY_OFF";
		}
	} else {
		if (mqtt_payload_equals(event, "RELAY_ON")) {
			relay_on = true;
			relay_set = true;
			action = "RELAY_ON";
		} else if (mqtt_payload_equals(event, "RELAY_OFF")) {
			relay_on = false;
			relay_set = true;
			action = "RELAY_OFF";
		}
	}

	if (!relay_set) {
		ESP_LOGW(TAG, "Invalid relay command payload: %s", payload);
		publish_relay_command_ack(action, false, "invalid relay command");
		if (root != NULL) {
			cJSON_Delete(root);
		}
		return;
	}

	ESP_LOGW(TAG, "Relay command received: %s", action);
	module_relay_set(relay_on);
	publish_relay_command_ack(action, true, "applied");

	if (root != NULL) {
		cJSON_Delete(root);
	}
}

esp_err_t module_mqtt_set_safety_limits_handler(module_mqtt_safety_limits_handler_t handler, void *user_data)
{
	s_safety_limits_handler = handler;
	s_safety_limits_handler_user_data = user_data;
	return ESP_OK;
}

esp_err_t module_mqtt_set_relay_command_handler(module_mqtt_relay_command_handler_t handler, void *user_data)
{
	s_relay_command_handler = handler;
	s_relay_command_handler_user_data = user_data;
	return ESP_OK;
}

/**
 * @brief MQTT event handler
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
	esp_mqtt_event_handle_t event = event_data;

	switch (event->event_id) {
		case MQTT_EVENT_CONNECTED:
			ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
			mqtt_connected = true;
			/* Subscribe to receive GUI commands, safety config and waveform requests */
			esp_mqtt_client_subscribe(mqtt_client, TOPIC_COMMAND_RELAY, 0);
			esp_mqtt_client_subscribe(mqtt_client, TOPIC_COMMAND_CONFIG, 0);
			esp_mqtt_client_subscribe(mqtt_client, TOPIC_WAVEFORM_REQUEST, 0);

			/* Transition aliases for older PC tools. */
			esp_mqtt_client_subscribe(mqtt_client, LEGACY_TOPIC_RELAY_CMD, 0);
			esp_mqtt_client_subscribe(mqtt_client, LEGACY_TOPIC_AICE_CMD, 0);
			esp_mqtt_client_subscribe(mqtt_client, LEGACY_TOPIC_WAVEFORM_CMD, 0);
			break;

		case MQTT_EVENT_DISCONNECTED:
			ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
			mqtt_connected = false;
			break;

		case MQTT_EVENT_SUBSCRIBED:
			ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
			break;

		case MQTT_EVENT_UNSUBSCRIBED:
			ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
			break;

		case MQTT_EVENT_PUBLISHED:
			ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
			break;

		case MQTT_EVENT_DATA:
			ESP_LOGI(TAG, "MQTT_EVENT_DATA");
			ESP_LOGI(TAG, "TOPIC=%.*s", event->topic_len, event->topic);
			ESP_LOGI(TAG, "DATA=%.*s", event->data_len, event->data);
			
			/* Process incoming commands from GUI */
			if (mqtt_topic_matches(event, TOPIC_COMMAND_CONFIG) || mqtt_topic_matches(event, LEGACY_TOPIC_AICE_CMD)) {
				handle_safety_limits_command(event);
			} else if (mqtt_topic_matches(event, TOPIC_WAVEFORM_REQUEST) || mqtt_topic_matches(event, LEGACY_TOPIC_WAVEFORM_CMD)) {
				handle_safety_limits_command(event);
			} else if (mqtt_topic_matches(event, TOPIC_COMMAND_RELAY) || mqtt_topic_matches(event, LEGACY_TOPIC_RELAY_CMD)) {
				handle_relay_command(event);
			}
			break;

		case MQTT_EVENT_ERROR:
			ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
			if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
				ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
			}
			break;

		default:
			ESP_LOGI(TAG, "Other event id:%d", event->event_id);
			break;
	}
}

/**
 * @brief Initialize MQTT module
 */
esp_err_t module_mqtt_init(void)
{
	ESP_LOGI(TAG, "Initializing MQTT module");

	/* MQTT client will be created when connecting */
	mqtt_connected = false;

	return ESP_OK;
}

/**
 * @brief Connect to MQTT broker
 */
esp_err_t module_mqtt_connect(const char *broker_ip, uint16_t broker_port)
{
	if (mqtt_client != NULL) {
		ESP_LOGI(TAG, "MQTT client already exists, stopping old client");
		esp_mqtt_client_stop(mqtt_client);
		esp_mqtt_client_destroy(mqtt_client);
		mqtt_client = NULL;
	}

	/* Build broker URI */
	char broker_uri[256];
	snprintf(broker_uri, sizeof(broker_uri), "mqtt://%s:%u", broker_ip, broker_port);

	ESP_LOGI(TAG, "Connecting to MQTT broker at %s", broker_uri);

	/* Configure MQTT client */
	const esp_mqtt_client_config_t mqtt_cfg = {
		.broker.address.uri = broker_uri,
		.credentials.username = NULL,
		.credentials.authentication.password = NULL,
	};

	mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
	if (mqtt_client == NULL) {
		ESP_LOGE(TAG, "Failed to create MQTT client");
		return ESP_FAIL;
	}

	/* Register event handler */
	esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

	/* Start MQTT client */
	esp_err_t err = esp_mqtt_client_start(mqtt_client);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
		esp_mqtt_client_destroy(mqtt_client);
		mqtt_client = NULL;
		return err;
	}

	ESP_LOGI(TAG, "MQTT client started");
	return ESP_OK;
}

/**
 * @brief Connect to MQTT broker using a full URI
 */
esp_err_t module_mqtt_connect_uri(const char *broker_uri)
{
	if (mqtt_client != NULL) {
		ESP_LOGI(TAG, "MQTT client already exists, stopping old client");
		esp_mqtt_client_stop(mqtt_client);
		esp_mqtt_client_destroy(mqtt_client);
		mqtt_client = NULL;
	}

	if (broker_uri == NULL) {
		ESP_LOGE(TAG, "broker_uri is NULL");
		return ESP_ERR_INVALID_ARG;
	}

	ESP_LOGI(TAG, "Connecting to MQTT broker at %s", broker_uri);

	const esp_mqtt_client_config_t mqtt_cfg = {
		.broker.address.uri = broker_uri,
		.credentials.username = NULL,
		.credentials.authentication.password = NULL,
	};

	mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
	if (mqtt_client == NULL) {
		ESP_LOGE(TAG, "Failed to create MQTT client");
		return ESP_FAIL;
	}

	esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

	esp_err_t err = esp_mqtt_client_start(mqtt_client);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
		esp_mqtt_client_destroy(mqtt_client);
		mqtt_client = NULL;
		return err;
	}

	ESP_LOGI(TAG, "MQTT client started");
	return ESP_OK;
}

/**
 * @brief Check if MQTT is connected
 */
bool module_mqtt_is_connected(void)
{
	return mqtt_connected && mqtt_client != NULL;
}

/**
 * @brief Publish relay status (JSON format)
 */
esp_err_t module_mqtt_publish_relay(bool relay_on)
{
	if (!module_mqtt_is_connected()) {
		return ESP_ERR_INVALID_STATE;
	}

	cJSON *root = cJSON_CreateObject();
	if (root == NULL) {
		return ESP_ERR_NO_MEM;
	}

	cJSON_AddStringToObject(root, "event_type", "RELAY_STATE");
	cJSON_AddBoolToObject(root, "relay", relay_on);

	char *payload = cJSON_PrintUnformatted(root);
	if (payload == NULL) {
		cJSON_Delete(root);
		return ESP_ERR_NO_MEM;
	}

	int msg_id = esp_mqtt_client_publish(mqtt_client, TOPIC_STATE_RELAY, payload, 0, 1, 0);

	cJSON_free(payload);
	cJSON_Delete(root);

	if (msg_id == -1) {
		ESP_LOGE(TAG, "Failed to publish relay status");
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "Published relay status (msg_id: %d)", msg_id);
	return ESP_OK;
}

/**
 * @brief Publish LED status
 */
esp_err_t module_mqtt_publish_led(uint8_t red, uint8_t green, uint8_t blue)
{
	if (!module_mqtt_is_connected()) {
		return ESP_ERR_INVALID_STATE;
	}

	char payload[64];
	snprintf(payload, sizeof(payload), "{\"r\":%d,\"g\":%d,\"b\":%d}", red, green, blue);

	int msg_id = esp_mqtt_client_publish(mqtt_client, TOPIC_STATE_LED, payload, 0, 1, 0);
	if (msg_id == -1) {
		ESP_LOGE(TAG, "Failed to publish LED status");
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "Published LED status: %s (msg_id: %d)", payload, msg_id);
	return ESP_OK;
}

/**
 * @brief Publish temperature reading (JSON format)
 */
esp_err_t module_mqtt_publish_temperature(float temp_celsius)
{
	if (!module_mqtt_is_connected()) {
		return ESP_ERR_INVALID_STATE;
	}

	cJSON *root = cJSON_CreateObject();
	if (root == NULL) {
		return ESP_ERR_NO_MEM;
	}

	cJSON_AddStringToObject(root, "event_type", "TEMPERATURE");
	cJSON_AddNumberToObject(root, "temperature_c", temp_celsius);

	char *payload = cJSON_PrintUnformatted(root);
	if (payload == NULL) {
		cJSON_Delete(root);
		return ESP_ERR_NO_MEM;
	}

	int msg_id = esp_mqtt_client_publish(mqtt_client, TOPIC_TELEMETRY_TEMPERATURE, payload, 0, 1, 0);

	cJSON_free(payload);
	cJSON_Delete(root);

	if (msg_id == -1) {
		ESP_LOGE(TAG, "Failed to publish temperature");
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "Published temperature (msg_id: %d)", msg_id);
	return ESP_OK;
}

/**
 * @brief Publish energy readings
 */
esp_err_t module_mqtt_publish_energy(float voltage_v, float current_a, float power_w, uint32_t energy_wh)
{
	if (!module_mqtt_is_connected()) {
		return ESP_ERR_INVALID_STATE;
	}

	cJSON *root = cJSON_CreateObject();
	if (root == NULL) {
		return ESP_ERR_NO_MEM;
	}

	cJSON_AddNumberToObject(root, "voltage", voltage_v);
	cJSON_AddNumberToObject(root, "current", current_a);
	cJSON_AddNumberToObject(root, "power", power_w);
	cJSON_AddNumberToObject(root, "energy", energy_wh);

	char *payload = cJSON_PrintUnformatted(root);
	if (payload == NULL) {
		cJSON_Delete(root);
		return ESP_ERR_NO_MEM;
	}

	int msg_id = esp_mqtt_client_publish(mqtt_client, TOPIC_TELEMETRY_ENERGY, payload, 0, 1, 0);

	cJSON_free(payload);
	cJSON_Delete(root);

	if (msg_id == -1) {
		ESP_LOGE(TAG, "Failed to publish energy readings");
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "Published energy readings (msg_id: %d)", msg_id);
	return ESP_OK;
}

/**
 * @brief Publish combined status
 */
esp_err_t module_mqtt_publish_status(float temperature_c,
                                     float vrms,
                                     float irms,
                                     float pf,
                                     float active_power,
                                     float reactive_power,
                                     float frequency,
                                     bool no_load,
                                     uint32_t energy_wh,
                                     bool relay_state)
{
	if (!module_mqtt_is_connected()) {
		return ESP_ERR_INVALID_STATE;
	}

	char payload[256];
	snprintf(payload, sizeof(payload),
		     "{"
		     "\"vrms\":%.2f,"
		     "\"irms\":%.3f,"
		     "\"pf\":%.3f,"
		     "\"active_power\":%.2f,"
		     "\"reactive_power\":%.2f,"
		     "\"frequency\":%.2f,"
		     "\"no_load\":%s,"
		     "\"energy_wh\":%" PRIu32 ","
		     "\"relay\":%s,"
		     "\"tmp_c\":%.2f"
		     "}",
		     vrms,
		     irms,
		     pf,
		     active_power,
		     reactive_power,
		     frequency,
		     no_load ? "true" : "false",
		     energy_wh,
		     relay_state ? "true" : "false",
		     temperature_c);

	ESP_LOGI(TAG, "Publishing status: %s", payload);
	int msg_id = esp_mqtt_client_publish(mqtt_client, TOPIC_TELEMETRY_STATUS, payload, 0, 1, 0);

	if (msg_id == -1) {
		ESP_LOGE(TAG, "Failed to publish status");
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "Published status (msg_id: %d)", msg_id);
	return ESP_OK;
}

/**
 * @brief Publish waveform chunk payload
 */
esp_err_t module_mqtt_publish_waveform_chunk(const char *payload)
{
	if (!module_mqtt_is_connected()) {
		return ESP_ERR_INVALID_STATE;
	}

	if (payload == NULL || payload[0] == '\0') {
		return ESP_ERR_INVALID_ARG;
	}

	int msg_id = esp_mqtt_client_publish(mqtt_client, TOPIC_WAVEFORM_DATA, payload, 0, 1, 0);
	if (msg_id == -1) {
		ESP_LOGE(TAG, "Failed to publish waveform chunk");
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "Published waveform chunk (msg_id: %d)", msg_id);
	return ESP_OK;
}

/**
 * @brief Disconnect from MQTT broker
 */
esp_err_t module_mqtt_disconnect(void)
{
	if (mqtt_client == NULL) {
		return ESP_ERR_INVALID_STATE;
	}

	ESP_LOGI(TAG, "Disconnecting from MQTT broker");
	esp_mqtt_client_stop(mqtt_client);
	esp_mqtt_client_destroy(mqtt_client);
	mqtt_client = NULL;
	mqtt_connected = false;
	s_safety_limits_handler = NULL;
	s_safety_limits_handler_user_data = NULL;
	s_relay_command_handler = NULL;
	s_relay_command_handler_user_data = NULL;

	return ESP_OK;
}
