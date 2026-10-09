#!/usr/bin/env python3
"""Static contract for required M3000 BLE transport behavior after diagnostic cleanup."""
from pathlib import Path
import re

project = Path(__file__).resolve().parents[1]
component = project / "components" / "gama_blefob"

advertising = (component / "gama_blefob_advertising.c").read_text()
core = (component / "gama_blefob.c").read_text()
gatt = (component / "gama_blefob_gatt.c").read_text()
internal = (component / "gama_blefob_internal.h").read_text()
notify = (component / "gama_blefob_notify.c").read_text()
util = (component / "gama_blefob_util.c").read_text()
commands = (component / "gama_blefob_commands.c").read_text()
config_source = (component / "gama_blefob_config.c").read_text()
public_header = (component / "include" / "gama_blefob.h").read_text()
app_main = (project / "main" / "app_main.c").read_text()
sdkconfig = (project / "sdkconfig").read_text()
sdkconfig_defaults = (project / "sdkconfig.defaults").read_text()
kconfig = (component / "Kconfig.projbuild").read_text()


def function_body(source: str, signature: str) -> str:
    """Return one C function body, scoped by balanced braces."""
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening:index + 1]
    raise AssertionError(f"unterminated function: {signature}")

# Exact Nordic UART UUIDs in NimBLE's required little-endian byte order.
expected_uuids = {
    "BLE_SHELL_NUS_SERVICE_UUID": (
        0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
        0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E,
    ),
    "BLE_SHELL_NUS_COMMAND_UUID": (
        0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
        0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E,
    ),
    "BLE_SHELL_NUS_RESPONSE_UUID": (
        0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
        0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E,
    ),
}
for macro, expected in expected_uuids.items():
    match = re.search(
        rf"#define\s+{macro}\s+BLE_UUID128_DECLARE\((.*?)\)", internal, re.DOTALL
    )
    assert match is not None, f"missing {macro}"
    actual = tuple(int(token, 16) for token in re.findall(r"0x[0-9A-Fa-f]{2}", match.group(1)))
    assert actual == expected, f"wrong NimBLE byte order for {macro}: {actual!r}"

# One selected app service: response is subscribable/readable and command is the
# only/last writable endpoint. Pair-required attribute encryption remains intact.
assert gatt.count(".uuid = BLE_SHELL_NUS_SERVICE_UUID") == 1
assert gatt.count(".uuid = BLE_SHELL_NUS_COMMAND_UUID") == 1
assert gatt.count(".uuid = BLE_SHELL_NUS_RESPONSE_UUID") == 1
response_line = next(line for line in gatt.splitlines() if ".uuid = BLE_SHELL_NUS_RESPONSE_UUID" in line)
command_line = next(line for line in gatt.splitlines() if ".uuid = BLE_SHELL_NUS_COMMAND_UUID" in line)
assert ".val_handle = &s_response_chr_val_handle" in response_line
assert "BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY" in response_line
assert "BLE_GATT_CHR_F_READ_ENC" in response_line
assert "BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP" in command_line
assert "BLE_GATT_CHR_F_WRITE_ENC" in command_line
write_lines = [line for line in gatt.splitlines() if "BLE_GATT_CHR_F_WRITE" in line]
notify_lines = [line for line in gatt.splitlines() if "BLE_GATT_CHR_F_NOTIFY" in line]
assert write_lines == [command_line], f"unexpected writable app endpoint(s): {write_lines!r}"
assert notify_lines == [response_line], f"unexpected notifying app endpoint(s): {notify_lines!r}"
for stale in (
    "BLE_SHELL_UUID_SERVICE", "BLE_SHELL_UUID_COMMAND", "BLE_SHELL_UUID_RESPONSE",
    "BLE_SHELL_UUID_STATUS", "BLE_SHELL_UUID_LOG", "BLE_SHELL_UUID_CONFIG",
    "BLE_SHELL_UUID_DIAGNOSTIC",
):
    assert stale not in internal + gatt + kconfig + sdkconfig + sdkconfig_defaults, f"stale UUID16 surface remains: {stale}"

