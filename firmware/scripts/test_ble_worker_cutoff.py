#!/usr/bin/env python3
"""Execute production BLE worker under a latched shutdown cutoff; it must yield."""
from pathlib import Path
import argparse,hashlib,json,subprocess
from test_safe_output_ownership import function

def main():
 root=Path(__file__).resolve().parents[1]
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--source',type=Path,default=root)
 p.add_argument('--out',type=Path,default=root/'verification/worker-cutoff')
 a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True)
 worker=a.source/'components/gama_blefob/gama_blefob_commands.c'
 prefix=r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <setjmp.h>
#include <assert.h>
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define CONFIG_BLE_SHELL_QUEUE_RECEIVE_TIMEOUT_MS 100
#define BLE_SHELL_EVT_BINARY 1
#define ESP_OK 0
#define ESP_LOGW(...) ((void)0)
typedef int esp_err_t;
typedef struct { int type; uint8_t data[20]; size_t len; } ble_shell_msg_t;
typedef int (*gama_blefob_binary_write_handler_t)(const uint8_t *,size_t,void *);
static struct { gama_blefob_binary_write_handler_t binary_write_handler; void *binary_write_user_ctx; } s_app_cfg;
static void *s_ble_shell_queue;
static ble_shell_msg_t *queued;
static jmp_buf done;
static unsigned peeks,denials,waits;
static int xQueuePeek(void*q,void*p,unsigned ticks) {
 assert(ticks>0);if(++peeks==1000)longjmp(done,1);
 *(ble_shell_msg_t**)p=queued;return pdTRUE;
}
static int xQueueReceive(void*q,void*p,unsigned ticks) {assert(!"dequeue under closed cutoff");return 0;}
static bool ble_shell_stack_enter(void){++denials;return false;}
static void ble_shell_stack_exit(void){assert(!"unowned lease release");}
static void ble_shell_handle_command(char*p){assert(!"dispatch under closed cutoff");}
static void vTaskDelay(unsigned ticks){assert(ticks>=1);++waits;longjmp(done,1);}
'''
 suffix=r'''
int main(void){
 queued=calloc(1,sizeof(*queued));assert(queued);
 if(!setjmp(done))ble_shell_task(NULL);
 printf("peeks=%u denied_leases=%u blocking_waits=%u\n",peeks,denials,waits);
 free(queued);assert(waits==1&&peeks==1&&denials==1);return 0;
}
'''
 c=a.out/'worker.c';c.write_text(prefix+function(worker.read_text(),'ble_shell_task')+suffix)
 exe=a.out/'worker';cmd=['cc','-std=c11','-fsanitize=undefined','-Werror=implicit-function-declaration',str(c),'-o',str(exe)]
 r=subprocess.run(cmd,capture_output=True,text=True);(a.out/'compile.log').write_text(r.stdout+r.stderr)
 if r.returncode:return r.returncode
 r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=5)
 (a.out/'run.log').write_text(r.stdout+r.stderr)
 (a.out/'results.json').write_text(json.dumps({'closed_cutoff_worker_yields':r.returncode},indent=2))
 (a.out/'INPUT_SHA256.json').write_text(json.dumps({str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in [worker,Path(__file__),c]},indent=2))
 print(r.stdout+r.stderr,end='');return int(r.returncode!=0)
if __name__=='__main__':raise SystemExit(main())
