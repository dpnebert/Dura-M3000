#include "gama_blefob_internal.h"

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
static int ble_gap_event_cb(struct ble_gap_event *event, void *arg);

typedef enum {
	BLE_SHELL_MFG_COMPANY_ID_LSB = 0,
	BLE_SHELL_MFG_COMPANY_ID_MSB,
	BLE_SHELL_MFG_STATUS,
	BLE_SHELL_MFG_FW_MAJOR,
	BLE_SHELL_MFG_FW_MINOR,
	BLE_SHELL_MFG_PAIR_WINDOW_SECONDS,
	BLE_SHELL_MFG_FW_PATCH,
	BLE_SHELL_MFG_RESERVED,
} ble_shell_mfg_data_offset_t;

static void ble_advertise_start_active(void)
{
	const char *name;
	struct ble_gap_adv_params adv_params;
	struct ble_hs_adv_fields fields;
	struct ble_hs_adv_fields rsp_fields;
	uint8_t mfg_data[CONFIG_BLE_SHELL_MFG_DATA_LEN] = {0};
	uint8_t fw_patch = 0;
	int64_t now_us = esp_timer_get_time();
	int32_t duration_ms;
	int rc;

	if (!s_ble_synced || !s_cfg.ble_advertising_enabled) {
		return;
	}
	if (!s_advertising_window_open || s_adv_deadline_us <= now_us) {
		s_advertising_window_open = false;
		return;
	}
	if (ble_gap_adv_active()) {
		(void)ble_gap_adv_stop();
	}
	duration_ms = (int32_t)((s_adv_deadline_us - now_us) / BLE_SHELL_USECONDS_PER_MS);
	if (duration_ms < CONFIG_BLE_SHELL_ADV_MIN_DURATION_MS) {
		duration_ms = CONFIG_BLE_SHELL_ADV_MIN_DURATION_MS;
	}

	memset(&fields, 0, sizeof(fields));
	memset(&rsp_fields, 0, sizeof(rsp_fields));
	name = ble_svc_gap_device_name();
	uint8_t fw_major = 0;
	uint8_t fw_minor = 0;
	ble_shell_parse_semver(s_app_cfg.firmware_version, &fw_major, &fw_minor, &fw_patch);
	mfg_data[BLE_SHELL_MFG_COMPANY_ID_LSB] = (uint8_t)(CONFIG_BLE_SHELL_MFG_COMPANY_ID & 0xFF);
	mfg_data[BLE_SHELL_MFG_COMPANY_ID_MSB] = (uint8_t)((CONFIG_BLE_SHELL_MFG_COMPANY_ID >> 8) & 0xFF);
	mfg_data[BLE_SHELL_MFG_STATUS] = ble_shell_adv_status_byte();
	mfg_data[BLE_SHELL_MFG_FW_MAJOR] = fw_major;
	mfg_data[BLE_SHELL_MFG_FW_MINOR] = fw_minor;
	mfg_data[BLE_SHELL_MFG_PAIR_WINDOW_SECONDS] = (uint8_t)s_cfg.pair_window_seconds;
	mfg_data[BLE_SHELL_MFG_FW_PATCH] = fw_patch;
	mfg_data[BLE_SHELL_MFG_RESERVED] = 0;
	fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
	fields.name = (uint8_t *)name;
	fields.name_len = (uint8_t)strlen(name);
	fields.name_is_complete = 1;
	rc = ble_gap_adv_set_fields(&fields);
	if (rc != 0) {
		ESP_LOGE(TAG, "ble_gap_adv_set_fields failed rc=%d", rc);
		return;
	}
	rsp_fields.uuids128 = BLE_UUID128(BLE_SHELL_NUS_SERVICE_UUID);
	rsp_fields.num_uuids128 = 1;
	rsp_fields.uuids128_is_complete = 1;
	rsp_fields.mfg_data = mfg_data;
	rsp_fields.mfg_data_len = sizeof(mfg_data);
	rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
	if (rc != 0) {
		ESP_LOGE(TAG, "ble_gap_adv_rsp_set_fields failed rc=%d", rc);
		return;
	}

	memset(&adv_params, 0, sizeof(adv_params));
	adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
	adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
	rc = ble_gap_adv_start(s_ble_addr_type, NULL, duration_ms, &adv_params, ble_gap_event_cb, NULL);
	if (rc != 0) {
		ESP_LOGE(TAG, "ble_gap_adv_start failed rc=%d", rc);
		return;
	}
	ESP_LOGI(TAG, "BLE advertising window open for %ld ms", (long)duration_ms);
}

