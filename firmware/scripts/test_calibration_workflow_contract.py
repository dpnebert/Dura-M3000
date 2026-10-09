#!/usr/bin/env python3
"""Focused source/host contract for the AVR-equivalent bucket calibration flow."""
import hashlib
import re
import subprocess
import tempfile
from pathlib import Path

project = Path(__file__).resolve().parents[1]
board_source = (project / "components/dura_board/dura_board.c").read_text(encoding="utf-8")
assets_source = (project / "components/dura_board/dura_lcd_assets.c").read_text(encoding="utf-8")
assets_header = (project / "components/dura_board/include/dura_lcd_assets.h").read_text(encoding="utf-8")
meter_source = (project / "components/dura_meter/dura_meter.c").read_text(encoding="utf-8")
meter_header = (project / "components/dura_meter/include/dura_meter.h").read_text(encoding="utf-8")


def extract_braced(text: str, start: int) -> str:
    brace = text.find("{", start)
    assert brace >= 0, "opening brace not found"
    depth = 0
    in_string = in_char = escaped = line_comment = block_comment = False
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
                return text[start : i + 1]
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
        elif depth == 0 and re.match(r"\s*(?:case\s+\w+\s*:|default\s*:)", function[i:]):
            return function[start:i]
        i += 1
    return function[start:i]


def check(condition: bool, message: str, failures: list[str]) -> None:
    if not condition:
        failures.append(message)


failures: list[str] = []
handle_button = extract_function(board_source, "dura_board_handle_button")
cal_start = extract_switch_case(handle_button, "DURA_UI_CAL_START")
# The gallon/liter labels are stacked; the second label owns the shared body.
bucket = extract_switch_case(handle_button, "DURA_UI_BUCKET_LITER")
help_case = extract_switch_case(handle_button, "DURA_UI_CAL_HELP_LITER")
adjust = extract_switch_case(handle_button, "DURA_UI_CAL_ADJUST")
save_2 = extract_switch_case(handle_button, "DURA_UI_CAL_SAVE_2")
render_debug = extract_function(board_source, "dura_board_render_debug_screen")
render_bucket = extract_function(board_source, "render_cal_bucket")
cancel_helper_name = "ui_cancel_calibration_to_start"
cancel_helper_match = re.search(
    rf"\b{cancel_helper_name}\s*\([^;{{}}]*\)\s*\{{", board_source)
check(cancel_helper_match is not None,
      "missing common production calibration-cancel helper", failures)

# Cal is the one start action; entering the bucket is contingent on start success.
check("dura_meter_start_calibration() == ESP_OK" in cal_start,
      "Cal does not start meter calibration before bucket entry", failures)
check(cal_start.find("dura_meter_start_calibration()") < cal_start.find("DURA_UI_BUCKET_"),
      "bucket entry is not ordered after the calibration start call", failures)

# Exact approved calibration-progress composition remains binding.
normalized_render = re.sub(r"\s+", " ", render_bucket)
for literal in (
    'draw_legacy_text_aligned(\'C\', 64, 1, "CAL: Fill A Container", false);',
    'draw_legacy_text_aligned(\'C\', 64, 12, "Minimum Calibration Amount", false);',
    'draw_calibration_wheel(27, 36, meter->calibration_counts);',
    'draw_num24_right(104, 25, percent);',
    'draw_legacy_text_aligned(\'L\', 108, 34, "%", false);',
    'draw_legacy_buttons("Cancel", "Help", "Reset", "Cont");',
):
    check(literal in render_bucket,
          f"approved calibration composition changed: {literal}", failures)
check('snprintf(percent, sizeof(percent), "%u",' in normalized_render and
      "calibration_progress_percent(meter->calibration_counts)" in normalized_render,
      "percentage is not formatted from threshold-bound calibration progress", failures)
for obsolete in ("Fill a calibrated container", "Press 'Cont' when done.",
                 "calibration_progress_fill", "dura_lcd_draw_rect(14, 26",
                 "dura_lcd_fill_rect(16, 28"):
    check(obsolete not in render_bucket,
          f"obsolete calibration progress composition remains: {obsolete}", failures)

