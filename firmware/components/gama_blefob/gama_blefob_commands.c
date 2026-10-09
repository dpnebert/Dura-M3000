#include "gama_blefob_internal.h"

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
static bool ble_shell_is_maintenance_command(const char *cmd)
{
	return strcasecmp(cmd, "pair") == 0 ||
	       strcasecmp(cmd, "advertise") == 0 ||
	       strcasecmp(cmd, "stop_advertise") == 0 ||
	       strcasecmp(cmd, "clear_faults") == 0 ||
	       strcasecmp(cmd, "start_diagnostic") == 0 ||
	       strcasecmp(cmd, "stop_diagnostic") == 0 ||
	       strcasecmp(cmd, "factory_reset") == 0 ||
	       strcasecmp(cmd, "reboot") == 0 ||
	       BLE_SHELL_CMD_HAS_PREFIX(cmd, "set_name ") ||
	       BLE_SHELL_CMD_HAS_PREFIX(cmd, "set_serial ") ||
	       BLE_SHELL_CMD_HAS_PREFIX(cmd, "set_pair_window ") ||
	       BLE_SHELL_CMD_HAS_PREFIX(cmd, "ble_enable ") ||
	       BLE_SHELL_CMD_HAS_PREFIX(cmd, "diag_enable ") ||
	       BLE_SHELL_CMD_HAS_PREFIX(cmd, "ping_debug ");
}

static bool ble_shell_command_allowed(const char *cmd)
{
	if (!ble_shell_is_maintenance_command(cmd)) {
		return true;
	}
	switch (s_app_cfg.security_mode) {
	case GAMA_BLEFOB_SECURITY_OPEN:
		return true;
	case GAMA_BLEFOB_SECURITY_PAIR_REQUIRED:
		return s_advertising_window_open;
	case GAMA_BLEFOB_SECURITY_UNLOCK_COMMAND_REQUIRED:
		return s_maintenance_unlocked;
	default:
		return false;
	}
}

static esp_err_t ble_shell_emit_rf_button_event(uint32_t button, bool active)
{
	gama_blefob_rf_button_handler_t handler = s_app_cfg.rf_button_handler;
	void *ctx = s_app_cfg.rf_button_user_ctx;

	if (handler == NULL) {
		return ESP_ERR_INVALID_STATE;
	}
	return handler(button, active, ctx);
}

/* The existing bool setter leaves its destination untouched for invalid input.
 * Opposite sentinels converge only for a recognized value. Keep that grammar. */
static bool ble_shell_valid_bool_arg(const char *arg)
{
	bool from_false = false, from_true = true;
	ble_shell_set_bool_from_arg(arg, &from_false);
	ble_shell_set_bool_from_arg(arg, &from_true);
	return from_false == from_true;
}

