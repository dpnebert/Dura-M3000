#!/usr/bin/env python3
"""Reference-derived Help frames + real production board/meter navigation (host only).

No target build or hardware. Requires the retained legacy source; all generated
files go beneath --out. Reuses the existing actual-C board/LCD fixture, unchanged.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

from test_home_volume_ui import generate, config_defines
from test_safe_output_ownership import function
from test_calibration_save_recovery import enum

ROOT = Path(__file__).resolve().parents[1]
REFERENCE = Path('/home/omi/work_projects/dura/AVR/reference_source/Release_1_1_3/Src/Meter_Project')


def reference_data(ref):
    config = (ref/'Configuration.mbas').read_text(encoding='latin1')
    language = (ref/'Language.mbas').read_text(encoding='latin1')
    # Ignore misleading stale numeric comments; parse actual MenuItem entries.
    table = config.split('const Menulist as MenuItem[MAX_MENU] = (', 1)[1]
    rows = re.findall(r'^\s*\(([^()]+)\),?', table, re.M)[:60]
    rows = [[x.strip() for x in row.split(',')] for row in rows]
    assert len(rows) == 60 and all(len(row) == 12 for row in rows)
    strings = dict(re.findall(r'^const (\w+)\s*=\s*"([^"]*)"\s+code', language, re.M))
    def titles(index):
        return [strings[x[1:]] if x.startswith('@') else None for x in rows[index][2:7]]
    assert rows[49][1] == 'ADD_ON_ITEMS+1' and rows[50][1] == 'ADD_ON_ITEMS'
    assert rows[51][1] == 'DURAMMERCIAL_ITEM+1' and rows[52][1] == 'DURAMMERCIAL_ITEM'
    assert rows[53][1] == 'SWV_ADDON_ITEM+1' and rows[54][8] == '@swvButton2'
    assert all(titles(i) == titles(50) for i in (52,54))
    assert rows[25][0] == 'INFO_CENTER_ITEM+8'  # More -> Info start, NOT wrap.
    assert 'buttonOp3 = @startInfoFunc' in config and '@QRcodeData,  128, 64' in config
    info = [titles(i) for i in range(18,26)]
    # GS §7.2 and approved Main Menu supersede legacy Reset/Prog/Cal/Info
    # face-label guidance. No new navigation or button purpose is invented.
    info[3] = ['There are four physical', 'buttons on the meter body.',
               'Hold outside buttons', 'for Program Settings.', None]
    info[5][0] = 'Program Settings:'
    graphics = (ref/'Graphics.mbas').read_text(encoding='latin1')
    display = (ref/'Display.mbas').read_text(encoding='latin1')
    assert 'ptr = ptr + (i + ((j/8)*w))' in display
    assert 'if ((ptr^) and (1 << (j mod 8))) then' in display
    assert 'setpixel(x+i, y+j, color)' in display
    qr = re.search(r'const QRcodeData[^=]*=\s*\((.*?)\)', graphics, re.S)
    qr = bytes(int(x,16) for x in re.findall(r'0x([0-9a-fA-F]{2})',qr.group(1)))
    assert len(qr) == 1024
    return dict(info=info, contact=titles(50), reset=[titles(i) for i in range(15,18)],
                qr=list(qr), menus=rows)


def c_rows(name, rows):
    return 'static const char *const '+name+'[][5] = {\n'+''.join(
        '{'+','.join('NULL' if s is None else json.dumps(s) for s in row)+'},\n' for row in rows)+'};\n'


def previews(out):
    from PIL import Image, ImageDraw
    frames=[]
    for p in sorted(out.glob('*.fb')):
        data=p.read_bytes()
        image=Image.new('RGB',(128,64),'white')
        for y in range(64):
            for x in range(128):
                # Framebuffers use the board's physical Y flip; preview UI coordinates.
                physical_y=63-y
                if data[(physical_y//8)*128+x] & (1<<(physical_y%8)): image.putpixel((x,y),(0,0,0))
        image.resize((512,256),Image.Resampling.NEAREST).save(p.with_suffix('.png'))
        frames.append((p.stem,image))
    if frames:
        sheet=Image.new('RGB',(800,((len(frames)+1)//2)*235),'#dedede')
        draw=ImageDraw.Draw(sheet)
        for i,(name,im) in enumerate(frames):
            x=(i%2)*400;y=(i//2)*235
            draw.text((x+8,y+5),name,fill='black')
            sheet.paste(im.resize((384,192),Image.Resampling.NEAREST),(x+8,y+25))
        sheet.save(out/'contact-sheet.png')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--reference',type=Path,default=REFERENCE)
    parser.add_argument('--case',action='append')
    args=parser.parse_args()
    out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    data=reference_data(args.reference)
    (out/'reference-oracle.json').write_text(json.dumps(data,indent=2)+'\n')
    oracle=c_rows('reference_info',data['info'])+c_rows('reference_contact',[data['contact']])+c_rows('reference_reset',data['reset'])
    oracle+='static const uint8_t reference_qr[1024] = {'+','.join(map(str,data['qr']))+'};\n'
    (out/'help_reference_oracle.h').write_text(oracle)
    generate(ROOT,out)
    header=(ROOT/'components/dura_board/include/dura_board.h').read_text()
    ids=enum(header,'dura_ui_legacy_screen_id_t')+'\n'+enum(header,'dura_ui_legacy_menu_id_t')+'\n'
    ids+=function((ROOT/'components/dura_board/dura_board.c').read_text(),'ui_legacy_ids')+'\n'
    (out/'help_reference_ids.inc').write_text(ids)
    (out/'volume_config.h').write_text(config_defines(ROOT))
    inputs=[ROOT/'scripts/help_reference_completion_fixture.c',ROOT/'components/dura_meter/dura_meter.c',
            ROOT/'components/dura_meter/dura_control_policy.c',ROOT/'components/dura_board/dura_lcd_assets.c']
    cmd=['cc','-std=c11','-g','-pthread','-fsanitize=undefined','-Werror=implicit-function-declaration',
         '-I'+str(out),'-I'+str(ROOT/'tests'),'-I'+str(ROOT/'tests/recipe_ownership/include'),
         '-I'+str(ROOT/'components/dura_meter/include'),'-I'+str(ROOT/'components/dura_board/include'),
         *map(str,inputs),'-lm','-o',str(out/'test')]
    r=subprocess.run(cmd,capture_output=True,text=True)
    (out/'compile.log').write_text(' '.join(cmd)+'\n'+r.stdout+r.stderr)
    if r.returncode: print(r.stderr); return r.returncode
    cases=['setup48','setup50','setup53','setup-traversal','scan']
    cases += [f'info{i}' for i in range(8)] + ['info-navigation','info-entry']
    cases += [f'reset{i}' for i in range(3)] + ['held-calibration','safety','blank-buttons','wire-ids']
    results={}
    for case in args.case or cases:
        r=subprocess.run([str(out/'test'),case],cwd=out,capture_output=True,text=True,timeout=20)
        (out/(case+'.log')).write_text(r.stdout+r.stderr)
        results[case]=r.returncode
        print(('PASS ' if not r.returncode else 'FAIL ')+case)
        if r.returncode: print(r.stdout+r.stderr)
    (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    paths=[Path(__file__),ROOT/'components/dura_board/dura_board.c',ROOT/'components/dura_board/dura_st7567.c',
           ROOT/'scripts/test_home_volume_ui.py',ROOT/'tests/home_volume_ui.c',ROOT/'tests/safe_output.c',
           *inputs,*[args.reference/p for p in ('Configuration.mbas','Language.mbas','Graphics.mbas','Display.mbas')]]
    (out/'INPUT_SHA256.json').write_text(json.dumps({str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},indent=2)+'\n')
    previews(out)
    print(f'{sum(v==0 for v in results.values())}/{len(results)} passed')
    return int(any(results.values()))

if __name__=='__main__': sys.exit(main())
