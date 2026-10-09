#!/usr/bin/env python3
"""Compile actual generic formatters and escape utility; no application JSON seam."""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import subprocess
from test_active_ble_text_routing import function

PRELUDE = r'''
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#define CONFIG_BT_ENABLED 1
#define CONFIG_BT_NIMBLE_ENABLED 1
#define BLE_SHELL_MAC_STRING_LEN 18
#define BLE_SHELL_JSON_ESCAPE_BUF_LEN 256
#define CONFIG_BLE_SHELL_JSON_UNICODE_ESCAPE_LEN 7
#define CONFIG_IDF_TARGET "esp32s3"
#define BLE_SHELL_USECONDS_PER_SECOND 1000000LL
#define BLE_HS_CONN_HANDLE_NONE 65535
#define CONFIG_BLE_SHELL_PAIR_BUTTON_GPIO 0
#define CONFIG_BLE_SHELL_PAIR_BUTTON_ACTIVE_LOW 1
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_OK 0
struct {char project_id[128],module_name[128],module_version[128],firmware_version[128]; int security_mode; int (*status_handler)(char*,size_t,void*);void *user_ctx;} s_app_cfg;
struct {char serial_id[128],device_name[128];bool diagnostic_mode_enabled,ping_debug_enabled,ble_advertising_enabled;uint32_t pair_window_seconds;} s_cfg;
static bool s_advertising_window_open,s_fault_latched,s_maintenance_unlocked,s_ble_synced;
static int64_t s_adv_deadline_us;
static uint16_t s_conn_handle;
static uint32_t s_fault_code;
typedef struct {int revision,cores;} esp_chip_info_t;
void esp_chip_info(esp_chip_info_t *c){c->revision=1;c->cores=2;}
void ble_shell_get_mac_string(char *s,size_t n){snprintf(s,n,"00:01:02:03:04:05");}
uint32_t esp_get_free_heap_size(void){return UINT32_MAX;}
uint32_t esp_get_minimum_free_heap_size(void){return UINT32_MAX;}
uint32_t ble_shell_uptime_s(void){return UINT32_MAX;}
int64_t esp_timer_get_time(void){return 0;}
int esp_reset_reason(void){return 1;}
void setup(int c,int n){memset(&s_app_cfg,0,sizeof(s_app_cfg));memset(&s_cfg,0,sizeof(s_cfg));
memset(s_app_cfg.project_id,c,n);memset(s_app_cfg.module_name,c,n);memset(s_app_cfg.module_version,c,n);memset(s_app_cfg.firmware_version,c,n);memset(s_cfg.serial_id,c,n);memset(s_cfg.device_name,c,n);}
'''
def main():
    p=argparse.ArgumentParser(); root=Path(__file__).resolve().parents[1]
    p.add_argument('--out',type=Path,default=root.parent/'verification/transport/generic');a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True)
    comp=root/'components/gama_blefob';paths=[comp/'gama_blefob_util.c',comp/'gama_blefob_status.c',comp/'gama_blefob_internal.h']
    overflow=next(x for x in paths[2].read_text().splitlines() if x.startswith('#define BLE_SHELL_REPLY_OVERFLOW_JSON'))
    src=PRELUDE+'\n'+overflow+'\n'+function(paths[0].read_text(),'ble_shell_json_escape')+'\n'+paths[1].read_text().replace('#include "gama_blefob_internal.h"','')
    c=a.out/'generic.c';c.write_text(src);so=a.out/'generic.so'
    r=subprocess.run(['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror','-shared','-fPIC',str(c),'-o',str(so)],capture_output=True,text=True);(a.out/'compile.log').write_text(r.stdout+r.stderr)
    if r.returncode:print(r.stderr);return r.returncode
    lib=C.CDLL(str(so.resolve()));results={};lengths={}
    for name in ('info','config','status','diagnostic'):
        fn=getattr(lib,'ble_shell_make_'+name+'_json')
        for char,n,cap in ((ord('x'),31,8192),(1,31,8192),(ord('"'),31,8192),(1,127,8192),(ord('x'),31,64)):
            key=f'{name}-{char}-{n}-{cap}';lib.setup(char,n);b=C.create_string_buffer(cap)
            try:
                fn(b,C.c_size_t(cap));obj=json.loads(b.value);lengths[key]=len(b.value)
                if cap==64 or (char==1 and n==127 and name in ('info','config')):assert obj.get('error')=='response_overflow'
                elif name=='info':assert obj['project']==chr(char)*n and obj['serial']==chr(char)*n
                elif name=='config':assert obj['device_name']==chr(char)*n
                results[key]=0
            except Exception as e:results[key]=1;print(key,repr(e))
    (a.out/'results.json').write_text(json.dumps(results,indent=2)+'\n');(a.out/'lengths.json').write_text(json.dumps(lengths,indent=2)+'\n')
    (a.out/'INPUT_SHA256.json').write_text(json.dumps({str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in [*paths,Path(__file__),c]},indent=2)+'\n')
    print(f'{sum(v==0 for v in results.values())}/{len(results)} generic JSON cases pass');return int(any(results.values()))
if __name__=='__main__':raise SystemExit(main())
