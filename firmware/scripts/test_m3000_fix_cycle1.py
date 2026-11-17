#!/usr/bin/env python3
"""M3000 fix cycle 1 host tests (items 1, 4, 6): real board handlers/renderers,
real meter, genuine font/LCD pixel code; GPIO/NVS/clock are host seams only.
Not device, flash, power-loss or pump verification. Writes only under --out."""
import argparse, hashlib, json, subprocess, sys
from pathlib import Path
from test_home_volume_ui import generate, config_defines, ROOT

CASES = ['notice_null_safe', 'precal_empty_notices', 'precal_save_notices',
         'insufficient_flow', 'cal_help_pulses', 'factory_help_frame',
         'factory_seeded_cancel', 'factory_failures', 'factory_success']

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--source-root', type=Path, default=ROOT)
    a = p.parse_args()
    root = a.source_root.resolve(); out = a.out.resolve(); out.mkdir(parents=True, exist_ok=True)
    generate(root, out)
    d = out/'normal'; d.mkdir(exist_ok=True); (d/'volume_config.h').write_text(config_defines(root))
    inputs = [ROOT/'tests/m3000_fix_cycle1.c', root/'components/dura_meter/dura_meter.c',
              root/'components/dura_meter/dura_control_policy.c', root/'components/dura_board/dura_lcd_assets.c']
    cmd = ['cc', '-std=c11', '-g', '-pthread', '-fsanitize=undefined', '-Werror=implicit-function-declaration',
           *[f'-I{x}' for x in (d, out, ROOT/'tests', root/'tests/recipe_ownership/include',
                                root/'components/dura_meter/include', root/'components/dura_board/include')],
           *map(str, inputs), '-lm', '-o', str(d/'test')]
    r = subprocess.run(cmd, capture_output=True, text=True)
    (d/'compile.log').write_text(' '.join(cmd)+'\n'+r.stdout+r.stderr)
    if r.returncode:
        print(r.stderr); return r.returncode
    results = {}
    for case in CASES:
        r = subprocess.run([str(d/'test'), case], cwd=d, capture_output=True, text=True, timeout=30)
        (d/(case+'.log')).write_text(r.stdout+r.stderr); results[case] = r.returncode
        print(('PASS ' if not r.returncode else 'FAIL ')+case)
        if r.returncode: print((r.stdout+r.stderr)[-3000:])
    (out/'results.json').write_text(json.dumps(results, indent=2)+'\n')
    paths = [*inputs, root/'components/dura_board/dura_board.c', Path(__file__), out/'board_home_volume.inc']
    (out/'INPUT_SHA256.json').write_text(json.dumps({str(x): hashlib.sha256(x.read_bytes()).hexdigest() for x in paths}, indent=2)+'\n')
    print(f'{sum(v == 0 for v in results.values())}/{len(results)} passed')
    return int(any(results.values()))

if __name__ == '__main__':
    sys.exit(main())
