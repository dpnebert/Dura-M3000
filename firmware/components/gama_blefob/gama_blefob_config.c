#include "gama_blefob_internal.h"

static gama_blefob_security_mode_t ble_shell_default_security_mode(void)
{
#if CONFIG_BLE_SHELL_SECURITY_PAIR_WINDOW_REQUIRED
	return GAMA_BLEFOB_SECURITY_PAIR_REQUIRED;
#elif CONFIG_BLE_SHELL_SECURITY_UNLOCK_COMMAND_REQUIRED
	return GAMA_BLEFOB_SECURITY_UNLOCK_COMMAND_REQUIRED;
#else
	return GAMA_BLEFOB_SECURITY_OPEN;
#endif
}

void gama_blefob_config_init_default(gama_blefob_config_t *config)
{
	if (config == NULL) {
		return;
	}
	memset(config, 0, sizeof(*config));
	config->device_name = CONFIG_BLE_SHELL_DEVICE_NAME;
	config->project_id = CONFIG_BLE_SHELL_PROJECT_ID;
	config->module_name = CONFIG_BLE_SHELL_MODULE_NAME;
	config->module_version = CONFIG_BLE_SHELL_MODULE_VERSION;
	config->firmware_version = CONFIG_BLE_SHELL_FIRMWARE_VERSION;
	config->default_serial_id = BLE_SHELL_DEFAULT_SERIAL;
	config->default_pair_window_seconds = CONFIG_BLE_SHELL_PAIR_ADV_SECONDS;
#ifdef CONFIG_BLE_SHELL_ADV_ON_BOOT
	config->advertise_on_boot = true;
#else
	config->advertise_on_boot = false;
#endif
	config->shell_task_stack_size = CONFIG_BLE_SHELL_DIAGNOSTIC_TASK_STACK;
	config->init_nvs_if_needed = false;
	config->security_mode = ble_shell_default_security_mode();
}


#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
static esp_err_t ble_shell_nvs_load_string(nvs_handle_t nvs, const char *key, char *dst, size_t dst_len, const char *fallback)
{
	size_t len = dst_len;
	esp_err_t err = nvs_get_str(nvs, key, dst, &len);
	if (err == ESP_ERR_NVS_NOT_FOUND) {
		strlcpy(dst, fallback, dst_len);
		return ESP_OK;
	}
	if (err != ESP_OK || len == 0) {
		strlcpy(dst, fallback, dst_len);
		return err;
	}
	return ESP_OK;
}

void ble_shell_apply_app_config(const gama_blefob_config_t *config)
{
	gama_blefob_config_t defaults;
	if (config == NULL) {
		gama_blefob_config_init_default(&defaults);
		config = &defaults;
	}
	strlcpy(s_app_cfg.device_name, ble_shell_nonempty(config->device_name, CONFIG_BLE_SHELL_DEVICE_NAME), sizeof(s_app_cfg.device_name));
	strlcpy(s_app_cfg.project_id, ble_shell_nonempty(config->project_id, CONFIG_BLE_SHELL_PROJECT_ID), sizeof(s_app_cfg.project_id));
	strlcpy(s_app_cfg.module_name, ble_shell_nonempty(config->module_name, CONFIG_BLE_SHELL_MODULE_NAME), sizeof(s_app_cfg.module_name));
	strlcpy(s_app_cfg.module_version, ble_shell_nonempty(config->module_version, CONFIG_BLE_SHELL_MODULE_VERSION), sizeof(s_app_cfg.module_version));
	strlcpy(s_app_cfg.firmware_version, ble_shell_nonempty(config->firmware_version, CONFIG_BLE_SHELL_FIRMWARE_VERSION), sizeof(s_app_cfg.firmware_version));
	strlcpy(s_app_cfg.default_serial_id, ble_shell_nonempty(config->default_serial_id, BLE_SHELL_DEFAULT_SERIAL), sizeof(s_app_cfg.default_serial_id));
	s_app_cfg.default_pair_window_seconds = config->default_pair_window_seconds;
	if (s_app_cfg.default_pair_window_seconds < BLE_SHELL_PAIR_ADV_SECONDS_MIN || s_app_cfg.default_pair_window_seconds > BLE_SHELL_PAIR_ADV_SECONDS_MAX) {
		s_app_cfg.default_pair_window_seconds = CONFIG_BLE_SHELL_PAIR_ADV_SECONDS;
	}
	s_app_cfg.advertise_on_boot = config->advertise_on_boot;
	s_app_cfg.shell_task_stack_size = config->shell_task_stack_size ? config->shell_task_stack_size : CONFIG_BLE_SHELL_DIAGNOSTIC_TASK_STACK;
	s_app_cfg.init_nvs_if_needed = config->init_nvs_if_needed;
	s_app_cfg.security_mode = config->security_mode;
	s_app_cfg.command_handler = config->command_handler;
	s_app_cfg.binary_write_handler = config->binary_write_handler;
	s_app_cfg.binary_write_user_ctx = config->binary_write_user_ctx;
	s_app_cfg.rf_button_handler = config->rf_button_handler;
	s_app_cfg.rf_button_user_ctx = config->rf_button_user_ctx;
	s_app_cfg.status_handler = config->status_handler;
	s_app_cfg.connected_handler = config->connected_handler;
	s_app_cfg.disconnected_handler = config->disconnected_handler;
	s_app_cfg.user_activity_handler = config->user_activity_handler;
	s_app_cfg.user_ctx = config->user_ctx;
}

