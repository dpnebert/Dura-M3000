#!/usr/bin/env python3
"""Conformance contract for the authoritative 2026-09-08 M3000 UI feedback.

This contract binds the preserved customer image to the six source-verifiable
UI states represented by it: Main Menu, Calibration menu, Auto preset edit,
Recirc preset edit, Recirc run, and Manual run.  It locks exact labels,
renderer coordinates, alignments, legacy font/bitmap dimensions, defaults,
button routing, and the pump/valve mode policy.  Physical LCD pixel matching
and hardware output levels remain bench-verification items.
"""
from __future__ import annotations

import hashlib
import re
import subprocess
import tempfile
from pathlib import Path


project = Path(__file__).resolve().parents[1]
board_path = project / "components/dura_board/dura_board.c"
assets_path = project / "components/dura_board/dura_lcd_assets.c"
meter_path = project / "components/dura_meter/dura_meter.c"
policy_path = project / "components/dura_meter/dura_control_policy.c"
policy_header = project / "components/dura_meter/include/dura_control_policy.h"
defaults_path = project / "sdkconfig.defaults"
reference_path = (
    project.parent
    / "references/customer/2026-09-08-latest-firmware-feedback/latest-firmware-feedback.png"
)

board = board_path.read_text(encoding="utf-8")
assets = assets_path.read_text(encoding="utf-8")
meter = meter_path.read_text(encoding="utf-8")
defaults = defaults_path.read_text(encoding="utf-8")

EXPECTED_REFERENCE_SHA256 = "bb78a5d9909c0c22c837acfa1daf61be76c1ede8fdbd48252371c34ee1860c2d"
assert reference_path.is_file(), f"missing authoritative customer image: {reference_path}"
assert hashlib.sha256(reference_path.read_bytes()).hexdigest() == EXPECTED_REFERENCE_SHA256, (
    "authoritative 2026-09-08 customer-feedback image changed"
)


def extract_function(source: str, name: str) -> str:
    match = re.search(rf"(?:^|\n)[^\n;]*\b{re.escape(name)}\s*\([^;]*?\)\s*\{{", source)
    assert match is not None, f"missing function: {name}"
    start = source.find("{", match.start())
    depth = 0
    in_string = False
    escaped = False
    for pos in range(start, len(source)):
        ch = source[pos]
        if in_string:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == '"':
                in_string = False
            continue
        if ch == '"':
            in_string = True
        elif ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return source[match.start() : pos + 1]
    raise AssertionError(f"unterminated function: {name}")


def require_literal(function: str, literal: str, reason: str) -> None:
    assert literal in function, reason


def require_regex(text: str, pattern: str, reason: str) -> None:
    assert re.search(pattern, text, re.DOTALL | re.MULTILINE), reason


# Shared 128x64 renderer geometry and assets.
draw_button = extract_function(board, "draw_legacy_button")
require_literal(draw_button, "static const int x0[] = {1, 32, 63, 94};", "button box x coordinates changed")
require_literal(draw_button, "static const int xc[] = {16, 47, 78, 109};", "button label centers changed")
require_regex(draw_button, r"draw_legacy_text_aligned\('C',\s*xc\[slot\],\s*53,\s*label,\s*false\)", "button labels must be centered at y=53")
require_regex(draw_button, r"dura_lcd_draw_rect\(x0\[slot\],\s*52,\s*31,\s*12,\s*true\)", "button boxes must remain y=52, 31x12")

draw_titles = extract_function(board, "draw_legacy_titles")
for y, label in ((2, "l1"), (12, "l2"), (22, "l3"), (32, "l4"), (42, "l5")):
    require_regex(
        draw_titles,
        rf"draw_legacy_text_aligned\('C',\s*64,\s*{y},\s*{label},\s*false\)",
        f"title line {label} must remain centered at x=64 y={y}",
    )