check("#define DURA_MIN_FLOW_COUNTS       250U" in meter_header,
      "minimum-flow threshold is not exported by the meter contract", failures)
check("#define DURA_MIN_FLOW_COUNTS" not in meter_source,
      "meter source still owns a private duplicate of the calibration threshold", failures)

check("extern const uint8_t dura_asset_calibration_wheel_frames[8][69];" in assets_header,
      "calibration wheel frame contract missing from asset header", failures)
wheel_assets = re.search(
    r"const\s+uint8_t\s+dura_asset_calibration_wheel_frames\[8\]\[69\]\s*=\s*\{(.*?)\};",
    assets_source, re.DOTALL)
check(wheel_assets is not None, "eight 23x23 calibration wheel frames missing", failures)
if wheel_assets is not None:
    wheel_bytes = bytes(int(value, 16) for value in
                        re.findall(r"0x([0-9A-Fa-f]{2})", wheel_assets.group(1)))
    check(len(wheel_bytes) == 8 * 69,
          "calibration wheel frame byte count is not 8 x 69", failures)
    check(hashlib.sha256(wheel_bytes).hexdigest() ==
          "466ded37a15339ea145d95b0429d9c760ad14c1b4b447cbddb38a7f4a7ff186f",
          "calibration wheel pixels differ from the approved proposal", failures)

# Bucket, measured-amount, and second-save Cancel keys are source-bound to the
# one production helper so no screen can drift to a partial cancellation path.
normalized_bucket = re.sub(r"\s+", " ", bucket)
normalized_adjust = re.sub(r"\s+", " ", adjust)
normalized_save_2 = re.sub(r"\s+", " ", save_2)
check(bool(re.search(
    rf"if \(b == DURA_BUTTON_UP\) \{{ {cancel_helper_name}\(\); \}}",
    normalized_bucket)),
    "bucket Cancel is not UP -> common cancel helper", failures)
check(bool(re.search(
    r"else if \(b == DURA_BUTTON_DOWN\) \{ s_ui_help_page = 0u; s_ui_screen = "
    r"\(s_ui_screen == DURA_UI_BUCKET_LITER\) \? DURA_UI_CAL_HELP_LITER : DURA_UI_CAL_HELP_GAL; \}",
    normalized_bucket)), "bucket Help is not bound to DOWN", failures)
check("else if (b == DURA_BUTTON_SELECT) { (void)dura_meter_reset_calibration(); }" in normalized_bucket,
      "bucket Reset is not bound to SELECT", failures)
check("else if (b == DURA_BUTTON_BACK) { (void)dura_meter_continue_calibration(); }" in normalized_bucket,
      "bucket Cont is not bound to BACK", failures)
check("dura_meter_start_calibration" not in bucket,
      "Cont still invokes start calibration", failures)
check("dura_meter_stop_calibration" not in bucket,
      "Cancel/Reset still overload stop calibration", failures)
check(bool(re.search(
    rf"if \(b == DURA_BUTTON_UP\) \{{ {cancel_helper_name}\(\); \}}",
    normalized_adjust)),
    "measured-amount Cancel is not UP -> common cancel helper", failures)
check(bool(re.search(
    r"else if \(b == DURA_BUTTON_DOWN && s_ui_cal_measured_amount > 0\.01f\) \{ "
    r"s_ui_cal_measured_amount -= .*?; \}", normalized_adjust)),
    "measured-amount decrement is not bound to DOWN", failures)
check(bool(re.search(
    r"else if \(b == DURA_BUTTON_SELECT\) \{ s_ui_cal_measured_amount \+= .*?; \}",
    normalized_adjust)), "measured-amount increment is not bound to SELECT", failures)
check(bool(re.search(
    r"else if \(b == DURA_BUTTON_BACK && dura_meter_accept_calibration_amount"
    r"\(s_ui_cal_measured_amount\) == ESP_OK\) \{ .*?s_ui_screen = DURA_UI_CAL_SAVE_1; \}",
    normalized_adjust)), "measured-amount Cont is not bound to BACK/success", failures)
