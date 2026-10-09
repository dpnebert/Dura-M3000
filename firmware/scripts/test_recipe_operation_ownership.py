#!/usr/bin/env python3
"""Host behavior tests; no firmware build, extraction, source rewriting or hardware.
Default uses this candidate's production files. --source-root permits sealed RED
baseline inputs; --coordinator permits unchanged baseline coordinator with new meter.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

TESTS = [
    'invalidate_cancel', 'invalidate_home', 'invalidate_fault', 'invalidate_defaults',
    'invalidate_load', 'invalidate_sleep', 'duplicate_configured', 'duplicate_largest',
    'duplicate_smallest', 'duplicate_later', 'happy_pause_resume', 'abort_revokes',
    'abort_unrelated', 'peer_abort_unrelated', 'peer_failure', 'save_failure',
    'ack_timeout', 'timeout_unrelated', 'follower_cancel', 'follower_replay',
    'follower_ack_failure', 'follower_happy', 'late_complete', 'reinit_preserves_owner',
    'serialized_boundaries', 'pending_abort', 'unpublished_follower_abort', 'reserved_cancel', 'remote_first_cancel', 'unpublished_follower_replay', 'abort_preserves_newer_owner',
]


def main():
    here = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=here)
    parser.add_argument('--coordinator', type=Path)
    parser.add_argument('--out', type=Path, default=here.parent / 'verification/recipe-ownership/latest')
    parser.add_argument('--test', action='append', choices=TESTS)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    testdir = here / 'tests/recipe_ownership'
    root = args.source_root
    modules = ['dura_meter', 'dura_recipe', 'dura_coordinator']
    inputs = [root / f'components/{m}/{m}.c' for m in modules]
    if args.coordinator:
        inputs[-1] = args.coordinator
    sources = inputs + [root / 'components/dura_meter/dura_control_policy.c', testdir / 'ownership.c']
    includes = [testdir / 'include'] + [root / f'components/{m}/include' for m in modules + ['dura_peers', 'dura_control_policy']]
    cmd = ['cc', '-std=c11', '-O0', '-g', '-pthread', '-fsanitize=undefined', '-fno-omit-frame-pointer']
    cmd += ['-I' + str(p) for p in includes]
    cmd += [str(p) for p in sources] + ['-Wl,--wrap=dura_meter_claim_recipe', '-Wl,--wrap=dura_meter_release_recipe', '-lm', '-o', str(args.out / 'ownership')]
    provenance = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}
    for include in includes:
        for p in include.rglob('*.h'):
            provenance[str(p)] = hashlib.sha256(p.read_bytes()).hexdigest()
    (args.out / 'INPUT_SHA256.json').write_text(json.dumps(provenance, indent=2) + '\n')
    compiled = subprocess.run(cmd, capture_output=True, text=True)
    (args.out / 'compile.log').write_text(' '.join(cmd) + '\n' + compiled.stdout + compiled.stderr)
    if compiled.returncode:
        print(compiled.stderr)
        return compiled.returncode
    results = {}
    logs = []
    for name in args.test or TESTS:
        try:
            run = subprocess.run([str(args.out / 'ownership'), name], capture_output=True, text=True, timeout=10)
            log = run.stdout + run.stderr
            results[name] = run.returncode
        except subprocess.TimeoutExpired:
            log = 'TIMEOUT (possible lock leak/deadlock)\n'
            results[name] = 124
        (args.out / (name + '.log')).write_text(log)
        verdict = 'PASS' if results[name] == 0 else 'FAIL'
        print(f'{verdict} {name}')
        logs.append(f'{verdict} {name}\n{log}')
    summary = f'{sum(v == 0 for v in results.values())}/{len(results)} passed'
    print(summary)
    (args.out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    (args.out / 'run.log').write_text('\n'.join(logs) + '\n' + summary + '\n')
    return int(any(results.values()))


if __name__ == '__main__':
    sys.exit(main())
