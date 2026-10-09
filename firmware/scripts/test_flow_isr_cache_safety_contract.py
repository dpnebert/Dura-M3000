#!/usr/bin/env python3
"""Regression contract for cache-safe M3000 flow GPIO interrupt handling."""
import re
from pathlib import Path

project = Path(__file__).resolve().parents[1]
board_source = (project / "components/dura_board/dura_board.c").read_text(encoding="utf-8")
active_config = (project / "sdkconfig").read_text(encoding="utf-8")
default_config = (project / "sdkconfig.defaults").read_text(encoding="utf-8")

flow_isr = re.search(
    r"static\s+void\s+IRAM_ATTR\s+flow_isr\s*\([^)]*\)\s*\{(?P<body>.*?)\n\}",
    board_source,
    re.DOTALL,
)
assert flow_isr is not None, "flow_isr must remain explicitly IRAM-resident"
assert "gpio_get_level(DURA_GPIO_FLOW_A)" in flow_isr.group("body")
assert "gpio_get_level(DURA_GPIO_FLOW_B)" in flow_isr.group("body")

required = "CONFIG_GPIO_CTRL_FUNC_IN_IRAM=y"
assert required in active_config, (
    "active sdkconfig must keep GPIO control functions in IRAM because flow_isr "
    "calls gpio_get_level while NVS can disable the flash cache"
)
assert required in default_config, (
    "sdkconfig.defaults must preserve GPIO IRAM placement across clean/reconfigured builds"
)

print("FLOW_ISR_CACHE_SAFETY_CONTRACT_PASS")
