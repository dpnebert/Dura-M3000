#!/usr/bin/env python3
"""Static regression for the customer-visible legacy batch UI contract.

This is a focused pre-build gate. It verifies the exact confirmed batch-screen
composition and key routing while hardware photographs remain the acceptance
boundary for glyph placement and visual parity.
"""
import re
import subprocess
import tempfile
from pathlib import Path

project = Path(__file__).resolve().parents[1]
board_source = (project / "components/dura_board/dura_board.c").read_text()
meter_source = (project / "components/dura_meter/dura_meter.c").read_text()
meter_header = (project / "components/dura_meter/include/dura_meter.h").read_text()


def extract_braced(text: str, start: int) -> str:
    brace = text.find("{", start)
    assert brace >= 0, "opening brace not found"
    depth = 0
    in_string = False
    in_char = False
    escaped = False
    line_comment = False
    block_comment = False
    i = brace
    while i < len(text):
        c = text[i]
        n = text[i + 1] if i + 1 < len(text) else ""
        if line_comment:
            if c == "\n":
                line_comment = False
        elif block_comment:
            if c == "*" and n == "/":
                block_comment = False
                i += 1
        elif in_string:
            if escaped:
                escaped = False
            elif c == "\\":
                escaped = True
            elif c == '"':
                in_string = False
        elif in_char:
            if escaped:
                escaped = False
            elif c == "\\":
                escaped = True
            elif c == "'":
                in_char = False
        elif c == "/" and n == "/":
            line_comment = True
            i += 1
        elif c == "/" and n == "*":
            block_comment = True
            i += 1
        elif c == '"':
            in_string = True
        elif c == "'":
            in_char = True
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
        i += 1
    raise AssertionError("unterminated braced block")


def extract_function(text: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;{{}}]*\)\s*\{{", text)
    assert match is not None, f"missing function {name}"
    return extract_braced(text, match.start())


def extract_switch_case(function: str, label: str) -> str:
    match = re.search(rf"\bcase\s+{label}\s*:", function)
    assert match is not None, f"missing case {label}"
    start = match.start()
    depth = 0
    i = match.end()
    while i < len(function):
        c = function[i]
        if c == "{":
            depth += 1
        elif c == "}":
            if depth == 0:
                break
            depth -= 1
        elif depth == 0:
            next_case = re.match(r"\s*(?:case\s+\w+\s*:|default\s*:)", function[i:])
            if next_case:
                return function[start:i]
        i += 1
    return function[start:i]


# Exact legacy/customer composition: two captions, large remaining amount,
# singular unit at the right edge, and Home / blank / Stop / Start softkeys.
render_run = extract_function(board_source, "render_batch_run")
for text in (
    '"Press Start to begin."',
    '"Press Stop to end."',
    '"Home"',
    '"Stop"',
    '"Start"',
    "meter->remaining_batch",
):
    assert text in render_run, f"batch run renderer missing {text}"
run_call = re.sub(r"\s+", " ", render_run)
assert re.search(
    r'draw_legacy_menu_screen\("Press Start to begin\.", "Press Stop to end\.", '
    r'NULL, NULL, NULL, auto_mode \? "Home" : "Stop", NULL, '
    r'auto_mode \? "Stop" : NULL, "Start"\);',
    run_call,
)
for diagnostic in ('"Batch Mode"', '"Dispensing"', '"Remaining ', '"Pump ON"', '"Pump OFF"'):
    assert diagnostic not in render_run, f"diagnostic composition remains in batch run renderer: {diagnostic}"

render_amount = extract_function(board_source, "render_legacy_batch_amount")
assert re.search(r"draw_num24_text\s*\(\s*2\s*,\s*26\s*,", render_amount)
assert re.search(
    r"draw_legacy_text_aligned\s*\(\s*'R'\s*,\s*128\s*,\s*40\s*,",
    render_amount,
)

unit_label = extract_function(board_source, "legacy_batch_unit_label")
for unit in ('"Gallon"', '"Litre"', '"Ounce"'):
    assert unit in unit_label, f"missing singular legacy unit {unit}"
for mapping in (
    r'case\s+DURA_UNITS_GALLON\s*:\s*return\s+"Gallon"',
    r'case\s+DURA_UNITS_LITER\s*:\s*return\s+"Litre"',
    r'case\s+DURA_UNITS_OUNCE\s*:\s*return\s+"Ounce"',
):
    assert re.search(mapping, unit_label)

formatter = extract_function(board_source, "format_legacy_batch_amount")
precision = extract_function(board_source, "volume_display_decimals")
# Compile the real dependency with normal SDK values; the separate matrix
# executes each per-unit override and exact cutoff neighbors.
precision_prelude = '\n'.join('#define ' + line.replace('=', ' ', 1)
    for line in (project / 'sdkconfig').read_text().splitlines()
    if line.startswith('CONFIG_DURA_BOARD_VOLUME_'))
precision_prelude += '\ntypedef enum { DURA_UNITS_GALLON, DURA_UNITS_LITER, DURA_UNITS_OUNCE } dura_units_t;\n'
precision_prelude += 'static int ' + precision + '\n'
formatter_harness = f'''\
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
{precision_prelude}
static void {formatter}
static void check(float value, dura_units_t units, const char *expected) {{
    char actual[24];
    format_legacy_batch_amount(actual, sizeof(actual), value, units);
    if (strcmp(actual, expected) != 0) {{
        fprintf(stderr, "%g -> %s, expected %s\\n", value, actual, expected);
        exit(1);
    }}
}}
int main(void) {{
    check(99.90f, DURA_UNITS_GALLON, "99.90");
    check(5.00f, DURA_UNITS_GALLON, "5.00");
    check(10.00f, DURA_UNITS_GALLON, "10.00");
    check(12.34f, DURA_UNITS_GALLON, "12.34");
    check(199.99f, DURA_UNITS_GALLON, "199.9");
    check(250.0f, DURA_UNITS_GALLON, "250.0");
    check(5.09f, DURA_UNITS_LITER, "5.09");
    /* Binary32 12.9 is below decimal 12.9: preserve running truncation. */
    check(12.9f, DURA_UNITS_OUNCE, "12.89");
    return 0;
}}
'''
with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "formatter.c"
    binary = Path(tmp) / "formatter"
    source.write_text(formatter_harness, encoding="utf-8")
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(source), "-lm", "-o", str(binary)],
        check=True,
    )
    subprocess.run([str(binary)], check=True)

