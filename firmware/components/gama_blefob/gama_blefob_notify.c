#include "gama_blefob_internal.h"

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
static uint8_t s_last_binary[BLE_SHELL_MAX_PAYLOAD];
static size_t s_last_binary_len;
static portMUX_TYPE s_last_binary_mux = portMUX_INITIALIZER_UNLOCKED;

static bool ble_shell_link_is_encrypted(void)
{
	struct ble_gap_conn_desc desc;
	return s_conn_handle != BLE_HS_CONN_HANDLE_NONE &&
	       ble_gap_conn_find(s_conn_handle, &desc) == 0 && desc.sec_state.encrypted;
}

/* Response identity is boot-local, not a request/session ID. All retained text
 * and its header fields share one lock; never call NimBLE while holding it.
 * Generation 1 is the existing initial "ready" response. */
static portMUX_TYPE s_reply_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_reply_generation = 1;
static uint16_t s_reply_len = sizeof("ready") - 1U;

static size_t ble_shell_reply_value_cap(uint16_t conn_handle)
{
    uint16_t mtu = ble_att_mtu(conn_handle);
    /* A missing connection reports 0. Retention/read construction still stays
     * bounded; actual notifications are separately readiness-gated. */
    if (mtu < 23U) mtu = 23U;
    size_t cap = mtu - 3U;
    return cap < BLE_ATT_ATTR_MAX_LEN ? cap : BLE_ATT_ATTR_MAX_LEN;
}

static void ble_shell_reply_header(uint8_t *out, uint8_t flag, uint16_t offset)
{
    out[0] = 0; out[1] = 'R'; out[2] = 1; out[3] = flag;
    for (unsigned i = 0; i < 4U; ++i) out[4U + i] = (uint8_t)(s_reply_generation >> (8U * i));
    out[8] = (uint8_t)s_reply_len; out[9] = (uint8_t)(s_reply_len >> 8);
    out[10] = (uint8_t)offset; out[11] = (uint8_t)(offset >> 8);
}

/* Called with s_reply_mux held. flag=0 selects ordinary short text or metadata;
 * explicit metadata/page/error fetches are always framed, even for short text. */
static size_t ble_shell_reply_copy(uint8_t *out, size_t cap, uint8_t flag,
                                  uint32_t generation, uint16_t offset)
{
    size_t size = BLE_SHELL_REPLY_HEADER_SIZE;
    if (flag == 0 && s_reply_len <= cap) {
        memcpy(out, s_last_response, s_reply_len);
        return s_reply_len;
    }
    if (flag == 0) flag = BLE_SHELL_REPLY_META;
    if (flag == BLE_SHELL_REPLY_PAGE) {
        if (generation != s_reply_generation) flag = BLE_SHELL_REPLY_STALE;
        else if (offset > s_reply_len) flag = BLE_SHELL_REPLY_OFFSET;
        else {
            size_t count = s_reply_len - offset;
            if (count > cap - size) count = cap - size;
            memcpy(out + size, s_last_response + offset, count);
            size += count;
        }
    }
    ble_shell_reply_header(out, flag, offset);
    return size;
}