check(bool(re.search(
    rf"if \(b == DURA_BUTTON_BACK\) \{{ {cancel_helper_name}\(\); \}}",
    normalized_save_2)),
    "second-save Cancel is not BACK -> common cancel helper", failures)
for case_name, case_source in (("bucket", bucket), ("measured-amount", adjust),
                               ("second-save", save_2)):
    check("dura_meter_cancel_calibration" not in case_source,
          f"{case_name} Cancel bypasses the common helper", failures)

cancel_meter = extract_function(meter_source, "dura_meter_cancel_calibration")
check("DURA_CAL_WAIT_SAVE" in cancel_meter,
      "meter cancellation does not accept WAIT_SAVE", failures)

# Help is a view over the active run, not a state transition or second start.
check("dura_meter_start_calibration" not in help_case,
      "Help return starts calibration a second time", failures)
check("DURA_UI_CAL_HELP_GAL" in render_debug and "DURA_UI_CAL_HELP_LITER" in render_debug,
      "render synchronization does not exempt active calibration Help", failures)
check(bool(re.search(
    r"calibration_mode\s*==\s*DURA_CAL_RUNNING.*?"
    r"s_ui_screen\s*!=\s*DURA_UI_CAL_HELP_GAL.*?"
    r"s_ui_screen\s*!=\s*DURA_UI_CAL_HELP_LITER",
    render_debug, re.DOTALL)), "active calibration render synchronization overwrites Help", failures)

# Dedicated APIs are public while start/stop remain available for protocol callers.
for api in (
    "dura_meter_start_calibration", "dura_meter_stop_calibration",
    "dura_meter_continue_calibration", "dura_meter_reset_calibration",
    "dura_meter_cancel_calibration",
):
    check(bool(re.search(rf"\besp_err_t\s+{api}\s*\(\s*void\s*\)\s*;", meter_header)),
          f"missing public calibration API {api}", failures)

# Calibration pulses are accepted immediately whenever the meter is RUNNING.
record_pulse_api = extract_function(meter_source, "dura_meter_record_pulse")
check("record_pulse_locked(count)" in record_pulse_api,
      "public pulse API must use the shared attribution core", failures)
record_pulse = extract_function(meter_source, "record_pulse_locked")
check("s_meter.screen == DURA_SCREEN_CALIBRATION" in record_pulse and
      "s_meter.calibration_mode == DURA_CAL_RUNNING" in record_pulse and
      "s_meter.calibration_counts + count" in record_pulse,
      "RUNNING calibration pulses are not recorded immediately", failures)

# Preserve the reviewed cross-interlocked/overlap-compatible qualifier byte-for-byte.
qualifier = extract_function(board_source, "flow_event_counts")
check(hashlib.sha256(qualifier.encode()).hexdigest() ==
      "162e9ccd30db4a01141424d2a73ef42e62566da85b8fbbe600943dcab7e5257c",
      "reviewed reed qualifier bytes changed", failures)

required_functions = (
    (board_source, "calibration_progress_percent"),
    (meter_source, "dura_meter_start_calibration"),
    (meter_source, "dura_meter_stop_calibration"),
    (meter_source, "dura_meter_continue_calibration"),
    (meter_source, "dura_meter_reset_calibration"),
    (meter_source, "dura_meter_cancel_calibration"),
)
if failures:
    for failure in failures:
        print(f"CALIBRATION_WORKFLOW_CONTRACT_FAIL: {failure}")
    raise SystemExit(1)

