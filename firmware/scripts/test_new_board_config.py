#!/usr/bin/env python3
"""PCB832 revX3 integration: real board header + verbatim production C paths.

Run standalone with Python and a host C compiler; ESP-IDF and hardware are not
required. Unchecked sdkconfig booleans are OMITTED from generated sdkconfig.h,
just as in IDF (not silently turned into explicit zeroes). Nine independent
absent/checked/explicit0 polarity/pullup combinations exercise header fallbacks.
Kconfig defaults are checked separately, never used to manufacture profile
values. Evidence includes commands, compiler/run logs, extracted C, and hashes.
This is host configuration/logic evidence, not electrical/target qualification.
"""
import argparse
import hashlib
import itertools
import json
from pathlib import Path
import re
import shlex
import subprocess

PROFILES = ('sdkconfig', 'sdkconfig.defaults', 'sdkconfig.customer-default')
GPIO = {'FLOW_A': 21, 'FLOW_B': 2, 'FIELD_SENSE_ADC': 8, 'RECIRC_INPUT': 35,
        'PUMP_OUTPUT': 47, 'RECIRC_EV_OUTPUT': 17, 'INJECT_EV_OUTPUT': 18,
        'LCD_BACKLIGHT': 14, 'KEY_1': 6, 'KEY_2': 7, 'KEY_3': 5, 'KEY_4': 4}
RECIRC = 'CONFIG_DURA_BOARD_RECIRC_INPUT_'


def parse_sdkconfig(text):
    values = {}
    for line in text.splitlines():
        match = re.fullmatch(r'(CONFIG_\w+)=(.*)', line)
        unset = re.fullmatch(r'# (CONFIG_\w+) is not set', line)
        if match:
            key, value = match.groups()
        elif unset:
            key, value = unset.group(1), 'n'
        else:
            continue
        if key in values and values[key] != value:
            raise ValueError(f'conflicting duplicate config: {key}')
        values[key] = value
    return values


def sdk_header(values):
    return '#pragma once\n' + ''.join(
        f'#define {key} {"1" if value == "y" else value}\n'
        for key, value in sorted(values.items()) if value != 'n')


def function(text, name):
    """Extract the actual definition, not its prototype or a parallel model."""
    match = re.search(r'^\w[^;{}]*\b' + re.escape(name) + r'\([^;]*?\)\s*\{', text, re.M)
    if not match:
        raise ValueError(f'missing production function: {name}')
    start = text.index('{', match.start())
    depth, end = 1, start + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[match.start():end]


def generated_production(root):
    board = (root/'components/dura_board/dura_board.c').read_text()
    lcd = (root/'components/dura_board/dura_st7567.c').read_text()
    # Include actual enum, button/flow tables, event type, and production constant.
    ui = re.search(r'typedef enum \{\s*DURA_UI_MAIN_MENU.*?\}\s*\w+;', board, re.S)
    tables = board[board.index('typedef struct {\n    dura_button_t id;'):
                   board.index('static uint8_t s_last_valid_reed;')]
    constant = re.search(r'^#define DURA_RECIRC_BATCH_DEFAULT_GAL .*$', board, re.M).group()
    result = ui.group()+'\n'+tables+'\n'+constant+'\n'
    for name in ('dura_board_button_wake_mask', 'dura_board_flow_wake_mask',
                 'dura_board_combined_wake_mask', 'dura_board_wake_group_from_ext1_status',
                 'apply_meter_outputs', 'dura_board_init'):
        result += function(board, name)+'\n'
    result += function(lcd, 'dura_lcd_set_backlight')+'\n'
    # The loop's exact Recirc sampling/edge/dispatch block, including feature gate.
    # Only the unrelated infinite task loop is replaced by an explicit test tick.
    gate = '#if CONFIG_DURA_BOARD_ENABLE_RECIRC_INPUT && !CONFIG_DURA_BOARD_LCD_HOLD_SPLASH_ON_BOOT'
    poll_start = board.rfind(gate, 0, board.index('        const bool recirc_pressed ='))
    poll_end = board.index('#endif', poll_start) + len('#endif')
    result += gate+'\n'+function(board, 'handle_external_recirc_press')+'\n#endif\n'
    result += 'static void production_recirc_tick(void) {\n'+board[poll_start:poll_end]+'\n}\n'
    return result