aligned = extract_function(board, "draw_legacy_text_aligned")
require_literal(aligned, "legacy_text_width(&dura_font_ui_small, text)", "small UI alignment must use the legacy small-font metrics")
require_literal(aligned, "if (orient == 'C') xa = x - (w / 2) + 1;", "center alignment origin changed")
require_literal(aligned, "else xa = x - w;", "right alignment origin changed")
require_literal(aligned, "draw_legacy_font_char", "small UI text must use the legacy font renderer")

require_regex(
    assets,
    r"dura_bitmap_durametertitle_bmp_glcd_bmp\s*=\s*\{\s*128u,\s*8u,\s*dura_asset_durametertitle_bmp_glcd_bmp",
    "Dura-Meter title asset must remain 128x8",
)
require_regex(
    assets,
    r"dura_font_ui_small\s*=\s*\{[^\n]*33u,\s*223u,\s*11u,\s*2u,[^\n]*dura_asset_comicSansMS7ptCharBitmaps",
    "shared UI font must remain the 11-pixel legacy Comic Sans 7pt asset",
)
require_regex(
    assets,
    r"dura_font_num_24\s*=\s*\{[^\n]*46u,\s*15u,\s*23u,\s*3u,[^\n]*dura_asset_arialNarrow24ptCharBitmaps",
    "preset/run numeric font must remain the 23-pixel Arial Narrow 24pt asset",
)
require_regex(
    assets,
    r"dura_font_num_40\s*=\s*\{[^\n]*46u,\s*13u,\s*40u,\s*3u,[^\n]*dura_asset_arialNarrow40ptCharBitmaps",
    "Manual numeric font must remain the 40-pixel Arial Narrow 40pt asset",
)
require_regex(extract_function(board, "draw_num24_char"), r"row\s*=\s*0;\s*row\s*<\s*23u", "24pt renderer height changed")
require_regex(extract_function(board, "draw_num40_char"), r"row\s*=\s*0;\s*row\s*<\s*40u", "40pt renderer height changed")

# Composition 1: Main Menu with Dan's 2026-09-10 title-removal/power follow-up.
home = extract_function(board, "draw_legacy_home")
for literal in (
    'dura_lcd_draw_bitmap_1bpp(0, 0, dura_asset_durametertitle_bmp_glcd_bmp, 128, 8, true);',
    "draw_font_asset_text_aligned(&dura_font_ui_small, 'C', 64, 9,",
    "draw_font_asset_text_aligned(&dura_font_ui_small, 'C', 64, 18,",
    "draw_font_asset_text_aligned(&dura_font_ui_small, 'C', 64, 27,",
    "draw_font_asset_text_aligned(&dura_font_ui_small, 'C', 64, 36,",
    'snprintf(power, sizeof(power), "Power: %u%%", (unsigned)battery.percent);',
    'snprintf(power, sizeof(power), "Power: --%%");',
    "draw_font_asset_text_aligned(&dura_font_ui_main_menu_10, 'L', 0, 44, power, false);",
    'draw_legacy_buttons("Man", "Calibr", "Recirc", "Auto");',
):
    require_literal(home, literal, f"Main Menu contract changed: {literal}")
assert '"Main Menu"' not in home, "removed Main Menu heading returned"
require_literal(board, "static dura_ui_screen_t s_ui_screen = DURA_UI_MAIN_MENU;", "startup screen must be Main Menu")

# Composition 2: Calibration menu shown after Calibr.
cal_start = extract_function(board, "render_cal_start")
for literal in (
    '"Select \'Calibr\' to calibrate"',
    '"and save as a \'PreCal\' ref #"',
    '"or select a cal ref # from"',
    '"\'Quick\'"',
    '"Home", "Calibr", "PreCal", "Quick"',
):
    require_literal(cal_start, literal, f"Calibration-menu contract changed: {literal}")

# Compositions 3 and 4: Auto/Recirc preset editing share the same amount renderer.
batch_edit = extract_function(board, "render_batch_edit")
for literal in (
    '"Enter the preset amount"',
    '"to be pumped."',
    'auto_mode ? "Home" : "Reset"',
    '"Dn\\x7f", "Up~", "Start"',
    "render_editable_batch_amount(s_ui_batch_amount, meter->selected_units);",
):
    require_literal(batch_edit, literal, f"Auto preset-edit contract changed: {literal}")