int ble_shell_reply_read(uint16_t conn_handle, struct os_mbuf *om)
{
    uint8_t value[BLE_ATT_ATTR_MAX_LEN];
    size_t cap = ble_shell_reply_value_cap(conn_handle);
    taskENTER_CRITICAL(&s_reply_mux);
    size_t size = ble_shell_reply_copy(value, cap, 0, 0, 0);
    taskEXIT_CRITICAL(&s_reply_mux);
    /* Current value, never the previous fetched page. Conservative MTU-3 also
     * fits a normal read's MTU-1, avoiding an overlong ATT attribute/blob. */
    return os_mbuf_append(om, value, size) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static esp_err_t ble_shell_send_binary_active(const uint8_t *data, size_t size)
{
	struct os_mbuf *om;
	int rc;

	if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_response_chr_val_handle == 0 || !s_response_notify_active) {
		return ESP_ERR_INVALID_STATE;
	}
	if (!ble_shell_link_is_encrypted()) return ESP_ERR_INVALID_STATE;
	om = ble_hs_mbuf_from_flat(data, size);
	if (om == NULL) return ESP_ERR_NO_MEM;
	rc = ble_gatts_notify_custom(s_conn_handle, s_response_chr_val_handle, om);
	return rc == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t ble_shell_send_binary(const uint8_t *data, size_t size)
{
    if (!ble_shell_stack_enter()) return ESP_ERR_INVALID_STATE;
    const esp_err_t err = ble_shell_send_binary_active(data, size);
    ble_shell_stack_exit();
    return err;
}

esp_err_t ble_shell_notify_binary(const uint8_t *data, size_t size)
{
	if (data == NULL || size == 0 || size > BLE_SHELL_MAX_PAYLOAD) {
		return ESP_ERR_INVALID_ARG;
	}
	taskENTER_CRITICAL(&s_last_binary_mux);
	memcpy(s_last_binary, data, size);
	s_last_binary_len = size;
	taskEXIT_CRITICAL(&s_last_binary_mux);
	return ble_shell_send_binary(data, size);
}

void ble_shell_replay_binary(void)
{
	uint8_t cached[BLE_SHELL_MAX_PAYLOAD];
	size_t cached_len;

	taskENTER_CRITICAL(&s_last_binary_mux);
	cached_len = s_last_binary_len;
	if (cached_len > 0) {
		memcpy(cached, s_last_binary, cached_len);
	}
	taskEXIT_CRITICAL(&s_last_binary_mux);
	if (cached_len > 0) {
		(void)ble_shell_send_binary(cached, cached_len);
	}
}

static void ble_shell_reply_send(uint8_t flag, uint32_t generation, uint16_t offset)
{
    if (!ble_shell_stack_enter()) return;
    uint8_t value[BLE_ATT_ATTR_MAX_LEN];
    size_t cap = ble_shell_reply_value_cap(s_conn_handle);
    taskENTER_CRITICAL(&s_reply_mux);
    size_t size = ble_shell_reply_copy(value, cap, flag, generation, offset);
    taskEXIT_CRITICAL(&s_reply_mux);
    /* Exactly one best-effort send. NimBLE consumes the mbuf on ALL outcomes;
     * neither allocation nor submission failure alters retained bytes. */
    (void)ble_shell_send_binary_active(value, size);
    ble_shell_stack_exit();
}

static bool ble_shell_reply_hex(const char *text, size_t digits, uint32_t *value)
{
    uint32_t parsed = 0;
    for (size_t i = 0; i < digits; ++i) {
        unsigned char c = (unsigned char)text[i];
        unsigned n;
        if (c >= '0' && c <= '9') n = c - '0';
        else if (c >= 'a' && c <= 'f') n = c - 'a' + 10U;
        else if (c >= 'A' && c <= 'F') n = c - 'A' + 10U;
        else return false;
        parsed = (parsed << 4) | n;
    }
    *value = parsed;
    return true;
}

bool ble_shell_reply_command(const char *cmd)
{
    if (strncasecmp(cmd, "reply", 5) != 0 ||
        (cmd[5] != '\0' && cmd[5] != ' ' && cmd[5] != '\t')) return false;
    uint8_t flag = BLE_SHELL_REPLY_BAD_REQUEST;
    uint32_t generation = 0, offset = 0;
    if (cmd[5] == '\0') flag = BLE_SHELL_REPLY_META;
    else if (strlen(cmd) == 19U && cmd[5] == ' ' && cmd[14] == ' ' &&
             ble_shell_reply_hex(cmd + 6, 8, &generation) &&
             ble_shell_reply_hex(cmd + 15, 4, &offset)) flag = BLE_SHELL_REPLY_PAGE;
    ble_shell_reply_send(flag, generation, (uint16_t)offset);
    return true;
}

void ble_shell_notify_response(const char *text)
{
    if (text == NULL) return;
    size_t size = strnlen(text, BLE_SHELL_TEXT_RESPONSE_CAPACITY);
    if (size == BLE_SHELL_TEXT_RESPONSE_CAPACITY) {
        text = BLE_SHELL_REPLY_OVERFLOW_JSON;
        size = sizeof(BLE_SHELL_REPLY_OVERFLOW_JSON) - 1U;
    }
    taskENTER_CRITICAL(&s_reply_mux);
    memcpy(s_last_response, text, size);
    s_last_response[size] = '\0';
    s_reply_len = (uint16_t)size;
    if (++s_reply_generation == 0) ++s_reply_generation;
    taskEXIT_CRITICAL(&s_reply_mux);
    ble_shell_reply_send(0, 0, 0);
}

void ble_shell_notify_status(void)
{
	ble_shell_make_status_json(s_last_status, sizeof(s_last_status));
}

void ble_shell_notify_log(const char *text)
{
	strlcpy(s_last_log, text, sizeof(s_last_log));
	ESP_LOGI(TAG, "event: %s", s_last_log);
}

void ble_shell_notify_diagnostic(const char *text)
{
	strlcpy(s_last_diagnostic, text, sizeof(s_last_diagnostic));
	ESP_LOGI(TAG, "diagnostic: %s", s_last_diagnostic);
}

#endif
