#!/usr/bin/env python3
"""Retired debug GPIO cannot own, configure or drive the production pump.

Standalone host C regression; no ESP-IDF or hardware required. Compile the real
board header, verbatim output setter and contiguous output-init block across
pump/EV gates and both pump/legacy debug polarities. Legacy defines are malicious
negative inputs, never an implementation of the retired feature. Static checks
also reject remnants in production code/Kconfig/all sdkconfig profiles.
Use --out for fresh evidence (commands, generated C, logs, hashes, JSON results).
"""
import argparse
import hashlib
import itertools
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys

# Keep standalone imports from writing cache files outside the evidence path.
sys.dont_write_bytecode = True
from test_new_board_config import GPIO_STUB, function, parse_sdkconfig, sdk_header


def generated_production(root):
    board = (root/'components/dura_board/dura_board.c').read_text()
    init = function(board, 'dura_board_init')
    start = init.index('    uint64_t output_mask = 0;')
    end = init.index('    /* Install only after the output GPIOs have been configured inactive. */', start)
    # Keep every gate/mask/write between these anchors, including obsolete code
    # when run against the RED source. Only unrelated boot/queue/LCD is omitted.
    return (function(board, 'apply_meter_outputs') + '\n'
            'static esp_err_t production_output_init(void) {\n' + init[start:end] +
            '    return ESP_OK;\n}\n')


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=root/'build-host-tests/debug-retirement')
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    results, harness_errors = [], []

    def check(kind, name, actual, expected):
        results.append({'kind': kind, 'name': name, 'actual': actual,
                        'expected': expected, 'pass': actual == expected})

    production_files = sorted(set(
        [p for base in ('components', 'main') for p in (root/base).rglob('*') if p.is_file()] +
        [p for p in root.glob('sdkconfig*') if p.is_file()]))
    remnants, menus = [], []
    for path in production_files:
        for lineno, line in enumerate(path.read_text().splitlines(), 1):
            if 'TICK_METER_DEBUG' in line:
                remnants.append(f'{path.relative_to(root)}:{lineno}:{line.strip()}')
            if re.search(r'Debug\s*/\s*test outputs', line, re.I):
                menus.append(f'{path.relative_to(root)}:{lineno}:{line.strip()}')
    check('static', 'no retired names in production or saved configs', remnants, [])
    check('static', 'no obsolete Debug / test outputs menu or saved headings', menus, [])

    generated = out/'production.inc'
    generated.write_text(generated_production(root))
    fixtures = root/'tests/debug_output_retirement_host.c'
    base = parse_sdkconfig((root/'sdkconfig').read_text())
    variants = []
    for pump, ev, pump_low, debug_low in itertools.product((0, 1), repeat=4):
        label = f'pump{pump}-ev{ev}-pumpLow{pump_low}-debugLow{debug_low}'
        case = out/label
        (case/'driver').mkdir(parents=True, exist_ok=True)
        (case/'hal').mkdir(exist_ok=True)
        values = base.copy()
        values.update({
            'CONFIG_DURA_BOARD_ENABLE_PUMP_OUTPUT': 'y' if pump else 'n',
            'CONFIG_DURA_BOARD_ENABLE_EV_OUTPUTS': 'y' if ev else 'n',
            'CONFIG_DURA_BOARD_PUMP_OUTPUT_ACTIVE_LOW': str(pump_low),
            'CONFIG_DURA_BOARD_ENABLE_LEGACY_POWER_OUTPUTS': 'n',
            # Negative-input coverage: even forcibly enabling the retired feature
            # at the colliding GPIO must not override production pump ownership.
            'CONFIG_DURA_BOARD_ENABLE_TICK_METER_DEBUG_OUTPUT': 'y',
            'CONFIG_DURA_BOARD_GPIO_TICK_METER_DEBUG': '47',
            'CONFIG_DURA_BOARD_TICK_METER_DEBUG_ACTIVE_LOW': str(debug_low),
        })
        (case/'sdkconfig.h').write_text(sdk_header(values))
        (case/'driver/gpio.h').write_text(GPIO_STUB)
        (case/'hal/adc_types.h').write_text('#pragma once\n#define ADC_ATTEN_DB_12 12\n')
        (case/'esp_err.h').write_text('#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n')
        cmd = ['cc', '-std=c11', '-g', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=undefined', '-fno-sanitize-recover=all',
               '-I'+str(case), '-I'+str(out),
               '-I'+str(root/'components/dura_board/include'),
               '-I'+str(root/'components/dura_meter/include'),
               f'-DTEST_PUMP={pump}', f'-DTEST_EV={ev}', f'-DTEST_PUMP_LOW={pump_low}',
               str(fixtures), '-o', str(case/'host')]
        compiled = subprocess.run(cmd, capture_output=True, text=True)
        (case/'compile.log').write_text(shlex.join(cmd)+'\n'+compiled.stdout+compiled.stderr)
        variant = {'name': label, 'compile_returncode': compiled.returncode}
        variants.append(variant)
        if compiled.returncode:
            harness_errors.append(label+': compile failed')
            continue
        try:
            run = subprocess.run([str(case/'host')], capture_output=True, text=True, timeout=10)
        except subprocess.TimeoutExpired as exc:
            (case/'run.log').write_text(str(exc)+'\n')
            harness_errors.append(label+': runtime timeout')
            continue
        (case/'run.log').write_text(shlex.join([str(case/'host')])+'\n'+run.stdout+run.stderr)
        matched = runtime_failures = 0
        for line in run.stdout.splitlines():
            match = re.fullmatch(r'(PASS|FAIL) (.*?) actual=(-?\d+) expected=(-?\d+)', line)
            if match:
                status, name, actual, expected = match.groups()
                actual, expected = int(actual), int(expected)
                check('behavioral', label+'/'+name, actual, expected)
                runtime_failures += actual != expected
                matched += 1
                if (status == 'PASS') != (actual == expected):
                    harness_errors.append(label+': inconsistent assertion status')
        footer = re.findall(r'^HARNESS_COMPLETE checks=(\d+) failures=(\d+)$', run.stdout, re.M)
        # Independent count stops an accidentally shortened/no-op fixture passing.
        expected_checks = 9 + 2*(4 + 9*(9 + pump + 2*ev))
        if (matched != expected_checks or run.returncode != int(bool(runtime_failures)) or
                run.stderr or footer != [(str(matched), str(runtime_failures))]):
            harness_errors.append(label+': invalid runtime; see '+str(case/'run.log'))
        variant.update({'run_returncode': run.returncode, 'checks': matched,
                        'failed': runtime_failures})

    inputs = production_files + [Path(__file__), root/'scripts/test_new_board_config.py', fixtures, generated]
    (out/'INPUT_SHA256.json').write_text(json.dumps(
        {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}, indent=2)+'\n')
    failures = [item for item in results if not item['pass']]
    report = {'checks': len(results), 'passed': len(results)-len(failures),
              'failed': len(failures), 'behavioral_failed': sum(item['kind'] == 'behavioral' for item in failures),
              'static_failed': sum(item['kind'] == 'static' for item in failures),
              'harness_errors': harness_errors, 'variants': variants, 'results': results}
    (out/'results.json').write_text(json.dumps(report, indent=2)+'\n')
    lines = [f"FAIL {item['name']}: actual={item['actual']!r} expected={item['expected']!r}" for item in failures]
    lines += ['HARNESS_ERROR '+error for error in harness_errors]
    lines += [f"DEBUG_OUTPUT_RETIREMENT: {len(variants)} variants, {report['checks']} checks, {report['passed']} passed, {report['failed']} failed ({report['behavioral_failed']} behavioral, {report['static_failed']} static), {len(harness_errors)} harness errors"]
    log = '\n'.join(lines)+'\n'
    (out/'run.log').write_text(log)
    print(log, end='')
    return 2 if harness_errors else int(bool(failures))


if __name__ == '__main__':
    raise SystemExit(main())