recirc_edit = extract_function(board, "render_recirc_edit")
for literal in (
    '"Enter the preset amount"',
    '"to be recirculated in tote."',
    '"Home", "Dn\\x7f", "Up~", "Start"',
    "render_editable_batch_amount(s_ui_batch_amount, meter->selected_units);",
):
    require_literal(recirc_edit, literal, f"Recirc preset-edit contract changed: {literal}")
editable_amount = extract_function(board, "render_editable_batch_amount")
require_literal(editable_amount, "draw_num24_text(2, 26, text);", "preset amount must remain left-aligned at x=2 y=26")
require_literal(editable_amount, "draw_legacy_text_aligned('R', 128, 40, legacy_batch_unit_label(units), false);", "preset unit must remain right-aligned at x=128 y=40")
format_editable = extract_function(board, "format_editable_batch_amount")
# 2026-09-11 approval generalizes precision to every unit; geometry stays locked.
require_literal(format_editable, "volume_display_decimals(amount, units)", "edit must use per-unit precision")
precision = extract_function(board, "volume_display_decimals")
for unit in ("GALLON", "LITER", "OUNCE"):
    require_literal(precision, f"CONFIG_DURA_BOARD_VOLUME_{unit}_DECIMALS", "unit precision is not SDK-controlled")
    require_regex(defaults, rf"^CONFIG_DURA_BOARD_VOLUME_{unit}_DECIMALS=2$", "normal unit default must be two decimals")
require_literal(precision, "(float)CONFIG_DURA_BOARD_VOLUME_2DP_LIMIT_TENTHS / 10.0f", "cutoff must use display-unit tenths")
require_literal(precision, "amount > cutoff && decimals > 1 ? 1 : decimals", "cutoff must not raise 0/1-decimal units")
require_regex(defaults, r"^CONFIG_DURA_BOARD_VOLUME_2DP_LIMIT_TENTHS=1999$", "normal cutoff must remain 199.9")
require_regex(board, r"#define\s+DURA_AUTO_BATCH_DEFAULT_GAL\s+10\.0f", "Auto default must remain 10 gallons")
require_regex(board, r"#define\s+DURA_RECIRC_BATCH_DEFAULT_GAL\s+250\.0f", "Recirc default must remain 250 gallons")

# Composition 5: Recirc run state.
recirc_run_render = extract_function(board, "render_recirc_run")
for literal in (
    '"Press Start to begin."',
    '"Press Stop to end."',
    '"Home", NULL, "Stop", "Start"',
    "render_legacy_batch_amount(meter->remaining_batch, meter->selected_units);",
):
    require_literal(recirc_run_render, literal, f"Recirc run-screen contract changed: {literal}")
legacy_amount = extract_function(board, "render_legacy_batch_amount")
require_literal(legacy_amount, "draw_num24_text(2, 26, text);", "run amount must remain left-aligned at x=2 y=26")
require_literal(legacy_amount, "draw_legacy_text_aligned('R', 128, 40, legacy_batch_unit_label(units), false);", "run unit must remain right-aligned at x=128 y=40")

# Auto run uses the same reference run composition with the 10.00 preset.
auto_run_render = extract_function(board, "render_batch_run")
for literal in (
    '"Press Start to begin."',
    '"Press Stop to end."',
    'auto_mode ? "Home" : "Stop"',
    'auto_mode ? "Stop" : NULL, "Start"',
    "render_legacy_batch_amount(meter->remaining_batch, meter->selected_units);",
):
    require_literal(auto_run_render, literal, f"Auto run-screen contract changed: {literal}")
require_literal(
    board,
    "case DURA_UI_AUTO_BATCH_RUN: render_batch_run(&meter, true); break;",
    "Auto run state must use the reference run renderer",
)

