#!/usr/bin/env python3
"""M3000 fix cycle 2 host tests (items 1, 2, 3): real board handlers and
renderers, real meter, genuine font/LCD pixel code; GPIO/NVS/clock are host
seams only. Adds per-draw coordinate/touch observers to the generated harness
so every drawn text row and the amount are bounded and non-overlapping on the
actual framebuffer. Not device, flash or pump verification. Writes only --out."""
import argparse, hashlib, json, subprocess, sys
from pathlib import Path
from test_home_volume_ui import generate, config_defines, ROOT

CASES = ['manual_layout', 'manual_ui_flow', 'operating_layout', 'batch_run_layout',
         'reset_direct_off', 'reset_pages', 'reset_scopes', 'reset_while_running',
         'factory_help_nav', 'precal_labels']

def instrument(inc: Path):
    s = inc.read_text()
    def rep(a, b):
        nonlocal s
        assert s.count(a) == 1, (a, s.count(a)); s = s.replace(a, b)
    rep('void dura_lcd_set_pixel(int x,int y,bool on) {\n    if(quantity_pixels && on) {',
        'void dura_lcd_set_pixel(int x,int y,bool on) {\n'
        '    if(quantity_pixels){if(x<tminx)tminx=x;if(x>tmaxx)tmaxx=x;if(y<tminy)tminy=y;if(y>tmaxy)tmaxy=y;}\n'
        '    if(quantity_pixels && on) {')
    rep('qminx=qminy=999;qmaxx=qmaxy=-1;', 'qminx=qminy=999;qmaxx=qmaxy=-1;tminx=tminy=999;tmaxx=tmaxy=-1;')
    rep('quantity_begin(text);actual_num40_right(x,y,text);',
        'quantity_begin(text);amt_x=x;amt_y=y;amt_kind=40;actual_num40_right(x,y,text);')
    rep('quantity_begin(text);actual_num24_text(x,y,text);',
        'quantity_begin(text);amt_x=x;amt_y=y;amt_kind=24;actual_num24_text(x,y,text);')
    rep('strcat(rendered_text,"\\n"); }', 'strcat(rendered_text,"\\n"); } text_log(orient,x,y,text);')
    s = ('static int tminx,tmaxx,tminy,tmaxy,amt_x,amt_y,amt_kind;\n'
         'static void text_log(char orient,int x,int y,const char *text);\n') + s
    inc.write_text(s)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--source-root', type=Path, default=ROOT)
    a = p.parse_args()
    root = a.source_root.resolve(); out = a.out.resolve(); out.mkdir(parents=True, exist_ok=True)
    generate(root, out)
    instrument(out/'board_home_volume.inc')
    d = out/'normal'; d.mkdir(exist_ok=True); (d/'volume_config.h').write_text(config_defines(root))
    inputs = [ROOT/'tests/m3000_fix_cycle2.c', root/'components/dura_meter/dura_meter.c',
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
        r = subprocess.run([str(d/'test'), case], cwd=d, capture_output=True, text=True, timeout=60)
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
