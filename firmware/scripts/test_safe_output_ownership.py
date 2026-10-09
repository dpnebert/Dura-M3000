#!/usr/bin/env python3
"""Real meter C + verbatim board output functions, stub GPIO/NVS, real host locks.
This establishes call ordering/state invariants, not hardware latency or actuation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

TESTS = ['pause', 'cancel', 'home', 'fault', 'sleep', 'completion', 'defaults', 'load',
         'start_open_failure', 'start_write_failure', 'start_commit_failure', 'resume_failure',
         'pending_home', 'pending_defaults', 'pending_load', 'cal_units', 'cal_coefficients',
         'cal_manual_stop', 'cal_stale_batch_cancel', 'stale_snapshot', 'owner_exclusion',
         'owner_aba', 'sleep_prepare_guard', 'happy_recirc', 'redundant_sync', 'threaded_output_serialization']


def function(text, name):
    match = re.search(r'^.*\b' + name + r'\([^;]*?\)\s*\{', text, re.M)
    assert match, name
    start = text.index('{', match.start())
    depth, end = 1, start + 1
    while depth:
        if text[end] == '{': depth += 1
        elif text[end] == '}': depth -= 1
        end += 1
    return text[match.start():end]


def main():
    here = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=here)
    parser.add_argument('--out', type=Path, default=here.parent / 'verification/safe-output/latest')
    parser.add_argument('--test', action='append', choices=TESTS)
    args = parser.parse_args()
    root, out = args.source_root, args.out
    out.mkdir(parents=True, exist_ok=True)
    board = root / 'components/dura_board/dura_board.c'
    header = root / 'components/dura_meter/include/dura_meter.h'
    text = board.read_text()
    prefix = '''#include <assert.h>
#include "dura_meter.h"
#define CONFIG_DURA_BOARD_ENABLE_PUMP_OUTPUT 1
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
#define ESP_RETURN_ON_ERROR(x, ...) do { int err=(x); if(err) return err; } while(0)
extern int gpio_set_level(int pin, int level);
extern int test_board_snapshot(dura_meter_snapshot_t *snapshot);
#define dura_meter_get_snapshot test_board_snapshot
'''
    current = 'DURA_METER_HAS_SYNC_OUTPUT_HANDLER' in header.read_text()
    functions = ([function(text, 'apply_meter_outputs')] if current else [])
    functions += [function(text, 'dura_board_sync_outputs')]
    suffix = '\nvoid test_board_bind(void) {\n' + ('assert(dura_meter_set_output_handler(apply_meter_outputs) == ESP_OK);\n' if current else '') + '}\n'
    extracted = out / 'board_outputs.c'
    extracted.write_text(prefix + '\n'.join(functions) + suffix)
    test = here / 'tests/safe_output.c'
    inputs = [root/'components/dura_meter/dura_meter.c', root/'components/dura_meter/dura_control_policy.c', extracted, test]
    includes = [here/'tests/recipe_ownership/include', root/'components/dura_meter/include']
    cmd = ['cc', '-std=c11', '-O0', '-g', '-pthread', '-fsanitize=undefined', '-fno-omit-frame-pointer']
    cmd += ['-I'+str(p) for p in includes] + [str(p) for p in inputs] + ['-lm', '-o', str(out/'safe-output')]
    provenance = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs+[board,header]}
    for d in includes:
        for p in d.rglob('*.h'): provenance[str(p)] = hashlib.sha256(p.read_bytes()).hexdigest()
    (out/'INPUT_SHA256.json').write_text(json.dumps(provenance,indent=2)+'\n')
    compiled = subprocess.run(cmd, capture_output=True, text=True)
    (out/'compile.log').write_text(' '.join(cmd)+'\n'+compiled.stdout+compiled.stderr)
    if compiled.returncode:
        print(compiled.stderr)
        return compiled.returncode
    results, logs = {}, []
    for name in args.test or TESTS:
        try:
            run = subprocess.run([str(out/'safe-output'),name],capture_output=True,text=True,timeout=10)
            results[name], log = run.returncode, run.stdout+run.stderr
        except subprocess.TimeoutExpired:
            results[name], log = 124, 'TIMEOUT\n'
        (out/(name+'.log')).write_text(log)
        line = ('PASS ' if not results[name] else 'FAIL ')+name
        print(line)
        logs.append(line+'\n'+log)
    (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    (out/'run.log').write_text('\n'.join(logs)+'\n')
    return int(any(results.values()))

if __name__ == '__main__':
    sys.exit(main())
