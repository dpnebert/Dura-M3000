#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "dura_legacy_protocol.h"
#define CONFIG_BT_ENABLED 1
#define CONFIG_BT_NIMBLE_ENABLED 1
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_TIMEOUT 0x107
#define GAMA_BLEFOB_MAX_PAYLOAD 256
#define BLE_SHELL_MAX_PAYLOAD GAMA_BLEFOB_MAX_PAYLOAD
#define BLE_SHELL_TEXT_RESPONSE_CAPACITY 8192U
#define BLE_SHELL_REPLY_OVERFLOW_JSON "{\"ok\":false,\"error\":\"response_overflow\",\"capacity\":8191}"
/* Paging is exercised against production in ble_reply_paging_host.c. */
static bool ble_shell_reply_command(const char *cmd) { (void)cmd; return false; }
#define BLE_SHELL_PAIR_ADV_SECONDS_MIN 1
#define BLE_SHELL_PAIR_ADV_SECONDS_MAX 600
#define CONFIG_BLE_SHELL_UNLOCK_TOKEN "test"
#define CONFIG_BLE_SHELL_REBOOT_DELAY_MS 1
#define DURA_APP_COMMAND_TIMEOUT_MS 5000
#define DURA_COMMAND_SOURCE_BLE 2
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOG_BUFFER_HEX_LEVEL(...) ((void)0)
#define ESP_ERROR_CHECK_WITHOUT_ABORT(x) ((void)(x))
#define ESP_RETURN_ON_FALSE(x,e,...) do { if (!(x)) return (e); } while(0)
enum { GAMA_BLEFOB_SECURITY_OPEN, GAMA_BLEFOB_SECURITY_PAIR_REQUIRED, GAMA_BLEFOB_SECURITY_UNLOCK_COMMAND_REQUIRED };
typedef esp_err_t (*gama_blefob_rf_button_handler_t)(uint32_t,bool,void *);
static struct { char device_name[32], serial_id[32]; bool ping_debug_enabled, diagnostic_mode_enabled, ble_advertising_enabled; uint32_t pair_window_seconds; } s_cfg;
static struct {
 int security_mode;
 esp_err_t (*command_handler)(const char *,char *,size_t,void *);
 esp_err_t (*binary_write_handler)(const uint8_t *,size_t,void *);
 gama_blefob_rf_button_handler_t rf_button_handler;
 void (*user_activity_handler)(void *);
 void *rf_button_user_ctx, *user_ctx;
} s_app_cfg;
static bool s_maintenance_unlocked, s_advertising_window_open, s_fault_latched, s_initialized = true;
static uint32_t s_fault_code;
static int responses, statuses, app_calls, buttons, last_button, binaries;
static char response[256];
static uint8_t binary[12]; static size_t binary_size;
static size_t strlcpy(char *d,const char *s,size_t n) { size_t l=strlen(s); if(n) { size_t k=l<n-1?l:n-1; memcpy(d,s,k); d[k]=0; } return l; }
static void ble_shell_notify_response(const char *s) { ++responses; strlcpy(response,s,sizeof(response)); }
static esp_err_t gama_blefob_respond(const char *s) { ble_shell_notify_response(s); return ESP_OK; }
static void ble_shell_notify_status(void) { ++statuses; }
static void ble_shell_notify_log(const char *s) { (void)s; }
static void ble_shell_notify_diagnostic(const char *s) { (void)s; }
#define JSON_STUB(name,value) static void name(char *o,size_t n) { snprintf(o,n,"{\"kind\":\"%s\"}",value); }
JSON_STUB(ble_shell_make_info_json,"info")
JSON_STUB(ble_shell_make_status_json,"status")
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
/* Queue execution is the seam. Real registered app handler and JSON wrapping
 * execute above it; command effects/RTOS scheduling are not simulated here. */