# Binary writes still flatten, queue, dispatch, and report handler failures.
binary_write = function_body(gatt, "static int ble_shell_enqueue_binary_write(")
for snippet in (
    "total_len == 0 || total_len > BLE_SHELL_MAX_PAYLOAD",
    "msg = calloc(1, sizeof(*msg))",
    "msg->type = BLE_SHELL_EVT_BINARY",
    "msg->len = total_len",
    "ble_hs_mbuf_to_flat(ctxt->om, msg->data, total_len, NULL)",
    "xQueueSend(s_ble_shell_queue, &msg, 0)",
    "free(msg)",
):
    assert snippet in binary_write, f"missing bounded queue-owned binary receive behavior: {snippet}"
assert binary_write.index("msg = calloc(1, sizeof(*msg))") < binary_write.index("xQueueSend(s_ble_shell_queue, &msg, 0)")
assert binary_write.count("free(msg)") >= 2, "binary message is not freed on every pre-queue failure"
for snippet in (
    "if (msg->type == BLE_SHELL_EVT_BINARY)",
    "handler(msg->data, msg->len, s_app_cfg.binary_write_user_ctx)",
    'ESP_LOGW(TAG, "binary write handler failed: %s"',
):
    assert snippet in commands, f"missing binary dispatch behavior: {snippet}"

text_write = function_body(gatt, "static int ble_shell_read_or_write_payload(")
for snippet in (
    "total_len == 0 || total_len >= dst_len",
    "BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN",
    "memchr(dst, '\\0', total_len) != NULL",
    "ble_hs_mbuf_to_flat(ctxt->om, dst, total_len, NULL)",
):
    assert snippet in text_write, f"text GATT writes do not fail closed: {snippet}"

# Response notification readiness, caching, and replay behavior remain intact.
assert "bool s_response_notify_active" in core
assert "extern bool s_response_notify_active" in internal
assert "s_response_notify_active = event->subscribe.cur_notify" in advertising
assert "if (event->subscribe.attr_handle != s_response_chr_val_handle)" in advertising
assert "s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_response_chr_val_handle == 0 || !s_response_notify_active" in notify
for snippet in (
    "static uint8_t s_last_binary[BLE_SHELL_MAX_PAYLOAD]",
    "static size_t s_last_binary_len",
    "static portMUX_TYPE s_last_binary_mux = portMUX_INITIALIZER_UNLOCKED",
    "memcpy(s_last_binary, data, size)",
    "s_last_binary_len = size",
    "static esp_err_t ble_shell_send_binary(",
    "void ble_shell_replay_binary(void)",
    "memcpy(cached, s_last_binary, cached_len)",
    "ble_shell_send_binary(cached, cached_len)",
):
    assert snippet in notify, f"missing cached replay behavior: {snippet}"
replay = function_body(notify, "void ble_shell_replay_binary(void)")
notify_binary = function_body(notify, "esp_err_t ble_shell_notify_binary(")
cache_order = (
    "taskENTER_CRITICAL(&s_last_binary_mux);",
    "memcpy(s_last_binary, data, size);",
    "s_last_binary_len = size;",
    "taskEXIT_CRITICAL(&s_last_binary_mux);",
)
cache_positions = [notify_binary.find(snippet) for snippet in cache_order]
assert all(position >= 0 for position in cache_positions) and cache_positions == sorted(cache_positions), "binary cache write is not wholly synchronized"
replay_order = (
    "taskENTER_CRITICAL(&s_last_binary_mux);",
    "cached_len = s_last_binary_len;",
    "memcpy(cached, s_last_binary, cached_len);",
    "taskEXIT_CRITICAL(&s_last_binary_mux);",
    "ble_shell_send_binary(cached, cached_len)",
)
replay_positions = [replay.find(snippet) for snippet in replay_order]
assert all(position >= 0 for position in replay_positions) and replay_positions == sorted(replay_positions), "binary cache replay is not snapshotted under the same lock"
assert "ble_shell_notify_binary(" not in replay, "replay must not copy the cache onto itself"
enc_change = advertising.split("case BLE_GAP_EVENT_ENC_CHANGE:", 1)[1].split("case ", 1)[0]
subscribe = advertising.split("case BLE_GAP_EVENT_SUBSCRIBE:", 1)[1].split("case ", 1)[0]
assert "event->enc_change.status == 0" in enc_change and "ble_shell_replay_binary();" in enc_change
assert "event->subscribe.cur_notify" in subscribe and "ble_shell_replay_binary();" in subscribe