# Composition 6: Manual run state, including the approved latest correction.
manual = extract_function(board, "render_manual_run")
for literal in (
    'dura_lcd_draw_bitmap_1bpp(0, 0, dura_asset_durametertitle_bmp_glcd_bmp, 128, 8, true);',
    "draw_num40_right(128, 12, amount);",
    "draw_legacy_text_aligned('L', 0, 42, power, false);",
    "draw_legacy_text_aligned('R', 128, 42, unit_label(meter->selected_units), false);",
    'draw_legacy_buttons("Home", "Reset", "Stop", "Start");',
):
    require_literal(manual, literal, f"Manual-screen contract changed: {literal}")
assert "draw_num24" not in manual, "Manual amount regressed to the smaller 24pt renderer"
num40_right = extract_function(board, "draw_num40_right")
require_literal(num40_right, "right_x - width", "Manual amount must compute its origin from the right edge")

# Button routing from all six photographed states.
button_router = extract_function(board, "dura_board_handle_button")
for literal in (
    "s_ui_screen = DURA_UI_MANUAL;",
    "s_ui_screen = DURA_UI_CAL_START;",
    "s_ui_screen = DURA_UI_RECIRC_EDIT;",
    "s_ui_screen = DURA_UI_AUTO_BATCH_EDIT;",
    "dura_meter_start_manual();",
    "dura_meter_stop_manual();",
    "dura_meter_start_batch_mode(s_ui_batch_amount, DURA_OPERATION_RECIRC)",
    "dura_meter_start_batch(s_ui_batch_amount)",
    "dura_meter_pause_batch();",
    "dura_meter_resume_batch();",
    "dura_meter_start_calibration()",
): 
    require_literal(button_router, literal, f"feedback-state button routing changed: {literal}")

# Separate yellow Recirc input is accepted only from Main Menu.
external_recirc = extract_function(board, "handle_external_recirc_press")
for literal in (
    "if (s_ui_screen != DURA_UI_MAIN_MENU) return;",
    "s_ui_batch_amount = DURA_RECIRC_BATCH_DEFAULT_GAL;",
    "s_ui_screen = DURA_UI_RECIRC_EDIT;",
):
    require_literal(external_recirc, literal, f"external Recirc Main-Menu gate changed: {literal}")

# Button-wake implementation iterates the complete four-entry physical table.
for literal in (
    '{DURA_BUTTON_UP, DURA_GPIO_KEY_4, "button1_left", NULL}',
    '{DURA_BUTTON_DOWN, DURA_GPIO_KEY_3, "button2", NULL}',
    '{DURA_BUTTON_SELECT, DURA_GPIO_KEY_2, "button3", NULL}',
    '{DURA_BUTTON_BACK, DURA_GPIO_KEY_1, "button4_right", NULL}',
):
    require_literal(board, literal, f"physical button table changed: {literal}")
button_wake = extract_function(board, "dura_board_button_wake_mask")
require_literal(button_wake, "i < sizeof(s_buttons) / sizeof(s_buttons[0])", "wake mask must traverse every physical button")
require_literal(button_wake, "mask |= (1ULL << s_buttons[i].gpio);", "wake mask must include each button GPIO")
deep_sleep = extract_function(board, "dura_board_enter_deep_sleep")
require_literal(deep_sleep, "prepare_deep_sleep_inputs()", "sleep must use the board's existing wake preparation")
require_literal(deep_sleep, "dura_meter_cancel_sleep_prepare()", "failed board preparation must release the activation fence")
require_literal(extract_function(board, "prepare_deep_sleep_inputs"), "const uint64_t button_mask = dura_board_button_wake_mask();", "deep sleep must consume the four-button wake mask")

