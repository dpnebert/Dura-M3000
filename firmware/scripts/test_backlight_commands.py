#!/usr/bin/env python3
"""Backlight activity through real app executor/meter and active BLE dispatcher.
Real parsing/decision/control functions, platform and unrelated command leaves only.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from test_active_ble_text_routing import function
from test_backlight_policy import generate_board


def main():
    root=Path(__file__).resolve().parents[1]
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True)
    out=p.parse_args().out.resolve();out.mkdir(parents=True,exist_ok=True)
    board,lcd=generate_board(root,out)
    app=root/'main/app_main.c'; text=app.read_text()
    names=['skip_spaces','command_args','parse_string_arg','parse_uint_arg','parse_float_arg',
           'parse_rest_arg','parse_bool_arg','no_extra_args','command_should_refresh_peers','dura_execute_command']
    if 'static esp_err_t dura_command_activity' in text:
        names.insert(-1, 'dura_command_activity')
    extracted='#include "dura_json.h"\n'+'\n'.join(function(text,n) for n in names)
    (out/'app_activity.inc').write_text(extracted)
    # Unvisited unrelated leaves fail loudly; no acceptance/policy is mocked.
    leaves=set(re.findall(r'\b((?:dura_recipe_|dura_coordinator_|dura_peers_|format_)[a-z_]+)\s*\(',extracted))
    leaves.update(['system_assignment_configured','save_system_assignment','clear_system_assignment',
                   'run_self_tests','find_test_recipe_preset','run_test_recipe_preset',
                   'refresh_peer_table_before_command','broadcast_identity_after_recipe_save',
                   'dura_ui_config_set','dura_ui_config_format_json','gama_blefob_open_pairing_window'])
    shim=''
    for name in sorted(leaves):
        value='0'
        if name=='find_test_recipe_preset': value='(const dura_test_recipe_preset_t *)0'
        if name=='dura_coordinator_state_name': value='(const char *)0'
        expr='abort()' if name=='refresh_peer_table_before_command' else f'(abort(), {value})'
        shim+=f'#define {name}(...) {expr}\n'
    (out/'app_leaves.inc').write_text(shim)
    fixture=root/'tests/backlight_commands.c'
    cmd=['cc','-std=c11','-g','-pthread','-Wall','-Wextra','-Werror','-Wno-unused-function',
         '-Wno-unused-variable','-Wno-unused-but-set-variable','-Wno-unused-parameter','-fsanitize=undefined',
         '-DCONFIG_DURA_BOARD_ENABLE_RECIRC_INPUT=0','-DCONFIG_DURA_APP_M3000_LEGACY_ONLY=1',
         '-I'+str(root/'tests/recipe_ownership/include'),'-I'+str(root/'main'),'-I'+str(out)]
    for component in ['dura_board','dura_meter','dura_recipe','dura_coordinator']:
        cmd+=['-I'+str(root/'components'/component/'include')]
    cmd += [str(fixture),str(root/'components/dura_meter/dura_meter.c'),
            str(root/'components/dura_meter/dura_control_policy.c'),'-lm','-o',str(out/'app-activity')]
    r=subprocess.run(cmd,capture_output=True,text=True)
    (out/'compile-app.log').write_text(' '.join(cmd)+'\n'+r.stdout+r.stderr)
    if r.returncode: print(r.stderr);return r.returncode
    cases=['start_batch','stop_batch','start_cal','stop_cal','auto_batch','sim_pulse',
           'set_fault','clear_fault','defaults','save','load','status','config','help',
           'bad_amount','bad_bool','bad_count','unknown','busy','off_command']
    results={}
    for case in cases:
        r=subprocess.run([str(out/'app-activity'),case],capture_output=True,text=True,timeout=10)
        results['app_'+case]=r.returncode
        (out/('app_'+case+'.log')).write_text(r.stdout+r.stderr)
        print(('FAIL ' if r.returncode else 'PASS ')+'app_'+case+' '+r.stdout+r.stderr,end='')
    # Reuse established F03 route fixture, but replace its enqueue mock with the
    # real production acceptance + activity functions. Queue/GPIO are seams.
    base=(root/'tests/active_ble_text_routing.c').read_text().split('int main(int argc')[0]
    old=function(base,'dura_board_enqueue_button')
    bt=board.read_text()
    new='''
#define DURA_BUTTON_NONE 9
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
#define pdTRUE 1
static int64_t now=100, s_backlight_activity_us, s_last_activity_us;
static uint64_t s_activity_generation;
static void *s_button_queue=(void*)1;
static bool queue_full;
static int64_t esp_timer_get_time(void){return now;}
static int xQueueSend(void*q,const void*b,int t){if(queue_full)return 0;++buttons;last_button=*(const int*)b;return pdTRUE;}
'''
    for n in ['dura_board_note_activity','dura_board_note_user_activity','dura_board_enqueue_button']:
        new+=function(bt,n)+'\n'
    base=base.replace(old,new)
    generic=root/'components/gama_blefob/gama_blefob_commands.c'
    util=root/'components/gama_blefob/gama_blefob_util.c'
    internal=root/'components/gama_blefob/gama_blefob_internal.h'
    h=internal.read_text();g=generic.read_text()
    # Compile the actual public/private config types and actual apply function:
    # a direct bind-to-private mock would miss a forgotten callback copy.
    config=root/'components/gama_blefob/gama_blefob_config.c'
    (out/'sdkconfig.h').write_text('/* Host configuration. */\n#define CONFIG_BLE_SHELL_STRING_FIELD_LEN 32\n')
    private_type=re.search(r'#define BLE_SHELL_STRING_FIELD_LEN .*',h).group()+'\n'+re.search(r'typedef struct\s*\{[^}]*\}\s*ble_shell_app_config_t;',h).group()
    base=base.replace('static esp_err_t gama_blefob_', 'esp_err_t gama_blefob_')
    base=re.sub(r'enum \{ GAMA_BLEFOB_SECURITY_OPEN[^;]*;', '', base)
    base=re.sub(r'typedef esp_err_t \(\*gama_blefob_rf_button_handler_t\)[^;]*;', '', base)
    base=base.replace('#define GAMA_BLEFOB_MAX_PAYLOAD 256', '#include "gama_blefob.h"')
    base=re.sub(r'static struct \{\n.*?\} s_app_cfg;',private_type+'\nstatic ble_shell_app_config_t s_app_cfg;',base,flags=re.S)
    inc='#include "dura_json.h"\n'+re.search(r'#define DURA_APP_COMMAND_MAX_LEN .*',text).group()+'\n'
    for macro in ['CONFIG_BLE_SHELL_DEVICE_NAME','CONFIG_BLE_SHELL_PROJECT_ID','CONFIG_BLE_SHELL_MODULE_NAME',
                  'CONFIG_BLE_SHELL_MODULE_VERSION','CONFIG_BLE_SHELL_FIRMWARE_VERSION','BLE_SHELL_DEFAULT_SERIAL']:
        inc+=f'#define {macro} "host"\n'
    inc+='#define CONFIG_BLE_SHELL_PAIR_ADV_SECONDS 30\n#define CONFIG_BLE_SHELL_DIAGNOSTIC_TASK_STACK 1024\n'
    inc+='#define gama_blefob_config_init_default(...) abort()\n'
    inc+=function(util.read_text(),'ble_shell_nonempty')+'\n'
    inc+=function(config.read_text(),'ble_shell_apply_app_config')+'\n'
    inc+=h[h.index('#define BLE_SHELL_CMD_PREFIX_LEN'):h.index('/* Nordic UART')]
    for n in ['str_trim','ble_shell_parse_positive_u32','ble_shell_set_bool_from_arg']:
        inc+=function(util.read_text(),n)+'\n'
    inc+=g[g.index('static bool ble_shell_is_maintenance_command'):g.index('void ble_shell_task')]
    inc+=function(g,'gama_blefob_dispatch_text')+'\n'
    if 'static void dura_ble_user_activity_handler' in text:
        inc+=function(text,'dura_ble_user_activity_handler')+'\n'
    for n in ['dura_error_code_from_message','format_app_ok_json','format_app_error_json',
              'response_is_plain_error','response_is_plain_ok','wrap_ble_app_response',
              'dura_ble_command_handler','dura_legacy_notify_snapshot',
              'dura_legacy_ble_binary_write_handler','dura_legacy_ble_rf_button_handler']:
        inc+=function(text,n)+'\n'
    inc+='static void bind_app(void) {\n gama_blefob_config_t ble_config={0};\n'
    for field in ['command_handler','binary_write_handler','rf_button_handler','user_activity_handler']:
        m=re.search(r'ble_config\.'+field+r'\s*=\s*[^;]+;',text)
        if m: inc+=m.group()+'\n'
    inc+='ble_shell_apply_app_config(&ble_config);\n}\n'
    (out/'active_route.inc').write_text(inc)
    (out/'ble_activity.c').write_text(base+(root/'tests/backlight_ble_commands.inc').read_text())
    cmd=['cc','-std=c11','-g','-Wall','-Wextra','-Wno-unused-function','-Wno-unused-parameter',
         '-Werror','-fsanitize=undefined','-I'+str(out),'-I'+str(root/'tests/host_include'),
         '-I'+str(root/'components/gama_blefob/include'), '-I'+str(root/'main'), '-I'+str(root/'components/dura_recipe/include'),
         '-I'+str(root/'components/dura_legacy_protocol/include'),str(out/'ble_activity.c'),
         str(root/'components/dura_legacy_protocol/dura_legacy_protocol.c'),'-o',str(out/'ble-activity')]
    r=subprocess.run(cmd,capture_output=True,text=True)
    (out/'compile-ble.log').write_text(' '.join(cmd)+'\n'+r.stdout+r.stderr)
    if r.returncode: print(r.stderr);return r.returncode
    cases=['passive','invalid','buttons','full','generic','denied','invalid_generic']
    for case in cases:
        r=subprocess.run([str(out/'ble-activity'),case],capture_output=True,text=True,timeout=10)
        results['ble_'+case]=r.returncode
        (out/('ble_'+case+'.log')).write_text(r.stdout+r.stderr)
        print(('FAIL ' if r.returncode else 'PASS ')+'ble_'+case+' '+r.stdout+r.stderr,end='')
    (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    files=[app,board,lcd,generic,util,internal,config,root/'components/gama_blefob/include/gama_blefob.h',fixture,Path(__file__),
           root/'tests/backlight_policy.c',root/'tests/backlight_ble_commands.inc',
           root/'tests/active_ble_text_routing.c',root/'components/dura_meter/dura_meter.c',
           root/'components/dura_meter/dura_control_policy.c',
           root/'components/dura_legacy_protocol/dura_legacy_protocol.c']
    files+=list(out.glob('*.inc'))+list(out.glob('*.c'))
    (out/'INPUT_SHA256.json').write_text(json.dumps({str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in files},indent=2)+'\n')
    return int(any(results.values()))

if __name__=='__main__':raise SystemExit(main())
