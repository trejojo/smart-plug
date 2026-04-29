#include "module_ble.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "esp_log.h"
#include <stdint.h>
#include <string.h>

static const char *TAG = "module_ble";

/* BLE credentials buffers */
static char ble_ssid[MAX_SSID_LEN + 1] = {0};
static char ble_password[MAX_PASSWORD_LEN + 1] = {0};
static bool ssid_received = false;
static bool password_received = false;
static const ble_uuid128_t creds_service_uuid = BLE_UUID128_INIT(CREDS_SERVICE_UUID);
static const ble_uuid128_t creds_ssid_uuid = BLE_UUID128_INIT(CREDS_SSID_CHAR_UUID);
static const ble_uuid128_t creds_password_uuid = BLE_UUID128_INIT(CREDS_PASSWORD_CHAR_UUID);

static uint16_t ssid_val_handle;
static uint16_t password_val_handle;

enum creds_attr_id {
	CREDS_ATTR_SSID = 1,
	CREDS_ATTR_PASSWORD = 2,
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
	char *target = NULL;
	size_t target_size = 0;

	switch (attr_id) {
		case CREDS_ATTR_SSID:
			target = ble_ssid;
			target_size = sizeof(ble_ssid);
			break;
		case CREDS_ATTR_PASSWORD:
			target = ble_password;
			target_size = sizeof(ble_password);
			break;
		default:
			return BLE_ATT_ERR_UNLIKELY;
	}

	switch (ctxt->op) {
		case BLE_GATT_ACCESS_OP_READ_CHR:
			return os_mbuf_append(ctxt->om, target, strlen(target)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;

		case BLE_GATT_ACCESS_OP_WRITE_CHR:
		{
			const uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
			if (len == 0 || len >= target_size) {
				return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
			}

			memset(target, 0, target_size);
			if (os_mbuf_copydata(ctxt->om, 0, len, target) != 0) {
				return BLE_ATT_ERR_UNLIKELY;
			}
			target[len] = '\0';

			if (attr_id == CREDS_ATTR_SSID) {
				ssid_received = true;
				ESP_LOGI(TAG, "Received SSID via BLE: %s", ble_ssid);
			} else {
				password_received = true;
				ESP_LOGI(TAG, "Received WiFi password via BLE");
			}

			return 0;
		}

		default:
			return BLE_ATT_ERR_UNLIKELY;
	}
}

static const struct ble_gatt_chr_def creds_chr_defs[] = {
	{
		.uuid = &creds_ssid_uuid.u,
		.access_cb = ble_gatt_access_cb,
		.arg = (void *)CREDS_ATTR_SSID,
		.flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
		.val_handle = &ssid_val_handle,
	},
	{
		.uuid = &creds_password_uuid.u,
		.access_cb = ble_gatt_access_cb,
		.arg = (void *)CREDS_ATTR_PASSWORD,
		.flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
		.val_handle = &password_val_handle,
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

	int rc = ble_gatts_count_cfg(gatt_svr_svcs);
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
	
	/* Initialize GATT server */
	int ret = ble_gatt_svr_init();
	if (ret != 0) {
		ESP_LOGE(TAG, "Failed to initialize GATT server: %d", ret);
		return;
	}
	
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
	
	/* Set the callback for BLE stack synchronization */
	ble_hs_cfg.sync_cb = ble_app_on_sync;
	
	/* Set the BLE device name */
	ble_svc_gap_device_name_set("SmartPlug");
	
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
 * @brief Check if credentials were received via BLE
 */
bool module_ble_credentials_received(void)
{
	return (ssid_received && password_received);
}

/**
 * @brief Get the received SSID credential
 */
esp_err_t module_ble_get_ssid(char *ssid)
{
	if (ssid == NULL) {
		return ESP_ERR_INVALID_ARG;
	}
	
	if (!ssid_received) {
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
	
	if (!password_received) {
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
	memset(ble_ssid, 0, sizeof(ble_ssid));
	memset(ble_password, 0, sizeof(ble_password));
	ssid_received = false;
	password_received = false;
	
	ESP_LOGI(TAG, "BLE credentials reset");
	
	return ESP_OK;
}