static esp_err_t dura_submit_command(int source,const char *cmd,char *out,size_t n,int timeout) {
 assert(source==DURA_COMMAND_SOURCE_BLE); ++app_calls;
 if(!strcmp(cmd,"stop_batch")) { snprintf(out,n,"ok: batch stopped"); return ESP_OK; }
 snprintf(out,n,"error: unknown command '%s'",cmd); return ESP_ERR_NOT_FOUND;
}
typedef int dura_button_t;
enum { DURA_BUTTON_UP=10, DURA_BUTTON_DOWN, DURA_BUTTON_SELECT, DURA_BUTTON_BACK };
typedef struct { uint16_t legacy_screen_id,legacy_menu_id; uint32_t calibration_counts; float meter_total,remaining_batch,batch_amount,calibration_measured_amount; uint8_t selected_units,backlight_timeout_sec; bool show_batch_total; } dura_board_ui_snapshot_t;
static esp_err_t dura_board_enqueue_button(dura_button_t b) { ++buttons; last_button=b; return ESP_OK; }
static esp_err_t dura_board_get_ui_snapshot(dura_board_ui_snapshot_t *s) { memset(s,0,sizeof(*s)); s->legacy_screen_id=0x1e; s->legacy_menu_id=2; s->selected_units=1; return ESP_OK; }
static esp_err_t gama_blefob_notify_binary(const uint8_t *p,size_t n) { ++binaries; assert(n<=sizeof(binary)); memcpy(binary,p,n); binary_size=n; return ESP_OK; }
/* Routing-only SDK boundary; real cutoff is exercised by ble_sleep. */
static bool ble_shell_stack_enter(void) { return true; }
static void ble_shell_stack_exit(void) {}
#include "active_route.inc"
static void reset(void) { responses=statuses=app_calls=buttons=binaries=0; response[0]=0; }
static esp_err_t write_bytes(const uint8_t *p,size_t n) { return s_app_cfg.binary_write_handler(p,n,NULL); }
static void text(const char *s,const char *expected,int calls) {
 reset(); (void)write_bytes((const uint8_t *)s,strlen(s));
 if(!strstr(response,expected)) { fprintf(stderr,"command %s: expected %s; got %s\n",s,expected,response); abort(); }
 assert(responses==1 && statuses==1 && app_calls==calls && binaries==0);
}
static void rejected(const uint8_t *p,size_t n) {
 dura_legacy_action_t a; esp_err_t expected=dura_legacy_parse(p,n,&a); assert(expected!=ESP_OK);
 reset(); assert(write_bytes(p,n)==expected); assert(!responses && !app_calls && !buttons && !binaries && !statuses);
}
int main(int argc,char **argv) {
 assert(argc==2); assert(s_initialized); bind_app(); s_cfg.ping_debug_enabled=true;
 if(!strcmp(argv[1],"ping")) {
  text("ping","pong",0); text(" PiNg ","pong",0);
  s_cfg.ping_debug_enabled=false; text("ping","ping debug mode disabled",0);
 } else if(!strcmp(argv[1],"gets")) {
  text("get_info","\"info\"",0); text("GET_STATUS","\"status\"",0); text("get_config","\"config\"",0);
  text("info","\"info\"",0); text("status","\"status\"",0); text("config","\"config\"",0);
 } else if(!strcmp(argv[1],"rf")) {
  const unsigned positions[]={1,2,4,8}; const int mapped[]={DURA_BUTTON_UP,DURA_BUTTON_DOWN,DURA_BUTTON_SELECT,DURA_BUTTON_BACK};
  for(int i=0;i<4;i++) { char cmd[48]; snprintf(cmd,sizeof(cmd),"set rf_button %u",positions[i]); text(cmd,"ok:",0); assert(buttons==1 && last_button==mapped[i]); snprintf(cmd,sizeof(cmd),"clear rf_button %u",positions[i]); text(cmd,"cleared",0); assert(!buttons); }
  text("set rf_button 3","error:",0); assert(!buttons);
  const char *bad[]={"0","no","1x","1 2","4294967296"};
  for(size_t i=0;i<sizeof(bad)/sizeof(*bad);i++) { char cmd[64]; snprintf(cmd,sizeof(cmd),"set rf_button %s",bad[i]); text(cmd,"positive integer",0); assert(!buttons); snprintf(cmd,sizeof(cmd),"clear rf_button %s",bad[i]); text(cmd,"positive integer",0); assert(!buttons); }
 } else if(!strcmp(argv[1],"app")) {
  text("stop_batch","\"ok\":true",1); assert(strstr(response,"batch stopped"));
  text("unknown_command","\"ok\":false",1);
 } else if(!strcmp(argv[1],"binary")) {
  const uint8_t connect[]={1,10,245},poll[]={1,28,227};
  const uint8_t home[]={10,29,30,2,0,0,0,0,100,1,1,154};
  reset(); assert(write_bytes(connect,3)==ESP_OK); assert(binaries==1 && !responses && !app_calls); assert(binary_size==sizeof(home) && !memcmp(binary,home,sizeof(home)));
  reset(); assert(write_bytes(poll,3)==ESP_OK); assert(binaries==1 && !responses);
  const uint8_t values[]={1,2,4,8};
  for(int i=0;i<4;i++) { uint8_t b[]={2,30,values[i],(uint8_t)~(30^values[i])}; reset(); assert(write_bytes(b,4)==ESP_OK); assert(buttons==1 && last_button==10+i && !app_calls); }
  const uint8_t bad[][4]={{2,10,245,0},{2,10,0,245},{1,28,226,0},{2,30,0,225},{2,30,3,226},{1,85,170,0},{1,10,0,0},{1,0x62,0x9d,0}};
  const size_t lengths[]={3,4,3,4,4,3,2,3};
  for(size_t i=0;i<sizeof(lengths)/sizeof(*lengths);i++) rejected(bad[i],lengths[i]);
 } else if(!strcmp(argv[1],"printable_binary")) {
  /* A counted but unsupported frame can be entirely printable, including 0x62.
   * A checksum-valid all-printable frame is impossible: XOR of ASCII has bit7=0,
   * so its complemented checksum has bit7=1. Malformed still must not dispatch. */
  uint8_t b[34]; memset(b,'b',sizeof(b)); b[0]=32; rejected(b,sizeof(b));
 } else if(!strcmp(argv[1],"bounds")) {
  rejected(NULL,0); rejected(NULL,4); uint8_t b[128]; memset(b,'x',sizeof(b)); rejected(b,sizeof(b)); rejected(b,0);
  const uint8_t nul[]={'p','i','n','g',0}; rejected(nul,sizeof(nul));
 }
 puts("ACTIVE_BLE_ROUTE_PASS"); return 0;
}
