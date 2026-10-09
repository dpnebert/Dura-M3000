#include "gama_blefob_internal.h"

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
static int append_to_mbuf(struct os_mbuf *om, const char *text)
{
	return os_mbuf_append(om, text, strlen(text)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

typedef enum {
	BLE_SHELL_DIS_MANUFACTURER = 1,
	BLE_SHELL_DIS_MODEL,
	BLE_SHELL_DIS_SERIAL,
	BLE_SHELL_DIS_HW_REV,
	BLE_SHELL_DIS_FW_REV,
} ble_shell_dis_field_t;

static int ble_shell_read_or_write_payload(struct ble_gatt_access_ctxt *ctxt, char *dst, size_t dst_len)
{
	uint16_t total_len = OS_MBUF_PKTLEN(ctxt->om);
	if (dst_len == 0 || total_len == 0 || total_len >= dst_len) {
		return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
	}
	int rc = ble_hs_mbuf_to_flat(ctxt->om, dst, total_len, NULL);
	if (rc != 0) {
		return BLE_ATT_ERR_UNLIKELY;
	}
	if (memchr(dst, '\0', total_len) != NULL) {
		return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
	}
	dst[total_len] = '\0';
	str_trim(dst);
	return 0;
}

static int ble_shell_enqueue_write(struct ble_gatt_access_ctxt *ctxt, uint8_t event_type)
{
	ble_shell_msg_t *msg = calloc(1, sizeof(*msg));
	if (msg == NULL) {
		return BLE_ATT_ERR_INSUFFICIENT_RES;
	}
	msg->type = event_type;
	int rc = ble_shell_read_or_write_payload(ctxt, (char *)msg->data, sizeof(msg->data));
	if (rc == 0) {
		msg->len = strlen((char *)msg->data);
		if (xQueueSend(s_ble_shell_queue, &msg, 0) == pdTRUE) {
			msg = NULL;
		} else {
			rc = BLE_ATT_ERR_INSUFFICIENT_RES;
		}
	}
	free(msg);
	return rc;
}

static int ble_shell_enqueue_binary_write(uint16_t conn_handle, uint16_t attr_handle,
					  struct ble_gatt_access_ctxt *ctxt)
{
	uint16_t total_len = OS_MBUF_PKTLEN(ctxt->om);
	ble_shell_msg_t *msg;
	(void)conn_handle;
	(void)attr_handle;

	if (total_len == 0 || total_len > BLE_SHELL_MAX_PAYLOAD) {
		return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
	}
	msg = calloc(1, sizeof(*msg));
	if (msg == NULL) {
		return BLE_ATT_ERR_INSUFFICIENT_RES;
	}
	msg->type = BLE_SHELL_EVT_BINARY;
	msg->len = total_len;
	if (ble_hs_mbuf_to_flat(ctxt->om, msg->data, total_len, NULL) != 0) {
		free(msg);
		return BLE_ATT_ERR_UNLIKELY;
	}
	if (xQueueSend(s_ble_shell_queue, &msg, 0) != pdTRUE) {
		free(msg);
		return BLE_ATT_ERR_INSUFFICIENT_RES;
	}
	return 0;
}

static int ble_command_chr_access_cb(uint16_t conn_handle, uint16_t attr_handle,
					    struct ble_gatt_access_ctxt *ctxt, void *arg)
{
	(void)arg;

	if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
		return BLE_ATT_ERR_UNLIKELY;
	}
	if (s_app_cfg.binary_write_handler != NULL) {
		return ble_shell_enqueue_binary_write(conn_handle, attr_handle, ctxt);
	}
	return ble_shell_enqueue_write(ctxt, BLE_SHELL_EVT_COMMAND);
}

static int ble_response_chr_access_cb(uint16_t conn_handle, uint16_t attr_handle,
					     struct ble_gatt_access_ctxt *ctxt, void *arg)
{
	(void)conn_handle;
	(void)attr_handle;
	(void)arg;
	if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
		return ble_shell_reply_read(conn_handle, ctxt->om);
	}
	return BLE_ATT_ERR_UNLIKELY;
}