# Compile and execute the extracted production mappings. Percentage is tied to
# the meter's acceptance threshold and saturates there; wheel phase changes only
# when the count changes and wraps through the eight approved frames.
progress = extract_function(board_source, "calibration_progress_percent")
wheel_phase = extract_function(board_source, "calibration_wheel_phase")
progress_harness = f'''\
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define DURA_MIN_FLOW_COUNTS 250U
static uint8_t {progress}
static uint8_t {wheel_phase}
static void expect_percent(uint32_t count, uint8_t expected) {{
    uint8_t actual = calibration_progress_percent(count);
    if (actual != expected || actual > 100u) {{
        fprintf(stderr, "count %u -> %u, expected %u\\n", count, actual, expected);
        exit(1);
    }}
}}
static void expect_phase(uint32_t count, uint8_t expected) {{
    uint8_t actual = calibration_wheel_phase(count);
    if (actual != expected || actual > 7u) {{
        fprintf(stderr, "phase %u -> %u, expected %u\\n", count, actual, expected);
        exit(1);
    }}
}}
int main(void) {{
    expect_percent(0u, 0u); expect_percent(1u, 0u); expect_percent(2u, 0u);
    expect_percent(3u, 1u); expect_percent(125u, 50u); expect_percent(160u, 64u);
    expect_percent(249u, 99u); expect_percent(250u, 100u);
    expect_percent(251u, 100u); expect_percent(UINT32_MAX, 100u);
    uint8_t prior = calibration_progress_percent(0u);
    for (uint32_t count = 1u; count <= DURA_MIN_FLOW_COUNTS; ++count) {{
        uint8_t percent = calibration_progress_percent(count);
        if (percent < prior || percent > 100u) return 2;
        prior = percent;
    }}
    expect_phase(0u, 0u); expect_phase(1u, 1u); expect_phase(7u, 7u);
    expect_phase(8u, 0u); expect_phase(9u, 1u); expect_phase(UINT32_MAX, 7u);
    return 0;
}}
'''

# Compile and execute the exact production helper against success/failure stubs.
# A failed meter cancellation must preserve both board-level UI fields.
cancel_helper = extract_function(board_source, cancel_helper_name)
helper_harness = f'''\
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 0x103
typedef enum {{ DURA_UI_CAL_START, DURA_UI_CAL_ADJUST, DURA_UI_CAL_SAVE_2 }} dura_ui_screen_t;
static dura_ui_screen_t s_ui_screen;
static bool s_ui_cal_measured_amount_valid;
static esp_err_t stub_cancel_result;
static unsigned stub_cancel_calls;
static esp_err_t dura_meter_cancel_calibration(void) {{
    ++stub_cancel_calls;
    return stub_cancel_result;
}}
static void {cancel_helper}
static void require(bool condition, const char *message) {{
    if (!condition) {{ fprintf(stderr, "%s\\n", message); exit(1); }}
}}
int main(void) {{
    s_ui_screen = DURA_UI_CAL_ADJUST;
    s_ui_cal_measured_amount_valid = true;
    stub_cancel_result = ESP_ERR_INVALID_STATE;
    ui_cancel_calibration_to_start();
    require(stub_cancel_calls == 1u, "helper did not call meter cancellation once");
    require(s_ui_screen == DURA_UI_CAL_ADJUST && s_ui_cal_measured_amount_valid,
            "helper changed UI state after failed cancellation");
    stub_cancel_result = ESP_OK;
    ui_cancel_calibration_to_start();
    require(stub_cancel_calls == 2u, "helper did not call meter cancellation on success");
    require(s_ui_screen == DURA_UI_CAL_START && !s_ui_cal_measured_amount_valid,
            "helper did not clear validity and return to start after success");
    return 0;
}}
'''

# Compile the exact production calibration operations against a minimal host
# state together with the exact production common helper, bucket/adjust/save-2 cases,
# and render-state synchronization. This directly exercises all four cancellation
# modes, UI success/failure behavior, pump/count/pending cleanup, committed
# coefficient preservation, and stop compatibility.
functions = "\n\n".join("esp_err_t " + extract_function(text, name)
                          for text, name in required_functions[1:])
bucket_case_start = handle_button.find("case DURA_UI_BUCKET_GAL:")
bucket_case_end = handle_button.find("case DURA_UI_CAL_HELP_GAL:", bucket_case_start)
bucket_cases = handle_button[bucket_case_start:bucket_case_end]
adjust_case = extract_switch_case(handle_button, "DURA_UI_CAL_ADJUST")
save_2_case = extract_switch_case(handle_button, "DURA_UI_CAL_SAVE_2")
sync_start = render_debug.find("if (meter.operation_mode == DURA_OPERATION_MANUAL")
sync_end = render_debug.find("if (should_show_provisioning_qr(&meter))", sync_start)
render_sync = render_debug[sync_start:sync_end]
check(bucket_case_start >= 0 and bucket_case_end > bucket_case_start,
      "production bucket button cases could not be extracted", failures)
