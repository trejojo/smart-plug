#include "module_mqtt.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "module_relay.h"
#include <inttypes.h>
#include <string.h>
#include "CJson.h"

static const char *TAG = "module_mqtt";

/* MQTT client handle */
static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_connected = false;

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
			/* Subscribe to receive GUI commands */
			esp_mqtt_client_subscribe(mqtt_client, "smartplug/cmd", 0);
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
			if (strncmp(event->topic, "smartplug/cmd", event->topic_len) == 0) {
				if (strncmp(event->data, "RELAY_ON", event->data_len) == 0) {
					ESP_LOGW(TAG, "Command received: Turning relay ON");
					module_relay_set(true);
				} else if (strncmp(event->data, "RELAY_OFF", event->data_len) == 0) {
					ESP_LOGW(TAG, "Command received: Turning relay OFF");
					module_relay_set(false);
				}
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

	int msg_id = esp_mqtt_client_publish(mqtt_client, "smartplug/relay", payload, 0, 1, 0);

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

	int msg_id = esp_mqtt_client_publish(mqtt_client, "smartplug/led", payload, 0, 1, 0);
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

	int msg_id = esp_mqtt_client_publish(mqtt_client, "smartplug/temperature", payload, 0, 1, 0);

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

	int msg_id = esp_mqtt_client_publish(mqtt_client, "smartplug/energy", payload, 0, 1, 0);

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
	int msg_id = esp_mqtt_client_publish(mqtt_client, "smartplug/status", payload, 0, 1, 0);

	if (msg_id == -1) {
		ESP_LOGE(TAG, "Failed to publish status");
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "Published status (msg_id: %d)", msg_id);
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

	return ESP_OK;
}
