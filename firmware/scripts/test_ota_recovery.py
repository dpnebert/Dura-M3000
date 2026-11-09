#!/usr/bin/env python3
"""Compile real OTA/provisioning C with injected SDK boundaries; no devices.
Run from any directory. Target TLS, bootloader and power-cut tests remain bench work.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import shutil
import subprocess
from test_safe_output_ownership import function


def body(path):
    return re.sub(r'^#include[^\n]*\n', '', path.read_text(), flags=re.M)


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=root/'verification/ota-recovery')
    a = parser.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    fixture = root/'tests/ota'
    sdk = (fixture/'sdk.h').read_text()
    ota = root/'main/dura_ota.c'
    app = (root/'main/app_main.c').read_text()
    board = (root/'components/dura_board/dura_board.c').read_text()
    cases = {
        'recovery': ['fresh_boot', 'verify_good', 'verify_bad', 'verify_mark_failure',
                     'verify_deadline', 'cleanup_retry', 'undefined_cleanup', 'slot_gate', 'task_failure',
                     'begin_failure', 'descriptor_failure', 'same_build', 'failed_build',
                     'wrong_project', 'interrupted', 'incomplete', 'finish_failure', 'success'],
        'integration': ['handoff_ok', 'handoff_veto', 'handoff_partial', 'handoff_start_failure',
                        'healthy', 'stalled_board', 'missing_ble', 'missing_commands', 'nvs_preserved',
                        'gesture', 'gesture_cancel', 'gesture_veto', 'ota_sleep_veto'],
        'wifi': ['success', 'failure', 'cancel', 'disconnect', 'credential_reset'],
    }
    sources = {
        'recovery': sdk + body(ota) + (fixture/'recovery.c').read_text(),
        'integration': sdk + body(ota) + '\n'.join(function(app,n) for n in
                       ['start_ota_wifi_provisioning','controller_boot_healthy','init_nvs']) +
                       '\nstatic int s_ui_screen; static bool s_ignore_buttons_until_release;\n'
                       'static int gesture_calls, gesture_result;\n'
                       'static int gesture_handler(void){++gesture_calls;return gesture_result;}\n'
                       'static int (*s_ota_provision_handler)(void)=gesture_handler;\n' +
                       function(board, 'handle_ota_gesture') + (fixture/'sleep_sdk.h').read_text() +
                       function(board, 'sleep_attempt_current') + (fixture/'integration.c').read_text(),
        'wifi': '#define TEST_WIFI\n' + sdk + body(root/'main/dura_wifi_provisioning.c') +
                (fixture/'wifi.c').read_text(),
    }
    results = {}
    for name, source in sources.items():
        c = a.out/(name+'.c')
        c.write_text(source)
        exe = a.out/name
        compiler = shutil.which('cc') or shutil.which('clang')
        if compiler is None:
            print('No C compiler found')
            return 1
        cmd = [compiler,'-std=c11','-Wall','-Werror=implicit-function-declaration',
               '-Wno-unused-function','-Wno-unused-variable','-Wno-misleading-indentation',
               '-g',str(c),'-o',str(exe)]
        if Path(compiler).stem.lower() != 'clang':
            cmd.insert(-3, '-fsanitize=undefined')
        r = subprocess.run(cmd, capture_output=True, text=True)
        (a.out/(name+'-compile.log')).write_text(r.stdout+r.stderr)
        if r.returncode:
            print(r.stderr)
            return r.returncode
        for case in cases[name]:
            r = subprocess.run([str(exe),case],capture_output=True,text=True,timeout=10)
            results[name+'/'+case] = r.returncode
            (a.out/(name+'-'+case+'.log')).write_text(r.stdout+r.stderr)
            print(('PASS ' if not r.returncode else 'FAIL ')+name+'/'+case)
    # Release inputs must select rollback and the correct CSV on clean builds.
    for name in ['sdkconfig','sdkconfig.defaults']:
        text = (root/name).read_text()
        assert 'CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y' in text
        assert 'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_ota.csv"' in text
    (a.out/'results.json').write_text(json.dumps(results,indent=2))
    files = [ota, root/'main/dura_wifi_provisioning.c', root/'main/app_main.c',
             root/'components/dura_board/dura_board.c',Path(__file__),*fixture.glob('*')]
    (a.out/'INPUT_SHA256.json').write_text(json.dumps({str(p.relative_to(root)):
        hashlib.sha256(p.read_bytes()).hexdigest() for p in files},indent=2))
    return int(any(results.values()))


if __name__ == '__main__':
    raise SystemExit(main())
