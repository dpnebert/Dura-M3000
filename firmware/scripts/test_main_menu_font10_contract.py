#!/usr/bin/env python3
"""Contract for existing 7 pt Main Menu instructions and unchanged 10-row power font."""
from __future__ import annotations

import hashlib
import re
import struct
from pathlib import Path

project = Path(__file__).resolve().parents[1]
assets_path = project / "components/dura_board/dura_lcd_assets.c"
header_path = project / "components/dura_board/include/dura_lcd_assets.h"
board_path = project / "components/dura_board/dura_board.c"

assets = assets_path.read_text(encoding="utf-8")
header = header_path.read_text(encoding="utf-8")
board = board_path.read_text(encoding="utf-8")


def parse_array(name: str) -> list[int]:
    match = re.search(
        rf"const\s+uint(?:8|16)_t\s+{re.escape(name)}(?:\[[^\]]*\])?\s*=\s*\{{(.*?)\}};",
        assets,
        re.DOTALL,
    )
    assert match is not None, f"missing asset array: {name}"
    body = re.sub(r"//.*", "", match.group(1))
    return [
        int(token.rstrip("uU"), 0)
        for token in re.findall(r"\b(?:0x[0-9A-Fa-f]+|\d+)[uU]?\b", body)
    ]


bitmaps = parse_array("dura_asset_comicSansMS6ptCharBitmaps")
descriptors = parse_array("dura_asset_comicSansMS6ptCharDescriptors")

assert len(bitmaps) == 940, f"unexpected bitmap byte count: {len(bitmaps)}"
assert len(descriptors) == 188, f"unexpected descriptor count: {len(descriptors)}"
assert hashlib.sha256(bytes(bitmaps)).hexdigest() == (
    "3ec8352e582ecd5bff185e7934b71a3a35d2d80fbca1ab9bb9b3a0616a46bb7e"
), "10-row bitmap payload changed"
assert hashlib.sha256(b"".join(struct.pack("<H", value) for value in descriptors)).hexdigest() == (
    "77fdf3f1232254b7a956e5482681b3d5dd30a6e88c69d65e8a2efdff3ebafe5c"
), "10-row descriptor payload changed"

expected_offset = 0
for glyph_index in range(94):
    width, offset = descriptors[glyph_index * 2 : glyph_index * 2 + 2]
    assert width > 0, f"zero-width glyph at ASCII {glyph_index + 33}"
    assert offset == expected_offset, f"bad offset at ASCII {glyph_index + 33}: {offset} != {expected_offset}"
    expected_offset += ((width + 7) // 8) * 10
assert expected_offset == len(bitmaps), "descriptor span does not consume the bitmap payload"

font_init = re.search(
    r"const\s+dura_lcd_font_asset_t\s+dura_font_ui_main_menu_10\s*=\s*\{(.*?)\};",
    assets,
    re.DOTALL,
)
assert font_init is not None, "missing dura_font_ui_main_menu_10 font descriptor"
for required in (
    '"dura_font_ui_main_menu_10"',
    "33u,",
    "94u,",
    "10u,",
    "2u,",
    "dura_asset_comicSansMS6ptCharBitmaps",
    "dura_asset_comicSansMS6ptCharDescriptors",
):
    assert required in font_init.group(1), f"font descriptor field missing: {required}"

assert "SHA-256 b82c53776058f291382ff7e008d4675839d2dc21eb295c66391f6fb0655d8fc0" in assets
assert "Glyphs are independently rasterized; no bitmap scaling." in assets
assert not re.search(r"^\s*\d+\|", assets, re.MULTILINE), "editor line-number prefixes remain in assets"
assert "extern const dura_lcd_font_asset_t dura_font_ui_main_menu_10;" in header

home = re.search(
    r"static\s+void\s+draw_legacy_home\s*\([^)]*\)\s*\{(?P<body>.*?)^\}",
    board,
    re.DOTALL | re.MULTILINE,
)
assert home is not None, "missing draw_legacy_home"
home_body = home.group("body")
assert '"Main Menu"' not in home_body, "removed Main Menu heading returned"
for y, text in (
    (9, "Hold outside buttons"),
    (18, "for Program Settings."),
    (27, "Tote Auto Recirc:"),
    (36, "auto recirculation."),
):
    pattern = (
        rf"draw_font_asset_text_aligned\s*\(\s*&dura_font_ui_small,\s*'C',\s*64,\s*{y},"
        rf"\s*\"{re.escape(text)}\",\s*false\s*\)"
    )
    assert re.search(pattern, home_body), f"Main Menu line is not using the existing 7 pt font: {text}"

for required in (
    "const dura_battery_measurement_t battery = dura_battery_get_measurement();",
    'snprintf(power, sizeof(power), "Power: %u%%", (unsigned)battery.percent);',
    'snprintf(power, sizeof(power), "Power: --%%");',
):
    assert required in home_body, f"Main Menu power field changed: {required}"
assert re.search(
    r"draw_font_asset_text_aligned\s*\(\s*&dura_font_ui_main_menu_10,\s*'L',\s*0,\s*44,"
    r"\s*power,\s*false\s*\)",
    home_body,
), "Main Menu power field must use the 10-row font at bottom-left x=0 y=44"

renderer = re.search(
    r"static\s+uint8_t\s+draw_font_asset_char\s*\([^)]*\)\s*\{(?P<body>.*?)^\}",
    board,
    re.DOTALL | re.MULTILINE,
)
assert renderer is not None, "missing direct font asset renderer"
renderer_body = renderer.group("body")
assert "font->height" in renderer_body
for forbidden in ("MAIN_MENU_FONT_X_DIVISOR", "source_x_begin", "source_y_begin"):
    assert forbidden not in renderer_body, f"bitmap scaling returned to Main Menu renderer: {forbidden}"

# Instruction widths use the original 7 pt asset; the 10-row asset above
# remains protected because the power field still uses it.
instruction_descriptors = parse_array("dura_asset_comicSansMS7ptCharDescriptors")
assert len(instruction_descriptors) == 446
assert re.search(
    r'const dura_lcd_font_asset_t dura_font_ui_small = \{ "dura_font_ui_small", 33u, 223u, 11u, 2u, '
    r'dura_asset_comicSansMS7ptCharBitmaps,', assets
), "existing 7 pt font binding changed"
widths = {}
for text in (
    "Hold outside buttons",
    "for Program Settings.",
    "Tote Auto Recirc:",
    "auto recirculation.",
):
    width = 0
    for ch in text:
        if ch == " ":
            width += 2
        else:
            width += instruction_descriptors[(ord(ch) - 33) * 2]
        width += 1
    widths[text] = width
    assert width <= 128, f"Main Menu line does not fit 128 pixels: {text} = {width}"

assert widths == {
    "Hold outside buttons": 87,
    "for Program Settings.": 91,
    "Tote Auto Recirc:": 75,
    "auto recirculation.": 77,
}, widths

print("MAIN_MENU_FONT10_CONTRACT_PASS")
