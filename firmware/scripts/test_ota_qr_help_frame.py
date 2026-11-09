#!/usr/bin/env python3
"""Host framebuffer for OTA Wi-Fi QR help. Links real espressif qrcode (not checkerboard stub)."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import subprocess
import sys
import zlib
from pathlib import Path

from test_home_volume_ui import ROOT, config_defines, generate

QR_ROOT = None  # set in main


def instrument(inc: Path) -> None:
    s = inc.read_text(encoding="utf-8")

    def rep(a: str, b: str) -> None:
        nonlocal s
        assert s.count(a) == 1, (a[:60], s.count(a))
        s = s.replace(a, b)

    rep(
        'strcat(rendered_text,"\\n"); }',
        'strcat(rendered_text,"\\n"); } text_log(orient,x,y,text);',
    )
    s = "static void text_log(char orient,int x,int y,const char *text);\n" + s

    # Replace checkerboard stub with real esp_qrcode_* from linked objects.
    stub = """typedef const uint8_t *esp_qrcode_handle_t;
typedef struct {void (*display_func)(esp_qrcode_handle_t);int max_qrcode_version,qrcode_ecc_level;} esp_qrcode_config_t;
#define ESP_QRCODE_CONFIG_DEFAULT() ((esp_qrcode_config_t){0})
#define ESP_QRCODE_ECC_LOW 0
#define ESP_LOG_WARN 2
static int qr_error;
static int format_provisioning_qr_payload(char *p,size_t n){snprintf(p,n,"host");return qr_error==1?ESP_FAIL:ESP_OK;}
static void draw_dynamic_qr_to_lcd(esp_qrcode_handle_t q){(void)q;}
static void esp_log_level_set(const char*s,int l){(void)s;(void)l;}
static int esp_qrcode_generate(const esp_qrcode_config_t*c,const char*p){(void)p;if(qr_error)return ESP_FAIL;if(c&&c->display_func)c->display_func((esp_qrcode_handle_t)1);return ESP_OK;}
static int esp_qrcode_get_size(esp_qrcode_handle_t qrcode) {(void)qrcode;return 21;}
static bool esp_qrcode_get_module(esp_qrcode_handle_t qrcode, int x, int y) {(void)qrcode;return ((x + y) & 1) == 0;}
static size_t strlcpy(char*d,const char*s,size_t n){snprintf(d,n,"%s",s);return strlen(s);}"""
    real = """#include "qrcode.h"
#define ESP_QRCODE_CONFIG_DEFAULT() ((esp_qrcode_config_t){0})
#define ESP_LOG_WARN 2
static int qr_error;
static int format_provisioning_qr_payload(char *p,size_t n){snprintf(p,n,"host");return qr_error==1?ESP_FAIL:ESP_OK;}
static void draw_dynamic_qr_to_lcd(esp_qrcode_handle_t q){(void)q;}
static void esp_log_level_set(const char*s,int l){(void)s;(void)l;}
/* esp_qrcode_generate / get_size / get_module come from managed_components/espressif__qrcode. */
static size_t strlcpy(char*d,const char*s,size_t n){snprintf(d,n,"%s",s);return strlen(s);}
#ifndef ESP_LOGI
#define ESP_LOGI(tag, fmt, ...) ((void)0)
#define ESP_LOGW(tag, fmt, ...) ((void)0)
#define ESP_LOGE(tag, fmt, ...) ((void)0)
#endif"""
    # Match either old FAIL stub or improved checkerboard stub
    if stub not in s:
        # try older FAIL-only stub pattern via regex
        m = re.search(
            r"typedef const uint8_t \*esp_qrcode_handle_t;.*?static size_t strlcpy\(char\*d,const char\*s,size_t n\)\{snprintf\(d,n,\"%s\",s\);return strlen\(s\);\}",
            s,
            re.S,
        )
        assert m, "qr stub block not found"
        s = s[: m.start()] + real + s[m.end() :]
    else:
        s = s.replace(stub, real, 1)

    # File-scope QR size is declared next to draw_ota in production; function()
    # extraction omits it, so reinstate it for the harness.
    if "static int s_ota_qr_size_px;" not in s:
        assert "static void draw_ota_qr_to_lcd" in s
        s = s.replace(
            "static void draw_ota_qr_to_lcd",
            "static int s_ota_qr_size_px;\nstatic void draw_ota_qr_to_lcd",
            1,
        )
    inc.write_text(s, encoding="utf-8")


