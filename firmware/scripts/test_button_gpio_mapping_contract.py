#!/usr/bin/env python3
"""Static contract for M3000 physical left-to-right button numbering.

Dan's 2026-08-28 bench capture pressed the four switches from far left to far
right and produced logical IDs 4, 3, 1, 2 with the stale GPIO configuration.
The resulting physical GPIO order is 4, 5, 7, 6.  This contract keeps the
existing logical-key table and requires both mutable and clean-build configs to
map those physical positions to logical IDs 1, 2, 3, 4.
"""
import re
from pathlib import Path

project = Path(__file__).resolve().parents[1]
board_source = (project / "components/dura_board/dura_board.c").read_text(encoding="utf-8")

expected_button_table = (
    r'\{\s*DURA_BUTTON_UP\s*,\s*DURA_GPIO_KEY_4\s*,\s*"button1_left"',
    r'\{\s*DURA_BUTTON_DOWN\s*,\s*DURA_GPIO_KEY_3\s*,\s*"button2"',
    r'\{\s*DURA_BUTTON_SELECT\s*,\s*DURA_GPIO_KEY_2\s*,\s*"button3"',
    r'\{\s*DURA_BUTTON_BACK\s*,\s*DURA_GPIO_KEY_1\s*,\s*"button4_right"',
)
for pattern in expected_button_table:
    assert re.search(pattern, board_source), f"logical button table changed: {pattern}"

expected_gpio_by_key = {
    "KEY_1": 6,
    "KEY_2": 7,
    "KEY_3": 5,
    "KEY_4": 4,
}
for config_name in ("sdkconfig", "sdkconfig.defaults"):
    config = (project / config_name).read_text(encoding="utf-8")
    for key, gpio in expected_gpio_by_key.items():
        setting = f"CONFIG_DURA_BOARD_GPIO_{key}={gpio}"
        assert re.search(rf"^{re.escape(setting)}$", config, re.MULTILINE), (
            f"{config_name} must contain {setting} for left-to-right button IDs 1,2,3,4"
        )

print("BUTTON_GPIO_MAPPING_CONTRACT_PASS")
