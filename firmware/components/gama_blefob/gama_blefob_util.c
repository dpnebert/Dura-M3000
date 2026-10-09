#include "gama_blefob_internal.h"

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
void str_trim(char *s)
{
	size_t len;
	while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
		memmove(s, s + 1, strlen(s));
	}
	len = strlen(s);
	while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) {
		s[--len] = '\0';
	}
}

uint32_t ble_shell_uptime_s(void)
{
	return (uint32_t)(esp_timer_get_time() / (uint64_t)BLE_SHELL_USECONDS_PER_SECOND);
}

void ble_shell_get_mac_string(char *out, size_t out_len)
{
	uint8_t mac[BLE_SHELL_MAC_ADDR_LEN] = {0};
	esp_read_mac(mac, ESP_MAC_BT);
	snprintf(out, out_len, "%02X:%02X:%02X:%02X:%02X:%02X",
		 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

uint8_t ble_shell_adv_status_byte(void)
{
	uint8_t status = BLE_SHELL_ADV_STATUS_OK;
	if (s_fault_latched) {
		status |= BLE_SHELL_ADV_STATUS_FAULT;
	}
	if (s_cfg.diagnostic_mode_enabled) {
		status |= BLE_SHELL_ADV_STATUS_DIAG;
	}
	if (s_advertising_window_open) {
		status |= BLE_SHELL_ADV_STATUS_UNLOCKED;
	}
	return status;
}

const char *ble_shell_nonempty(const char *text, const char *fallback)
{
	return (text != NULL && text[0] != '\0') ? text : fallback;
}

void ble_shell_json_escape(const char *src, char *dst, size_t dst_len)
{
	size_t used = 0;
	if (dst_len == 0) {
		return;
	}
	if (src == NULL) {
		dst[0] = '\0';
		return;
	}
	while (*src != '\0' && used + 1 < dst_len) {
		unsigned char c = (unsigned char)*src++;
		const char *esc = NULL;
		char tmp[CONFIG_BLE_SHELL_JSON_UNICODE_ESCAPE_LEN];
		switch (c) {
		case '\"': esc = "\\\""; break;
		case '\\': esc = "\\\\"; break;
		case '\b': esc = "\\b"; break;
		case '\f': esc = "\\f"; break;
		case '\n': esc = "\\n"; break;
		case '\r': esc = "\\r"; break;
		case '\t': esc = "\\t"; break;
		default:
			if (c < 0x20) {
				snprintf(tmp, sizeof(tmp), "\\u%04x", c);
				esc = tmp;
			}
			break;
		}
		if (esc != NULL) {
			size_t esc_len = strlen(esc);
			if (used + esc_len >= dst_len) {
				break;
			}
			memcpy(dst + used, esc, esc_len);
			used += esc_len;
		} else {
			dst[used++] = (char)c;
		}
	}
	dst[used] = '\0';
}

void ble_shell_parse_semver(const char *text, uint8_t *major, uint8_t *minor, uint8_t *patch)
{
	unsigned long values[CONFIG_BLE_SHELL_SEMVER_FIELD_COUNT] = {0};
	char *end = NULL;
	const char *p = text;
	for (size_t i = 0; i < CONFIG_BLE_SHELL_SEMVER_FIELD_COUNT && p != NULL && *p != '\0'; ++i) {
		values[i] = strtoul(p, &end, 10);
		if (end == p) {
			break;
		}
		if (*end == '.') {
			p = end + 1;
		} else {
			p = end;
		}
	}
	*major = values[0] > 255 ? 255 : (uint8_t)values[0];
	*minor = values[1] > 255 ? 255 : (uint8_t)values[1];
	*patch = values[2] > 255 ? 255 : (uint8_t)values[2];
}

void ble_shell_set_bool_from_arg(const char *arg, bool *dst)
{
	if (strcmp(arg, "1") == 0 || strcasecmp(arg, "true") == 0 || strcasecmp(arg, "on") == 0 || strcasecmp(arg, "yes") == 0) {
		*dst = true;
	} else if (strcmp(arg, "0") == 0 || strcasecmp(arg, "false") == 0 || strcasecmp(arg, "off") == 0 || strcasecmp(arg, "no") == 0) {
		*dst = false;
	}
}

esp_err_t ble_shell_parse_positive_u32(const char *text, uint32_t *value)
{
	char *end = NULL;
	unsigned long parsed;

	if (text == NULL || value == NULL) {
		return ESP_ERR_INVALID_ARG;
	}
	while (*text == ' ' || *text == '\t') {
		text++;
	}
	parsed = strtoul(text, &end, 10);
	while (end != NULL && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
		end++;
	}
	if (end == text || (end != NULL && *end != '\0') || parsed == 0 || parsed > UINT32_MAX) {
		return ESP_ERR_INVALID_ARG;
	}
	*value = (uint32_t)parsed;
	return ESP_OK;
}

#endif
