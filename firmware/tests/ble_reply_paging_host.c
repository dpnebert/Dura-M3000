#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#define CONFIG_BT_ENABLED 1
#define CONFIG_BT_NIMBLE_ENABLED 1
#define BLE_SHELL_MAX_PAYLOAD 1024
#define BLE_SHELL_PAIR_ADV_SECONDS_MIN 1
#define BLE_SHELL_PAIR_ADV_SECONDS_MAX 600
#define CONFIG_BLE_SHELL_UNLOCK_TOKEN "test"
#define CONFIG_BLE_SHELL_REBOOT_DELAY_MS 1
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NO_MEM 0x101
#define BLE_HS_CONN_HANDLE_NONE 0xffff
#define BLE_ATT_ATTR_MAX_LEN 512
#define BLE_ATT_ERR_INSUFFICIENT_RES 0x11
#define BLE_ATT_ERR_UNLIKELY 0x0e
#define BLE_GATT_ACCESS_OP_READ_CHR 0
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGI(...) ((void)0)
#define ESP_ERROR_CHECK_WITHOUT_ABORT(x) ((void)(x))
#define portMUX_TYPE pthread_mutex_t
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
static _Thread_local int critical;
#define taskENTER_CRITICAL(m) do { pthread_mutex_lock(m); ++critical; } while(0)
#define taskEXIT_CRITICAL(m) do { --critical; pthread_mutex_unlock(m); } while(0)
typedef int esp_err_t;
enum { GAMA_BLEFOB_SECURITY_OPEN, GAMA_BLEFOB_SECURITY_PAIR_REQUIRED, GAMA_BLEFOB_SECURITY_UNLOCK_COMMAND_REQUIRED };
typedef esp_err_t (*gama_blefob_rf_button_handler_t)(uint32_t,bool,void *);
static struct { char device_name[32], serial_id[32]; bool ping_debug_enabled, diagnostic_mode_enabled, ble_advertising_enabled; uint32_t pair_window_seconds; } s_cfg;
static struct {
 int security_mode;
 esp_err_t (*command_handler)(const char *,char *,size_t,void *);
 gama_blefob_rf_button_handler_t rf_button_handler;
 void (*user_activity_handler)(void *);
 void *rf_button_user_ctx, *user_ctx;
} s_app_cfg;
static bool s_maintenance_unlocked, s_advertising_window_open, s_fault_latched, s_initialized = true;
static uint32_t s_fault_code;
static uint16_t s_conn_handle=7, s_response_chr_val_handle=9, mtu=23;
static bool s_response_notify_active=true, encrypted=true, stopped, fail_alloc, fail_send, fail_append, fail_response_alloc;
static _Thread_local int leases;
static int sends, allocations, app_calls, activity, statuses;
static size_t app_capacity;
static uint8_t sent[8192]; static size_t sent_len;
static char app_text[8193];
static int app_result;
struct os_mbuf { uint8_t data[8192]; size_t len; };
struct ble_gap_conn_desc { struct { bool encrypted; } sec_state; };
struct ble_gatt_access_ctxt { int op; struct os_mbuf *om; };
static uint16_t ble_att_mtu(uint16_t conn) { assert(!critical); return mtu; }
static int ble_gap_conn_find(uint16_t conn, struct ble_gap_conn_desc *desc) { assert(!critical); desc->sec_state.encrypted=encrypted; return conn==s_conn_handle?0:1; }
static struct os_mbuf *ble_hs_mbuf_from_flat(const void *p, uint16_t n) {
 assert(!critical && leases>0); ++allocations; if(fail_alloc)return NULL;
 struct os_mbuf *om=malloc(sizeof(*om)); assert(om && n<=sizeof(om->data)); memcpy(om->data,p,n); om->len=n; return om;
}
static int ble_gatts_notify_custom(uint16_t conn,uint16_t attr,struct os_mbuf *om) {
 assert(!critical && leases>0); ++sends; memcpy(sent,om->data,om->len); sent_len=om->len; free(om); return fail_send?1:0;
}
static int os_mbuf_append(struct os_mbuf *om,const void *p,uint16_t n) {
 assert(!critical); if(fail_append)return 1; assert(om->len+n<=sizeof(om->data)); memcpy(om->data+om->len,p,n); om->len+=n; return 0;
}
static bool ble_shell_stack_enter(void) { if(stopped)return false; ++leases; return true; }
static void ble_shell_stack_exit(void) { assert(leases>0); --leases; }
static size_t strlcpy(char *d,const char *s,size_t n) { size_t l=strlen(s); if(n){size_t k=l<n-1?l:n-1;memcpy(d,s,k);d[k]=0;}return l; }
#define JSON_STUB(name,value) static void name(char *o,size_t n) { snprintf(o,n,"{\"kind\":\"%s\"}",value); }
JSON_STUB(ble_shell_make_info_json,"info")
static void ble_shell_make_status_json(char *o,size_t n) { ++statuses; snprintf(o,n,"{}"); }
JSON_STUB(ble_shell_make_config_json,"config")
JSON_STUB(ble_shell_make_diagnostic_json,"diagnostic")
static void ble_shell_open_pairing_window(uint32_t n,const char *s) {}
static void ble_shell_close_pairing_window(void) {}
static esp_err_t ble_shell_config_save(void) { return ESP_OK; }
static esp_err_t ble_shell_config_factory_reset(void) { return ESP_OK; }
static int ble_svc_gap_device_name_set(const char *s) { return 0; }
static const char *esp_err_to_name(esp_err_t e) { return "host-error"; }
static void vTaskDelay(int n) {}
static void esp_restart(void) {}
static void *response_calloc(size_t n,size_t size) { if(fail_response_alloc)return NULL; return calloc(n,size); }
#define calloc response_calloc
#include "reply_production.inc"
#undef calloc
static void note_activity(void *ctx) { ++activity; }
static esp_err_t app_handler(const char *cmd,char *out,size_t n,void *ctx) {
 ++app_calls; app_capacity=n;
 if(!strcmp(cmd,"unterminated")){memset(out,'x',n);return ESP_OK;}
 strlcpy(out,app_text,n); return app_result;
}
static void dispatch(const char *cmd) { assert(gama_blefob_dispatch_text(cmd)==ESP_OK); assert(!leases); }
static size_t cap(void) { return mtu-3u>512u?512u:mtu-3u; }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0]|((uint16_t)p[1]<<8)); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static uint32_t check_header(unsigned flag,size_t total,size_t offset) {
 assert(sent_len>=12 && sent_len<=cap()); assert(sent[0]==0 && sent[1]=='R' && sent[2]==1 && sent[3]==flag);
 assert(u32(sent+4)!=0 && u16(sent+8)==total && u16(sent+10)==offset); if(flag!=2)assert(sent_len==12); return u32(sent+4);
}
static void fetch(uint32_t gen,unsigned off) { char cmd[48]; snprintf(cmd,sizeof(cmd),"reply %08x %04x",gen,off); assert(strlen(cmd)==19); dispatch(cmd); }
static void fill(size_t n) { assert(n<sizeof(app_text)); for(size_t i=0;i<n;i++)app_text[i]=(char)(' '+(i%95)); app_text[n]=0; }
static void mtu_case(unsigned m) {
 mtu=m; fill(8191); dispatch("long"); assert(app_calls==1 && sends==1);
 uint32_t gen=check_header(1,8191,0); uint8_t got[8191]; size_t off=0;
 while(off<8191) { fetch(gen,off); assert(check_header(2,8191,off)==gen); size_t n=sent_len-12; assert(n>0 && n<=cap()-12); memcpy(got+off,sent+12,n); off+=n; }
 assert(!memcmp(got,app_text,sizeof(got))); fetch(gen,8191); assert(check_header(2,8191,8191)==gen && sent_len==12);
 assert(app_calls==1 && !activity && statuses==1);
 /* Dump actual production packets for independent Python codec verification. */
 printf("GEN %u MTU %u TOTAL 8191\n",gen,m);
 for(off=0;off<8191;off+=sent_len-12) { fetch(gen,off); for(size_t i=0;i<sent_len;i++)printf("%02x",sent[i]); putchar('\n'); }
}
static int read_value(struct os_mbuf *om) { struct ble_gatt_access_ctxt c={.op=BLE_GATT_ACCESS_OP_READ_CHR,.om=om}; om->len=0; return ble_response_chr_access_cb(s_conn_handle,9,&c,NULL); }
static void *writer(void *unused) { char text[8192]; for(int i=0;i<2000;i++){memset(text,'A'+i%2,8191);text[8191]=0;ble_shell_notify_response(text);} return NULL; }
int main(int argc,char **argv) {
 assert(argc==2); s_app_cfg.command_handler=app_handler; s_app_cfg.user_activity_handler=note_activity; s_cfg.ping_debug_enabled=true;
 const char *t=argv[1];
 if(!strncmp(t,"mtu",3))mtu_case((unsigned)atoi(t+3));
 else if(!strcmp(t,"short")) {
  const unsigned ms[]={23,247,256,517};
  for(size_t i=0;i<4;i++){mtu=ms[i];fill(cap());dispatch("short");assert(sent_len==cap()&&!memcmp(sent,app_text,sent_len));fill(cap()+1);dispatch("long");check_header(1,cap()+1,0);}
  dispatch("ping");assert(sent_len==4&&!memcmp(sent,"pong",4));
  /* IT-01: actual raw notifications must not collide with legacy counts. */
  mtu=247;
  const char *names[]={"generic_unknown","app_unknown_json","valid_json_125"};
  for(size_t c=0;c<3;c++) {
   if(c==0) {
    char command[79];memset(command,'x',78);command[78]=0;
    s_app_cfg.command_handler=NULL;dispatch(command);s_app_cfg.command_handler=app_handler;
   } else {
    if(c==1) strcpy(app_text,"{\"ok\":false,\"command\":\"xxxxxxxx\",\"error\":{\"code\":\"UNKNOWN_COMMAND\",\"message\":\"unknown command 'xxxxxxxx'\",\"esp_err\":\"0x105\"}}");
    else {memset(app_text,'x',125);memcpy(app_text,"{\"value\":\"",10);app_text[123]='\"';app_text[124]='}';app_text[125]=0;}
    dispatch("capture");
   }
   assert(sent_len==(c==0?103u:125u));
   printf("SHORT %s ",names[c]);for(size_t j=0;j<sent_len;j++)printf("%02x",sent[j]);putchar('\n');
  }
 } else if(!strcmp(t,"capacity")) {
  fill(8191);dispatch("max"); assert(app_capacity>=8192);assert(sizeof(s_last_response)>=8192);assert(sizeof(s_last_binary)==1024);check_header(1,8191,0);
 } else if(!strcmp(t,"overflow")) {
  mtu=517;fill(8192);ble_shell_notify_response(app_text);assert(sent[0]=='{' && sent_len<512);sent[sent_len]=0;assert(strstr((char*)sent,"response_overflow"));
  dispatch("unterminated");assert(sent[0]=='{');sent[sent_len]=0;assert(strstr((char*)sent,"response_overflow"));
  app_result=ESP_ERR_INVALID_SIZE;strcpy(app_text,"{\"partial\":");dispatch("overflow");assert(sent[0]=='{');sent[sent_len]=0;assert(strstr((char*)sent,"response_overflow"));
 } else if(!strcmp(t,"read")) {
  struct os_mbuf om;fill(8191);dispatch("long");uint8_t meta[12];memcpy(meta,sent,12);
  assert(!read_value(&om));assert(om.len==12&&!memcmp(om.data,meta,12));uint32_t gen=u32(meta+4);fetch(gen,0);assert(!read_value(&om)&&om.len==12&&!memcmp(om.data,meta,12));
  dispatch("ping");assert(!read_value(&om)&&om.len==4&&!memcmp(om.data,"pong",4));fail_append=true;assert(read_value(&om)==BLE_ATT_ERR_INSUFFICIENT_RES);
 } else if(!strcmp(t,"recovery")) {
  fill(1000);dispatch("effect");uint32_t gen=check_header(1,1000,0);int count=app_calls;
  fetch(gen,24);uint8_t old[20];memcpy(old,sent,sent_len);fetch(gen,24);assert(!memcmp(old,sent,20));fetch(gen,0);check_header(2,1000,0);
  dispatch("reply");assert(check_header(1,1000,0)==gen);assert(app_calls==count&&statuses==1&&!activity);
  dispatch("ping");fetch(gen,0);uint32_t next=check_header(0x81,4,0);assert(next!=gen);dispatch("reply");assert(check_header(1,4,0)==next);
 } else if(!strcmp(t,"invalid")) {
  fill(30);dispatch("effect");uint32_t gen=check_header(1,30,0);int count=app_calls;
  const char *bad[]={"reply 1 0","reply 00000000 zzzz","reply 000000000 0000","reply 00000001 00000","reply 00000001 0000 extra","reply junk","reply 00000001 -001","reply 00000001 +001","reply\t00000001 0000"};
  for(size_t i=0;i<sizeof(bad)/sizeof(*bad);i++){dispatch(bad[i]);assert(check_header(0x83,30,0)==gen);}
  fetch(gen,31);assert(check_header(0x82,30,31)==gen);fetch(gen,0xffff);check_header(0x82,30,0xffff);fetch(0,0);check_header(0x81,30,0);assert(app_calls==count&&statuses==1&&!activity);
 } else if(!strcmp(t,"failures")) {
  fill(100);fail_alloc=true;dispatch("effect");assert(app_calls==1&&!sends);fail_alloc=false;dispatch("reply");uint32_t gen=check_header(1,100,0);
  fail_send=true;fetch(gen,0);fail_send=false;fetch(gen,0);assert(check_header(2,100,0)==gen);
  fail_response_alloc=true;dispatch("reply");assert(check_header(1,100,0)==gen);fetch(gen,0);assert(check_header(2,100,0)==gen);dispatch("effect");assert(app_calls==1);fail_response_alloc=false;
  dispatch("reply");assert(u32(sent+4)!=gen);assert(allocations>0);
 } else if(!strcmp(t,"gates")) {
  fill(100);for(int i=0;i<4;i++){s_conn_handle=i==0?BLE_HS_CONN_HANDLE_NONE:7;encrypted=i!=1;s_response_notify_active=i!=2;stopped=i==3;ble_shell_notify_response(app_text);assert(!sends&&!allocations);}
  stopped=false;s_conn_handle=7;encrypted=true;s_response_notify_active=true;dispatch("reply");check_header(1,100,0);
  stopped=true;assert(gama_blefob_dispatch_text("effect")==ESP_ERR_INVALID_STATE);assert(!app_calls);stopped=false;assert(!leases);
 } else if(!strcmp(t,"binary")) {
  const uint8_t raw[]={10,29,30,2,0,0,0,0,100,1,1,154};fill(100);dispatch("effect");uint32_t gen=check_header(1,100,0);
  assert(ble_shell_notify_binary(raw,sizeof(raw))==ESP_OK);assert(sent_len==sizeof(raw)&&!memcmp(sent,raw,sizeof(raw)));ble_shell_replay_binary();assert(sent_len==sizeof(raw)&&!memcmp(sent,raw,sizeof(raw)));
  dispatch("reply");assert(check_header(1,100,0)==gen);assert(ble_shell_notify_binary(NULL,1)==ESP_ERR_INVALID_ARG);assert(ble_shell_notify_binary(raw,0)==ESP_ERR_INVALID_ARG);
  encrypted=false;assert(ble_shell_notify_binary(raw,sizeof(raw))==ESP_ERR_INVALID_STATE);
 } else if(!strcmp(t,"concurrent")) {
  s_response_notify_active=false;pthread_t thread;assert(!pthread_create(&thread,NULL,writer,NULL));
  for(int i=0;i<2000;i++){struct os_mbuf om;assert(!read_value(&om));assert(om.len<=cap());if(om.len==12){assert(om.data[0]==0&&om.data[1]=='R'&&om.data[2]==1&&om.data[3]==1);assert(u16(om.data+8)==8191);}}
  assert(!pthread_join(thread,NULL));
 } else assert(false);
 assert(!critical&&!leases);puts("BLE_REPLY_PAGING_PASS");return 0;
}