# Security policy/callback plumbing and CCCD capacity remain unchanged.
for snippet in (
    "ble_hs_cfg.reset_cb = ble_on_reset",
    "ble_hs_cfg.sync_cb = ble_on_sync",
    "ble_hs_cfg.sm_bonding = s_app_cfg.security_mode == GAMA_BLEFOB_SECURITY_PAIR_REQUIRED",
    "ble_hs_cfg.sm_mitm = 0",
    "ble_hs_cfg.sm_sc = 1",
    "ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT",
    "BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID",
    "ble_store_config_init();",
):
    assert snippet in core, f"missing security behavior: {snippet}"
assert "CONFIG_BT_NIMBLE_MAX_CCCDS=16" in sdkconfig
assert "CONFIG_BT_NIMBLE_MAX_CCCDS=16" in sdkconfig_defaults

advertise_start = function_body(advertising, "void ble_advertise_start_remaining(void)")
assert "if (!ble_shell_stack_enter()) return;" in advertise_start
assert "ble_advertise_start_active();" in advertise_start
assert "ble_shell_stack_exit();" in advertise_start
advertise_start = function_body(advertising, "static void ble_advertise_start_active(void)")
for snippet in (
    "rsp_fields.uuids128 = BLE_UUID128(BLE_SHELL_NUS_SERVICE_UUID);",
    "rsp_fields.num_uuids128 = 1;",
    "rsp_fields.uuids128_is_complete = 1;",
    "rsp_fields.mfg_data = mfg_data;",
    "rsp_fields.mfg_data_len = sizeof(mfg_data);",
):
    assert snippet in advertise_start, f"NUS/manufacturer scan response field is missing: {snippet}"
assert expected_uuids["BLE_SHELL_NUS_SERVICE_UUID"] == (
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E,
), "advertised NUS service UUID contract changed"

# Task creation and application callback wiring remain present.
for snippet in (
    'xTaskCreate(ble_shell_task, "ble_shell", s_app_cfg.shell_task_stack_size',
    "nimble_port_freertos_init(ble_host_task)",
):
    assert snippet in core, f"missing BLE task creation behavior: {snippet}"
for snippet in (
    "gama_blefob_binary_write_handler_t binary_write_handler;",
    "void *binary_write_user_ctx;",
    "gama_blefob_rf_button_handler_t rf_button_handler;",
    "void *rf_button_user_ctx;",
    "gama_blefob_register_binary_write_handler(",
    "gama_blefob_register_rf_button_handler(",
):
    assert snippet in public_header, f"missing public callback contract: {snippet}"
for snippet in (
    "s_app_cfg.binary_write_handler = config->binary_write_handler;",
    "s_app_cfg.binary_write_user_ctx = config->binary_write_user_ctx;",
    "s_app_cfg.rf_button_handler = config->rf_button_handler;",
    "s_app_cfg.rf_button_user_ctx = config->rf_button_user_ctx;",
):
    assert snippet in config_source, f"init-time callback not copied: {snippet}"

app_init = function_body(app_main, "void app_main(void)")
pre_init_callbacks = (
    "ble_config.binary_write_handler = dura_legacy_ble_binary_write_handler;",
    "ble_config.binary_write_user_ctx = NULL;",
    "ble_config.rf_button_handler = dura_legacy_ble_rf_button_handler;",
    "ble_config.rf_button_user_ctx = NULL;",
    "ble_config.connected_handler = dura_ble_connected_handler;",
    "ble_config.disconnected_handler = dura_ble_disconnected_handler;",
)
ble_init_position = app_init.index("ESP_ERROR_CHECK(gama_blefob_init(&ble_config));")
for snippet in pre_init_callbacks:
    position = app_init.find(snippet)
    assert 0 <= position < ble_init_position, f"callback is not installed before BLE start: {snippet}"
assert "gama_blefob_register_binary_write_handler(" not in app_init
assert "gama_blefob_register_rf_button_handler(" not in app_init
for snippet in (
    "dura_board_set_ui_change_callback(",
    "dura_legacy_ble_ui_change_handler, NULL",
    "dura_board_get_ui_snapshot(&initial_snapshot)",
    "dura_legacy_notify_snapshot(&initial_snapshot)",
):
    assert snippet in app_init, f"missing UI snapshot callback/seed: {snippet}"
