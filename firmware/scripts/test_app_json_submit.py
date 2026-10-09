#!/usr/bin/env python3
"""Compile actual submit function with bounded queue seams and host stack usage."""
from pathlib import Path
import subprocess, sys, json, re
from test_active_ble_text_routing import function
root=Path(__file__).resolve().parents[1]
out=(Path(sys.argv[1]) if len(sys.argv)>1 else root/'build-host-tests/app-json-submit').resolve();out.mkdir(parents=True,exist_ok=True)
app=(root/'main/app_main.c').read_text(); body=function(app,'dura_submit_command')
assert not re.search(r'dura_command_reply_t\s+\w+\s*[;=]',body)
assert 'xQueueReceive(s_app_command_reply_queue, reply, 0)' in body
prefix=r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "esp_check.h"
typedef unsigned TickType_t;
typedef int dura_command_source_t;
#define DURA_APP_COMMAND_MAX_LEN 128
#define DURA_APP_RESPONSE_MAX_LEN 8192
#define MALLOC_CAP_8BIT 0
#define pdTRUE 1
#define TAG "test"
#define ESP_ERR_TIMEOUT 0x107
typedef struct { int source; uint32_t request_id; char command[128]; } dura_command_request_t;
typedef struct { uint32_t request_id; esp_err_t err; char response[8192]; } dura_command_reply_t;
static void *s_app_command_queue=(void*)1,*s_app_command_reply_queue=(void*)2,*s_app_command_submit_mutex=(void*)3;
static uint32_t s_app_command_next_request_id;
static int mode,drains,receives,sends,gives;
static void *allocated;
static uint32_t sent_id;
static size_t strlcpy(char*d,const char*s,size_t n){size_t l=strlen(s);if(n){size_t k=l<n-1?l:n-1;memcpy(d,s,k);d[k]=0;}return l;}
static int xSemaphoreTake(void*m,TickType_t t){(void)m;(void)t;return 1;}
static void xSemaphoreGive(void*m){(void)m;++gives;}
static void *heap_caps_calloc(size_t n,size_t s,int cap){(void)cap;allocated=mode==3?NULL:calloc(n,s);return allocated;}
static TickType_t xTaskGetTickCount(void){return 0;}
static int xQueueSend(void*q,const void*r,TickType_t t){(void)q;(void)t;++sends;sent_id=((const dura_command_request_t*)r)->request_id;return mode==2?0:1;}
static int xQueueReceive(void*q,void*p,TickType_t t){
 (void)q;assert(p==allocated);
 dura_command_reply_t*r=p;
 if(!t){++drains;if(drains==1){memset(r,0x5a,sizeof(*r));return 1;}return 0;}
 ++receives;if(mode==1)return 0;
 r->request_id=receives==1?sent_id-1:sent_id;r->err=ESP_OK;
 strcpy(r->response,receives==1?"stale":"complete");return 1;
}
'''
post=r'''
int main(void){
 char result[128];
 for(mode=0;mode<4;++mode){drains=receives=sends=gives=0;
  esp_err_t e=dura_submit_command(1,"ping",result,sizeof(result),50);
  assert(gives==1);
  if(mode==0){assert(e==ESP_OK);assert(!strcmp(result,"complete"));assert(receives==2);}
  if(mode==1){assert(e==ESP_ERR_TIMEOUT);assert(strstr(result,"response timeout"));}
  if(mode==2){assert(e==ESP_ERR_TIMEOUT);assert(strstr(result,"queue full"));}
  if(mode==3){assert(e==ESP_ERR_NO_MEM);assert(!sends);}
  if(mode!=3)assert(drains==2);
 }
 puts("PASS heap drain identity; late reply filtering; timeout; queue full; allocation failure; mutex release");
}
'''
src=out/'submit.c';src.write_text(prefix+body+post)
cmd=['cc','-std=c11','-O2','-fno-inline','-fstack-usage','-Wall','-Wextra','-Werror','-I'+str(root/'tests/recipe_ownership/include'),str(src),'-o',str(out/'submit')]
r=subprocess.run(cmd,capture_output=True,text=True);(out/'compile.log').write_text(r.stdout+r.stderr);assert r.returncode==0,r.stderr
r=subprocess.run([str(out/'submit')],capture_output=True,text=True);assert r.returncode==0,r.stderr
stack='\n'.join(p.read_text() for p in out.glob('*.su'))
frame=[x for x in stack.splitlines() if 'dura_submit_command' in x];assert frame and all(int(x.split('\t')[1])<1024 for x in frame)
(out/'results.json').write_text(json.dumps({'behavior':'PASS','source_no_response_stack_local':'PASS','host_stack':frame,'boundary':'Host function/queue seams only; not ESP32 runtime BLE stack proof'},indent=2)+'\n')
print(r.stdout,stack)
