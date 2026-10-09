#!/usr/bin/env python3
"""Customer Main Menu/Recirc/output/deep-sleep contract for M3000.

The policy portion is compiled and executed on the host.  The remaining checks
bind the policy to the customer UI, assigned M3000 GPIOs, enabled pump/sleep
defaults, disabled EV/Recirc-input defaults, BLE inhibition, and the existing
four-button EXT1 wake mask.
"""
from __future__ import annotations

import re
import subprocess
import tempfile
from pathlib import Path
from customer_title_oracle import check_customer_title


project = Path(__file__).resolve().parents[1]
board = (project / "components/dura_board/dura_board.c").read_text(encoding="utf-8")
board_h = (project / "components/dura_board/include/dura_board.h").read_text(encoding="utf-8")
meter = (project / "components/dura_meter/dura_meter.c").read_text(encoding="utf-8")
meter_h = (project / "components/dura_meter/include/dura_meter.h").read_text(encoding="utf-8")
app = (project / "main/app_main.c").read_text(encoding="utf-8")
kconfig = (project / "components/dura_board/Kconfig.projbuild").read_text(encoding="utf-8")
defaults = (project / "sdkconfig.defaults").read_text(encoding="utf-8")

policy_c = project / "components/dura_meter/dura_control_policy.c"
policy_h = project / "components/dura_meter/include/dura_control_policy.h"
assert policy_c.is_file(), "missing host-testable control policy implementation"
assert policy_h.is_file(), "missing host-testable control policy header"


def require_config(setting: str) -> None:
    assert re.search(rf"^{re.escape(setting)}$", defaults, re.MULTILINE), (
        f"sdkconfig.defaults must contain: {setting}"
    )


harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "dura_control_policy.h"

static void expect_outputs(dura_operation_mode_t mode, bool pump,
                           bool expected_pump, bool expected_recirc,
                           bool expected_inject)
{
    const dura_output_intent_t out = dura_control_output_intent(mode, pump);
    assert(out.pump == expected_pump);
    assert(out.recirc_ev == expected_recirc);
    assert(out.inject_ev == expected_inject);
}

