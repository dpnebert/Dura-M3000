#!/usr/bin/env python3
"""Executable host/static contract for low-battery blink and button-hold acceleration."""
from __future__ import annotations

import re
import subprocess
import tempfile
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> int:
    include_dir = PROJECT / "components/dura_board/include"
    behavior_source = PROJECT / "components/dura_board/dura_ui_behavior.c"
    config_source = PROJECT / "components/dura_board/dura_ui_config.c"
    board_source = (PROJECT / "components/dura_board/dura_board.c").read_text(encoding="utf-8")
    app_source = (PROJECT / "main/app_main.c").read_text(encoding="utf-8")
    kconfig = (PROJECT / "components/dura_board/Kconfig.projbuild").read_text(encoding="utf-8")
    defaults = (PROJECT / "sdkconfig.defaults").read_text(encoding="utf-8")
    active = (PROJECT / "sdkconfig").read_text(encoding="utf-8")
    cmake = (PROJECT / "components/dura_board/CMakeLists.txt").read_text(encoding="utf-8")

    require(behavior_source.is_file(), "missing pure UI behavior implementation")
    require(config_source.is_file(), "missing persisted UI runtime configuration implementation")
    with tempfile.TemporaryDirectory(prefix="dura-ui-behavior-host-") as tmp:
        binary = Path(tmp) / "dura_ui_behavior_host"
        command = [
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-I", str(include_dir),
            str(PROJECT / "tests/dura_ui_behavior_host.c"),
            str(behavior_source), "-o", str(binary),
        ]
        print("+ " + " ".join(command), flush=True)
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)

    config_defaults = {
        "DURA_BOARD_LOW_BATTERY_THRESHOLD_PERCENT": "20",
        "DURA_BOARD_LOW_BATTERY_BLINK_ON_MS": "1000",
        "DURA_BOARD_LOW_BATTERY_BLINK_OFF_MS": "1000",
        "DURA_BOARD_BUTTON_HOLD_MAX_RATE_X": "10",
    }
    for symbol, value in config_defaults.items():
        require(f"config {symbol}" in kconfig, f"missing Kconfig symbol {symbol}")
        setting = f"CONFIG_{symbol}={value}"
        require(re.search(rf"^{re.escape(setting)}$", defaults, re.MULTILINE),
                f"sdkconfig.defaults missing {setting}")
        require(re.search(rf"^{re.escape(setting)}$", active, re.MULTILINE),
                f"sdkconfig missing {setting}")

    for marker in (
        "dura_ui_battery_percent_visible",
        "dura_ui_button_repeat_interval_ms",
        "ui_button_is_repeatable",
        "esp_timer_get_time",
    ):
        require(marker in board_source, f"board integration missing {marker}")

    persistence = config_source.read_text(encoding="utf-8")
    for marker in ("nvs_open", "nvs_get_blob", "nvs_set_blob", "nvs_commit"):
        require(marker in persistence, f"runtime configuration persistence missing {marker}")

    for marker in ('"ui_config"', '"ui_config_set"', "dura_ui_config_format_json"):
        require(marker in app_source, f"BLE/UART command path missing {marker}")
    require("ui_config_set <threshold_pct> <blink_on_ms> <blink_off_ms> <max_rate_x>" in app_source,
            "help text does not document the runtime UI configuration command")

    for source in ("dura_ui_behavior.c", "dura_ui_config.c"):
        require(source in cmake, f"board component does not compile {source}")
    require("nvs_flash" in cmake, "board component does not require nvs_flash for runtime configuration")

    print("DURA_UI_BEHAVIOR_CONTRACT_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
