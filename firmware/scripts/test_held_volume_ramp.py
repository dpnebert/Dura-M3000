#!/usr/bin/env python3
"""Real board hold consumer + meter, injected clock/hardware only; no device I/O."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
from test_home_volume_ui import generate, config_defines

ROOT = Path(__file__).resolve().parents[1]
DEFAULTS = {'CONFIG_DURA_BOARD_HOLD_DELAY_MS':400}
for unit,start,cap in [('GALLON',10,500),('LITER',40,2000),('OUNCE',100,5000)]:
    DEFAULTS.update({f'CONFIG_DURA_BOARD_HOLD_{unit}_START_CENTI':start,
                     f'CONFIG_DURA_BOARD_HOLD_{unit}_DOUBLING_MS':1000,
                     f'CONFIG_DURA_BOARD_HOLD_{unit}_MAX_CENTI':cap})

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,required=True)
    args=p.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    generate(ROOT,out)
    config=config_defines(ROOT)
    # Explicit proposed configuration for RED when new symbols do not exist yet.
    # GREEN uses actual sdkconfig values, with a separately varied menuconfig profile.
    actual={k:int(v) for k,v in re.findall(r'^(CONFIG_DURA_BOARD_HOLD_\w+)=(\d+)$',
             (ROOT/'sdkconfig').read_text(),re.M)}
    profiles={'normal':dict(DEFAULTS,**actual),'independent':{
        'CONFIG_DURA_BOARD_HOLD_DELAY_MS':250,
        'CONFIG_DURA_BOARD_HOLD_GALLON_START_CENTI':13,
        'CONFIG_DURA_BOARD_HOLD_GALLON_DOUBLING_MS':700,
        'CONFIG_DURA_BOARD_HOLD_GALLON_MAX_CENTI':91,
        'CONFIG_DURA_BOARD_HOLD_LITER_START_CENTI':27,
        'CONFIG_DURA_BOARD_HOLD_LITER_DOUBLING_MS':1700,
        'CONFIG_DURA_BOARD_HOLD_LITER_MAX_CENTI':123,
        'CONFIG_DURA_BOARD_HOLD_OUNCE_START_CENTI':101,
        'CONFIG_DURA_BOARD_HOLD_OUNCE_DOUBLING_MS':3000,
        'CONFIG_DURA_BOARD_HOLD_OUNCE_MAX_CENTI':345}}
    results={}
    inputs=[ROOT/'tests/held_volume_ramp.c',ROOT/'components/dura_meter/dura_meter.c',
            ROOT/'components/dura_meter/dura_control_policy.c',ROOT/'components/dura_board/dura_lcd_assets.c']
    for name,settings in profiles.items():
        d=out/name;d.mkdir(exist_ok=True)
        base='\n'.join(line for line in config.splitlines() if not line.startswith('#define CONFIG_DURA_BOARD_HOLD_'))
        (d/'volume_config.h').write_text(base+'\n'+'\n'.join(f'#define {k} {v}' for k,v in settings.items())+'\n')
        cmd=['cc','-std=c11','-g','-pthread','-fsanitize=undefined','-fno-sanitize-recover=undefined',
             '-Werror=implicit-function-declaration','-I'+str(d),'-I'+str(out),
             '-I'+str(ROOT/'tests/recipe_ownership/include'),'-I'+str(ROOT/'components/dura_meter/include'),
             '-I'+str(ROOT/'components/dura_board/include'),*map(str,inputs),'-lm','-o',str(d/'test')]
        run=subprocess.run(cmd,text=True,capture_output=True)
        (d/'build.log').write_text(run.stdout+run.stderr)
        results[name]={'build_exit':run.returncode,'cases':{}}
        if run.returncode:print(name,'COMPILE_FAIL',run.stderr[-4000:]);continue
        for case in ['rates','lifecycle','runtime_override']:
            run=subprocess.run([str(d/'test'),case],text=True,capture_output=True,timeout=40)
            (d/(case+'.log')).write_text(run.stdout+run.stderr)
            results[name]['cases'][case]={'exit':run.returncode,'stdout':run.stdout,'stderr':run.stderr}
            print(name,case,'exit',run.returncode,run.stdout[-300:],run.stderr[-500:])
    ok=all(v['build_exit']==0 and set(v['cases'])=={'rates','lifecycle','runtime_override'} and
           all(c['exit']==0 for c in v['cases'].values()) for v in results.values())
    files=inputs+[ROOT/'components/dura_board/dura_board.c',ROOT/'components/dura_board/dura_ui_behavior.c',
        ROOT/'components/dura_board/include/dura_ui_behavior.h',ROOT/'scripts/test_home_volume_ui.py',Path(__file__)]
    receipt={'status':'PASS' if ok else 'FAIL','results':results,'inputs':{str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in files}}
    (out/'RESULTS.json').write_text(json.dumps(receipt,indent=2)+'\n')
    return 0 if ok else 1
if __name__=='__main__':raise SystemExit(main())