# Verified runtime state transitions behind Man/Calibr/Recirc/Auto/Start/Stop.
for name, required in {
    "dura_meter_start_manual": ("DURA_OPERATION_MANUAL", "set_pump_locked(true)"),
    "dura_meter_stop_manual": ("set_pump_locked(false)", "DURA_OPERATION_IDLE"),
    "dura_meter_start_batch": ("DURA_OPERATION_AUTO", "dura_meter_start_batch_mode("),
    "dura_meter_start_batch_mode": ("start_batch(amount_in_selected_units, operation_mode, 0)",),
    "dura_meter_pause_batch": ("set_pump_locked(false)", "DURA_BATCH_PAUSED"),
    "dura_meter_resume_batch": ("DURA_BATCH_PAUSED", "finish_batch_activation(activation, true)"),
    "dura_meter_cancel_batch": ("set_pump_locked(false)", "DURA_OPERATION_IDLE"),
    "dura_meter_start_calibration": ("DURA_OPERATION_CALIBRATION", "set_pump_locked(true)"),
    "dura_meter_cancel_calibration": ("set_pump_locked(false)", "DURA_OPERATION_IDLE"),
}.items():
    body = extract_function(meter, name)
    for literal in required:
        require_literal(body, literal, f"{name} transition contract changed: {literal}")

activation = extract_function(meter, "finish_batch_activation")
assert activation.index("dura_meter_save()") < activation.index("set_pump_locked(true)")
for guard in ("s_meter.activation_pending", "s_meter.activation_id != activation_id", "saved != ESP_OK"):
    require_literal(activation, guard, "batch activation lost a persistence/cancellation guard")

# Full-function PCB832 revX3 baseline. Physical output acceptance is separate.
for setting in (
    "CONFIG_DURA_BOARD_ENABLE_PUMP_OUTPUT=y",
    "CONFIG_DURA_BOARD_GPIO_PUMP_OUTPUT=47",
    "CONFIG_DURA_BOARD_ENABLE_EV_OUTPUTS=y",
    "CONFIG_DURA_BOARD_ENABLE_RECIRC_INPUT=y",
):
    require_regex(defaults, rf"^{re.escape(setting)}$", f"compiled baseline missing {setting}")
for disabled in (
    "CONFIG_DURA_BOARD_PUMP_OUTPUT_ACTIVE_LOW=y",
):
    assert not re.search(rf"^{re.escape(disabled)}$", defaults, re.MULTILINE), (
        f"compiled baseline must not contain {disabled}"
    )

# Execute the pure output policy so labels/modes are backed by behavior, not names alone.
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include "dura_control_policy.h"

static void check(dura_operation_mode_t mode, bool enabled,
                  bool pump, bool recirc, bool inject)
{
    const dura_output_intent_t out = dura_control_output_intent(mode, enabled);
    assert(out.pump == pump);
    assert(out.recirc_ev == recirc);
    assert(out.inject_ev == inject);
}

int main(void)
{
    check(DURA_OPERATION_IDLE, false, false, false, false);
    check(DURA_OPERATION_MANUAL, true, true, false, true);
    check(DURA_OPERATION_CALIBRATION, true, true, true, false);
    check(DURA_OPERATION_RECIRC, true, true, true, false);
    check(DURA_OPERATION_AUTO, true, true, false, true);
    check(DURA_OPERATION_MANUAL, false, false, false, false);
    check(DURA_OPERATION_CALIBRATION, false, false, false, false);
    check(DURA_OPERATION_RECIRC, false, false, false, false);
    check(DURA_OPERATION_AUTO, false, false, false, false);
    puts("LATEST_FEEDBACK_OUTPUT_POLICY_PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory() as temp_dir:
    temp = Path(temp_dir)
    harness_path = temp / "latest_feedback_output_policy.c"
    executable = temp / "latest_feedback_output_policy"
    harness_path.write_text(harness, encoding="utf-8")
    subprocess.run(
        [
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            f"-I{policy_header.parent}", str(policy_path), str(harness_path),
            "-o", str(executable),
        ],
        check=True,
    )
    result = subprocess.run([str(executable)], check=True, capture_output=True, text=True)
    assert result.stdout.strip() == "LATEST_FEEDBACK_OUTPUT_POLICY_PASS"

print("LATEST_FEEDBACK_UI_CONTRACT_PASS")