void ble_advertise_start_remaining(void)
{
    if (!ble_shell_stack_enter()) return;
    ble_advertise_start_active();
    ble_shell_stack_exit();
}

void ble_shell_open_pairing_window(uint32_t seconds, const char *reason)
{
    if (!ble_shell_stack_enter()) return;
	if (seconds < BLE_SHELL_PAIR_ADV_SECONDS_MIN) {
		seconds = 1;
	}
	if (seconds > BLE_SHELL_PAIR_ADV_SECONDS_MAX) {
		seconds = 600;
	}
	s_adv_deadline_us = esp_timer_get_time() + ((int64_t)seconds * BLE_SHELL_USECONDS_PER_SECOND);
	s_advertising_window_open = true;
	ble_shell_notify_log(reason ? reason : "pair window opened");
	ble_advertise_start_remaining();
	ble_shell_notify_status();
    ble_shell_stack_exit();
}

void ble_shell_close_pairing_window(void)
{
    if (!ble_shell_stack_enter()) return;
	s_advertising_window_open = false;
	s_adv_deadline_us = 0;
	if (ble_gap_adv_active()) {
		(void)ble_gap_adv_stop();
	}
	ble_shell_notify_log("pair window closed");
	ble_shell_notify_status();
    ble_shell_stack_exit();
}

static int ble_gap_event_active(struct ble_gap_event *event, void *arg)
{
	(void)arg;

	switch (event->type) {
	case BLE_GAP_EVENT_CONNECT:
		if (event->connect.status == 0) {
			s_conn_handle = event->connect.conn_handle;
			s_response_notify_active = false;
			ble_shell_notify_log("ble connected");
			if (s_app_cfg.connected_handler != NULL) {
				s_app_cfg.connected_handler(s_app_cfg.user_ctx);
			}
			ble_shell_notify_status();
		} else {
			ESP_LOGW(TAG, "CONNECT status=%d failed", event->connect.status);
			ble_advertise_start_remaining();
		}
		return 0;
	case BLE_GAP_EVENT_ENC_CHANGE:
		if (event->enc_change.status == 0) ble_shell_replay_binary();
		return 0;
	case BLE_GAP_EVENT_SUBSCRIBE:
		if (event->subscribe.attr_handle != s_response_chr_val_handle) {
			return 0;
		}
		s_response_notify_active = event->subscribe.cur_notify;
		if (s_response_notify_active) ble_shell_replay_binary();
		return 0;
	case BLE_GAP_EVENT_DISCONNECT:
		s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
		s_response_notify_active = false;
		ble_shell_notify_log("ble disconnected");
		if (s_app_cfg.disconnected_handler != NULL) {
			s_app_cfg.disconnected_handler(s_app_cfg.user_ctx);
		}
		ble_shell_open_pairing_window(s_cfg.pair_window_seconds, "disconnect advertising window opened");
		return 0;
	case BLE_GAP_EVENT_ADV_COMPLETE:
		if (s_advertising_window_open && esp_timer_get_time() < s_adv_deadline_us) {
			ble_advertise_start_remaining();
		} else {
			s_advertising_window_open = false;
			ESP_LOGI(TAG, "BLE advertising window expired");
		}
		return 0;
	default:
		return 0;
	}
}

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    if (!ble_shell_stack_enter()) return 0;
    const int rc = ble_gap_event_active(event, arg);
    ble_shell_stack_exit();
    return rc;
}

void ble_on_sync(void)
{
	int rc = ble_hs_id_infer_auto(0, &s_ble_addr_type);
	if (rc != 0) {
		ESP_LOGE(TAG, "ble_hs_id_infer_auto failed rc=%d", rc);
		return;
	}
	s_ble_synced = true;
	if (s_app_cfg.advertise_on_boot) {
		ble_shell_open_pairing_window(s_cfg.pair_window_seconds, "boot advertising window opened");
	}
	ESP_LOGI(TAG, "BLE stack synced");
    ble_shell_sleep_synced();
}

void ble_on_reset(int reason)
{
	s_ble_synced = false;
	ESP_LOGW(TAG, "BLE host reset reason=%d", reason);
}

#endif
