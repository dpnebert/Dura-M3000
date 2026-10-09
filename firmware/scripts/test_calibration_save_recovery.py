#!/usr/bin/env python3
"""F01 behavioral tests: real meter + verbatim board calibration handlers.
Host pthread locks/NVS/GPIO stubs are not target flash, timing or LCD evidence.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
from test_safe_output_ownership import function

TESTS = [f'{mode}_{failure}_{action}' for mode in ('bucket', 'quick')
         for failure in ('open', 'write', 'commit') for action in ('retry', 'cancel')]
TESTS += [f'bucket_slot_{slot}' for slot in range(6)]
TESTS += ['bucket_liter', 'bucket_ounce', 'invalid_finish', 'late_write_retry']
TESTS += [f'{mode}_blocked_{action}' for mode in ('bucket', 'quick')
          for action in ('observe', 'cancel', 'defaults', 'load', 'home', 'fault', 'sleep')]
TESTS += ['bucket_waiting_cancel', 'quick_waiting_home', 'bucket_double_finish']


def enum(text, name):
    return re.search(r'typedef enum\s*\{[^}]*\}\s*' + name + r';', text).group()


def main():
    here = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=here)
    parser.add_argument('--out', type=Path, default=here.parent / 'verification/F01/latest')
    parser.add_argument('--test', action='append', choices=TESTS)
    args = parser.parse_args()
    root, out = args.source_root, args.out
    out.mkdir(parents=True, exist_ok=True)
    board_path = root / 'components/dura_board/dura_board.c'
    board_header = root / 'components/dura_board/include/dura_board.h'
    board = board_path.read_text()
    handle = function(board, 'dura_board_handle_button')
    cases = handle[handle.index('    case DURA_UI_CAL_START:'):handle.index('    case DURA_UI_BATCH_SET:')]
    render = function(board, 'dura_board_render_debug_screen')
    transitions = render[render.index('    if (meter.operation_mode'):render.index('    if (should_show_provisioning_qr')]
    # Keep real values and complete helper bodies; isolate only calibration cases
    # from the surrounding hardware/provisioning/non-calibration dispatch.
    generated = '#include "dura_meter.h"\n'
    generated += enum(board_header.read_text(), 'dura_button_t') + '\n'
    generated += enum(board, 'dura_ui_screen_t') + '\n'
    generated += '''static dura_ui_screen_t s_ui_screen;
static bool s_ui_cal_measured_amount_valid;
static float s_ui_cal_measured_amount;
static uint8_t s_ui_quick_ref, s_ui_help_page;
'''
    generated += '\n'.join(function(board, name) for name in (
        'ui_go_main', 'ui_go_home', 'ui_cancel_calibration_to_start',
        'choose_precal_fluid', 'store_precal_fluid'))
    generated += '\nstatic void ui_button(dura_button_t b) {\n dura_meter_snapshot_t meter = snap();\n switch(s_ui_screen) {\n' + cases + '\n default: CHECK(false); }\n}\n'
    generated += 'static void ui_refresh(void) {\n dura_meter_snapshot_t meter = snap();\n' + transitions + '}\n'
    generated += '''#define CONFIG_DURA_BOARD_ENABLE_PUMP_OUTPUT 1
#define CONFIG_DURA_BOARD_ENABLE_EV_OUTPUTS 1
#define DURA_GPIO_PUMP_OUTPUT 0
#define DURA_GPIO_RECIRC_EV_OUTPUT 1
#define DURA_GPIO_INJECT_EV_OUTPUT 2
#define DURA_PUMP_OUTPUT_ACTIVE_LEVEL 1
#define DURA_PUMP_OUTPUT_INACTIVE_LEVEL 0
#define DURA_RECIRC_EV_ACTIVE_LEVEL 1
#define DURA_RECIRC_EV_INACTIVE_LEVEL 0
#define DURA_INJECT_EV_ACTIVE_LEVEL 1
#define DURA_INJECT_EV_INACTIVE_LEVEL 0
'''
    generated += function(board, 'apply_meter_outputs') + '\n'
    generated += function(board, 'dura_board_sync_outputs') + '\n'
    generated += 'void test_board_bind(void) { CHECK(dura_meter_set_output_handler(apply_meter_outputs)==ESP_OK); }\n'
    extracted = out / 'board_calibration_ui.inc'
    extracted.write_text(generated)
    inputs = [root/'components/dura_meter/dura_meter.c', root/'components/dura_meter/dura_control_policy.c', here/'tests/calibration_save_recovery.c']
    includes = [out, here/'tests/recipe_ownership/include', root/'components/dura_meter/include']
    cmd = ['cc', '-std=c11', '-O0', '-g', '-pthread', '-fsanitize=undefined', '-fno-omit-frame-pointer']
    cmd += ['-I'+str(p) for p in includes] + [str(p) for p in inputs] + ['-lm', '-o', str(out/'calibration-save')]
    provenance = inputs + [extracted, board_path, board_header, here/'tests/safe_output.c', Path(__file__)]
    for include in includes[1:]:
        provenance += list(include.rglob('*.h'))
    (out/'INPUT_SHA256.json').write_text(json.dumps({str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in provenance},indent=2)+'\n')
    compiled = subprocess.run(cmd,capture_output=True,text=True)
    (out/'compile.log').write_text(' '.join(cmd)+'\n'+compiled.stdout+compiled.stderr)
    if compiled.returncode:
        print(compiled.stderr)
        return compiled.returncode
    results, logs = {}, []
    for name in args.test or TESTS:
        try:
            run = subprocess.run([str(out/'calibration-save'),name],capture_output=True,text=True,timeout=10)
            results[name], log = run.returncode, run.stdout+run.stderr
        except subprocess.TimeoutExpired:
            results[name], log = 124, 'TIMEOUT\n'
        (out/(name+'.log')).write_text(log)
        line = ('PASS ' if not results[name] else 'FAIL ')+name
        print(line, flush=True)
        logs.append(line+'\n'+log)
    summary = f'{sum(v == 0 for v in results.values())}/{len(results)} passed'
    print(summary)
    (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    (out/'run.log').write_text('\n'.join(logs)+'\n'+summary+'\n')
    return int(any(results.values()))

if __name__ == '__main__':
    sys.exit(main())
