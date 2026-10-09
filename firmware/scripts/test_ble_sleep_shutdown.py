#!/usr/bin/env python3
"""Execute verbatim board sleep transaction plus real BLE lifecycle/advertising C.
SDK/RTOS boundaries are injected; no radio or target-timing claim.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from test_safe_output_ownership import function


def main():
    root = Path(__file__).resolve().parents[1]
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, default=root/'verification/ble-sleep')
    p.add_argument('--source', type=Path, default=root, help='Production tree; fixtures remain from this script (RED replay)')
    p.add_argument('--sdk', type=Path, default=Path('/home/omi/esp/esp-idf-v6.0.2'))
    a = p.parse_args(); a.out.mkdir(parents=True, exist_ok=True)
    board = a.source/'components/dura_board/dura_board.c'
    names = ['dura_board_set_ble_connected', 'dura_board_note_activity',
             'dura_board_note_user_activity', 'update_backlight',
             'config_rtc_wake_input', 'prepare_deep_sleep_inputs',
             'restore_deep_sleep_inputs', 'sleep_attempt_current', 'dura_board_enter_deep_sleep']
    base = (root/'tests/sleep_prepare_recovery.c').read_text().split('static void decision(void)')[0]
    base = base.replace('/* BOARD_FUNCTIONS */', '\n'.join(function(board.read_text(), n) for n in names))
    base = base.replace('void esp_deep_sleep_start(void){++sleep_calls;longjmp(slept,1);}',
                        'void esp_deep_sleep_start(void){++sleep_calls;longjmp(slept,1);}\n'
                        'int esp_deep_sleep_try_to_start(void);')

    # These are SDK boundaries in the historical preparation-only harness.
    base = base.replace('int gama_blefob_sleep_shutdown(void){return ESP_OK;}', '')
    base = base.replace('int gama_blefob_sleep_resume(void){return ESP_OK;}', '')
    base = base.replace('int esp_deep_sleep_try_to_start(void){esp_deep_sleep_start();return ESP_FAIL;}', '')
    base = base.replace('/* BOARD_FUNCTIONS */', '')
    base = base.replace('int dura_board_render_debug_screen(void){return ESP_OK;}', 'int dura_board_render_debug_screen(void){return !strcmp(test,"render_fail_radio")?ESP_FAIL:ESP_OK;}')
    base = base.replace('CHECK(!i.pump&&!i.recirc_ev&&!i.inject_ev);', 'if(s_sleep_recovery_pending)CHECK(!i.pump&&!i.recirc_ev&&!i.inject_ev);')
    base = base.replace('pthread_mutex_lock(m);return pdTRUE;', 'int b=test_sem_take(m);if(b>=0)return b;pthread_mutex_lock(m);return pdTRUE;')
    base = base.replace('void xSemaphoreGive(SemaphoreHandle_t m){pthread_mutex_unlock(m);}', 'void xSemaphoreGive(SemaphoreHandle_t m){if(!test_sem_give(m))pthread_mutex_unlock(m);}')
    base = '#include "ble_sleep_shim.h"\n' + base
    c = a.out/'board_sleep.c'
    c.write_text(base + '\n' + (root/'tests/ble_sleep_board.c').read_text())
    component = a.source/'components/gama_blefob'
    sources = [component/'gama_blefob.c', component/'gama_blefob_advertising.c', component/'gama_blefob_notify.c']
    if (component/'gama_blefob_sleep.c').exists():
        sources.append(component/'gama_blefob_sleep.c')
    import re
    (a.out/'ble_sleep_types.h').write_text(re.sub(r'^#include "[^\"]+"\n', '', (component/'gama_blefob_internal.h').read_text(), flags=re.M))
    worker = component/'gama_blefob_commands.c'
    worker_c = a.out/'worker.c'
    worker_c.write_text('#include "ble_sleep_shim.h"\n'+function(worker.read_text(), 'ble_shell_task')+'\n')
    sdk = a.sdk/'components/bt/host/nimble/nimble/porting/npl/freertos/src/npl_os_freertos.c'
    sdk_text = sdk.read_text()
    start = sdk_text.index('void\nnpl_freertos_event_init(')
    end = sdk_text.index('\nvoid\nnpl_freertos_event_reset(', start)
    (a.out/'sdk_event.inc').write_text(sdk_text[start:end])
    generated = [c, worker_c]
    for source in sources:
        target = a.out/source.name
        target.write_text(source.read_text().replace('#include "gama_blefob_internal.h"', '#include "ble_sleep_shim.h"'))
        generated.append(target)
    # Production lifecycle and worker come from --source; current fixtures and
    # extracted pinned SDK event bodies are identical for RED and GREEN.
    inputs = [*generated, root/'tests/ble_sleep_shim.c', root/'tests/ble_sleep_sdk_event.c', a.source/'components/dura_meter/dura_meter.c',
              a.source/'components/dura_meter/dura_control_policy.c']
    cmd = ['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-g', '-pthread', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
           '-Werror=implicit-function-declaration', '-I'+str(a.out), '-I'+str(component/'include'), '-I'+str(root/'tests'),
           '-I'+str(root/'tests/recipe_ownership/include'),
           '-I'+str(root/'components/dura_meter/include'), *map(str, inputs), '-lm', '-o', str(a.out/'ble-sleep-test')]
    r = subprocess.run(cmd, capture_output=True, text=True)
    (a.out/'compile.log').write_text(' '.join(cmd)+'\n'+r.stdout+r.stderr)
    if r.returncode: print(r.stderr); return r.returncode
    cases = ['shutdown', 'established', 'host_established', 'racing', 'reject', 'reject_retry',
             'stop_fail', 'deinit_fail', 'init_fail', 'sync_fail', 'adv_fail', 'activity_shutdown',
             'busy_user', 'restore_fail_radio', 'render_fail_radio', 'paused_reject', 'disabled',
             'off_reject', 'expired_reject',
             'worker_binary', 'worker_text', 'worker_peek', 'veto_retry']
    results = {}; logs = []
    for case in cases:
        r = subprocess.run([str(a.out/'ble-sleep-test'), case], capture_output=True, text=True, timeout=10,
                           env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
        results[case] = r.returncode; logs.append(case+'\n'+r.stdout+r.stderr)
        print(('PASS ' if not r.returncode else 'FAIL ')+case)
    (a.out/'run.log').write_text('\n'.join(logs))
    (a.out/'results.json').write_text(json.dumps(results, indent=2)+'\n')
    originals = [board, *sources, component/'gama_blefob_internal.h', component/'include/gama_blefob.h', Path(__file__), root/'tests/sleep_prepare_recovery.c', root/'tests/ble_sleep_board.c', root/'tests/ble_sleep_shim.c', root/'tests/ble_sleep_shim.h', root/'components/dura_meter/dura_meter.c', root/'components/dura_meter/dura_control_policy.c']
    originals += [worker, sdk, root/'tests/ble_sleep_sdk_event.c', *inputs]
    (a.out/'INPUT_SHA256.json').write_text(json.dumps({str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in originals}, indent=2)+'\n')
    return int(any(results.values()))

if __name__ == '__main__':
    sys.exit(main())
