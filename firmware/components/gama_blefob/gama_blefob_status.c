#include "gama_blefob_internal.h"

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
#include <stdarg.h>

/* Void public formatters fail closed: never expose a successful prefix. */
static void ble_shell_json_overflow(char *out, size_t cap)
{
    const char *error = BLE_SHELL_REPLY_OVERFLOW_JSON;
    if (cap == 0 || out == NULL) return;
    if (strlen(error) < cap) memcpy(out, error, strlen(error) + 1U);
    else if (cap >= 3U) memcpy(out, "{}", 3U);
    else out[0] = '\0';
}

static bool ble_shell_json_format(char *out, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap) {
        ble_shell_json_overflow(out, cap);
        return false;
    }
    return true;
}

static bool ble_shell_escape_complete(const char *src, char *dst, size_t cap)
{
    size_t needed = 1U;
    for (const unsigned char *p = (const unsigned char *)src; p && *p; ++p) {
        needed += (*p == '"' || *p == '\\' || *p == '\b' || *p == '\f' ||
                   *p == '\n' || *p == '\r' || *p == '\t') ? 2U : (*p < 32U ? 6U : 1U);
        if (needed > cap) return false;
    }
    ble_shell_json_escape(src, dst, cap);
    return true;
}
#define ESCAPE_OR_FAIL(src, dst) do { \
    if (!ble_shell_escape_complete(src, dst, sizeof(dst))) { \
        ble_shell_json_overflow(out, out_len); return; \
    } \
} while (0)
void ble_shell_make_info_json(char *out, size_t out_len)
{
	esp_chip_info_t chip;
	char mac[BLE_SHELL_MAC_STRING_LEN];
	char project[BLE_SHELL_JSON_ESCAPE_BUF_LEN];
	char module_name[BLE_SHELL_JSON_ESCAPE_BUF_LEN];
	char module_version[BLE_SHELL_JSON_ESCAPE_BUF_LEN];
	char firmware_version[BLE_SHELL_JSON_ESCAPE_BUF_LEN];
	char serial[BLE_SHELL_JSON_ESCAPE_BUF_LEN];
	esp_chip_info(&chip);
	ble_shell_get_mac_string(mac, sizeof(mac));
	ESCAPE_OR_FAIL(s_app_cfg.project_id, project);
	ESCAPE_OR_FAIL(s_app_cfg.module_name, module_name);
	ESCAPE_OR_FAIL(s_app_cfg.module_version, module_version);
	ESCAPE_OR_FAIL(s_app_cfg.firmware_version, firmware_version);
	ESCAPE_OR_FAIL(s_cfg.serial_id, serial);
	ble_shell_json_format(out, out_len,
		 "{\"project\":\"%s\",\"module_name\":\"%s\",\"module_version\":\"%s\",\"soc_name\":\"%s\",\"soc_version\":%d,\"fw\":\"%s\",\"build\":\"%s %s\",\"cores\":%d,\"mac\":\"%s\",\"serial\":\"%s\"}",
		 project, module_name, module_version, CONFIG_IDF_TARGET, chip.revision,
		 firmware_version, __DATE__, __TIME__, chip.cores, mac, serial);
}

void ble_shell_make_status_json(char *out, size_t out_len)
{
	uint32_t heap = esp_get_free_heap_size();
	int64_t remaining_s = 0;
	if (s_advertising_window_open && s_adv_deadline_us > esp_timer_get_time()) {
		remaining_s = (s_adv_deadline_us - esp_timer_get_time()) / BLE_SHELL_USECONDS_PER_SECOND;
	}
	ble_shell_json_format(out, out_len,
		 "{\"uptime_s\":%lu,\"connected\":%s,\"advertising_window\":%s,\"adv_remaining_s\":%lld,\"fault\":%s,\"fault_code\":%lu,\"diag\":%s,\"maintenance_unlocked\":%s,\"heap_free\":%lu,\"reset_reason\":%d}",
		 (unsigned long)ble_shell_uptime_s(),
		 s_conn_handle != BLE_HS_CONN_HANDLE_NONE ? "true" : "false",
		 s_advertising_window_open ? "true" : "false",
		 (long long)remaining_s,
		 s_fault_latched ? "true" : "false",
		 (unsigned long)s_fault_code,
		 s_cfg.diagnostic_mode_enabled ? "true" : "false",
		 s_maintenance_unlocked ? "true" : "false",
		 (unsigned long)heap,
		 esp_reset_reason());
	if (s_app_cfg.status_handler != NULL) {
		int err = s_app_cfg.status_handler(out, out_len, s_app_cfg.user_ctx);
		if (err == ESP_ERR_INVALID_SIZE || strnlen(out, out_len) == out_len)
			ble_shell_json_overflow(out, out_len);
	}
}

void ble_shell_make_config_json(char *out, size_t out_len)
{
	char device_name[BLE_SHELL_JSON_ESCAPE_BUF_LEN];
	char serial[BLE_SHELL_JSON_ESCAPE_BUF_LEN];
	ESCAPE_OR_FAIL(s_cfg.device_name, device_name);
	ESCAPE_OR_FAIL(s_cfg.serial_id, serial);
	ble_shell_json_format(out, out_len,
		 "{\"device_name\":\"%s\",\"serial\":\"%s\",\"adv_enabled\":%s,\"pair_window_s\":%lu,\"diag_enabled\":%s,\"ping_debug_enabled\":%s,\"security_mode\":%d,\"pair_gpio\":%d,\"pair_active_low\":%s}",
		 device_name, serial,
		 s_cfg.ble_advertising_enabled ? "true" : "false",
		 (unsigned long)s_cfg.pair_window_seconds,
		 s_cfg.diagnostic_mode_enabled ? "true" : "false",
		 s_cfg.ping_debug_enabled ? "true" : "false",
		 s_app_cfg.security_mode,
		 CONFIG_BLE_SHELL_PAIR_BUTTON_GPIO,
		 CONFIG_BLE_SHELL_PAIR_BUTTON_ACTIVE_LOW ? "true" : "false");
}

void ble_shell_make_diagnostic_json(char *out, size_t out_len)
{
	char mac[BLE_SHELL_MAC_STRING_LEN];
	ble_shell_get_mac_string(mac, sizeof(mac));
	ble_shell_json_format(out, out_len,
		 "{\"uptime_s\":%lu,\"heap_free\":%lu,\"heap_min_free\":%lu,\"mac\":\"%s\",\"nvs\":\"ok\",\"ble_synced\":%s,\"pair_gpio\":%d}",
		 (unsigned long)ble_shell_uptime_s(),
		 (unsigned long)esp_get_free_heap_size(),
		 (unsigned long)esp_get_minimum_free_heap_size(),
		 mac,
		 s_ble_synced ? "true" : "false",
		 CONFIG_BLE_SHELL_PAIR_BUTTON_GPIO);
}

#endif
