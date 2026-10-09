#!/usr/bin/env python3
"""Regression for removal of the superseded M3000 BLE lifecycle diagnostics.

The historical command path is retained so existing verification runners do not
silently lose a gate. The selected Nordic-UART schema itself is covered by
scripts/test_ble_transport_contract.py.
"""
from pathlib import Path

project = Path(__file__).resolve().parents[1]
component = project / "components" / "gama_blefob"

advertising = (component / "gama_blefob_advertising.c").read_text()
core = (component / "gama_blefob.c").read_text()
gatt = (component / "gama_blefob_gatt.c").read_text()
internal = (component / "gama_blefob_internal.h").read_text()
notify = (component / "gama_blefob_notify.c").read_text()
util = (component / "gama_blefob_util.c").read_text()
commands = (component / "gama_blefob_commands.c").read_text()
app_main = (project / "main" / "app_main.c").read_text()

# Scattered bench-only lifecycle logs and byte/stack probes are removed. Normal
# error reporting is intentionally not prohibited by this regression.
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
    "s_status_chr_val_handle",
    "s_log_chr_val_handle",
    "s_diagnostic_chr_val_handle",
    "ble_shell_log_bytes",
    "BLE_SHELL_DIAG_HEX_MAX_BYTES",
    "ble_status_chr_access_cb",
    "ble_log_chr_access_cb",
    "ble_config_chr_access_cb",
    "ble_diagnostic_chr_access_cb",
):
    assert symbol not in advertising + core + gatt + internal + notify + util, (
        f"diagnostic/stale GATT symbol remains: {symbol}"
    )

for source in (commands, app_main):
    assert "uxTaskGetStackHighWaterMark" not in source
    assert "stack hwm:" not in source
assert "ble_shell_log_stack_hwm" not in commands
assert "log_current_task_stack_hwm" not in app_main

print("BLE_DIAGNOSTIC_CLEANUP_CONTRACT_PASS")
