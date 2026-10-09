#!/usr/bin/env python3
"""PCB832 revX3 pump mapping contract; historical filename retained.

The authorized new-board profile drives pump47, not the old GPIO21 LED bench.
Both reed inputs remain active-low/pulled-up. Physical acceptance is separate.
"""
import re
from pathlib import Path

project = Path(__file__).resolve().parents[1]
board_path = project / "components/dura_board/dura_board.c"
header_path = project / "components/dura_board/include/dura_board.h"
board_source = board_path.read_text(encoding="utf-8")
header_source = header_path.read_text(encoding="utf-8")


def require(config: str, setting: str, config_name: str) -> None:
    assert re.search(rf"^{re.escape(setting)}$", config, re.MULTILINE), (
        f"{config_name} must contain {setting} for the authorized PCB832 revX3 profile"
    )


defaults = (project / "sdkconfig.defaults").read_text(encoding="utf-8")
kconfig = (project / "components/dura_board/Kconfig.projbuild").read_text(encoding="utf-8")
overlay_name = "sdkconfig.output-test.defaults"
overlay = (project / overlay_name).read_text(encoding="utf-8")
require(defaults, "CONFIG_DURA_BOARD_ENABLE_HARDWARE_TASK=y", "sdkconfig.defaults")
require(defaults, "CONFIG_DURA_BOARD_ENABLE_PUMP_OUTPUT=y", "sdkconfig.defaults")
assert re.search(
    r"config DURA_BOARD_ENABLE_PUMP_OUTPUT\s+bool .*?\s+depends on DURA_BOARD_ENABLE_HARDWARE_TASK\s+default y",
    kconfig,
    re.DOTALL,
), "pump output Kconfig default must be enabled for the authorized PCB832 revX3 profile"
require(defaults, "CONFIG_DURA_BOARD_GPIO_PUMP_OUTPUT=47", "sdkconfig.defaults")
require(defaults, "# CONFIG_DURA_BOARD_PUMP_OUTPUT_ACTIVE_LOW is not set", "sdkconfig.defaults")
require(defaults, "CONFIG_DURA_BOARD_GPIO_FLOW_A=21", "sdkconfig.defaults")
require(defaults, "CONFIG_DURA_BOARD_GPIO_FLOW_B=2", "sdkconfig.defaults")
require(defaults, "CONFIG_DURA_BOARD_FLOW_ACTIVE_LOW=y", "sdkconfig.defaults")
require(defaults, "CONFIG_DURA_BOARD_FLOW_PULLUP=y", "sdkconfig.defaults")
require(overlay, "CONFIG_DURA_BOARD_ENABLE_PUMP_OUTPUT=y", overlay_name)

# Polarity/gate checks retained; runtime publication is now under the meter lock.
for level, condition in (("INACTIVE", "!intent.pump"), ("ACTIVE", "intent.pump")):
    assert re.search(
        r"#if CONFIG_DURA_BOARD_ENABLE_PUMP_OUTPUT\s+if \(" + re.escape(condition) +
        r"\) gpio_set_level\(DURA_GPIO_PUMP_OUTPUT, DURA_PUMP_OUTPUT_" + level + r"_LEVEL\);",
        board_source,
    ), f"pump {level} output must follow serialized intent under the feature gate"
assert "return dura_meter_sync_outputs();" in board_source
assert "dura_meter_set_output_handler(apply_meter_outputs)" in board_source

assert "#define DURA_PUMP_OUTPUT_ACTIVE_LEVEL        DURA_ACTIVE_LEVEL(CONFIG_DURA_BOARD_PUMP_OUTPUT_ACTIVE_LOW)" in header_source
assert "#define DURA_PUMP_OUTPUT_INACTIVE_LEVEL      DURA_INACTIVE_LEVEL(CONFIG_DURA_BOARD_PUMP_OUTPUT_ACTIVE_LOW)" in header_source
assert "gpio_set_level(DURA_GPIO_PUMP_OUTPUT, DURA_PUMP_OUTPUT_INACTIVE_LEVEL);" in board_source
assert "(void)dura_board_sync_outputs();" in board_source
assert "uint32_t pulses = dura_meter_drain_flow();" in board_source
assert "dura_meter_set_flow_handler(flow_boundary)" in board_source
assert "(void)dura_meter_record_pulse(pulses);" not in board_source, "split drain/attribute race must not return"

print("PUMP_OUTPUT_BENCH_CONTRACT_PASS")