# Optional layouts and disconnected transport are nonfatal; ONLY these results
# are tolerated. Keep the fatal body exact so this cannot become a blanket ignore.
initial_guard = re.search(
    r"if\s*\((initial_snapshot_err[^{}]+)\)\s*\{([^{}]+)\}", app_init
)
assert initial_guard is not None, "missing initial notification status guard"
terms = {re.sub(r"\s+", "", term) for term in initial_guard.group(1).split("&&")}
assert terms == {
    "initial_snapshot_err!=ESP_OK",
    "initial_snapshot_err!=ESP_ERR_NOT_SUPPORTED",
    "initial_snapshot_err!=ESP_ERR_INVALID_STATE",
}, f"initial notification tolerated-status policy changed: {terms}"
assert re.sub(r"\s+", "", initial_guard.group(2)) == "ESP_ERROR_CHECK(initial_snapshot_err);", \
    "unexpected initial notification errors must remain fatal"
assert 'xTaskCreate(app_command_task, "dura_app_commands",' in app_main

snapshot_handler = function_body(app_main, "static esp_err_t dura_legacy_notify_snapshot(")
for snippet in (
    "dura_legacy_serialize_snapshot(&legacy, frame, sizeof(frame), &frame_len)",
    "gama_blefob_notify_binary(frame, frame_len)",
):
    assert snippet in snapshot_handler, f"missing screen notification behavior: {snippet}"

binary_handler = function_body(app_main, "static esp_err_t dura_legacy_ble_binary_write_handler(")
for snippet in (
    "bool printable_text = data != NULL && text_size > 0u && text_size < DURA_APP_COMMAND_MAX_LEN && !counted;",
    "(size_t)data[0] + 2u == size",
    "printable_text = data[i] >= 0x20u && data[i] <= 0x7eu;",
    "command[text_size] = '\\0';",
    "return gama_blefob_dispatch_text(command);",
    "dura_legacy_parse(data, size, &action)",
    "case DURA_LEGACY_BUTTON_POSITION_1: button = DURA_BUTTON_UP; break;",
    "case DURA_LEGACY_BUTTON_POSITION_2: button = DURA_BUTTON_DOWN; break;",
    "case DURA_LEGACY_BUTTON_POSITION_3: button = DURA_BUTTON_SELECT; break;",
    "case DURA_LEGACY_BUTTON_POSITION_4: button = DURA_BUTTON_BACK; break;",
    "dura_board_enqueue_button(button)",
):
    assert snippet in binary_handler, f"missing binary/text integration behavior: {snippet}"

# F03: prohibit the old generic bypass and duplicate notification path.
assert "dura_ble_command_handler(" not in binary_handler
assert "gama_blefob_respond(" not in binary_handler
text_dispatch = function_body(commands, "esp_err_t gama_blefob_dispatch_text(")
assert "ble_shell_handle_command(text);" in text_dispatch
assert "len >= BLE_SHELL_MAX_PAYLOAD" in text_dispatch
assert "esp_err_t gama_blefob_dispatch_text(const char *command);" in public_header
shell_dispatch = function_body(commands, "static void ble_shell_handle_command(")
assert shell_dispatch.index('strcasecmp(cmd, "ping")') < shell_dispatch.index("s_app_cfg.command_handler(cmd,")
assert "s_app_cfg.command_handler(cmd, out, BLE_SHELL_TEXT_RESPONSE_CAPACITY, s_app_cfg.user_ctx)" in shell_dispatch
assert "ble_shell_notify_response(out[0] != '\\0' ? out : \"ok\")" in shell_dispatch
assert "ble_shell_notify_status();" in shell_dispatch
for product_token in ("dura_", "DURA_BUTTON", "DURA_LEGACY"):
    assert product_token not in commands, f"product behavior leaked into generic dispatcher: {product_token}"

