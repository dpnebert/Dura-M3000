#!/usr/bin/env python3
"""Convert Dura legacy MikroBasic GLCD/font assets into ESP-IDF C assets.

Source of truth:
  /home/omi/work_projects/dura/AVR/reference_source/Release_1_1_3/Src/Meter_Project

Generated outputs:
  components/dura_board/include/dura_lcd_assets.h
  components/dura_board/dura_lcd_assets.c
"""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC_DIR = Path('/home/omi/work_projects/dura/AVR/reference_source/Release_1_1_3/Src/Meter_Project')
OUT_DIR = ROOT / 'components' / 'dura_board'
INCLUDE_DIR = OUT_DIR / 'include'

SOURCE_FILES = [
    'Graphics.mbas',
    'gLCDfont.mbas',
    'fontComicSans7.mbas',
    'fontComicSans16.mbas',
    'fontArialNumbers24.mbas',
    'fontArialNumbers40.mbas',
]

BITMAP_META = {
    'QRcodeData': (128, 64, 'Full-screen QR code bitmap from Graphics.mbas'),
    'durametertitle_bmp_glcd_bmp': (128, 8, 'Dura-Meter title bitmap from Graphics.mbas'),
    'logoData': (128, 64, 'Dura full-screen boot logo/splash bitmap from Graphics.mbas'),
    'battery_icon_glcd_bmp': (15, 32, 'Battery icon bitmap; legacy declaration is byte[(32*15)/8] but draw calls use width=15 height=32'),
    'thick_glcd_bmp': (17, 5, 'THICK text/icon bitmap from Graphics.mbas'),
    'thin_glcd_bmp': (13, 5, 'THIN text/icon bitmap from Graphics.mbas'),
    'thick_pointer': (7, 5, '7x5 thick pointer bitmap from Graphics.mbas'),
}

FONT_META = {
    # name prefix: public_name, first_char, char_count, height, space_width, description
    'font': ('dura_font_legacy_5x7', 0, 255, 7, 1, 'Legacy 5x7 fixed-width GLCD font from gLCDfont.mbas'),
    'comicSansMS7pt': ('dura_font_ui_small', 33, 223, 11, 2, 'Legacy small variable-width UI font from fontComicSans7.mbas'),
    'comicSansMS16pt': ('dura_font_ui_16', ord('.'), 12, 17, 2, 'Legacy Comic Sans 16pt limited numeric punctuation font from fontComicSans16.mbas'),
    'arialNarrow24pt': ('dura_font_num_24', ord('.'), 15, 23, 3, 'Legacy Arial Narrow 24pt numeric font from fontArialNumbers24.mbas'),
    'arialNarrow40pt': ('dura_font_num_40', ord('.'), 13, 40, 3, 'Legacy Arial Narrow 40pt numeric font from fontArialNumbers40.mbas'),
}


def c_name(name: str) -> str:
    return 'dura_asset_' + re.sub(r'[^A-Za-z0-9_]', '_', name)


def c_array(values: list[int], ctype: str) -> str:
    per_line = 16 if ctype == 'uint8_t' else 12
    rows = []
    for i in range(0, len(values), per_line):
        chunk = values[i:i + per_line]
        if ctype == 'uint8_t':
            rows.append('    ' + ', '.join(f'0x{v:02X}u' for v in chunk))
        else:
            rows.append('    ' + ', '.join(f'{v}u' for v in chunk))
    return ',\n'.join(rows)


def parse_arrays() -> list[dict]:
    arrays = []
    pattern = re.compile(
        r'const\s+(\w+)\s+as\s+(byte|word)\s*\[([^\]]+)\]\s*=\s*\((.*?)\)\s*(?:code)?',
        re.IGNORECASE | re.DOTALL,
    )
    for filename in SOURCE_FILES:
        text = (SRC_DIR / filename).read_text(errors='ignore')
        stripped_lines = []
        for line in text.splitlines():
            stripped_lines.append(line.split("'", 1)[0])
        stripped = '\n'.join(stripped_lines)
        for match in pattern.finditer(stripped):
            name, typ, dim, body = match.group(1), match.group(2).lower(), match.group(3).strip(), match.group(4)
            values = [int(token, 0) for token in re.findall(r'0x[0-9A-Fa-f]+|\b\d+\b', body)]
            arrays.append({'file': filename, 'name': name, 'type': typ, 'dim': dim, 'values': values})
    return arrays