check(sync_start >= 0 and sync_end > sync_start,
      "production render synchronization could not be extracted", failures)
meter_harness = f'''\
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 0x103
#define DURA_MIN_FLOW_COUNTS 250U
#define METER_LOCK() do {{}} while (0)
#define METER_UNLOCK() do {{}} while (0)
#define TAG "host"
typedef enum {{ DURA_SCREEN_HOME, DURA_SCREEN_CALIBRATION, DURA_SCREEN_BATCH }} dura_screen_t;
typedef enum {{ DURA_BATCH_IDLE, DURA_BATCH_EDIT, DURA_BATCH_RUNNING, DURA_BATCH_PAUSED, DURA_BATCH_DONE }} dura_batch_mode_t;
typedef enum {{ DURA_CAL_IDLE, DURA_CAL_RUNNING, DURA_CAL_WAIT_MEASURED_AMOUNT, DURA_CAL_WAIT_SAVE }} dura_calibration_mode_t;
typedef enum {{ DURA_OPERATION_IDLE, DURA_OPERATION_MANUAL, DURA_OPERATION_CALIBRATION,
                DURA_OPERATION_RECIRC, DURA_OPERATION_AUTO }} dura_operation_mode_t;
typedef enum {{ DURA_UNITS_GALLON, DURA_UNITS_LITER, DURA_UNITS_OUNCE }} dura_units_t;
typedef enum {{ DURA_BUTTON_NONE, DURA_BUTTON_UP, DURA_BUTTON_DOWN, DURA_BUTTON_SELECT, DURA_BUTTON_BACK }} dura_button_t;
typedef enum {{
    DURA_UI_HOME, DURA_UI_MANUAL, DURA_UI_RECIRC_RUN, DURA_UI_CAL_START,
    DURA_UI_BUCKET_GAL, DURA_UI_BUCKET_LITER,
    DURA_UI_CAL_HELP_GAL, DURA_UI_CAL_HELP_LITER, DURA_UI_CAL_ADJUST,
    DURA_UI_CAL_SAVE_1, DURA_UI_CAL_SAVE_2, DURA_UI_BATCH_SET,
    DURA_UI_BATCH_RUN, DURA_UI_AUTO_BATCH_RUN, DURA_UI_BATCH_COMPLETE
}} dura_ui_screen_t;
typedef struct {{ float gallon, liter, ounce; }} dura_cal_coefficients_t;
typedef struct {{
    dura_screen_t screen; dura_batch_mode_t batch_mode; dura_calibration_mode_t calibration_mode;
    dura_operation_mode_t operation_mode;
    bool pump_enabled; bool fault_latched; uint32_t calibration_counts;
    bool activation_pending; uint32_t recipe_owner_id;
    bool show_auto_batch; dura_units_t selected_units; dura_cal_coefficients_t pending_calibration;
    uint8_t pending_quick_reference;
    float working_coef_gal, working_coef_liter, working_coef_ounce;
}} host_meter_t;
static host_meter_t s_meter;
static bool s_load_in_progress, s_sleep_prepared;
static bool s_calibration_save_in_progress;
static uint64_t s_control_epoch;
static bool {extract_function(meter_source, "operation_busy_locked")}
static dura_ui_screen_t s_ui_screen;
static bool s_ui_cal_measured_amount_valid;
static float s_ui_cal_measured_amount;
static uint8_t s_ui_help_page;
static void host_log(const char *tag, const char *format, ...) {{ (void)tag; (void)format; }}
#define ESP_LOGW(...) host_log(__VA_ARGS__)
static const char *log_context_prefix(void) {{ return "host"; }}
static esp_err_t set_pump_locked(bool enabled) {{
    if (enabled && s_meter.fault_latched) return ESP_ERR_INVALID_STATE;
    if (enabled && s_meter.screen != DURA_SCREEN_BATCH && s_meter.screen != DURA_SCREEN_CALIBRATION)
        return ESP_ERR_INVALID_STATE;
    s_meter.pump_enabled = enabled;
    return ESP_OK;
}}
{functions}
static void {cancel_helper}
static esp_err_t dura_meter_accept_calibration_amount(float amount) {{ (void)amount; return ESP_ERR_INVALID_STATE; }}
static esp_err_t store_precal_fluid(uint8_t profile) {{ (void)profile; return ESP_OK; }}
static void handle_cal_button(dura_button_t b) {{
    host_meter_t meter = s_meter;
    switch (s_ui_screen) {{
{bucket_cases}
{adjust_case}
{save_2_case}
    default: break;
    }}
}}
static void synchronize_render_screen(void) {{
    host_meter_t meter = s_meter;
{render_sync}
}}
static void require(bool condition, const char *message) {{
    if (!condition) {{ fprintf(stderr, "%s\\n", message); exit(1); }}
}}
static void require_cancelled(const char *message) {{
    require(s_meter.calibration_mode == DURA_CAL_IDLE && !s_meter.pump_enabled &&
            s_meter.calibration_counts == 0u && s_meter.pending_calibration.gallon == 0.0f &&
            s_meter.pending_calibration.liter == 0.0f && s_meter.pending_calibration.ounce == 0.0f,
            message);
    require(s_meter.working_coef_gal == 11.0f && s_meter.working_coef_liter == 22.0f &&
            s_meter.working_coef_ounce == 33.0f,
            "cancellation changed committed coefficients");
}}
int main(void) {{
    s_meter.batch_mode = DURA_BATCH_IDLE;
    s_meter.working_coef_gal = 11.0f;
    s_meter.working_coef_liter = 22.0f;
    s_meter.working_coef_ounce = 33.0f;

    /* IDLE is the only rejected cancellation mode and must preserve meter state. */
    s_meter.screen = DURA_SCREEN_HOME;
    s_meter.calibration_mode = DURA_CAL_IDLE;
    s_meter.calibration_counts = 77u;
    s_meter.pending_calibration.gallon = 1.0f;
    require(dura_meter_cancel_calibration() == ESP_ERR_INVALID_STATE,
            "IDLE cancellation was not rejected");
    require(s_meter.screen == DURA_SCREEN_HOME && s_meter.calibration_mode == DURA_CAL_IDLE &&
            s_meter.calibration_counts == 77u && s_meter.pending_calibration.gallon == 1.0f,
            "rejected IDLE cancellation changed meter state");

    /* RUNNING cancellation is exercised through the exact bucket case/helper. */
    s_meter.calibration_counts = 0u;
    s_meter.pending_calibration.gallon = 0.0f;
    require(dura_meter_start_calibration() == ESP_OK, "start failed");
    require(s_meter.calibration_mode == DURA_CAL_RUNNING && s_meter.pump_enabled &&
            s_meter.calibration_counts == 0u,
            "start did not enter RUNNING/pump-on/count-zero");
    s_meter.calibration_counts = 12u;
    require(dura_meter_continue_calibration() == ESP_OK, "insufficient Cont failed");
    require(s_meter.calibration_mode == DURA_CAL_RUNNING && s_meter.pump_enabled &&
            s_meter.calibration_counts == 0u,
            "insufficient Cont did not restart the fill");
    s_meter.calibration_counts = 17u;
    require(dura_meter_reset_calibration() == ESP_OK, "Reset failed");
    require(s_meter.calibration_mode == DURA_CAL_RUNNING && s_meter.pump_enabled &&
            s_meter.calibration_counts == 0u,
            "Reset did not preserve RUNNING/pump-on");
    s_meter.calibration_counts = 999u;
    s_meter.pending_calibration.gallon = 1.0f;
    s_meter.pending_calibration.liter = 2.0f;
    s_meter.pending_calibration.ounce = 3.0f;
    s_ui_screen = DURA_UI_BUCKET_GAL;
    handle_cal_button(DURA_BUTTON_UP);
    require_cancelled("RUNNING bucket Cancel did not discard calibration");
    require(s_ui_screen == DURA_UI_CAL_START && !s_ui_cal_measured_amount_valid,
            "RUNNING bucket Cancel did not return UI to start");

    /* WAIT_MEASURED_AMOUNT cancellation is exercised through adjustment Cancel. */
    require(dura_meter_start_calibration() == ESP_OK, "restart for measured Cancel failed");
    s_meter.calibration_counts = DURA_MIN_FLOW_COUNTS;
    require(dura_meter_continue_calibration() == ESP_OK, "sufficient Cont failed");
    require(s_meter.calibration_mode == DURA_CAL_WAIT_MEASURED_AMOUNT && !s_meter.pump_enabled &&
            s_meter.calibration_counts == DURA_MIN_FLOW_COUNTS,
            "sufficient Cont did not advance with count preserved");
    s_ui_screen = DURA_UI_BUCKET_LITER;
    synchronize_render_screen();
    require(s_ui_screen == DURA_UI_CAL_ADJUST && s_ui_cal_measured_amount_valid,
            "WAIT_MEASURED_AMOUNT did not synchronize to adjustment");
    s_meter.pending_calibration.gallon = 4.0f;
    handle_cal_button(DURA_BUTTON_UP);
    require_cancelled("WAIT_MEASURED_AMOUNT Cancel did not discard calibration");
    require(s_ui_screen == DURA_UI_CAL_START && !s_ui_cal_measured_amount_valid,
            "measured-amount Cancel did not return UI to start");
    synchronize_render_screen();
    require(s_ui_screen == DURA_UI_CAL_START,
            "render synchronization did not preserve start after measured Cancel");

    /* WAIT_SAVE cancellation is exercised through the exact second-save Cancel. */
    s_meter.calibration_mode = DURA_CAL_WAIT_SAVE;
    s_meter.screen = DURA_SCREEN_CALIBRATION;
    s_meter.calibration_counts = DURA_MIN_FLOW_COUNTS;
    s_meter.pending_calibration.gallon = 5.0f;
    s_meter.pending_calibration.liter = 6.0f;
    s_meter.pending_calibration.ounce = 7.0f;
    s_ui_screen = DURA_UI_CAL_SAVE_2;
    s_ui_cal_measured_amount_valid = true;
    handle_cal_button(DURA_BUTTON_BACK);
    require_cancelled("WAIT_SAVE second-save Cancel did not discard calibration");
    require(s_ui_screen == DURA_UI_CAL_START && !s_ui_cal_measured_amount_valid,
            "second-save Cancel did not return UI to start");

    /* Each source-bound case preserves its board state when meter cancel rejects IDLE. */
    s_ui_screen = DURA_UI_BUCKET_GAL;
    handle_cal_button(DURA_BUTTON_UP);
    require(s_ui_screen == DURA_UI_BUCKET_GAL, "failed bucket Cancel changed UI state");
    s_ui_screen = DURA_UI_CAL_ADJUST;
    s_ui_cal_measured_amount_valid = true;
    handle_cal_button(DURA_BUTTON_UP);
    require(s_ui_screen == DURA_UI_CAL_ADJUST && s_ui_cal_measured_amount_valid,
            "failed measured-amount Cancel changed UI state");
    s_ui_screen = DURA_UI_CAL_SAVE_2;
    handle_cal_button(DURA_BUTTON_BACK);
    require(s_ui_screen == DURA_UI_CAL_SAVE_2 && s_ui_cal_measured_amount_valid,
            "failed second-save Cancel changed UI state");

    s_meter.calibration_mode = DURA_CAL_RUNNING;
    s_meter.pump_enabled = true;
    s_meter.calibration_counts = DURA_MIN_FLOW_COUNTS;
    require(dura_meter_stop_calibration() == ESP_OK &&
            s_meter.calibration_mode == DURA_CAL_WAIT_MEASURED_AMOUNT,
            "public stop calibration compatibility changed");
    return 0;
}}
'''

with tempfile.TemporaryDirectory() as tmp:
    tmp_path = Path(tmp)
    for name, source in (("progress", progress_harness), ("helper", helper_harness),
                         ("meter", meter_harness)):
        c_path = tmp_path / f"{name}.c"
        binary = tmp_path / name
        c_path.write_text(source, encoding="utf-8")
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(c_path), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

print("CALIBRATION_WORKFLOW_CONTRACT_PASS")