static int dis_read_static_cb(uint16_t conn_handle, uint16_t attr_handle,
			      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
	char *text = calloc(1, BLE_SHELL_MAX_PAYLOAD);
	uintptr_t which = (uintptr_t)arg;
	(void)conn_handle;
	(void)attr_handle;
	if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
		free(text);
		return BLE_ATT_ERR_UNLIKELY;
	}
	if (text == NULL) {
		return BLE_ATT_ERR_INSUFFICIENT_RES;
	}
	switch (which) {
	case BLE_SHELL_DIS_MANUFACTURER:
		free(text);
		return append_to_mbuf(ctxt->om, "ESP BLE Shell");
	case BLE_SHELL_DIS_MODEL:
		free(text);
		return append_to_mbuf(ctxt->om, s_app_cfg.module_name);
	case BLE_SHELL_DIS_SERIAL:
		free(text);
		return append_to_mbuf(ctxt->om, s_cfg.serial_id);
	case BLE_SHELL_DIS_HW_REV:
		free(text);
		return append_to_mbuf(ctxt->om, s_app_cfg.module_version);
	case BLE_SHELL_DIS_FW_REV:
		free(text);
		return append_to_mbuf(ctxt->om, s_app_cfg.firmware_version);
	default: {
		ble_shell_make_info_json(text, BLE_SHELL_MAX_PAYLOAD);
		int rc = append_to_mbuf(ctxt->om, text);
		free(text);
		return rc;
	}
	}
}

const struct ble_gatt_svc_def gatt_svcs[] = {
	{
		.type = BLE_GATT_SVC_TYPE_PRIMARY,
		.uuid = BLE_UUID16_DECLARE(CONFIG_BLE_SHELL_UUID_DIS_SERVICE),
		.characteristics = (struct ble_gatt_chr_def[]) {
			{ .uuid = BLE_UUID16_DECLARE(CONFIG_BLE_SHELL_UUID_DIS_MANUFACTURER), .access_cb = dis_read_static_cb, .arg = (void *)BLE_SHELL_DIS_MANUFACTURER, .flags = BLE_GATT_CHR_F_READ },
			{ .uuid = BLE_UUID16_DECLARE(CONFIG_BLE_SHELL_UUID_DIS_MODEL), .access_cb = dis_read_static_cb, .arg = (void *)BLE_SHELL_DIS_MODEL, .flags = BLE_GATT_CHR_F_READ },
			{ .uuid = BLE_UUID16_DECLARE(CONFIG_BLE_SHELL_UUID_DIS_SERIAL), .access_cb = dis_read_static_cb, .arg = (void *)BLE_SHELL_DIS_SERIAL, .flags = BLE_GATT_CHR_F_READ },
			{ .uuid = BLE_UUID16_DECLARE(CONFIG_BLE_SHELL_UUID_DIS_HW_REV), .access_cb = dis_read_static_cb, .arg = (void *)BLE_SHELL_DIS_HW_REV, .flags = BLE_GATT_CHR_F_READ },
			{ .uuid = BLE_UUID16_DECLARE(CONFIG_BLE_SHELL_UUID_DIS_FW_REV), .access_cb = dis_read_static_cb, .arg = (void *)BLE_SHELL_DIS_FW_REV, .flags = BLE_GATT_CHR_F_READ },
			{0},
		},
	},
	{
		.type = BLE_GATT_SVC_TYPE_PRIMARY,
		.uuid = BLE_SHELL_NUS_SERVICE_UUID,
		.characteristics = (struct ble_gatt_chr_def[]) {
			{ .uuid = BLE_SHELL_NUS_RESPONSE_UUID, .access_cb = ble_response_chr_access_cb, .val_handle = &s_response_chr_val_handle, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC },
			{ .uuid = BLE_SHELL_NUS_COMMAND_UUID, .access_cb = ble_command_chr_access_cb, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC },
			{0},
		},
	},
	{0},
};

#endif