def pgm_to_png(pgm: Path, png: Path, scale: int = 4) -> None:
    data = pgm.read_bytes()
    assert data.startswith(b"P5\n"), pgm
    parts = data.split(b"\n", 3)
    wh = parts[1].decode().split()
    w, h = int(wh[0]), int(wh[1])
    pixels = parts[3]
    assert len(pixels) == w * h
    sw, sh = w * scale, h * scale
    raw = bytearray()
    for y in range(sh):
        raw.append(0)
        sy = y // scale
        row = pixels[sy * w : (sy + 1) * w]
        for x in range(sw):
            v = row[x // scale]
            raw.extend((v, v, v))

    def chunk(tag: bytes, payload: bytes) -> bytes:
        return (
            struct.pack(">I", len(payload))
            + tag
            + payload
            + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
        )

    ihdr = struct.pack(">IIBBBBB", sw, sh, 8, 2, 0, 0, 0)
    png.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )


def write_esp_log_stub(out: Path) -> Path:
    """Only esp_log.h extras; do not shadow recipe_ownership esp_err.h."""
    inc = out / "qr_host_inc"
    inc.mkdir(exist_ok=True)
    (inc / "esp_log.h").write_text(
        "#pragma once\n#ifndef ESP_LOGI\n"
        "#define ESP_LOGI(tag, fmt, ...) ((void)0)\n"
        "#define ESP_LOGW(tag, fmt, ...) ((void)0)\n"
        "#define ESP_LOGE(tag, fmt, ...) ((void)0)\n"
        "#endif\n",
        encoding="utf-8",
    )
    return inc


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--source-root", type=Path, default=ROOT)
    a = ap.parse_args()
    root = a.source_root.resolve()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    global QR_ROOT
    QR_ROOT = root / "managed_components" / "espressif__qrcode"

    generate(root, out)
    instrument(out / "board_home_volume.inc")
    d = out / "normal"
    d.mkdir(exist_ok=True)
    (d / "volume_config.h").write_text(config_defines(root), encoding="utf-8")
    qr_inc = write_esp_log_stub(out)

    inputs = [
        ROOT / "tests/m3000_ota_qr_help.c",
        root / "components/dura_meter/dura_meter.c",
        root / "components/dura_meter/dura_control_policy.c",
        root / "components/dura_board/dura_lcd_assets.c",
        QR_ROOT / "qrcodegen.c",
        QR_ROOT / "esp_qrcode_wrapper.c",
        QR_ROOT / "esp_qrcode_main.c",
    ]
    cmd = [
        "cc",
        "-std=c11",
        "-g",
        "-pthread",
        "-fsanitize=undefined",
        "-Werror=implicit-function-declaration",
        "-include", "stdlib.h",
        *[
            f"-I{x}"
            for x in (
                d,
                out,
                ROOT / "tests",
                root / "tests/recipe_ownership/include",
                qr_inc,
                QR_ROOT / "include",
                QR_ROOT,  # qrcodegen.h
                root / "components/dura_meter/include",
                root / "components/dura_board/include",
            )
        ],
        *map(str, inputs),
        "-lm",
        "-o",
        str(d / "test"),
    ]
    r = subprocess.run(cmd, capture_output=True, text=True)
    (d / "compile.log").write_text(" ".join(cmd) + "\n" + r.stdout + r.stderr, encoding="utf-8")
    if r.returncode:
        print(r.stderr[-4000:] if r.stderr else r.stdout)
        return r.returncode
    r = subprocess.run([str(d / "test")], cwd=d, capture_output=True, text=True, timeout=60)
    (d / "run.log").write_text(r.stdout + r.stderr, encoding="utf-8")
    print(r.stdout)
    if r.returncode:
        print(r.stderr)
        return r.returncode
    pgm = d / "ota-wifi-qr-help.pgm"
    png = out / "ota-wifi-qr-help.png"
    assert pgm.is_file(), "missing PGM"
    pgm_to_png(pgm, png)
    (out / "ota-wifi-qr-help.pgm").write_bytes(pgm.read_bytes())
    results = {"ota_qr_help": 0, "png": str(png), "qr": "real-espressif-qrcode"}
    (out / "results.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    paths = [
        root / "components/dura_board/dura_board.c",
        root / "components/dura_board/include/dura_qr_help.h",
        Path(__file__),
        out / "board_home_volume.inc",
    ]
    (out / "INPUT_SHA256.json").write_text(
        json.dumps({str(x): hashlib.sha256(x.read_bytes()).hexdigest() for x in paths}, indent=2)
        + "\n",
        encoding="utf-8",
    )
    print(f"PNG {png}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