# Starting a batch must preserve the operator-entered amount exactly until a
# qualified reed event decrements the remaining count.  Count quantization must
# not create a visible pre-flow decrement such as 10.0 -> 9.99.
remaining_display = extract_function(meter_source, "batch_remaining_display_value")
remaining_harness = f'''\
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static float {remaining_display}
static void check_close(const char *name, float actual, float expected) {{
    if (fabsf(actual - expected) > 0.00001f) {{
        fprintf(stderr, "%s: got %.8f expected %.8f\\n", name, actual, expected);
        exit(1);
    }}
}}
int main(void) {{
    const float coefficient = 0.00892857f;
    const uint32_t initial_counts = 1120u;
    check_close("no reed pulse", batch_remaining_display_value(10.0f, initial_counts,
                initial_counts, coefficient), 10.0f);
    check_close("first reed pulse", batch_remaining_display_value(10.0f, initial_counts,
                initial_counts - 1u, coefficient), coefficient * (float)(initial_counts - 1u));
    check_close("complete", batch_remaining_display_value(10.0f, initial_counts,
                0u, coefficient), 0.0f);
    return 0;
}}
'''
with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "batch_remaining.c"
    binary = Path(tmp) / "batch_remaining"
    source.write_text(remaining_harness, encoding="utf-8")
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(source), "-lm", "-o", str(binary)],
        check=True,
    )
    subprocess.run([str(binary)], check=True)

# Public entry points share the same persisted-OFF activation helper.
assert "dura_meter_start_batch_mode(amount_in_selected_units, DURA_OPERATION_AUTO)" in extract_function(meter_source, "dura_meter_start_batch")
assert "start_batch(amount_in_selected_units, operation_mode, 0)" in extract_function(meter_source, "dura_meter_start_batch_mode")
start_batch = extract_function(meter_source, "start_batch")
for marker in (
    "s_meter.batch_requested_amount = amount;",
    "s_meter.batch_initial_counts = counts;",
):
    assert marker in start_batch, f"batch start missing exact pre-flow display marker: {marker}"
get_snapshot = extract_function(meter_source, "dura_meter_get_snapshot")
assert "batch_remaining_display_value(s_meter.batch_requested_amount" in get_snapshot
record_pulse_api = extract_function(meter_source, "dura_meter_record_pulse")
assert "record_pulse_locked(count)" in record_pulse_api
record_pulse = extract_function(meter_source, "record_pulse_locked")
assert "s_meter.preset_batch_counts -= count;" in record_pulse

