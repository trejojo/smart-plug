#include "module_ble.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "esp_log.h"
#include "cJSON.h"
#include <stdint.h>
#include <string.h>

static const char *TAG = "module_ble";

/* BLE credentials JSON buffer */
static char ble_json_creds[MAX_JSON_LEN] = {0};
static bool credentials_received = false;
static char ble_ssid[MAX_SSID_LEN + 1] = {0};
static char ble_password[MAX_PASSWORD_LEN + 1] = {0};

static const ble_uuid128_t creds_service_uuid = BLE_UUID128_INIT(CREDS_SERVICE_UUID);
static const ble_uuid128_t creds_json_uuid = BLE_UUID128_INIT(CREDS_JSON_CHAR_UUID);

static uint16_t json_val_handle;

enum creds_attr_id {
	CREDS_ATTR_JSON = 1,
};

/* Forward declarations */
static int ble_gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg);
static int ble_gatt_svr_init(void);
static void ble_app_advertise(void);
static void ble_app_on_sync(void);

/**
 * @brief GAP event callback
 */

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
	switch (event->type) {
		case BLE_GAP_EVENT_CONNECT:
			ESP_LOGI(TAG, "BLE client connected");
			break;
		
		case BLE_GAP_EVENT_DISCONNECT:
			ESP_LOGI(TAG, "BLE client disconnected (reason: %d)", event->disconnect.reason);
			/* Resume advertising */
			ble_app_advertise();
			break;
		
		default:
			break;
	}
	
	return 0;
}

static int ble_gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
	(void)conn_handle;
	(void)attr_handle;

	const uintptr_t attr_id = (uintptr_t)arg;

	if (attr_id != CREDS_ATTR_JSON) {
		return BLE_ATT_ERR_UNLIKELY;
	}

	switch (ctxt->op) {
		case BLE_GATT_ACCESS_OP_READ_CHR:
			return os_mbuf_append(ctxt->om, (const uint8_t *)ble_json_creds, strlen(ble_json_creds)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;

		case BLE_GATT_ACCESS_OP_WRITE_CHR:
		{
			const uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
			if (len == 0 || len >= MAX_JSON_LEN) {
				return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
			}

			/* Clear previous JSON data */
			memset(ble_json_creds, 0, sizeof(ble_json_creds));
		
			/* Copy JSON data from BLE characteristic */
			if (os_mbuf_copydata(ctxt->om, 0, len, (uint8_t *)ble_json_creds) != 0) {
				return BLE_ATT_ERR_UNLIKELY;
			}
			ble_json_creds[len] = '\0';

			/* Parse JSON and extract ssid/password */
			cJSON *root = cJSON_Parse(ble_json_creds);
			if (root == NULL) {
				ESP_LOGE(TAG, "Failed to parse JSON: %s", ble_json_creds);
				memset(ble_json_creds, 0, sizeof(ble_json_creds));
				return BLE_ATT_ERR_UNLIKELY;
			}

			/* Extract SSID */
			cJSON *ssid_obj = cJSON_GetObjectItem(root, "ssid");
			if (ssid_obj == NULL || ssid_obj->valuestring == NULL) {
				ESP_LOGE(TAG, "Missing 'ssid' field in JSON");
				cJSON_Delete(root);
				memset(ble_json_creds, 0, sizeof(ble_json_creds));
				return BLE_ATT_ERR_UNLIKELY;
			}

			/* Extract Password */
			cJSON *pass_obj = cJSON_GetObjectItem(root, "password");
			if (pass_obj == NULL || pass_obj->valuestring == NULL) {
				ESP_LOGE(TAG, "Missing 'password' field in JSON");
				cJSON_Delete(root);
				memset(ble_json_creds, 0, sizeof(ble_json_creds));
				return BLE_ATT_ERR_UNLIKELY;
			}

			/* Store SSID and password in local buffers */
			size_t ssid_len = strlen(ssid_obj->valuestring);
			size_t pass_len = strlen(pass_obj->valuestring);

			if (ssid_len >= MAX_SSID_LEN || pass_len >= MAX_PASSWORD_LEN) {
				ESP_LOGE(TAG, "SSID or password exceeds max length");
				cJSON_Delete(root);
				memset(ble_json_creds, 0, sizeof(ble_json_creds));
				return BLE_ATT_ERR_UNLIKELY;
			}

			/* Copy SSID and password to local buffers */
			memset(ble_ssid, 0, sizeof(ble_ssid));
			memset(ble_password, 0, sizeof(ble_password));
			strcpy(ble_ssid, ssid_obj->valuestring);
			strcpy(ble_password, pass_obj->valuestring);

			/* Mark credentials as received */
			credentials_received = true;

			ESP_LOGI(TAG, "Received credentials via BLE JSON (SSID: %s)", ble_ssid);

			cJSON_Delete(root);
			return 0;
		}

		default:
			return BLE_ATT_ERR_UNLIKELY;
	}
}

static const struct ble_gatt_chr_def creds_chr_defs[] = {
	{
		.uuid = &creds_json_uuid.u,
		.access_cb = ble_gatt_access_cb,
		.arg = (void *)CREDS_ATTR_JSON,
		.flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
		.val_handle = &json_val_handle,
	},
	{
		0,
	},
};

static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
	{
		.type = BLE_GATT_SVC_TYPE_PRIMARY,
		.uuid = &creds_service_uuid.u,
		.characteristics = creds_chr_defs,
	},
	{
		0,
	},
};