rf_handler = function_body(app_main, "static esp_err_t dura_legacy_ble_rf_button_handler(")
for snippet in (
    "if (!active) return ESP_OK;",
    "case DURA_LEGACY_BUTTON_POSITION_1: local_button = DURA_BUTTON_UP; break;",
    "case DURA_LEGACY_BUTTON_POSITION_2: local_button = DURA_BUTTON_DOWN; break;",
    "case DURA_LEGACY_BUTTON_POSITION_3: local_button = DURA_BUTTON_SELECT; break;",
    "case DURA_LEGACY_BUTTON_POSITION_4: local_button = DURA_BUTTON_BACK; break;",
    "dura_board_enqueue_button(local_button)",
):
    assert snippet in rf_handler, f"missing RF receiver emulation behavior: {snippet}"
for forbidden in ("gpio_set_level", "dura_meter_start_batch", "dura_meter_stop_batch"):
    assert forbidden not in binary_handler + rf_handler, f"BLE callback bypasses board event owner: {forbidden}"

# Advertising lifecycle remains bounded, and every disconnect opens one fresh full window.
open_window = advertising.split("void ble_shell_open_pairing_window(", 1)[1].split(
    "void ble_shell_close_pairing_window", 1
)[0]
for snippet in (
    "s_adv_deadline_us = esp_timer_get_time() + ((int64_t)seconds * BLE_SHELL_USECONDS_PER_SECOND);",
    "s_advertising_window_open = true;",
    "ble_advertise_start_remaining();",
):
    assert snippet in open_window, f"missing bounded-window behavior: {snippet}"

disconnect = advertising.split("case BLE_GAP_EVENT_DISCONNECT:", 1)[1].split("case ", 1)[0]
disconnect_order = (
    "s_conn_handle = BLE_HS_CONN_HANDLE_NONE;",
    "s_response_notify_active = false;",
    "if (s_app_cfg.disconnected_handler != NULL)",
    "s_app_cfg.disconnected_handler(s_app_cfg.user_ctx);",
    'ble_shell_open_pairing_window(s_cfg.pair_window_seconds, "disconnect advertising window opened");',
)
positions = [disconnect.find(snippet) for snippet in disconnect_order]
assert all(position >= 0 for position in positions), "disconnect lifecycle step missing"
assert positions == sorted(positions), "disconnect state/callback/advertising ordering changed"
assert "ble_advertise_start_remaining();" not in disconnect, "disconnect reuses old absolute deadline"

connect = advertising.split("case BLE_GAP_EVENT_CONNECT:", 1)[1].split("case ", 1)[0]
failed_connect = connect.split("} else {", 1)[1].split("return 0;", 1)[0]
assert "ble_advertise_start_remaining();" in failed_connect
assert "ble_shell_open_pairing_window" not in failed_connect, "failed connect refreshes deadline"

adv_complete = advertising.split("case BLE_GAP_EVENT_ADV_COMPLETE:", 1)[1].split("\tdefault:", 1)[0]
assert "esp_timer_get_time() < s_adv_deadline_us" in adv_complete
assert adv_complete.count("ble_advertise_start_remaining();") == 1
assert "s_advertising_window_open = false;" in adv_complete
assert "ble_shell_open_pairing_window" not in adv_complete, "expiry refreshes deadline"

# Diagnostic lifecycle markers, state, byte dump helper, and stack HWM probes are gone.
for marker in (
    '"ENC_CHANGE', '"IDENTITY_RESOLVED', '"SUBSCRIBE_RESPONSE', '"MTU conn=',
    '"GATT_MAP', '"DISCONNECT conn=', '"NOTIFY_TX', '"RX_BINARY',
    '"RX_BINARY_BYTES', '"NOTIFY_SUBMIT', '"NOTIFY_BYTES', '"NOTIFY_RESULT',
    '"NOTIFY_QUEUE',
):
    assert marker not in advertising + gatt + notify, f"diagnostic marker remains: {marker}"
for symbol in (
    "s_response_notify_ever_active",
    "s_command_chr_val_handle",
    "ble_shell_log_bytes",
    "BLE_SHELL_DIAG_HEX_MAX_BYTES",
):
    assert symbol not in advertising + core + gatt + internal + notify + util, f"diagnostic symbol remains: {symbol}"
for source in (commands, app_main):
    assert "uxTaskGetStackHighWaterMark" not in source
    assert "stack hwm:" not in source
assert "ble_shell_log_stack_hwm" not in commands
assert "log_current_task_stack_hwm" not in app_main

print("BLE_TRANSPORT_CONTRACT_PASS")