# Editable presets use the shared tenths-step adjuster and customer precision
# rule: per-unit SDK precision, capped at one above 199.9 by default. The host harness executes
# the extracted production helpers rather than a Python reimplementation.
adjuster = extract_function(board_source, "adjust_batch_amount_tenths")
editable_formatter = extract_function(board_source, "format_editable_batch_amount")
assert "lroundf" in adjuster
assert "amount_tenths" in adjuster
assert "0.1f" not in adjuster
assert re.search(r"amount_tenths\s*<\s*10", adjuster)
assert "volume_display_decimals(amount, units)" in editable_formatter
assert "amount > cutoff && decimals > 1 ? 1 : decimals" in precision
assert re.search(r'"%\.\*f"', editable_formatter)
adjuster_harness = f'''\
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
{precision_prelude}
static float {adjuster}
static void {editable_formatter}
static void check(float actual, float expected, const char *expected_text) {{
    char text[24];
    format_editable_batch_amount(text, sizeof(text), actual, DURA_UNITS_GALLON);
    if (actual != expected || strcmp(text, expected_text) != 0) {{
        fprintf(stderr, "%g -> %s, expected %g -> %s\\n",
                actual, text, expected, expected_text);
        exit(1);
    }}
}}
int main(void) {{
    check(adjust_batch_amount_tenths(10.0f, -1), 9.9f, "9.90");
    check(adjust_batch_amount_tenths(10.0f, 1), 10.1f, "10.10");
    check(adjust_batch_amount_tenths(1.0f, -1), 1.0f, "1.00");
    check(adjust_batch_amount_tenths(1.04f, -1), 1.0f, "1.00");
    check(adjust_batch_amount_tenths(250.0f, 1), 250.1f, "250.1");
    float amount = 10.0f;
    for (int i = 0; i < 10000; ++i) {{
        amount = adjust_batch_amount_tenths(amount, -1);
        amount = adjust_batch_amount_tenths(amount, 1);
    }}
    check(amount, 10.0f, "10.00");
    return 0;
}}
'''
with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "batch_adjuster.c"
    binary = Path(tmp) / "batch_adjuster"
    source.write_text(adjuster_harness, encoding="utf-8")
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(source), "-lm", "-o", str(binary)],
        check=True,
    )
    subprocess.run([str(binary)], check=True)

render_edit = extract_function(board_source, "render_batch_edit")
for text in (
    '"Enter the preset amount"',
    '"to be pumped."',
    '"Home"',
    '"Reset"',
    '"Start"',
):
    assert text in render_edit, f"batch edit renderer missing {text}"
assert "render_editable_batch_amount(s_ui_batch_amount" in render_edit
edit_call = re.sub(r"\s+", " ", render_edit)
assert re.search(
    r'draw_legacy_menu_screen\("Enter the preset amount", "to be pumped\.", '
    r'NULL, NULL, NULL, auto_mode \? "Home" : "Reset", "Dn\\x7f", "Up~", "Start"\);',
    edit_call,
)

render_debug = extract_function(board_source, "dura_board_render_debug_screen")
assert 'render_popup_confirm("BATCH PRESET", "FINISHED")' in render_debug
assert 'render_popup_confirm("FLOW DETECTION", "ERROR")' in render_debug
for mapping in (
    "case DURA_UI_BATCH_SET: render_batch_edit(&meter, false); break;",
    "case DURA_UI_BATCH_RUN: render_batch_run(&meter, false); break;",
    "case DURA_UI_AUTO_BATCH_EDIT: render_batch_edit(&meter, true); break;",
    "case DURA_UI_AUTO_BATCH_RUN: render_batch_run(&meter, true); break;",
):
    assert mapping in render_debug
assert re.search(
    r"meter\.batch_mode\s*==\s*DURA_BATCH_DONE.*?DURA_UI_BATCH_COMPLETE.*?"
    r"DURA_BATCH_RUNNING.*?DURA_BATCH_PAUSED.*?"
    r"meter\.show_auto_batch\s*\?\s*DURA_UI_AUTO_BATCH_RUN\s*:\s*DURA_UI_BATCH_RUN",
    render_debug,
    re.DOTALL,
)

# Home from Auto Batch Run must stop the pump and leave RUNNING/PAUSED state;
# otherwise the renderer's meter-state reconciliation immediately forces the
# screen back to Auto Batch Run. Stop pauses; Start resumes.
handle_button = extract_function(board_source, "dura_board_handle_button")
home_case = extract_switch_case(handle_button, "DURA_UI_HOME")
assert re.search(
    r"b\s*==\s*DURA_BUTTON_BACK.*?dura_meter_set_auto_batch\(true\)\s*==\s*ESP_OK.*?"
    r"s_ui_screen\s*=\s*DURA_UI_AUTO_BATCH_EDIT\s*;",
    home_case,
    re.DOTALL,
), "Home button 4 must enter Auto Batch mode and its edit screen as one transition"
assert "DURA_UI_BATCH_SET" not in home_case, \
    "Home button 4 must not enter the legacy switch-controlled manual Batch screen"
manual_edit_case = extract_switch_case(handle_button, "DURA_UI_BATCH_SET")
auto_edit_case = extract_switch_case(handle_button, "DURA_UI_AUTO_BATCH_EDIT")
for edit_case in (manual_edit_case, auto_edit_case):
    assert "adjust_batch_amount_tenths(s_ui_batch_amount, -1)" in edit_case
    assert "adjust_batch_amount_tenths(s_ui_batch_amount, 1)" in edit_case
    assert "s_ui_batch_amount -= 1.0f" not in edit_case
    assert "s_ui_batch_amount += 1.0f" not in edit_case