/**
 * @brief Initialize GATT server with Credentials Service
 */
static int ble_gatt_svr_init(void)
{
	ble_svc_gap_init();
	ble_svc_gatt_init();

	int rc = ble_svc_gap_device_name_set("SmartPlug");
	if (rc != 0) {
		ESP_LOGE(TAG, "ble_svc_gap_device_name_set failed: %d", rc);
		return rc;
	}

	rc = ble_gatts_count_cfg(gatt_svr_svcs);
	if (rc != 0) {
		ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
		return rc;
	}

	rc = ble_gatts_add_svcs(gatt_svr_svcs);
	if (rc != 0) {
		ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
		return rc;
	}

	ESP_LOGI(TAG, "GATT credentials service registered");
	return 0;
}

/**
 * @brief Setup BLE advertising parameters and begin advertising
 */
static void ble_app_advertise(void)
{
	struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    uint8_t own_addr_type;
    int rc;

    /* Get the address type for the current device */
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error determining address type: %d", rc);
        return;
    }
	
	/* Set advertisement fields */
	memset(&fields, 0, sizeof(fields));

    // flags: general discoverable, BR/EDR unsupported
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
	
	/* Advertise Credentials Service UUID */
	fields.uuids128 = &creds_service_uuid;
	fields.num_uuids128 = 1;
	fields.uuids128_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error en set_fields: %d", rc);
        return;
    }
	
	/* Start advertising */
	memset(&adv_params, 0, sizeof(adv_params));
	adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;  /* Undirected connectable advertising */
	adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;  /* General discoverable */
	adv_params.itvl_min = BLE_GAP_ADV_ITVL_MS(500);  /* 500ms interval */
	adv_params.itvl_max = BLE_GAP_ADV_ITVL_MS(500);
	
	rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event, NULL);
	if (rc != 0) {
		ESP_LOGE(TAG, "Failed to start advertising: %d", rc);
		return;
	}
	
	char buf[BLE_UUID_STR_LEN];
    ESP_LOGI(TAG, "BLE advertising started | Name: SmartPlug | Service UUID: %s", 
         ble_uuid_to_str(&creds_service_uuid.u, buf));
}

/**
 * @brief Called when BLE stack is synchronized/ready
 */
static void ble_app_on_sync(void)
{
	ESP_LOGI(TAG, "BLE stack synchronized");
	
	/* Start advertising */
	ble_app_advertise();
}

/**
 * @brief BLE host event callback
 */
static void ble_host_task(void *param)
{
	nimble_port_run();
}

/**
 * @brief Initialize BLE module
 */
esp_err_t module_ble_init(void)
{
	ESP_LOGI(TAG, "Initializing BLE module");
	
	/* Initialize NimBLE port */
	int ret = nimble_port_init();
	if (ret != 0) {
		ESP_LOGE(TAG, "Failed to initialize NimBLE port: %d", ret);
		return ESP_FAIL;
	}

	/* Register GATT services before host task starts. */
	ret = ble_gatt_svr_init();
	if (ret != 0) {
		ESP_LOGE(TAG, "Failed to initialize GATT server: %d", ret);
		return ESP_FAIL;
	}
	
	/* Set the callback for BLE stack synchronization */
	ble_hs_cfg.sync_cb = ble_app_on_sync;
	
	/* Start NimBLE task */
	nimble_port_freertos_init(ble_host_task);
	
	ESP_LOGI(TAG, "BLE module initialized");
	
	return ESP_OK;
}

/**
 * @brief Start BLE advertising
 */
esp_err_t module_ble_start_advertising(void)
{
	ESP_LOGI(TAG, "Starting BLE advertising");
	
	/* Check if BLE is already advertising - if so, stop first */
	if (ble_gap_adv_active()) {
		ble_gap_adv_stop();
	}
	
	ble_app_advertise();
	
	return ESP_OK;
}

/**
 * @brief Stop BLE advertising
 */
esp_err_t module_ble_stop_advertising(void)
{
	ESP_LOGI(TAG, "Stopping BLE advertising");
	
	int ret = ble_gap_adv_stop();
	if (ret != 0) {
		ESP_LOGE(TAG, "Failed to stop advertising: %d", ret);
		return ESP_FAIL;
	}
	
	return ESP_OK;
}

/**
 * @brief Check if credentials were received via BLE (JSON parsed successfully)
 */
bool module_ble_credentials_received(void)
{
	return credentials_received;
}

/**
 * @brief Get the received SSID credential
 */
esp_err_t module_ble_get_ssid(char *ssid)
{
    if (ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!credentials_received) {
        return ESP_ERR_NOT_FOUND;
    }
    
    strcpy(ssid, ble_ssid);
    return ESP_OK;
}

/**
 * @brief Get the received password credential
 */
esp_err_t module_ble_get_password(char *password)
{
    if (password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!credentials_received) {
        return ESP_ERR_NOT_FOUND;
    }
    
    strcpy(password, ble_password);
    return ESP_OK;
}

/**
 * @brief Reset received credentials (clear the BLE buffers)
 */
esp_err_t module_ble_reset_credentials(void)
{
    memset(ble_json_creds, 0, sizeof(ble_json_creds));
    memset(ble_ssid, 0, sizeof(ble_ssid));
    memset(ble_password, 0, sizeof(ble_password));
    
    // Cambiado: Reset al flag unificado
    credentials_received = false; 
    
    ESP_LOGI(TAG, "BLE credentials reset");
    
    return ESP_OK;
}
