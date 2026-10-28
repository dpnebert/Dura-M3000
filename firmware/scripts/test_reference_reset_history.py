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

MODES = ('idle', 'manual', 'auto', 'recirc', 'calibration', 'recipe', 'paused', 'fault', 'sleep')
TESTS = [f'factory_active_{mode}' for mode in MODES]
TESTS += ['factory_empty', 'factory_saturation', 'factory_water_unchanged',
          'factory_pending_activation', 'factory_pending_calibration']
TESTS += [f'{which}_failure{n}' for which in ('factory', 'batch', 'total') for n in range(1, 5)]
TESTS += [f'{which}_{mode}' for which in ('batch', 'total', 'meter') for mode in MODES]
TESTS += [f'{which}_ingress' for which in ('factory', 'batch', 'total')]
CHARACTERIZATIONS = ['characterize_rate', 'characterize_rounding']


def main():
    here = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=here)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--test', choices=TESTS + CHARACTERIZATIONS, action='append')
    args = parser.parse_args()
    root, out = args.source_root.resolve(), args.out.resolve()
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
