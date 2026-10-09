#!/usr/bin/env python3
"""Compile verbatim production board owner/init/input/LCD paths with real meter.
Only platform, NVS, LCD drawing leaves and unrelated menu leaves are shimmed.
No rewritten policy or owner loop. Evidence includes extracted C and input hashes.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from test_active_ble_text_routing import function


def generate_board(root, out):
    board = root/'components/dura_board/dura_board.c'
    lcd = root/'components/dura_board/dura_st7567.c'
    text = board.read_text()
    ui = text[text.index('typedef enum {\n    DURA_UI_MAIN_MENU'):text.index('static bool s_low_battery_blink_active;')]
    event = re.search(r'typedef struct\s*\{[^}]*\}\s*dura_flow_event_t;', text).group()
    generated = ui+'\n'+event+'\n'
    # New helpers are included if present, allowing the identical tests to run RED.
    for name in ['dura_board_set_ble_connected', 'dura_board_note_activity',
                 'dura_board_note_user_activity', 'update_backlight']:
        if re.search(r'\b'+name+r'\([^;]*\)\s*\{', text):
            generated += function(text, name)+'\n'
    for name in ['flow_isr', 'flow_event_counts', 'dura_board_take_flow_pulses', 'flow_boundary',
                 'ui_go_main', 'ui_go_home', 'adjust_batch_amount_tenths',
                 'ui_cancel_batch_to_home',
                 'dura_board_handle_button', 'ui_button_is_repeatable',
                 'handle_external_recirc_press', 'dura_board_enqueue_button', 'dispatch_ui_button',
                 'board_task']:
        generated += function(text, name)+'\n'
    generated += function(lcd.read_text(), 'dura_lcd_set_backlight')+'\n'
    generated += function(lcd.read_text(), 'dura_lcd_init')+'\n'
    generated += function(text, 'dura_board_init')+'\n'
    (out/'board_backlight.inc').write_text(generated)
    return board, lcd


def main():
    root = Path(__file__).resolve().parents[1]
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', required=True, type=Path)
    a = p.parse_args(); out = a.out.resolve(); out.mkdir(parents=True, exist_ok=True)
    board, lcd = generate_board(root, out)
    inputs = [root/'tests/backlight_policy.c', root/'components/dura_meter/dura_meter.c',
              root/'components/dura_meter/dura_control_policy.c']
    cases = ['boot_off', 'wake_off', 'boot_on', 'wake_on', 'cal_off', 'cal_timeout',
             'cal_wait_timeout', 'local_dark_action', 'local_hold', 'local_repeat',
             'wake_release', 'remote', 'remote_full', 'remote_invalid', 'flow', 'flow_bounce',
             'flow_boundary', 'recirc', 'recirc_rejected', 'ble_connect', 'ble_disconnect',
             'sleep_only_activity', 'sleep_60', 'ble_veto', 'off_events', 'setting_off', 'setting_on']
    cases += ['timeout_'+str(n) for n in range(10, 31)]
    cases += ['persisted_boot', 'persisted_wake', 'persisted_cal',
              'load_off', 'load_timed', 'load_open_error', 'load_missing',
              'load_invalid_marker', 'load_invalid_backlight', 'load_read_error', 'load_invalid_timed']
    results = {}
    for enabled in [0, 1]:
        cmd = ['cc', '-std=c11', '-g', '-pthread', '-Wall', '-Wextra',
               '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-unused-variable', '-Werror',
               '-fsanitize=undefined', '-DCONFIG_DURA_BOARD_ENABLE_RECIRC_INPUT='+str(enabled),
               '-I'+str(out), '-I'+str(root/'tests/recipe_ownership/include'),
               '-I'+str(root/'components/dura_meter/include'),
               '-I'+str(root/'components/dura_board/include'), *map(str, inputs),
               '-lm', '-o', str(out/('backlight-'+str(enabled)))]
        r = subprocess.run(cmd, capture_output=True, text=True)
        (out/('compile-'+str(enabled)+'.log')).write_text(' '.join(cmd)+'\n'+r.stdout+r.stderr)
        if r.returncode:
            print(r.stderr); return r.returncode
        for case in cases:
            r = subprocess.run([str(out/('backlight-'+str(enabled))), case], capture_output=True, text=True, timeout=10)
            key = str(enabled)+'_'+case
            results[key] = r.returncode
            (out/(key+'.log')).write_text(r.stdout+r.stderr)
            print(('FAIL ' if r.returncode else 'PASS ')+key+' '+r.stdout+r.stderr, end='')
    (out/'results.json').write_text(json.dumps(results, indent=2)+'\n')
    provenance = inputs+[board, lcd, Path(__file__), out/'board_backlight.inc']
    provenance += list((root/'components/dura_meter/include').glob('*.h'))
    provenance += list((root/'tests/recipe_ownership/include').rglob('*.h'))
    (out/'INPUT_SHA256.json').write_text(json.dumps({str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in provenance}, indent=2)+'\n')
    return int(any(results.values()))

if __name__ == '__main__':
    raise SystemExit(main())
