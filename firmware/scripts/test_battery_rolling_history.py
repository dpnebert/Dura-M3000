#!/usr/bin/env python3
"""Host behavioral contract: compile exact battery production TUs, never extract/rewrite.

Only IDF ADC/time/log/GPIO/config headers are stubbed. GNU ld --wrap observes
conversion calls and delegates to the real, separately compiled battery math.
Every scenario starts a fresh process; no production-private state is accessed.
This is not target-build, electrical, timing-on-hardware, or UI acceptance.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parents[1]
BASIC = ["acquisition_counts", "startup_1_to_16", "overwrite_17", "multiple_wraps",
         "extreme_values", "mean_before_conversion", "timing_getter_purity",
         "idempotent_init"]
FAULTS = ["discard1", "discard2", "discard3", "discard4", "read", "calibration",
          "negative1", "negative_min"]
INIT_FAULTS = ["io", "unit_route", "channel_route", "new_unit", "channel", "calibration"]
CASES = BASIC + [f"error_{fault}_{filled}" for fault in FAULTS for filled in (0, 1, 15, 16, 19)]
CASES += [f"init_{fault}" for fault in INIT_FAULTS]

STUBS = {
    "esp_err.h": """#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_SUPPORTED 0x106
const char *esp_err_to_name(esp_err_t error);
""",
    "driver/gpio.h": "#pragma once\ntypedef int gpio_num_t;\n",
    "hal/adc_types.h": """#pragma once
typedef int adc_unit_t;
typedef int adc_channel_t;
#define ADC_UNIT_1 0
#define ADC_UNIT_2 1
#define ADC_CHANNEL_0 0
#define ADC_ATTEN_DB_12 3
#define ADC_BITWIDTH_DEFAULT 0
#define ADC_ULP_MODE_DISABLE 0
""",
    "esp_adc/adc_oneshot.h": """#pragma once
#include "esp_err.h"
#include "hal/adc_types.h"
typedef void *adc_oneshot_unit_handle_t;
typedef struct { adc_unit_t unit_id; int ulp_mode; } adc_oneshot_unit_init_cfg_t;
typedef struct { int atten; int bitwidth; } adc_oneshot_chan_cfg_t;
esp_err_t adc_oneshot_io_to_channel(int gpio, adc_unit_t *unit, adc_channel_t *channel);
esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *cfg, adc_oneshot_unit_handle_t *unit);
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t unit, adc_channel_t channel, const adc_oneshot_chan_cfg_t *cfg);
esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t unit);
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t unit, adc_channel_t channel, int *raw);
""",
    "esp_adc/adc_cali.h": """#pragma once
#include "esp_err.h"
typedef void *adc_cali_handle_t;
esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t cali, int raw, int *mv);
""",
    "esp_adc/adc_cali_scheme.h": """#pragma once
#include "esp_adc/adc_cali.h"
#include "hal/adc_types.h"
typedef struct { adc_unit_t unit_id; adc_channel_t chan; int atten; int bitwidth; } adc_cali_curve_fitting_config_t;
esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *cfg, adc_cali_handle_t *cali);
""",
    "esp_timer.h": "#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n",
    "esp_log.h": """#pragma once
void test_log(const char *tag, const char *format, ...) __attribute__((format(printf, 2, 3)));
#define ESP_LOGI test_log
#define ESP_LOGW test_log
""",
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=HERE)
    parser.add_argument("--out", type=Path, help="NEW evidence directory; omitted uses temporary output")
    args = parser.parse_args()
    root = args.source_root.resolve()
    if args.out is None:
        with tempfile.TemporaryDirectory(prefix="dura-battery-rolling-") as temporary:
            return run(root, Path(temporary) / "evidence")
    return run(root, args.out.resolve())


def run(root: Path, out: Path) -> int:
    out.mkdir(parents=True, exist_ok=False)
    # Reproduce actual sdkconfig values; the hardware gate alone is deliberately
    # selected twice on the command line. No production header is substituted.
    config = []
    for line in (root / "sdkconfig").read_text().splitlines():
        if line.startswith("CONFIG_") and "=" in line:
            key, value = line.split("=", 1)
            if key != "CONFIG_DURA_BOARD_ENABLE_HARDWARE_TASK":
                config.append(f"#define {key} {'1' if value == 'y' else value}")
    stubs = out / "stubs"
    for name, content in {**STUBS, "sdkconfig.h": "#pragma once\n" + "\n".join(config) + "\n"}.items():
        path = stubs / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)

    board = root / "components/dura_board"
    inputs = [board / "dura_battery.c", board / "dura_battery_math.c",
              HERE / "tests/battery_rolling_history_host.c"]
    provenance = inputs + [Path(__file__).resolve(), root / "sdkconfig"]
    provenance += [board / "include" / name for name in
                   ("dura_battery.h", "dura_battery_math.h", "dura_board.h")]
    provenance += sorted(stubs.rglob("*.h"))
    hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in provenance}
    (out / "INPUT_SHA256.json").write_text(json.dumps(hashes, indent=2) + "\n")
    commands, results, transcript = [], {}, []
    for enabled in (1, 0):
        binary = out / ("battery-enabled" if enabled else "battery-disabled")
        command = ["cc", "-std=c11", "-O0", "-g", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=undefined", "-fno-sanitize-recover=all",
                   f"-DCONFIG_DURA_BOARD_ENABLE_HARDWARE_TASK={enabled}",
                   "-I", str(stubs), "-I", str(board / "include")]
        command += [str(p) for p in inputs]
        command += ["-Wl,--wrap=dura_battery_apply_divider_mv",
                    "-Wl,--wrap=dura_battery_percent_from_mv", "-o", str(binary)]
        commands.append(command)
        compiled = subprocess.run(command, capture_output=True, text=True)
        (out / f"compile-{enabled}.log").write_text(shlex.join(command) + "\n" + compiled.stdout + compiled.stderr)
        if compiled.returncode:
            print(compiled.stdout + compiled.stderr)
            return compiled.returncode
        for case in CASES if enabled else ["disabled"]:
            cmd = [str(binary), case]
            commands.append(cmd)
            run = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
            log = run.stdout + run.stderr
            (out / f"{case}.log").write_text(shlex.join(cmd) + "\n" + log)
            results[case] = run.returncode
            line = ("PASS " if run.returncode == 0 else "FAIL ") + case
            print(line, flush=True)
            if run.returncode:
                print(log, end="", flush=True)
            transcript.append(line + "\n" + log)
    summary = f"{sum(code == 0 for code in results.values())}/{len(results)} passed"
    print(summary, flush=True)
    (out / "run.log").write_text("\n".join(transcript) + "\n" + summary + "\n")
    (out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    (out / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
    # Verify exact input bytes did not change during compilation/execution.
    assert hashes == {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in provenance}
    return int(any(results.values()))


if __name__ == "__main__":
    sys.exit(main())