void ble_shell_config_defaults(void)
{
	strlcpy(s_cfg.device_name, s_app_cfg.device_name, sizeof(s_cfg.device_name));
	strlcpy(s_cfg.serial_id, s_app_cfg.default_serial_id, sizeof(s_cfg.serial_id));
	s_cfg.ble_advertising_enabled = true;
	s_cfg.diagnostic_mode_enabled = false;
	s_cfg.ping_debug_enabled = false;
	s_cfg.pair_window_seconds = s_app_cfg.default_pair_window_seconds;
}

void ble_shell_config_load(void)
{
	nvs_handle_t nvs;
	uint8_t u8 = 0;
	uint32_t u32 = 0;

	ble_shell_config_defaults();
	if (nvs_open(BLE_SHELL_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
		return;
	}

	(void)ble_shell_nvs_load_string(nvs, "dev_name", s_cfg.device_name, sizeof(s_cfg.device_name), s_app_cfg.device_name);
	(void)ble_shell_nvs_load_string(nvs, "serial", s_cfg.serial_id, sizeof(s_cfg.serial_id), s_app_cfg.default_serial_id);
	if (nvs_get_u8(nvs, "adv_en", &u8) == ESP_OK) {
		s_cfg.ble_advertising_enabled = u8 != 0;
	}
	if (nvs_get_u8(nvs, "diag_en", &u8) == ESP_OK) {
		s_cfg.diagnostic_mode_enabled = u8 != 0;
	}
	if (nvs_get_u8(nvs, "ping_dbg", &u8) == ESP_OK) {
		s_cfg.ping_debug_enabled = u8 != 0;
	}
	if (nvs_get_u32(nvs, "pair_sec", &u32) == ESP_OK && u32 >= BLE_SHELL_PAIR_ADV_SECONDS_MIN && u32 <= BLE_SHELL_PAIR_ADV_SECONDS_MAX) {
		s_cfg.pair_window_seconds = u32;
	}
	if (s_app_cfg.advertise_on_boot) {
		/* Bench/prototype builds must remain discoverable after flashing even if
		 * stale NVS previously saved advertising disabled. */
		s_cfg.ble_advertising_enabled = true;
	}
	nvs_close(nvs);
}

esp_err_t ble_shell_config_save(void)
{
	nvs_handle_t nvs;
	esp_err_t err = nvs_open(BLE_SHELL_NVS_NAMESPACE, NVS_READWRITE, &nvs);
	if (err != ESP_OK) {
		return err;
	}
	err = nvs_set_str(nvs, "dev_name", s_cfg.device_name);
	if (err == ESP_OK) err = nvs_set_str(nvs, "serial", s_cfg.serial_id);
	if (err == ESP_OK) err = nvs_set_u8(nvs, "adv_en", s_cfg.ble_advertising_enabled ? 1 : 0);
	if (err == ESP_OK) err = nvs_set_u8(nvs, "diag_en", s_cfg.diagnostic_mode_enabled ? 1 : 0);
	if (err == ESP_OK) err = nvs_set_u8(nvs, "ping_dbg", s_cfg.ping_debug_enabled ? 1 : 0);
	if (err == ESP_OK) err = nvs_set_u32(nvs, "pair_sec", s_cfg.pair_window_seconds);
	if (err == ESP_OK) err = nvs_commit(nvs);
	nvs_close(nvs);
	return err;
}

esp_err_t ble_shell_config_factory_reset(void)
{
	nvs_handle_t nvs;
	esp_err_t err = nvs_open(BLE_SHELL_NVS_NAMESPACE, NVS_READWRITE, &nvs);
	if (err == ESP_OK) {
		err = nvs_erase_all(nvs);
		if (err == ESP_OK) {
			err = nvs_commit(nvs);
		}
		nvs_close(nvs);
	}
	ble_shell_config_defaults();
	return err;
}

#endif
