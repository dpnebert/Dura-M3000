#include "ble_sleep_shim.h"
#include <setjmp.h>
void vTaskDelay(unsigned ticks){assert(ticks > 0);}
const char *ble_test;
int stop_calls,deinit_calls,init_calls,adv_calls,host_starts,worker_starts;
bool radio_up,host_link,host_done;
static bool advertising;
static void (*host_fn)(void*);
static jmp_buf host_exit;
int (*gap_cb)(struct ble_gap_event*,void*);
struct ble_hs_cfg_struct ble_hs_cfg;
const struct ble_gatt_svc_def gatt_svcs[]={{0}};
static struct {int count;} sems[8];
static int sem_count;
SemaphoreHandle_t xSemaphoreCreateBinary(void){assert(sem_count<8);return (void*)&sems[sem_count++];}
int test_sem_take(SemaphoreHandle_t p){for(int i=0;i<sem_count;i++)if(p==(void*)&sems[i]){int n=sems[i].count;sems[i].count=0;return n;}return -1;}
bool test_sem_give(SemaphoreHandle_t p){for(int i=0;i<sem_count;i++)if(p==(void*)&sems[i]){sems[i].count=1;return true;}return false;}
__attribute__((weak)) int gama_blefob_sleep_shutdown(void){return ESP_OK;}
__attribute__((weak)) int gama_blefob_sleep_resume(void){return ESP_OK;}
__attribute__((weak)) bool ble_shell_stack_enter(void){return true;}
__attribute__((weak)) void ble_shell_stack_exit(void){}
QueueHandle_t xQueueCreate(unsigned n,size_t s){return (void*)2;}
static ble_shell_msg_t *queued_msg;
static jmp_buf worker_exit;
static bool worker_running,interleaved;
int xQueuePeek(QueueHandle_t q,void*p,unsigned t){
 assert(t>0);
 if(!queued_msg){if(worker_running)longjmp(worker_exit,1);return pdFALSE;}
 *(ble_shell_msg_t**)p=queued_msg;
 if(!interleaved&&!strcmp(ble_test,"worker_peek")){interleaved=true;test_worker_interleave();}
 return pdTRUE;
}
int xQueueReceive(QueueHandle_t q,void*p,unsigned t){
 if(!queued_msg){if(worker_running&&t)longjmp(worker_exit,1);return pdFALSE;}
 *(ble_shell_msg_t**)p=queued_msg;queued_msg=NULL;
 if(worker_running&&!interleaved){interleaved=true;test_worker_interleave();}
 return pdTRUE;
}
void test_worker_run(bool binary){
 queued_msg=calloc(1,sizeof(*queued_msg));assert(queued_msg);
 queued_msg->type=binary?BLE_SHELL_EVT_BINARY:BLE_SHELL_EVT_COMMAND;
 strcpy((char*)queued_msg->data,"manual_start");queued_msg->len=strlen((char*)queued_msg->data);
 worker_running=true;
 if(!setjmp(worker_exit))ble_shell_task(NULL);
 worker_running=false;assert(!queued_msg&&interleaved);
}
int xTaskCreate(void(*f)(void*),const char*n,unsigned s,void*a,unsigned p,void*h){worker_starts++;return pdPASS;}
void nimble_port_run(void){host_done=true;}
void vTaskSuspend(void*p){longjmp(host_exit,1);}
void nimble_port_freertos_init(void(*f)(void*)){host_starts++;host_fn=f;host_done=false;if(strcmp(ble_test,"sync_fail")||init_calls==1)ble_hs_cfg.sync_cb();}
void nimble_port_freertos_deinit(void){assert(host_done);}
int nimble_port_init(void){init_calls++;if(init_calls>1&&!strcmp(ble_test,"init_fail"))return ESP_FAIL;radio_up=true;return ESP_OK;}
int nimble_port_stop(void){assert(!host_critical_depth);stop_calls++;test_shutdown_injection();if(!strcmp(ble_test,"stop_fail"))return ESP_FAIL;host_link=false;advertising=false;if(!setjmp(host_exit))host_fn(NULL);return ESP_OK;}
int nimble_port_deinit(void){assert(host_done);deinit_calls++;if(!strcmp(ble_test,"deinit_fail"))return ESP_FAIL;test_event_pool_teardown();radio_up=false;return ESP_OK;}
int nvs_flash_init(void){return ESP_OK;}
void ble_svc_gap_init(void){}
void ble_svc_gatt_init(void){}
int ble_svc_gap_device_name_set(const char*s){assert(radio_up);return ESP_OK;}
const char *ble_svc_gap_device_name(void){assert(radio_up);return "M3000";}
int ble_gatts_count_cfg(const struct ble_gatt_svc_def*s){return ESP_OK;}
int ble_gatts_add_svcs(const struct ble_gatt_svc_def*s){return ESP_OK;}
int ble_gap_adv_active(void){assert(radio_up);return advertising;}
int ble_gap_adv_stop(void){assert(radio_up);advertising=false;return 0;}
int ble_gap_adv_set_fields(const struct ble_hs_adv_fields*s){assert(radio_up);return 0;}
int ble_gap_adv_rsp_set_fields(const struct ble_hs_adv_fields*s){assert(radio_up);return 0;}
int ble_gap_adv_start(uint8_t a,void*p,int32_t ms,const struct ble_gap_adv_params*s,int(*cb)(struct ble_gap_event*,void*),void*x){assert(radio_up);adv_calls++;gap_cb=cb;if(init_calls>1&&!strcmp(ble_test,"adv_fail"))return ESP_FAIL;advertising=true;return 0;}
int ble_hs_id_infer_auto(int a,uint8_t*b){*b=0;return 0;}
int ble_gap_terminate(uint16_t h,int reason){assert(radio_up);host_link=false;return 0;}
static bool host_locked;
void ble_hs_lock(void){assert(!host_locked&&!host_critical_depth);host_locked=true;}
void ble_hs_unlock(void){assert(host_locked);host_locked=false;}
void ble_gap_conn_foreach_handle(ble_gap_conn_foreach_handle_fn*cb,void*a){assert(radio_up&&host_locked);if(host_link)cb(42,a);}
void ble_npl_eventq_put(void*q,struct ble_npl_event*e){assert(radio_up);struct ble_npl_event_freertos *v=e->event;v->fn(e);}
void *nimble_port_get_dflt_eventq(void){assert(radio_up);return (void*)1;}
void test_connect(void){host_link=true;struct ble_gap_event e={.type=BLE_GAP_EVENT_CONNECT,.connect={.status=0,.conn_handle=42}};assert(gap_cb);gap_cb(&e,0);}
void test_disconnect(void){host_link=false;struct ble_gap_event e={.type=BLE_GAP_EVENT_DISCONNECT};assert(gap_cb);gap_cb(&e,0);}
void ble_shell_apply_app_config(const gama_blefob_config_t*c){s_app_cfg.advertise_on_boot=true;s_app_cfg.default_pair_window_seconds=60;}
void ble_shell_config_load(void){s_cfg.ble_advertising_enabled=true;s_cfg.pair_window_seconds=60;}
void ble_store_config_init(void){}

void ble_shell_make_status_json(char*s,size_t n){if(n)*s=0;}
int ble_gap_conn_find(uint16_t h,struct ble_gap_conn_desc*d){assert(radio_up);d->sec_state.encrypted=true;return 0;}
struct os_mbuf *ble_hs_mbuf_from_flat(const void*p,size_t n){assert(radio_up);static struct os_mbuf m;return &m;}
int ble_gatts_notify_custom(uint16_t h,uint16_t a,struct os_mbuf*m){assert(radio_up);return 0;}
uint16_t ble_att_mtu(uint16_t conn){return 23;}
int os_mbuf_append(struct os_mbuf *om,const void *data,uint16_t len){return 0;}
size_t strlcpy(char*d,const char*s,size_t n){size_t len=strlen(s);if(n){size_t c=len<n-1?len:n-1;memcpy(d,s,c);d[c]=0;}return len;}
uint8_t ble_shell_adv_status_byte(void){return 0;}
void ble_shell_parse_semver(const char*s,uint8_t*a,uint8_t*b,uint8_t*c){*a=*b=*c=0;}
