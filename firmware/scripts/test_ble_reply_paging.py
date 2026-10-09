#!/usr/bin/env python3
"""Host-compile real reply transport/dispatcher/GATT callback; preserve RED separately.
SDK allocation, notification, connection and RTOS boundaries alone are substituted.
No target build/radio operations. --source permits replay against a frozen source.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
from test_active_ble_text_routing import function


def main():
    root = Path(__file__).resolve().parents[1]
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, default=root)
    p.add_argument('--out', type=Path, default=root.parent/'verification/transport/latest')
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    comp = a.source/'components/gama_blefob'
    paths = [comp/f'gama_blefob{x}' for x in ('_notify.c', '_commands.c', '_gatt.c', '_internal.h', '.c', '_util.c')]
    notify, commands, gatt, internal, core, util = [f.read_text() for f in paths]
    public = comp/'include/gama_blefob.h'
    constants = re.findall(r'^#define (?:GAMA_BLEFOB_TEXT_RESPONSE_CAPACITY|BLE_SHELL_TEXT_RESPONSE_CAPACITY|BLE_SHELL_REPLY_\w+).*', public.read_text()+'\n'+internal, re.M)
    # Full defines rather than regex capture names.
    constants = [line for line in (public.read_text()+'\n'+internal).splitlines() if re.match(r'#define (?:GAMA_BLEFOB_TEXT_RESPONSE_CAPACITY|BLE_SHELL_TEXT_RESPONSE_CAPACITY|BLE_SHELL_REPLY_\w+)\b', line)]
    generated = '\n'.join(constants)+'\n'
    generated += internal[internal.index('#define BLE_SHELL_CMD_PREFIX_LEN'):internal.index('/* Nordic UART')]
    generated += '\n'.join(re.findall(r'^char s_last_\w+\[[^;]+;', core, re.M))+'\n'
    generated += notify.replace('#include "gama_blefob_internal.h"', '')+'\n'
    for name in ('str_trim', 'ble_shell_parse_positive_u32', 'ble_shell_set_bool_from_arg'):
        generated += function(util, name)+'\n'
    generated += commands[commands.index('static bool ble_shell_is_maintenance_command'):commands.index('void ble_shell_task')]
    generated += function(commands, 'gama_blefob_dispatch_text')+'\n'
    generated += function(gatt, 'append_to_mbuf')+'\n'
    generated += function(gatt, 'ble_response_chr_access_cb')+'\n'
    inc = a.out/'reply_production.inc'
    inc.write_text(generated)
    fixture = root/'tests/ble_reply_paging_host.c'
    inputs = [*paths, public, fixture, Path(__file__), inc]
    (a.out/'INPUT_SHA256.json').write_text(json.dumps({str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}, indent=2)+'\n')
    cmd = ['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-parameter', '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-pthread', '-I'+str(a.out), str(fixture), '-o', str(a.out/'reply-test')]
    r = subprocess.run(cmd, capture_output=True, text=True)
    (a.out/'compile.log').write_text(' '.join(cmd)+'\n'+r.stdout+r.stderr)
    if r.returncode:
        print(r.stderr)
        return r.returncode
    cases = ['mtu23', 'mtu247', 'mtu256', 'mtu517', 'short', 'capacity', 'overflow', 'read', 'recovery', 'invalid', 'failures', 'gates', 'binary', 'concurrent']
    results = {}
    for case in cases:
        r = subprocess.run([str(a.out/'reply-test'), case], capture_output=True, text=True, timeout=30, env={**os.environ, 'ASAN_OPTIONS':'detect_leaks=1'})
        (a.out/(case+'.log')).write_text(r.stdout+r.stderr)
        results[case] = r.returncode
        print(('FAIL ' if r.returncode else 'PASS ')+case)
    (a.out/'results.json').write_text(json.dumps(results, indent=2)+'\n')
    return int(any(results.values()))


if __name__ == '__main__':
    raise SystemExit(main())