static void ble_shell_handle_command(char *cmd)
{
	str_trim(cmd);
	/* Fetching is read-only and allocation-independent; do not dispatch an app
	 * command, replace retained text, refresh status or mark user activity. */
	if (ble_shell_reply_command(cmd)) return;
	char *out = calloc(1, BLE_SHELL_TEXT_RESPONSE_CAPACITY);
	char *arg;
	bool user_activity = false;
	if (out == NULL) {
		ble_shell_notify_response("error: BLE command response allocation failed");
		return;
	}
	ble_shell_notify_log(cmd);

	if (BLE_SHELL_CMD_HAS_PREFIX(cmd, "unlock ")) {
		arg = BLE_SHELL_CMD_ARG(cmd, "unlock ");
		str_trim(arg);
		if (CONFIG_BLE_SHELL_UNLOCK_TOKEN[0] != '\0' && strcmp(arg, CONFIG_BLE_SHELL_UNLOCK_TOKEN) == 0) {
			s_maintenance_unlocked = true;
			user_activity = true;
			ble_shell_notify_response("ok: maintenance commands unlocked");
		} else {
			ble_shell_notify_response("error: invalid unlock token");
		}
	} else if (strcasecmp(cmd, "lock") == 0) {
		s_maintenance_unlocked = false;
		user_activity = true;
		ble_shell_notify_response("ok: maintenance commands locked");
	} else if (!ble_shell_command_allowed(cmd)) {
		ble_shell_notify_response("error: command requires active pair window or unlock");
	} else if (strcasecmp(cmd, "ping") == 0) {
		if (s_cfg.ping_debug_enabled) {
			ble_shell_notify_response("pong");
		} else {
			ble_shell_notify_response("error: ping debug mode disabled");
		}
	} else if (strcasecmp(cmd, "get_info") == 0 || strcasecmp(cmd, "info") == 0) {
		ble_shell_make_info_json(out, BLE_SHELL_TEXT_RESPONSE_CAPACITY);
		ble_shell_notify_response(out);
	} else if (strcasecmp(cmd, "get_status") == 0 || strcasecmp(cmd, "status") == 0) {
		ble_shell_make_status_json(out, BLE_SHELL_TEXT_RESPONSE_CAPACITY);
		ble_shell_notify_response(out);
	} else if (strcasecmp(cmd, "get_config") == 0 || strcasecmp(cmd, "config") == 0) {
		ble_shell_make_config_json(out, BLE_SHELL_TEXT_RESPONSE_CAPACITY);
		ble_shell_notify_response(out);
	} else if (strcasecmp(cmd, "list commands") == 0 || strcasecmp(cmd, "help") == 0) {
		ble_shell_notify_response("commands: ping; info; status; config; set/clear rf_button <button>; pair; stop_advertise; clear_faults; start/stop_diagnostic; set_name; set_serial; set_pair_window; ble_enable; diag_enable; ping_debug; lock/unlock");
	} else if (BLE_SHELL_CMD_HAS_PREFIX(cmd, "set rf_button ")) {
		uint32_t button = 0;
		esp_err_t err = ble_shell_parse_positive_u32(BLE_SHELL_CMD_ARG(cmd, "set rf_button "), &button);
		if (err != ESP_OK) {
			ble_shell_notify_response("error: rf_button must be a positive integer");
		} else {
			err = ble_shell_emit_rf_button_event(button, true);
			snprintf(out, BLE_SHELL_TEXT_RESPONSE_CAPACITY, "%s: rf_button %lu set",
				 err == ESP_OK ? "ok" : "error", (unsigned long)button);
			ble_shell_notify_response(out);
		}
	} else if (BLE_SHELL_CMD_HAS_PREFIX(cmd, "clear rf_button ")) {
		uint32_t button = 0;
		esp_err_t err = ble_shell_parse_positive_u32(BLE_SHELL_CMD_ARG(cmd, "clear rf_button "), &button);
		if (err != ESP_OK) {
			ble_shell_notify_response("error: rf_button must be a positive integer");
		} else {
			err = ble_shell_emit_rf_button_event(button, false);
			snprintf(out, BLE_SHELL_TEXT_RESPONSE_CAPACITY, "%s: rf_button %lu cleared",
				 err == ESP_OK ? "ok" : "error", (unsigned long)button);
			ble_shell_notify_response(out);
		}
	} else if (strcasecmp(cmd, "pair") == 0 || strcasecmp(cmd, "advertise") == 0) {
		ble_shell_open_pairing_window(s_cfg.pair_window_seconds, "command opened pair window");
		user_activity = true;
		ble_shell_notify_response("ok: pair window opened");
	} else if (strcasecmp(cmd, "stop_advertise") == 0) {
		ble_shell_close_pairing_window();
		user_activity = true;
		ble_shell_notify_response("ok: pair window closed");
	} else if (strcasecmp(cmd, "clear_faults") == 0) {
		s_fault_latched = false;
		s_fault_code = 0;
		user_activity = true;
		ble_shell_notify_response("ok: faults cleared");
	} else if (strcasecmp(cmd, "start_diagnostic") == 0) {
		s_cfg.diagnostic_mode_enabled = true;
		user_activity = true;
		(void)ble_shell_config_save();
		ble_shell_make_diagnostic_json(out, BLE_SHELL_TEXT_RESPONSE_CAPACITY);
		ble_shell_notify_diagnostic(out);
		ble_shell_notify_response("ok: diagnostic mode enabled");
	} else if (strcasecmp(cmd, "stop_diagnostic") == 0) {
		s_cfg.diagnostic_mode_enabled = false;
		user_activity = true;
		(void)ble_shell_config_save();
		ble_shell_notify_diagnostic("diagnostic idle");
		ble_shell_notify_response("ok: diagnostic mode disabled");
	} else if (strcasecmp(cmd, "factory_reset") == 0) {
		esp_err_t err = ble_shell_config_factory_reset();
		user_activity = err == ESP_OK;
		ESP_ERROR_CHECK_WITHOUT_ABORT(ble_svc_gap_device_name_set(s_cfg.device_name));
		snprintf(out, BLE_SHELL_TEXT_RESPONSE_CAPACITY, "factory_reset:%s", esp_err_to_name(err));
		ble_shell_notify_response(out);
	} else if (strcasecmp(cmd, "reboot") == 0) {
		if (s_app_cfg.user_activity_handler != NULL) {
			s_app_cfg.user_activity_handler(s_app_cfg.user_ctx);
		}
		ble_shell_notify_response("ok: rebooting");
		vTaskDelay(pdMS_TO_TICKS(CONFIG_BLE_SHELL_REBOOT_DELAY_MS));
		esp_restart();
	} else if (BLE_SHELL_CMD_HAS_PREFIX(cmd, "set_name ")) {
		arg = BLE_SHELL_CMD_ARG(cmd, "set_name ");
		str_trim(arg);
		strlcpy(s_cfg.device_name, arg, sizeof(s_cfg.device_name));
		user_activity = true;
		ESP_ERROR_CHECK_WITHOUT_ABORT(ble_svc_gap_device_name_set(s_cfg.device_name));
		(void)ble_shell_config_save();
		ble_shell_notify_response("ok: device name saved");
	} else if (BLE_SHELL_CMD_HAS_PREFIX(cmd, "set_serial ")) {
		arg = BLE_SHELL_CMD_ARG(cmd, "set_serial ");
		str_trim(arg);
		strlcpy(s_cfg.serial_id, arg, sizeof(s_cfg.serial_id));
		user_activity = true;
		(void)ble_shell_config_save();
		ble_shell_notify_response("ok: serial saved");
	} else if (BLE_SHELL_CMD_HAS_PREFIX(cmd, "set_pair_window ")) {
		uint32_t seconds = 0;
		if (ble_shell_parse_positive_u32(BLE_SHELL_CMD_ARG(cmd, "set_pair_window "), &seconds) != ESP_OK ||
		    seconds < BLE_SHELL_PAIR_ADV_SECONDS_MIN || seconds > BLE_SHELL_PAIR_ADV_SECONDS_MAX) {
			ble_shell_notify_response("error: pair window must be 1..600 seconds");
		} else {
			s_cfg.pair_window_seconds = seconds;
			user_activity = true;
			(void)ble_shell_config_save();
			ble_shell_notify_response("ok: pair window saved");
		}
	} else if (BLE_SHELL_CMD_HAS_PREFIX(cmd, "ble_enable ")) {
		arg = BLE_SHELL_CMD_ARG(cmd, "ble_enable ");
		str_trim(arg);
		ble_shell_set_bool_from_arg(arg, &s_cfg.ble_advertising_enabled);
		user_activity = ble_shell_valid_bool_arg(arg);
		(void)ble_shell_config_save();
		ble_shell_notify_response("ok: BLE advertising enable saved");
	} else if (BLE_SHELL_CMD_HAS_PREFIX(cmd, "diag_enable ")) {
		arg = BLE_SHELL_CMD_ARG(cmd, "diag_enable ");
		str_trim(arg);
		ble_shell_set_bool_from_arg(arg, &s_cfg.diagnostic_mode_enabled);
		user_activity = ble_shell_valid_bool_arg(arg);
		(void)ble_shell_config_save();
		ble_shell_notify_response("ok: diagnostic enable saved");
	} else if (BLE_SHELL_CMD_HAS_PREFIX(cmd, "ping_debug ")) {
		arg = BLE_SHELL_CMD_ARG(cmd, "ping_debug ");
		str_trim(arg);
		ble_shell_set_bool_from_arg(arg, &s_cfg.ping_debug_enabled);
		user_activity = ble_shell_valid_bool_arg(arg);
		(void)ble_shell_config_save();
		ble_shell_notify_response("ok: ping debug enable saved");
	} else {
		esp_err_t handler_err = ESP_ERR_NOT_FOUND;
		if (s_app_cfg.command_handler != NULL) {
			out[0] = '\0';
			handler_err = s_app_cfg.command_handler(cmd, out, BLE_SHELL_TEXT_RESPONSE_CAPACITY, s_app_cfg.user_ctx);
		}
		if (handler_err == ESP_ERR_INVALID_SIZE) {
			/* Producers must report capacity failure, not return a silently
			 * NUL-terminated partial prefix. Unterminated output is also caught
			 * by the bounded retention function below. */
			ble_shell_notify_response(BLE_SHELL_REPLY_OVERFLOW_JSON);
		} else if (handler_err == ESP_OK || out[0] != '\0') {
			ble_shell_notify_response(out[0] != '\0' ? out : "ok");
		} else {
			snprintf(out, BLE_SHELL_TEXT_RESPONSE_CAPACITY, "error: unknown command '%s'", cmd);
			ble_shell_notify_response(out);
		}
	}
	if (user_activity && s_app_cfg.user_activity_handler != NULL) {
		s_app_cfg.user_activity_handler(s_app_cfg.user_ctx);
	}
	ble_shell_notify_status();
	free(out);
}

