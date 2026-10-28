#!/usr/bin/env python3
"""Candidate43 owner-authorized UI tests: real handlers/meter/fonts/pixels, no device."""
import argparse, hashlib, json, subprocess
from pathlib import Path
from test_home_volume_ui import generate, config_defines, ROOT

def main():
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True)
    p.add_argument('--group',choices=('batch','cal','all'),default='all');a=p.parse_args()
    out=a.out.resolve();out.mkdir(parents=True,exist_ok=True);generate(ROOT,out)
    configs={'normal':config_defines()}
    if a.group!='cal':
        for u in ('GALLON','LITER','OUNCE'):
            for d in (0,1,2):
                configs[f'{u}-{d}']='\n'.join(f'#define CONFIG_DURA_BOARD_VOLUME_{v}_DECIMALS {d if u==v else 2}' for v in ('GALLON','LITER','OUNCE'))+'\n#define CONFIG_DURA_BOARD_VOLUME_2DP_LIMIT_TENTHS 1999\n'
    results={}
    for cfg,defines in configs.items():
        dest=out/cfg;dest.mkdir(exist_ok=True);(dest/'volume_config.h').write_text(defines)
        inputs=[ROOT/'tests/ui_ready_scope.c',ROOT/'components/dura_meter/dura_meter.c',ROOT/'components/dura_meter/dura_control_policy.c',ROOT/'components/dura_board/dura_lcd_assets.c']
        cmd=['cc','-std=c11','-g','-pthread','-fsanitize=undefined','-Werror=implicit-function-declaration',*[f'-I{x}' for x in (dest,out,ROOT/'tests/recipe_ownership/include',ROOT/'components/dura_meter/include',ROOT/'components/dura_board/include')],*map(str,inputs),'-lm','-o',str(dest/'test')]
        r=subprocess.run(cmd,capture_output=True,text=True);(dest/'compile.log').write_text(' '.join(cmd)+'\n'+r.stdout+r.stderr)
        if r.returncode: print(r.stderr);return r.returncode
        cases=(['precision','retention','live_units','navigation','external','range'] if a.group!='cal' else [])+(['manual_pixels','settings','lifetime','cal_help','cal_units','precal','cal_feedback','quick','notice_fit'] if a.group!='batch' else [])
        cases += ['hold_precision','hold_calibration','popup_auto','popup_recirc','popup_batch','popup_flow_error']
        if cfg!='normal':cases=['precision','range','hold_precision','hold_calibration']
        for case in cases:
            r=subprocess.run([str(dest/'test'),case],cwd=dest,capture_output=True,text=True,timeout=20)
            (dest/(case+'.log')).write_text(r.stdout+r.stderr);results[cfg+'/'+case]=r.returncode
            print(('PASS ' if not r.returncode else 'FAIL ')+cfg+'/'+case)
            if r.returncode:print(r.stdout+r.stderr)
    paths=[*inputs,ROOT/'components/dura_board/dura_board.c',ROOT/'scripts/test_home_volume_ui.py',Path(__file__),out/'board_home_volume.inc']
    (out/'INPUT_SHA256.json').write_text(json.dumps({str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},indent=2)+'\n')
    (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    print(f'{sum(v==0 for v in results.values())}/{len(results)} passed');return int(any(results.values()))
if __name__=='__main__':raise SystemExit(main())