def emit(arrays: list[dict]) -> None:
    INCLUDE_DIR.mkdir(parents=True, exist_ok=True)

    header = ['''#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t width;
    uint16_t height;
    const uint8_t *data;
    size_t data_len;
    const char *name;
} dura_lcd_bitmap_asset_t;

typedef struct {
    const char *name;
    uint8_t first_char;
    uint16_t char_count;
    uint8_t height;
    uint8_t space_width;
    const uint8_t *bitmaps;
    size_t bitmaps_len;
    const uint16_t *descriptors;
    size_t descriptors_len;
} dura_lcd_font_asset_t;

''']

    source = ['''// Generated from Dura legacy MikroBasic reference assets.
// Source directory: /home/omi/work_projects/dura/AVR/reference_source/Release_1_1_3/Src/Meter_Project
// Do not hand-edit byte data; regenerate from the reference source if assets change.

#include "dura_lcd_assets.h"

''']

    for array in arrays:
        cname = c_name(array['name'])
        ctype = 'uint8_t' if array['type'] == 'byte' else 'uint16_t'
        header.append(f'extern const {ctype} {cname}[{len(array["values"])}]; // {array["file"]}:{array["name"]}\n')
        source.append(f'// Source: {array["file"]} const {array["name"]} as {array["type"]}[{array["dim"]}] ({len(array["values"])} values)\n')
        source.append(f'const {ctype} {cname}[{len(array["values"])}] = {{\n{c_array(array["values"], ctype)}\n}};\n\n')

    header.append('\n// Bitmap asset descriptors\n')
    source.append('// Bitmap asset descriptors\n')
    for name, (width, height, description) in BITMAP_META.items():
        if any(array['name'] == name for array in arrays):
            header.append(f'extern const dura_lcd_bitmap_asset_t dura_bitmap_{name};\n')
            source.append(
                f'const dura_lcd_bitmap_asset_t dura_bitmap_{name} = '
                f'{{ {width}u, {height}u, {c_name(name)}, sizeof({c_name(name)}), "{name}" }}; '
                f'// {description}\n'
            )

    header.append('\n// Font asset descriptors\n')
    source.append('\n// Font asset descriptors\n')
    for prefix, (asset, first, count, height, space, description) in FONT_META.items():
        if prefix == 'font':
            bitmap_name = 'font'
            descriptor_name = None
        else:
            bitmap_name = prefix + 'CharBitmaps'
            descriptor_name = prefix + 'CharDescriptors'
        bitmap_array = next((array for array in arrays if array['name'] == bitmap_name), None)
        descriptor_array = next((array for array in arrays if array['name'] == descriptor_name), None) if descriptor_name else None
        if bitmap_array is None:
            continue
        header.append(f'extern const dura_lcd_font_asset_t {asset};\n')
        descriptor_pointer = c_name(descriptor_array['name']) if descriptor_array else 'NULL'
        descriptor_len = f'(sizeof({descriptor_pointer}) / sizeof({descriptor_pointer}[0]))' if descriptor_array else '0u'
        source.append(
            f'const dura_lcd_font_asset_t {asset} = '
            f'{{ "{asset}", {first}u, {count}u, {height}u, {space}u, '
            f'{c_name(bitmap_array["name"])}, sizeof({c_name(bitmap_array["name"])}), '
            f'{descriptor_pointer}, {descriptor_len} }}; // {description}\n'
        )

    header.append('''
#ifdef __cplusplus
}
#endif
''')

    (INCLUDE_DIR / 'dura_lcd_assets.h').write_text(''.join(header))
    (OUT_DIR / 'dura_lcd_assets.c').write_text(''.join(source))


def main() -> None:
    arrays = parse_arrays()
    emit(arrays)
    print(f'Converted {len(arrays)} arrays')
    for array in arrays:
        print(f"- {array['file']}: {array['name']} {array['type']} values={len(array['values'])} dim={array['dim']}")


if __name__ == '__main__':
    main()
