#!/usr/bin/env python3
"""Executable host/static contract for routed DURA_BATT_ADC firmware and UI."""
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
    math_source = PROJECT / "components/dura_board/dura_battery_math.c"
    hardware_source = PROJECT / "components/dura_board/dura_battery.c"
    board_source = (PROJECT / "components/dura_board/dura_board.c").read_text(encoding="utf-8")
    cmake = (PROJECT / "components/dura_board/CMakeLists.txt").read_text(encoding="utf-8")

    require(math_source.is_file(), "missing pure battery math implementation")
    require(hardware_source.is_file(), "missing ADC oneshot battery implementation")
    with tempfile.TemporaryDirectory(prefix="dura-battery-host-") as tmp:
        binary = Path(tmp) / "dura_battery_math_host"
        command = [
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-I", str(include_dir),
            str(PROJECT / "tests/dura_battery_math_host.c"),
            str(math_source), "-o", str(binary),
        ]
        print("+ " + " ".join(command), flush=True)
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)

    adc = hardware_source.read_text(encoding="utf-8")
    for marker in (
        "adc_oneshot_io_to_channel(DURA_GPIO_BATTERY_ADC",
        "unit_id != ADC_UNIT_1 || channel != ADC_CHANNEL_0",
        "ADC_ATTEN_DB_12",
        "ADC_BITWIDTH_DEFAULT",
        "adc_cali_create_scheme_curve_fitting",
        "adc_cali_raw_to_voltage",
        "DURA_BATTERY_DISCARD_SAMPLES",
        "DURA_BATTERY_AVERAGE_SAMPLES",
        "DURA_BATTERY_SAMPLE_PERIOD_MS",
    ):
        require(marker in adc, f"missing ADC contract marker: {marker}")

    for marker in (
        "#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG",
        "esp_log_level_set(TAG, ESP_LOG_DEBUG)",
        "battery ADC sample: raw_avg=%u pad_mv=%u battery_mv=%u percent=%u",
    ):
        require(marker not in adc, f"temporary ADC DEBUG diagnostic remains: {marker}")
    require("adc1_get_raw" not in adc and "adc2_get_raw" not in adc,
            "deprecated legacy ADC API is prohibited")

    for marker in (
        '"Power: %u%%"',
        '"Power: -- %%"',
        '"Voltage: --.-- V"',
        "dura_battery_format_voltage",
        "dura_battery_get_measurement",
    ):
        require(marker in board_source, f"missing battery UI contract marker: {marker}")
    require('"Powering Up", "Power Level", "-- %"' not in board_source,
            "fake constant battery screen remains")
    require('draw_legacy_menu_screen("Battery", show_battery_percentage ? power : NULL' in board_source,
            "Battery screen does not render the measured power percentage")
    require("dura_battery.c" in cmake and "dura_battery_math.c" in cmake,
            "battery sources are not registered")
    require("if(CONFIG_DURA_BOARD_ENABLE_HARDWARE_TASK)" in cmake and
            "list(APPEND dura_board_requires esp_adc)" in cmake,
            "esp_adc must be linked only when the hardware task is enabled")
    require("#if CONFIG_DURA_BOARD_ENABLE_HARDWARE_TASK" in adc and
            "ESP_ERR_NOT_SUPPORTED" in adc,
            "QEMU/no-hardware battery stub is missing")
    print("DURA_BATTERY_CONTRACT_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