GPIO_STUB = '''#pragma once
#include <stdint.h>
typedef int gpio_num_t;
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; } gpio_config_t;
enum { GPIO_MODE_INPUT=1, GPIO_MODE_OUTPUT=2, GPIO_PULLUP_DISABLE=0,
       GPIO_PULLUP_ENABLE=1, GPIO_PULLDOWN_DISABLE=0, GPIO_PULLDOWN_ENABLE=1,
       GPIO_INTR_DISABLE=0, GPIO_INTR_NEGEDGE=1, GPIO_INTR_POSEDGE=2, GPIO_INTR_ANYEDGE=3 };
int gpio_config(const gpio_config_t *config);
int gpio_set_level(gpio_num_t pin, int level);
int gpio_get_level(gpio_num_t pin);
'''


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=root/'build-host-tests/new-board')
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    results = []

    def check(name, actual, expected):
        results.append({'name': name, 'actual': actual, 'expected': expected,
                        'pass': actual == expected})

    configs = {name: parse_sdkconfig((root/name).read_text()) for name in PROFILES}
    for profile, values in configs.items():
        for pin, expected in GPIO.items():
            key = 'CONFIG_DURA_BOARD_GPIO_'+pin
            check(profile+'/'+key, values.get(key), str(expected))
        for feature in ('HARDWARE_TASK', 'PUMP_OUTPUT', 'EV_OUTPUTS', 'RECIRC_INPUT', 'DEEP_SLEEP'):
            key = 'CONFIG_DURA_BOARD_ENABLE_'+feature
            check(profile+'/'+key, values.get(key), 'y')
        for key in ('CONFIG_SPIRAM', 'CONFIG_DURA_BOARD_LCD_BACKLIGHT_ACTIVE_LOW',
                    RECIRC+'ACTIVE_LOW', RECIRC+'PULLUP', RECIRC+'PULLDOWN'):
            # Require explicitly disabled in the profile: omission would permit
            # a future Kconfig default to silently change clean/reconfigured builds.
            value = values.get(key)
            check(profile+'/'+key+' disabled', value in ('n', '0'), True)
        check(profile+'/8MB choice', values.get('CONFIG_ESPTOOLPY_FLASHSIZE_8MB'), 'y')
        check(profile+'/8MB string', values.get('CONFIG_ESPTOOLPY_FLASHSIZE'), '"8MB"')
        other_sizes = [key for key, val in values.items()
                       if re.fullmatch(r'CONFIG_ESPTOOLPY_FLASHSIZE_\d+MB', key)
                       and key != 'CONFIG_ESPTOOLPY_FLASHSIZE_8MB' and val == 'y']
        check(profile+'/no other flash size selected', other_sizes, [])
        check(profile+'/target', values.get('CONFIG_IDF_TARGET'), '"esp32s3"')
        check(profile+'/PSRAM legacy alias off', values.get('CONFIG_ESP32S3_SPIRAM_SUPPORT', 'n') in ('n', '0'), True)
        assigned_spares = {key: val for key, val in values.items()
                           if key.startswith('CONFIG_DURA_BOARD_GPIO_') and val in ('38', '48')}
        check(profile+'/spares 38 and 48 not assigned', assigned_spares, {})

    kconfig = (root/'components/dura_board/Kconfig.projbuild').read_text()
    # Preserve the existing Kconfig keypad defaults (4/5/6/7), independently of
    # authorized normal/customer profile overrides (6/7/5/4). No keypad swap.
    kexpected = {'DURA_BOARD_GPIO_'+key: str(val) for key, val in GPIO.items()}
    kexpected.update({'DURA_BOARD_GPIO_KEY_1': '4', 'DURA_BOARD_GPIO_KEY_2': '5',
                      'DURA_BOARD_GPIO_KEY_3': '6', 'DURA_BOARD_GPIO_KEY_4': '7',
                      'DURA_BOARD_RECIRC_INPUT_ACTIVE_LOW': 'n',
                      'DURA_BOARD_RECIRC_INPUT_PULLUP': 'n',
                      'DURA_BOARD_RECIRC_INPUT_PULLDOWN': 'n',
                      'DURA_BOARD_LCD_BACKLIGHT_ACTIVE_LOW': 'n',
                      'DURA_BOARD_RECIRC_EV_OUTPUT_ACTIVE_LOW': 'n',
                      'DURA_BOARD_INJECT_EV_OUTPUT_ACTIVE_LOW': 'n'})
    for key, expected in kexpected.items():
        block = re.search(r'^config '+key+r'\s*\n(.*?)(?=^(?:config|menu|endmenu|choice|endchoice)\b|\Z)', kconfig, re.M | re.S)
        defaults = re.findall(r'^\s+default\s+([^\n]+)', block.group(1), re.M) if block else []
        check('Kconfig/'+key, defaults, [expected])

    generated = out/'production.inc'
    generated.write_text(generated_production(root))
    fixtures = root/'tests/new_board_config_host.c'
    variants = [(name, values.copy(), None) for name, values in configs.items()]
    for pump, ev, recirc in itertools.product(('n', 'y'), repeat=3):
        values = configs['sdkconfig'].copy()
        for name, value in [('PUMP_OUTPUT', pump), ('EV_OUTPUTS', ev), ('RECIRC_INPUT', recirc)]:
            values['CONFIG_DURA_BOARD_ENABLE_'+name] = value
        variants.append((f'gates-{pump}-{ev}-{recirc}', values, 'gates'))
    for enabled, active, pullup in itertools.product(('n', 'y'), ('absent', 'checked', 'explicit0'), ('absent', 'checked', 'explicit0')):
        values = configs['sdkconfig'].copy()
        values['CONFIG_DURA_BOARD_ENABLE_RECIRC_INPUT'] = enabled
        for suffix, mode in (('ACTIVE_LOW', active), ('PULLUP', pullup)):
            values.pop(RECIRC+suffix, None)
            if mode != 'absent':
                values[RECIRC+suffix] = 'y' if mode == 'checked' else '0'
        variants.append((f'recirc-{enabled}-{active}-{pullup}', values,
                         (0 if active == 'checked' else 1, 1 if pullup == 'checked' else 0)))

    harness_errors = []
    for label, values, polarity in variants:
        case = out/label
        (case/'driver').mkdir(parents=True, exist_ok=True)
        (case/'hal').mkdir(exist_ok=True)
        (case/'sdkconfig.h').write_text(sdk_header(values))
        (case/'driver/gpio.h').write_text(GPIO_STUB)
        (case/'hal/adc_types.h').write_text('#pragma once\n#define ADC_ATTEN_DB_12 12\n')
        (case/'esp_err.h').write_text('#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_ERR_NO_MEM 257\n')
        cmd = ['cc', '-std=c11', '-g', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-unused-variable',
               '-fsanitize=undefined', '-fno-sanitize-recover=all',
               '-I'+str(case), '-I'+str(out),
               '-I'+str(root/'components/dura_board/include'),
               '-I'+str(root/'components/dura_meter/include')]
        if polarity == 'gates':
            cmd += ['-DTEST_GATES_ONLY=1']
        elif polarity is not None:
            cmd += ['-DTEST_POLARITY_ONLY=1', '-DTEST_ACTIVE_LEVEL='+str(polarity[0]),
                    '-DTEST_PULLUP='+str(polarity[1])]
        cmd += [str(fixtures), str(root/'components/dura_meter/dura_control_policy.c'),
                '-o', str(case/'host')]
        compiled = subprocess.run(cmd, capture_output=True, text=True)
        (case/'compile.log').write_text(shlex.join(cmd)+'\n'+compiled.stdout+compiled.stderr)
        if compiled.returncode:
            harness_errors.append(label+': compile failed; see '+str(case/'compile.log'))
            continue
        run = subprocess.run([str(case/'host')], capture_output=True, text=True, timeout=10)
        (case/'run.log').write_text(run.stdout+run.stderr)
        matched = runtime_failures = 0
        for line in run.stdout.splitlines():
            match = re.fullmatch(r'(PASS|FAIL) (.*?) actual=(-?\d+) expected=(-?\d+)', line)
            if match:
                status, name, actual, expected = match.groups()
                check(label+'/'+name, int(actual), int(expected))
                runtime_failures += int(actual) != int(expected)
                if (status == 'PASS') != (int(actual) == int(expected)):
                    harness_errors.append(label+': inconsistent assertion status')
                matched += 1
        expected_rc = int(runtime_failures != 0)
        footer = re.findall(r'^HARNESS_COMPLETE checks=(\d+) failures=(\d+)$', run.stdout, re.M)
        # Require normal fixture completion and C-only exit/assertion consistency.
        # Static profile failures do not alter an executable's expected status.
        if (not matched or run.returncode != expected_rc or run.stderr or
                footer != [(str(matched), str(runtime_failures))]):
            harness_errors.append(label+': invalid runtime; see '+str(case/'run.log'))

    inputs = [Path(__file__), fixtures, generated,
              root/'components/dura_board/dura_board.c',
              root/'components/dura_board/dura_st7567.c',
              root/'components/dura_board/include/dura_board.h',
              root/'components/dura_board/Kconfig.projbuild',
              root/'components/dura_meter/include/dura_meter.h',
              root/'components/dura_meter/include/dura_control_policy.h',
              root/'components/dura_meter/dura_control_policy.c'] + [root/name for name in PROFILES]
    (out/'INPUT_SHA256.json').write_text(json.dumps(
        {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}, indent=2)+'\n')
    failures = [item for item in results if not item['pass']]
    report = {'checks': len(results), 'passed': len(results)-len(failures),
              'failed': len(failures), 'harness_errors': harness_errors, 'results': results}
    (out/'results.json').write_text(json.dumps(report, indent=2)+'\n')
    lines = [f"FAIL {item['name']}: actual={item['actual']!r} expected={item['expected']!r}" for item in failures]
    lines += ['HARNESS_ERROR '+error for error in harness_errors]
    lines += [f"NEW_BOARD_CONFIG: {report['checks']} checks, {report['passed']} passed, {report['failed']} failed, {len(harness_errors)} harness errors"]
    log = '\n'.join(lines)+'\n'
    (out/'run.log').write_text(log)
    print(log, end='')
    return 2 if harness_errors else int(bool(failures))


if __name__ == '__main__':
    raise SystemExit(main())
