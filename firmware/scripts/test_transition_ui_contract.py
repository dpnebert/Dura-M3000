#!/usr/bin/env python3
"""Static regression gate for the hardware-confirmed cold-start transition.

COM25 acceptance evidence establishes this exact sequence:
  logoData visible 500 ms -> legacy Powering up popup 2000 ms -> current UI.
Pixel placement still requires post-flash hardware acceptance.
"""
import re
from pathlib import Path

project = Path(__file__).resolve().parents[1]
board_source = (project / "components/dura_board/dura_board.c").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def extract_braced(text: str, start: int) -> str:
    brace = text.find("{", start)
    require(brace >= 0, "opening brace not found")
    depth = 0
    i = brace
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
        i += 1
    raise AssertionError("unterminated braced block")


def extract_function(text: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;{{}}]*\)\s*\{{", text)
    require(match is not None, f"missing function {name}")
    return extract_braced(text, match.start())


require(
    re.search(
        r"(?m)^[ \t]*#define[ \t]+DURA_BOOT_LOGO_HOLD_MS[ \t]+500u?[ \t]*$",
        board_source,
    ) is not None,
    "cold-start logo hold must be exactly the AVR/customer 500 ms state",
)
require(
    re.search(
        r"(?m)^[ \t]*#define[ \t]+DURA_BOOT_POWERING_UP_HOLD_MS[ \t]+2000u?[ \t]*$",
        board_source,
    ) is not None,
    "Powering up popup hold must be exactly the AVR/customer 2000 ms state",
)

popup = extract_function(board_source, "draw_boot_powering_up_popup")
for required in (
    '"Powering up"',
    "legacy_text_width(&dura_font_ui_small, message)",
    "dura_lcd_fill_rect(x, y, w, h, false)",
    "draw_legacy_text_aligned('C', 64, y + 3, message, false)",
    "draw_legacy_rounded_box(x, y, w, h)",
    "draw_legacy_rounded_box(x - 1, y - 1, w + 2, h + 2)",
    "return dura_lcd_flush()",
):
    require(required in popup, f"startup popup missing legacy contract: {required}")

# Exact one-line AVR popup geometry: Comic Sans 7 height 11 + PU_BOX_BORDER 5.
for geometry in (
    r"const[ \t]+int[ \t]+h[ \t]*=[ \t]*11[ \t]*\+[ \t]*5;",
    r"const[ \t]+int[ \t]+w[ \t]*=[ \t]*legacy_text_width\(&dura_font_ui_small, message\)[ \t]*\+[ \t]*5;",
    r"const[ \t]+int[ \t]+y[ \t]*=[ \t]*\(DURA_LCD_HEIGHT[ \t]*-[ \t]*h\)[ \t]*/[ \t]*2;",
    r"const[ \t]+int[ \t]+x[ \t]*=[ \t]*\(DURA_LCD_WIDTH[ \t]*-[ \t]*w\)[ \t]*/[ \t]*2;",
):
    require(
        re.search(rf"(?m)^[ \t]*{geometry}[ \t]*$", popup) is not None,
        f"startup popup geometry statement changed: {geometry}",
    )

# Display.mbas greyout(1, 1, 125, 62, WHITE_COLOR) clears exactly two
# alternating cell sets and retains the complementary logo cells. Validate each
# complete nested pass so one pass cannot borrow the other's clear operation.
greyout_passes = (
    (
        r"for\s*\(int i = 1; i < \(1 \+ 125\); i \+= 2\)",
        r"for\s*\(int j = 1; j < \(1 \+ 62 - 1\); j \+= 2\)",
    ),
    (
        r"for\s*\(int i = 2; i < \(2 \+ 125\); i \+= 2\)",
        r"for\s*\(int j = 2; j < \(2 \+ 62\); j \+= 2\)",
    ),
)
for outer_pattern, inner_pattern in greyout_passes:
    outer_match = re.search(outer_pattern, popup)
    require(outer_match is not None, f"greyout outer loop changed: {outer_pattern}")
    outer_block = extract_braced(popup, outer_match.start())
    require(
        re.search(inner_pattern, outer_block) is not None,
        f"greyout inner loop changed: {inner_pattern}",
    )
    require(
        outer_block.count("dura_lcd_set_pixel(i, j, false);") == 1,
        "each checkerboard pass must contain exactly one false-polarity clear operation",
    )
require(
    popup.count("dura_lcd_set_pixel(i, j, false);") == 2,
    "popup must contain exactly the two checkerboard clear operations",
)

rounded = extract_function(board_source, "draw_legacy_rounded_box")
rounded_contract = (
    "dura_lcd_draw_rect(x, y, w, h, true)",
    "dura_lcd_set_pixel(x, y, false)",
    "dura_lcd_set_pixel(x + 1, y + 1, true)",
    "dura_lcd_set_pixel(x, y + h - 1, false)",
    "dura_lcd_set_pixel(x + 1, y + h - 2, true)",
    "dura_lcd_set_pixel(x + w - 1, y, false)",
    "dura_lcd_set_pixel(x + w - 2, y + 1, true)",
    "dura_lcd_set_pixel(x + w - 1, y + h - 1, false)",
    "dura_lcd_set_pixel(x + w - 2, y + h - 2, true)",
)
for operation in rounded_contract:
    require(operation in rounded, f"legacy rounded outline changed: {operation}")
require(
    rounded.count("dura_lcd_set_pixel(") == 8,
    "legacy rounded outline must contain exactly the eight AVR corner operations",
)

init = extract_function(board_source, "dura_board_init")
cold_guard = re.search(r"if\s*\(\s*!deep_sleep_wake\s*\)\s*\{", init)
require(
    cold_guard is not None,
    "accepted startup transition must be restricted to cold boot/reset",
)
cold_block = extract_braced(init, cold_guard.start())

hold_guard = re.search(
    r"(?m)^[ \t]*#if[ \t]+!CONFIG_DURA_BOARD_LCD_HOLD_SPLASH_ON_BOOT[ \t]*$",
    cold_block,
)
require(
    hold_guard is not None,
    "transition must retain the exact negative hold-splash compile guard",
)
require(
    re.search(r"(?m)^[ \t]*#endif[ \t]*$", cold_block[hold_guard.end():]) is not None,
    "transition hold-splash guard is not closed",
)

# Keep the accepted cold-start operations exact, unique, and in order while the
# enclosing runtime guard prevents them from running after deep sleep.
cold_start_operations = (
    'ESP_RETURN_ON_ERROR(dura_lcd_draw_splash(), TAG, "lcd splash");',
    "vTaskDelay(pdMS_TO_TICKS(DURA_BOOT_LOGO_HOLD_MS));",
    'ESP_RETURN_ON_ERROR(draw_boot_powering_up_popup(), TAG, "lcd powering-up popup");',
    "vTaskDelay(pdMS_TO_TICKS(DURA_BOOT_POWERING_UP_HOLD_MS));",
    'ESP_RETURN_ON_ERROR(dura_lcd_draw_splash(), TAG, "lcd restore splash after popup");',
)
cursor = -1
for operation in cold_start_operations:
    require(cold_block.count(operation) == 1, f"cold-start operation changed or duplicated: {operation}")
    position = cold_block.find(operation)
    require(position > cursor, f"cold-start operation order changed: {operation}")
    cursor = position

for operation in cold_start_operations:
    require(
        init.count(operation) == cold_block.count(operation),
        f"cold-start operation escaped the deep-sleep exclusion: {operation}",
    )

cold_end = cold_guard.start() + len(cold_block)
normal_path = init[cold_end:]
require(
    'ESP_RETURN_ON_ERROR(dura_board_render_debug_screen(), TAG, "lcd debug initial draw");'
    in normal_path,
    "current UI must be drawn after the cold-start-only transition block",
)
require(
    "dura_meter_snapshot_t meter;" in normal_path,
    "initial UI snapshot marker missing after the transition",
)
require("pdMS_TO_TICKS(5000)" not in init, "obsolete five-second bare splash hold remains")
print("transition UI contract: PASS")
