#!/usr/bin/env python3
"""Fail if a second product firmware version literal appears outside dura_meter.h."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
METER_H = ROOT / "components" / "dura_meter" / "include" / "dura_meter.h"
APP_MAIN = ROOT / "main" / "app_main.c"
DEFAULTS = ROOT / "sdkconfig.defaults"

ver_re = re.compile(
    r'#\s*define\s+DURA_METER_FIRMWARE_VERSION\s+"([0-9]+\.[0-9]+\.[0-9]+)"'
)
meter = METER_H.read_text(encoding="utf-8")
m = ver_re.search(meter)
assert m, "DURA_METER_FIRMWARE_VERSION missing in dura_meter.h"
version = m.group(1)
assert meter.count(f'DURA_METER_FIRMWARE_VERSION "{version}"') == 1

# Product trees that must not duplicate the literal version string.
scan_roots = [
    ROOT / "components" / "dura_board",
    ROOT / "components" / "dura_meter",
    ROOT / "components" / "dura_control_policy",
    ROOT / "components" / "dura_peers",
    ROOT / "components" / "dura_quick_cal",
    ROOT / "main",
]
literal = f'"{version}"'
offenders = []
for root in scan_roots:
    if not root.is_dir():
        continue
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        if path.suffix.lower() not in {".c", ".h", ".py", ".txt", ".md", ".cmake"}:
            continue
        if path.resolve() == METER_H.resolve():
            continue
        data = path.read_text(encoding="utf-8", errors="replace")
        if literal in data:
            offenders.append(str(path.relative_to(ROOT)))

assert not offenders, (
    f"Product version literal {literal} must appear only in dura_meter.h; also found in: "
    + ", ".join(offenders)
)

app = APP_MAIN.read_text(encoding="utf-8")
assert "ble_config.firmware_version = DURA_METER_FIRMWARE_VERSION" in app, (
    "app_main must pass DURA_METER_FIRMWARE_VERSION into the BLE shell config"
)

defaults = DEFAULTS.read_text(encoding="utf-8")
ble_assign = re.search(
    r'CONFIG_BLE_SHELL_FIRMWARE_VERSION="([^"]*)"', defaults
)
assert ble_assign, "sdkconfig.defaults must still define CONFIG_BLE_SHELL_FIRMWARE_VERSION fallback"
assert ble_assign.group(1) != version, (
    "sdkconfig.defaults must not pin CONFIG_BLE_SHELL_FIRMWARE_VERSION to the product version"
)
assert ble_assign.group(1) in {"UNSET", "0.0.0", "fallback"}, (
    f"unexpected BLE fallback {ble_assign.group(1)!r}; use UNSET"
)

print(f"SINGLE_FIRMWARE_VERSION_PASS version={version}")
