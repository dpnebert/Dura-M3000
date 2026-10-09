#!/usr/bin/env python3
"""Host-only real meter + verbatim board sleep transaction/decision, injected drivers."""
import argparse, hashlib, json, subprocess, sys
from pathlib import Path
from test_safe_output_ownership import function

def main():
    root = Path(__file__).resolve().parents[1]
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, default=root/'verification/F04/latest')
    a = p.parse_args(); a.out.mkdir(parents=True, exist_ok=True)
    board = root/'components/dura_board/dura_board.c'; text = board.read_text()
    names = ['dura_board_set_ble_connected', 'dura_board_note_activity',
             'dura_board_note_user_activity', 'update_backlight',
             'config_rtc_wake_input', 'prepare_deep_sleep_inputs',
             'restore_deep_sleep_inputs', 'sleep_attempt_current', 'dura_board_enter_deep_sleep']
    funcs = '\n'.join(function(text,n) for n in names if n+'(' in text)
    task = function(text,'board_task')
    decision = task[task.index('        dura_meter_snapshot_t sleep_meter;'):task.index('\n#endif',task.index('        dura_meter_snapshot_t sleep_meter;'))]
    fixture = (root/'tests/sleep_prepare_recovery.c').read_text()
    c = a.out/'sleep_transaction.c'
    c.write_text(fixture.replace('/* BOARD_FUNCTIONS */',funcs).replace('/* BOARD_DECISION */',decision))
    inputs=[c,root/'components/dura_meter/dura_meter.c',root/'components/dura_meter/dura_control_policy.c']
    cmd=['cc','-std=c11','-g','-pthread','-fsanitize=undefined','-I'+str(root/'tests/recipe_ownership/include'),'-I'+str(root/'components/dura_meter/include'),*[str(x) for x in inputs],'-lm','-o',str(a.out/'sleep-test')]
    r=subprocess.run(cmd,capture_output=True,text=True); (a.out/'compile.log').write_text(r.stdout+r.stderr)
    if r.returncode: print(r.stderr); return r.returncode
    cases=['save_open','save_write','save_commit','activity_save','ble_save','ble_aba_save','activity_rtc','ble_rtc','ble_aba_rtc','known_ble','decision_stale','restore_fail','retry']+['gpio_'+str(i) for i in range(1,32)]
    results={}; logs=[]
    for case in cases:
        r=subprocess.run([str(a.out/'sleep-test'),case],capture_output=True,text=True,timeout=10)
        results[case]=r.returncode; logs.append(case+'\n'+r.stdout+r.stderr)
        print(('PASS ' if r.returncode==0 else 'FAIL ')+case)
    (a.out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    (a.out/'run.log').write_text('\n'.join(logs))
    (a.out/'INPUT_SHA256.json').write_text(json.dumps({str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs+[board,root/'tests/sleep_prepare_recovery.c']},indent=2)+'\n')
    return int(any(results.values()))
if __name__=='__main__': sys.exit(main())