void ble_shell_task(void *arg)
{
	ble_shell_msg_t *msg = NULL;
	(void)arg;
	while (1) {
		/* Readiness does not transfer ownership. Never hold a lease while
		 * waiting, and never own a dequeued command without a lease: sleep
		 * must either veto this worker or drain the still-queued command. */
		if (xQueuePeek(s_ble_shell_queue, &msg, pdMS_TO_TICKS(CONFIG_BLE_SHELL_QUEUE_RECEIVE_TIMEOUT_MS)) != pdTRUE) continue;
		msg = NULL; /* The peeked pointer is not ours and may already be freed. */
		if (!ble_shell_stack_enter()) {
			/* A failed stop can leave a nonempty queue behind a closed cutoff.
			 * Peek then returns immediately: yield a guaranteed nonzero tick,
			 * outside the lease, rather than starving the idle/other tasks. */
			vTaskDelay(1);
			continue;
		}
		if (xQueueReceive(s_ble_shell_queue, &msg, 0) == pdTRUE && msg != NULL) {
			if (msg->type == BLE_SHELL_EVT_BINARY) {
				gama_blefob_binary_write_handler_t handler = s_app_cfg.binary_write_handler;
				if (handler != NULL) {
					esp_err_t err = handler(msg->data, msg->len, s_app_cfg.binary_write_user_ctx);
					if (err != ESP_OK) {
						ESP_LOGW(TAG, "binary write handler failed: %s", esp_err_to_name(err));
					}
				}
			} else {
				ble_shell_handle_command((char *)msg->data);
			}
			free(msg);
			msg = NULL;
		}
		ble_shell_stack_exit();
	}
}

#endif

esp_err_t gama_blefob_dispatch_text(const char *command)
{
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
    if (command == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    size_t len = strlen(command);
    if (len == 0u || len >= BLE_SHELL_MAX_PAYLOAD) return ESP_ERR_INVALID_SIZE;
    char text[BLE_SHELL_MAX_PAYLOAD];
    memcpy(text, command, len + 1u);
    if (!ble_shell_stack_enter()) return ESP_ERR_INVALID_STATE;
    ble_shell_handle_command(text);
    ble_shell_stack_exit();
    return ESP_OK;
#else
    (void)command;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
