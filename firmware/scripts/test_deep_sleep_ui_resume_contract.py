#!/usr/bin/env python3
"""Static regression gate for cold-boot versus deep-sleep UI behavior.

Cold power-up/reset must retain the accepted logo -> Powering up -> UI sequence.
A deep-sleep wake must instead redraw the retained safe UI screen immediately and
must not route the wake-causing button press into the restored screen.
"""
import re
from pathlib import Path

project = Path(__file__).resolve().parents[1]
board = (project / "components/dura_board/dura_board.c").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def extract_braced(text: str, start: int) -> str:
    brace = text.find("{", start)
    require(brace >= 0, "opening brace not found")
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start:index + 1]
    raise AssertionError("unterminated braced block")


def extract_function(text: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;{{}}]*\)\s*\{{", text)
    require(match is not None, f"missing function {name}")
    return extract_braced(text, match.start())


# Logical UI/navigation state that changes what the user sees must live in the
# RTC-retained data section rather than ordinary DRAM.
for declaration in (
    r"RTC_DATA_ATTR\s+static\s+bool\s+s_provisioning_qr_dismissed\s*;",
    r"RTC_DATA_ATTR\s+static\s+dura_ui_screen_t\s+s_ui_screen\s*=\s*DURA_UI_MAIN_MENU\s*;",
    r"RTC_DATA_ATTR\s+static\s+uint8_t\s+s_ui_setup_item\s*;",
    r"RTC_DATA_ATTR\s+static\s+uint8_t\s+s_ui_reset_item\s*;",
    r"RTC_DATA_ATTR\s+static\s+uint8_t\s+s_ui_help_page\s*;",
    r"RTC_DATA_ATTR\s+static\s+uint8_t\s+s_ui_info_page\s*;",
    r"RTC_DATA_ATTR\s+static\s+float\s+s_ui_batch_amount\s*=\s*DURA_AUTO_BATCH_DEFAULT_GAL\s*;",
    r"RTC_DATA_ATTR\s+static\s+float\s+s_ui_cal_measured_amount\s*;",
    r"RTC_DATA_ATTR\s+static\s+bool\s+s_ui_cal_measured_amount_valid\s*;",
    r"RTC_DATA_ATTR\s+static\s+uint8_t\s+s_ui_quick_ref\s*=\s*17u\s*;",
):
    require(re.search(declaration, board) is not None, f"UI state is not RTC-retained: {declaration}")

init = extract_function(board, "dura_board_init")
require(
    re.search(
        r"const\s+bool\s+deep_sleep_wake\s*=\s*"
        r"esp_sleep_get_wakeup_cause\s*\(\s*\)\s*!=\s*ESP_SLEEP_WAKEUP_UNDEFINED\s*;",
        init,
    ) is not None,
    "board initialization must distinguish deep-sleep wake from cold boot/reset",
)
require(
    re.search(r"s_ignore_buttons_until_release\s*=\s*deep_sleep_wake\s*;", init) is not None,
    "deep-sleep wake must arm wake-button suppression",
)

cold_guard = re.search(r"if\s*\(\s*!deep_sleep_wake\s*\)\s*\{", init)
require(cold_guard is not None, "cold-start presentation must be guarded from deep-sleep wake")
cold_block = extract_braced(init, cold_guard.start())
for operation in (
    "dura_lcd_draw_splash()",
    "vTaskDelay(pdMS_TO_TICKS(DURA_BOOT_LOGO_HOLD_MS))",
    "draw_boot_powering_up_popup()",
    "vTaskDelay(pdMS_TO_TICKS(DURA_BOOT_POWERING_UP_HOLD_MS))",
):
    require(operation in cold_block, f"cold-start sequence missing: {operation}")
require(
    "dura_board_render_debug_screen()" not in cold_block,
    "normal UI redraw must occur after both cold-start and deep-sleep paths",
)
require(
    init.find("dura_board_render_debug_screen()", cold_guard.end() + len(cold_block)) >= 0,
    "retained UI must be redrawn after the cold-start-only block",
)

board_task = extract_function(board, "board_task")
ignore_guard = re.search(r"if\s*\(\s*s_ignore_buttons_until_release\s*\)\s*\{", board_task)
require(ignore_guard is not None, "board task must suppress the wake-causing held button")
ignore_block = extract_braced(board_task, ignore_guard.start())
require(
    re.search(r"if\s*\(\s*b\s*==\s*DURA_BUTTON_NONE\s*\)", ignore_block) is not None,
    "wake-button suppression must remain active until every button is released",
)
require(
    "s_ignore_buttons_until_release = false;" in ignore_block,
    "wake-button suppression must clear after release",
)
require(
    "b = DURA_BUTTON_NONE;" in ignore_block,
    "the wake-causing button must not be dispatched to the restored screen",
)

print("DEEP_SLEEP_UI_RESUME_CONTRACT_PASS")