int main(void)
{
    expect_outputs(DURA_OPERATION_IDLE, false, false, false, false);
    expect_outputs(DURA_OPERATION_MANUAL, true, true, false, true);
    expect_outputs(DURA_OPERATION_CALIBRATION, true, true, true, false);
    expect_outputs(DURA_OPERATION_RECIRC, true, true, true, false);
    expect_outputs(DURA_OPERATION_AUTO, true, true, false, true);
    expect_outputs(DURA_OPERATION_RECIRC, false, false, false, false);

    const dura_sleep_policy_input_t idle = {
        .operation_mode = DURA_OPERATION_IDLE,
        .pump_enabled = false,
        .batch_active = false,
        .calibration_active = false,
        .fault_latched = false,
        .ble_connected = false,
        .inactive_ms = 20000,
        .timeout_sec = 20,
    };
    assert(dura_control_sleep_allowed(&idle));

    dura_sleep_policy_input_t blocked = idle;
    blocked.ble_connected = true;
    assert(!dura_control_sleep_allowed(&blocked));
    blocked = idle;
    blocked.operation_mode = DURA_OPERATION_RECIRC;
    assert(!dura_control_sleep_allowed(&blocked));
    blocked = idle;
    blocked.pump_enabled = true;
    assert(!dura_control_sleep_allowed(&blocked));
    blocked = idle;
    blocked.batch_active = true;
    assert(!dura_control_sleep_allowed(&blocked));
    blocked = idle;
    blocked.calibration_active = true;
    assert(!dura_control_sleep_allowed(&blocked));
    blocked = idle;
    blocked.fault_latched = true;
    assert(!dura_control_sleep_allowed(&blocked));
    blocked = idle;
    blocked.inactive_ms = 19999;
    assert(!dura_control_sleep_allowed(&blocked));
    blocked = idle;
    blocked.timeout_sec = 0;
    assert(!dura_control_sleep_allowed(&blocked));

    puts("CONTROL_POLICY_HOST_PASS");
    return 0;
}
'''

with tempfile.TemporaryDirectory() as temp_dir:
    temp = Path(temp_dir)
    harness_c = temp / "policy_test.c"
    executable = temp / "policy_test"
    harness_c.write_text(harness, encoding="utf-8")
    subprocess.run(
        [
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            f"-I{policy_h.parent}", str(policy_c), str(harness_c),
            "-o", str(executable),
        ],
        check=True,
    )
    result = subprocess.run([str(executable)], check=True, capture_output=True, text=True)
    assert result.stdout.strip() == "CONTROL_POLICY_HOST_PASS"

# Customer-visible navigation and requested labels/text.
for token in (
    "DURA_UI_MAIN_MENU",
    "DURA_UI_MANUAL",
    "DURA_UI_RECIRC_EDIT",
    "DURA_UI_RECIRC_RUN",
    '"Man", "Calibr", "Recirc", "Auto"',
    '"Home", "Reset", "Stop", "Start"',
    '"Home", "Dn\\x7f", "Up~", "Start"',

    '"for Program Settings."',
    '"auto recirculation."',
    '"Select \'Calibr\' to calibrate"',
    '"and save as a \'PreCal\' ref #"',
    '"or select a cal ref # from"',
    '"\'Quick\'"',
    "DURA_AUTO_BATCH_DEFAULT_GAL",
    "DURA_RECIRC_BATCH_DEFAULT_GAL",
):
    assert token in board, f"missing customer UI contract token: {token}"

assert re.search(
    r"render_recirc_edit.*?Enter the preset amount.*?to be recirculated in tote\..*?"
    r"Home.*?Dn.*?Up~.*?Start",
    board,
    re.DOTALL,
), "Recirc edit screen must match the supplied text and soft keys"
assert re.search(
    r"render_recirc_run.*?Press Start to begin\..*?Press Stop to end\..*?"
    r"Home.*?NULL.*?Stop.*?Start",
    board,
    re.DOTALL,
), "Recirc run screen must expose Home / blank / Stop / Start"
# M-3000 product naming preserves genuine legacy artwork, not a literal label.
check_customer_title(project)
assert re.search(r"render_manual_run.*?\"Power: %u%%\".*?\"Home\", \"Reset\", \"Stop\", \"Start\"", board, re.DOTALL)
manual_render = re.search(
    r"static void render_manual_run\([^)]*\)\s*\{.*?^\}",
    board,
    re.DOTALL | re.MULTILINE,
)
assert manual_render, "missing Manual-screen renderer"
assert re.search(
    r"draw_num40_right\s*\(\s*128\s*,\s*12\s*,\s*amount\s*\)",
    manual_render.group(0),
), "Manual amount must retain the approved large 40-point, right-aligned renderer"
assert "draw_num24" not in manual_render.group(0), (
    "Manual amount must not regress to the smaller 24-point renderer"
)

assert re.search(r"s_ui_screen\s*=\s*DURA_UI_MAIN_MENU", board), (
    "startup UI must initialize to the Main Menu"
)
assert "dura_meter_start_manual" in meter and "dura_meter_stop_manual" in meter
assert "dura_meter_start_batch_mode" in meter
assert "dura_operation_mode_t operation_mode;" in meter_h
start_calibration = re.search(
    r"esp_err_t\s+dura_meter_start_calibration\s*\([^)]*\)\s*\{.*?\n\}",
    meter,
    re.DOTALL,
)
assert start_calibration, "missing calibration-start implementation"
# The reviewed F04 meter routes admission through the shared locked predicate
# (candidate32 verification/F04/quality-review.md, findings 4 and 6). Preserve
# the rejection contract across that helper boundary, not obsolete inlining.
assert re.search(
    r"if\s*\(operation_busy_locked\(\)\)\s*\{\s*METER_UNLOCK\(\);\s*"
    r"return ESP_ERR_INVALID_STATE;", start_calibration.group(0)
), "calibration start must reject busy operations before changing state"
busy = re.search(r"static bool operation_busy_locked\(void\)\s*\{.*?\n\}", meter, re.DOTALL)
assert busy and "s_meter.operation_mode != DURA_OPERATION_IDLE" in busy.group(0), (
    "calibration start must reject an already active Manual/Recirc/Auto operation"
)

# Assigned GPIOs and compile-time feature gates.
for kconfig_symbol in (
    "config DURA_BOARD_ENABLE_PUMP_OUTPUT",
    "config DURA_BOARD_ENABLE_RECIRC_INPUT",
    "config DURA_BOARD_GPIO_RECIRC_INPUT",
    "config DURA_BOARD_ENABLE_EV_OUTPUTS",
    "config DURA_BOARD_GPIO_RECIRC_EV_OUTPUT",
    "config DURA_BOARD_GPIO_INJECT_EV_OUTPUT",
    "config DURA_BOARD_ENABLE_DEEP_SLEEP",
    "config DURA_BOARD_DEEP_SLEEP_DELAY_SEC",
):
    assert kconfig_symbol in kconfig, f"missing Kconfig symbol: {kconfig_symbol}"

for symbol in (
    "CONFIG_DURA_BOARD_ENABLE_RECIRC_INPUT",
    "CONFIG_DURA_BOARD_ENABLE_EV_OUTPUTS",
):
    require_config(f"{symbol}=y")
require_config("CONFIG_DURA_BOARD_ENABLE_PUMP_OUTPUT=y")
require_config("CONFIG_DURA_BOARD_ENABLE_DEEP_SLEEP=y")
require_config("CONFIG_DURA_BOARD_DEEP_SLEEP_DELAY_SEC=60")
require_config("CONFIG_DURA_BOARD_GPIO_RECIRC_INPUT=35")
require_config("CONFIG_DURA_BOARD_GPIO_RECIRC_EV_OUTPUT=17")
require_config("CONFIG_DURA_BOARD_GPIO_INJECT_EV_OUTPUT=18")

for token in (
    "DURA_GPIO_RECIRC_INPUT",
    "DURA_GPIO_RECIRC_EV_OUTPUT",
    "DURA_GPIO_INJECT_EV_OUTPUT",
    "DURA_RECIRC_EV_OUTPUT_INACTIVE_LEVEL",
    "DURA_INJECT_EV_OUTPUT_INACTIVE_LEVEL",
):
    assert token in board_h, f"missing board GPIO contract: {token}"

assert "#if CONFIG_DURA_BOARD_ENABLE_EV_OUTPUTS" in board
# F04-reviewed serialized ownership computes intent in the meter, not board.
# Keep the real policy -> registered board output path protected.
publish = re.search(r"static void sync_outputs_locked\(void\)\s*\{.*?\n\}", meter, re.DOTALL)
assert publish and "s_output_handler(dura_control_output_intent(" in publish.group(0)
assert "dura_meter_set_output_handler(apply_meter_outputs)" in board
assert "static void apply_meter_outputs(dura_output_intent_t intent)" in board
assert "gpio_set_level(DURA_GPIO_RECIRC_EV_OUTPUT" in board
assert "gpio_set_level(DURA_GPIO_INJECT_EV_OUTPUT" in board
assert "CONFIG_DURA_BOARD_ENABLE_RECIRC_INPUT" in board
assert "s_ui_screen == DURA_UI_MAIN_MENU" in board

# End-to-end inactivity/sleep binding.  BLE callbacks must keep the board's
# connection state current, and both local and remote activity reset timeout.
assert "dura_board_note_activity" in board_h
assert "dura_board_set_ble_connected" in board_h
assert "dura_control_sleep_allowed" in board
assert ".timeout_sec = CONFIG_DURA_BOARD_DEEP_SLEEP_DELAY_SEC" in board
assert "dura_board_enter_deep_sleep" in board
assert app.count("dura_board_set_ble_connected(") >= 2

# Customer Recirc default and run-screen key behavior.
assert re.search(r"#define\s+DURA_AUTO_BATCH_DEFAULT_GAL\s+10\.0f", board)
assert re.search(r"#define\s+DURA_RECIRC_BATCH_DEFAULT_GAL\s+250\.0f", board)
assert board.count("s_ui_batch_amount = DURA_RECIRC_BATCH_DEFAULT_GAL;") >= 1
assert board.count("s_ui_batch_amount = DURA_AUTO_BATCH_DEFAULT_GAL;") >= 1
recirc_run = re.search(
    r"case\s+DURA_UI_RECIRC_RUN\s*:\s*\n\s*if\s*\(b.*?\n\s*break;",
    board,
    re.DOTALL,
)
assert recirc_run, "missing Recirc run button routing"
assert "DURA_BUTTON_UP" in recirc_run.group(0) and "dura_meter_cancel_batch()" in recirc_run.group(0)
assert "DURA_BUTTON_SELECT" in recirc_run.group(0) and "dura_meter_pause_batch()" in recirc_run.group(0)
assert "DURA_BUTTON_BACK" in recirc_run.group(0) and "dura_meter_resume_batch()" in recirc_run.group(0)

# Preserve every onboard physical push button as an EXT1 wake source.
for key in ("DURA_GPIO_KEY_1", "DURA_GPIO_KEY_2", "DURA_GPIO_KEY_3", "DURA_GPIO_KEY_4"):
    assert key in board, f"missing wake key: {key}"

print("CUSTOMER_RECIRC_SLEEP_CONTRACT_PASS")