#!/usr/bin/env python3
"""Reset/history actual-source tests. No board/UI source or shared runner edits.

Each successful case is followed by a NEW process loading committed mock NVS.
Water-reset characterization remains held. Batch conversion now asserts adopted legacy truncation.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import re

MODES = ('idle', 'manual', 'auto', 'recirc', 'calibration', 'recipe', 'paused', 'fault', 'sleep')
TESTS = [f'factory_active_{mode}' for mode in MODES]
TESTS += ['factory_empty', 'factory_saturation', 'factory_water_unchanged',
          'factory_pending_activation', 'factory_pending_calibration']
TESTS += [f'{which}_failure{n}' for which in ('factory', 'batch', 'total') for n in range(1, 5)]
TESTS += [f'{which}_{mode}' for which in ('batch', 'total', 'meter') for mode in MODES]
TESTS += [f'{which}_ingress' for which in ('factory', 'batch', 'total')]
CHARACTERIZATIONS = ['characterize_rate', 'characterize_rounding']


def item1_reset_ui_persistence_preservation(root):
    """Exercise factory reset at setup item 9 + DOWN, not the item-3 totals reset."""
    board = (root/'components/dura_board/dura_board.c').read_text(encoding='utf-8')
    meter = (root/'components/dura_meter/dura_meter.c').read_text(encoding='utf-8')
    handler = extract_function(board, 'handle_setup_button')
    assert handler, 'setup button handler is missing'
    item8 = re.search(r's_ui_setup_item == 8u && b == DURA_BUTTON_DOWN[^}]*?s_ui_setup_item = 9u;', handler, re.S)
    item9 = re.search(
        r's_ui_setup_item == 9u && b == DURA_BUTTON_DOWN[^}]*?'
        r'dura_meter_factory_defaults\(\).*?dura_meter_save\(\).*?ui_go_home\(\);',
        handler, re.S)
    assert item8, 'setup item 8 does not advance to factory-reset confirmation item 9'
    assert item9, 'setup item 9 + DOWN does not factory-default, save, and return home'
    defaults = extract_function(meter, 'dura_meter_factory_defaults')
    save = extract_function(meter, 'dura_meter_save')
    assert defaults, 'dura_meter_factory_defaults() is missing'
    assert save, 'dura_meter_save() is missing'
    print('ITEM1_FACTORY_RESET_UI_PERSISTENCE_ORACLE_PASS')


def preserve_total_reset_coverage(root):
    """Keep the separate item-3 total-reset regression coverage intact."""
    board = (root/'components/dura_board/dura_board.c').read_text(encoding='utf-8')
    meter = (root/'components/dura_meter/dura_meter.c').read_text(encoding='utf-8')
    reset = extract_function(board, 'reset_current_total')
    assert reset, 'reset screen has no reset_current_total handler'
    assert 'case DURA_UI_RESET_TOTALS:' in board
    assert 'else if (b == DURA_BUTTON_DOWN) reset_current_total();' in board
    assert 'dura_meter_reset_meter_total()' in reset
    assert 'dura_meter_reset_batch_total()' in reset
    assert 'dura_meter_reset_total_total()' in reset
    for name in ('dura_meter_reset_meter_total', 'dura_meter_reset_batch_total',
                 'dura_meter_reset_total_total'):
        match = extract_function(meter, name)
        assert match, name + ' is missing'
        assert 'dura_meter_save()' in match, name + ' does not persist by production save path'
    print('ITEM1_TOTAL_RESET_PRESERVATION_PASS')


def extract_function(source, name):
    match = re.search(r'(?:^|\n)[^\n;]*\b' + re.escape(name) + r'\s*\([^;]*?\)\s*\{', source)
    if not match:
        return None
    start = source.find('{', match.start())
    depth, end = 0, start
    while end < len(source):
        if source[end] == '{':
            depth += 1
        elif source[end] == '}':
            depth -= 1
            if depth == 0:
                return source[match.start():end + 1]
        end += 1
    return None


def main():
    here = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=here)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--test', choices=TESTS + CHARACTERIZATIONS, action='append')
    args = parser.parse_args()
    root, out = args.source_root.resolve(), args.out.resolve()
    item1_reset_ui_persistence_preservation(root)
    preserve_total_reset_coverage(root)
    out.mkdir(parents=True, exist_ok=False)  # Fail closed: never overwrite RED/GREEN.
    inputs = [root/'components/dura_meter/dura_meter.c',
              root/'components/dura_meter/dura_control_policy.c',
              here/'tests/reference_reset_history_host.c']
    includes = [here/'tests/recipe_ownership/include', root/'components/dura_meter/include']
    provenance = inputs + [Path(__file__).resolve(), root/'components/dura_recipe/include/dura_json_builder.h']
    for include in includes:
        provenance += sorted(include.rglob('*.h'))
    hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in provenance}
    (out/'INPUT_SHA256.json').write_text(json.dumps(hashes, indent=2)+'\n')
    binary = out/'reference-reset-history'
    # Existing ESP_LOG stubs erase their arguments; suppress only unused logging symbols.
    cmd = ['cc', '-std=c11', '-O0', '-g', '-pthread', '-Wall', '-Wextra',
           '-Wno-unused-variable', '-Wno-unused-function', '-fsanitize=undefined', '-fno-sanitize-recover=all',
           '-fno-omit-frame-pointer']
    cmd += ['-I'+str(p) for p in includes] + [str(p) for p in inputs] + ['-lm', '-o', str(binary)]
    run = subprocess.run(cmd, capture_output=True, text=True)
    (out/'compile.log').write_text(' '.join(cmd)+'\n'+run.stdout+run.stderr)
    if run.returncode:
        print(run.stdout+run.stderr)
        return run.returncode
    if run.stderr:
        print(run.stderr)
    (out/'compiler.txt').write_text(subprocess.check_output(['cc', '--version'], text=True))
    env = dict(os.environ, UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    results, logs = {}, []
    for name in args.test or TESTS + CHARACTERIZATIONS:
        store = out/(name+'.nvs')
        entry = {'case_exit': None, 'reboot_exit': None, 'characterization_only': name in CHARACTERIZATIONS}
        log = ''
        for phase, command in [('case_exit', [str(binary), name, str(store)]),
                               ('reboot_exit', [str(binary), 'reboot', str(store)])]:
            try:
                run = subprocess.run(command, capture_output=True, text=True, timeout=10, env=env)
                entry[phase] = run.returncode
                log += '$ '+' '.join(command)+'\n'+run.stdout+run.stderr
            except subprocess.TimeoutExpired:
                entry[phase] = 124
                log += f'TIMEOUT {phase}\n'
            if entry[phase]:
                break
        results[name] = entry
        (out/(name+'.log')).write_text(log)
        passed = entry['case_exit'] == entry['reboot_exit'] == 0
        line = ('PASS ' if passed else 'FAIL ')+name
        print(line, flush=True)
        logs.append(line+'\n'+log)
    passed = sum(r['case_exit'] == r['reboot_exit'] == 0 for r in results.values())
    summary = f'{passed}/{len(results)} cases passed (each PASS includes a fresh-process reboot)'
    print(summary)
    (out/'results.json').write_text(json.dumps(results, indent=2)+'\n')
    (out/'run.log').write_text('\n'.join(logs)+'\n'+summary+'\n')
    drift = [p for p, digest in hashes.items() if hashlib.sha256(Path(p).read_bytes()).hexdigest() != digest]
    (out/'SOURCE_NONDRIFT.json').write_text(json.dumps({'changed_during_run': drift}, indent=2)+'\n')
    return int(passed != len(results) or bool(drift))


if __name__ == '__main__':
    sys.exit(main())