assert re.search(r"DURA_BUTTON_UP\)\s+s_ui_batch_amount\s*=\s*10\.0f", manual_edit_case)
assert "dura_meter_start_batch(s_ui_batch_amount)" in manual_edit_case
assert "dura_meter_start_batch(s_ui_batch_amount)" in auto_edit_case
auto_case = extract_switch_case(handle_button, "DURA_UI_AUTO_BATCH_RUN")
assert re.search(r"b\s*==\s*DURA_BUTTON_UP", auto_case)
assert "ui_cancel_batch_to_home()" in auto_case
assert re.search(
    r"b\s*==\s*DURA_BUTTON_SELECT.*?DURA_BATCH_RUNNING.*?dura_meter_pause_batch\(\)",
    auto_case,
    re.DOTALL,
)
assert re.search(
    r"b\s*==\s*DURA_BUTTON_BACK.*?DURA_BATCH_PAUSED.*?dura_meter_resume_batch\(\)",
    auto_case,
    re.DOTALL,
)
assert "DURA_BUTTON_DOWN" not in auto_case
# Home now applies to every state, not only RUNNING/PAUSED. The helper's
# safe-RAM fallback guards below retain rejection/persistence safety.
assert re.search(r"if\s*\(b == DURA_BUTTON_UP\)\s*\{\s*ui_cancel_batch_to_home\(\);", auto_case)

complete_case = extract_switch_case(handle_button, "DURA_UI_BATCH_COMPLETE")
assert "ui_cancel_batch_to_home()" in complete_case
assert re.search(
    r"if\s*\(\s*b\s*==\s*DURA_BUTTON_UP\s*\)\s*\{\s*"
    r"ui_cancel_batch_to_home\(\);",
    complete_case,
    re.DOTALL,
)

cancel_to_home = extract_function(board_source, "ui_cancel_batch_to_home")
assert "dura_meter_cancel_batch()" in cancel_to_home
assert "dura_meter_get_snapshot(&updated)" in cancel_to_home
fallback_start = cancel_to_home.index("if (dura_meter_get_snapshot(&updated)")
fallback_branch = extract_braced(cancel_to_home, fallback_start)
for guard in ("updated.batch_mode == DURA_BATCH_EDIT", "updated.batch_mode == DURA_BATCH_IDLE",
              "updated.operation_mode == DURA_OPERATION_IDLE", "updated.calibration_mode == DURA_CAL_IDLE",
              "!updated.pump_enabled", "updated.recipe_owner_id == 0"):
    assert guard in fallback_branch
assert "ui_go_main();" in fallback_branch
go_main = extract_function(board_source, "ui_go_main")
assert "dura_meter_set_screen_home()" in go_main
assert "s_ui_screen = DURA_UI_MAIN_MENU;" in go_main

# Preserve the approved calibration composition exactly: explicit minimum,
# count-driven wheel, threshold-bound percentage, and unchanged controls.
render_cal_bucket = extract_function(board_source, "render_cal_bucket")
calibration_call = re.sub(r"\s+", " ", render_cal_bucket)
for literal in (
    'draw_legacy_text_aligned(\'C\', 64, 1, "CAL: Fill A Container", false);',
    'draw_legacy_text_aligned(\'C\', 64, 12, "Minimum Calibration Amount", false);',
    'draw_calibration_wheel(27, 36, meter->calibration_counts);',
    'draw_num24_right(104, 25, percent);',
    'draw_legacy_text_aligned(\'L\', 108, 34, "%", false);',
    'draw_legacy_buttons("Cancel", "Help", "Reset", "Cont");',
):
    assert literal in render_cal_bucket
assert 'snprintf(percent, sizeof(percent), "%u",' in calibration_call
assert "calibration_progress_percent(meter->calibration_counts)" in calibration_call
assert "calibration_progress_fill" not in render_cal_bucket

assert re.search(r"\besp_err_t\s+dura_meter_cancel_batch\s*\(\s*void\s*\)\s*;", meter_header)
cancel_batch = extract_function(meter_source, "dura_meter_cancel_batch")
ordered_cancel_steps = (
    "set_pump_locked(false)",
    "s_meter.screen = DURA_SCREEN_BATCH;",
    "s_meter.batch_mode = DURA_BATCH_EDIT;",
    "s_meter.preset_batch_counts = 0;",
    "METER_UNLOCK();",
    "return dura_meter_save();",
)
cursor = -1
for step in ordered_cancel_steps:
    cursor = cancel_batch.find(step, cursor + 1)
    assert cursor >= 0, f"cancel-batch ordered step missing after prior step: {step}"

print("BATCH_UI_CONTRACT_PASS")
