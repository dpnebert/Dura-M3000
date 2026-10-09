/* Physical GPIO calls are observations, not real hardware. Fresh process/case. */
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dura_meter.h"
#include "freertos/semphr.h"
#include "nvs.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d %s\n",__func__,__LINE__,#x); exit(1); } } while(0)
_Thread_local unsigned host_critical_depth;
static atomic_int gpio[3], on_edges, writes;
static int fail_open,fail_write,fail_commit;
static bool safe_required, snapshot_fault;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond=PTHREAD_COND_INITIALIZER;
static bool block_commit,commit_entered,release_commit,stop_waiting;
static _Thread_local bool stop_thread;
extern int dura_board_sync_outputs(void);
extern void test_board_bind(void);
static dura_meter_snapshot_t snap(void){dura_meter_snapshot_t s; CHECK(dura_meter_get_snapshot(&s)==ESP_OK); return s;}
int test_board_snapshot(dura_meter_snapshot_t*s){int e=dura_meter_get_snapshot(s); if(snapshot_fault){snapshot_fault=false; CHECK(dura_meter_set_fault(7)==ESP_OK);} return e;}
static void off(void){CHECK(!gpio[0]&&!gpio[1]&&!gpio[2]);}
int gpio_set_level(int pin,int level){
#ifdef DURA_METER_HAS_SYNC_OUTPUT_HANDLER
 CHECK(host_critical_depth>0);
#endif
 CHECK(pin>=0&&pin<3); if(level && !gpio[pin]) ++on_edges; gpio[pin]=level; ++writes; return ESP_OK;
}
int64_t esp_timer_get_time(void){return 1000000;}
SemaphoreHandle_t xSemaphoreCreateMutex(void){pthread_mutex_t*m=malloc(sizeof(*m)); CHECK(m); CHECK(!pthread_mutex_init(m,NULL));return m;}
int xSemaphoreTake(SemaphoreHandle_t m,unsigned t){CHECK(!host_critical_depth); if(safe_required)off(); if(stop_thread){pthread_mutex_lock(&gate);stop_waiting=true;pthread_cond_broadcast(&cond);pthread_mutex_unlock(&gate);}CHECK(!pthread_mutex_lock(m));return pdTRUE;}
void xSemaphoreGive(SemaphoreHandle_t m){CHECK(!pthread_mutex_unlock(m));}
int nvs_open(const char*s,int mode,nvs_handle_t*h){CHECK(!host_critical_depth);if(safe_required)off();*h=1;if(mode==NVS_READONLY)return ESP_ERR_NVS_NOT_FOUND;return fail_open?ESP_FAIL:ESP_OK;}
int nvs_get_u32(int h,const char*k,uint32_t*v){return ESP_ERR_NVS_NOT_FOUND;}
int nvs_get_u8(int h,const char*k,uint8_t*v){return ESP_ERR_NVS_NOT_FOUND;}
int nvs_get_blob(int h,const char*k,void*v,size_t*l){return ESP_ERR_NVS_NOT_FOUND;}
int nvs_set_u32(int h,const char*k,uint32_t v){CHECK(!host_critical_depth);if(safe_required)off();return fail_write?ESP_FAIL:ESP_OK;}
int nvs_set_u8(int h,const char*k,uint8_t v){return nvs_set_u32(h,k,v);}
int nvs_set_blob(int h,const char*k,const void*v,size_t n){return nvs_set_u32(h,k,0);}
int nvs_commit(int h){
 CHECK(!host_critical_depth);if(safe_required)off();
 pthread_mutex_lock(&gate);if(block_commit){commit_entered=true;pthread_cond_broadcast(&cond);while(!release_commit)pthread_cond_wait(&cond,&gate);block_commit=false;}pthread_mutex_unlock(&gate);
 return fail_commit?ESP_FAIL:ESP_OK;
}
void nvs_close(int h){}
static void fresh(void){CHECK(dura_meter_init()==ESP_OK);test_board_bind();CHECK(dura_board_sync_outputs()==ESP_OK);off();}
static void start_batch(void){CHECK(dura_meter_start_batch(10)==ESP_OK);CHECK(dura_board_sync_outputs()==ESP_OK);CHECK(gpio[0]);}
static void shutdown_test(const char*t){
 start_batch();safe_required=true;fail_commit=1;int e=ESP_OK;
 if(!strcmp(t,"pause"))e=dura_meter_pause_batch();
 else if(!strcmp(t,"cancel"))e=dura_meter_cancel_batch();
 else if(!strcmp(t,"home"))e=dura_meter_set_screen_home();
 else if(!strcmp(t,"fault"))e=dura_meter_set_fault(7);
 else if(!strcmp(t,"sleep"))e=dura_meter_prepare_for_sleep();
 else if(!strcmp(t,"completion"))e=dura_meter_record_pulse(snap().preset_batch_counts);
 else if(!strcmp(t,"defaults"))e=dura_meter_factory_defaults();
 else if(!strcmp(t,"load"))e=dura_meter_load();
 else CHECK(false);
 off();CHECK(!snap().pump_enabled);(void)e;
}
static void start_failure(const char*t){
 on_edges=0;
 if(strstr(t,"open"))fail_open=1;else if(strstr(t,"write"))fail_write=1;else fail_commit=1;
 CHECK(dura_meter_start_batch(10)!=ESP_OK);CHECK(dura_board_sync_outputs()==ESP_OK);off();CHECK(!snap().pump_enabled);CHECK(on_edges==0);
}
static void resume_failure(void){start_batch();CHECK(dura_meter_pause_batch()==ESP_OK);CHECK(dura_board_sync_outputs()==ESP_OK);on_edges=0;fail_commit=1;CHECK(dura_meter_resume_batch()!=ESP_OK);CHECK(dura_board_sync_outputs()==ESP_OK);off();CHECK(!snap().pump_enabled);CHECK(on_edges==0);}
static int start_result,stop_result;
static void *start_worker(void*p){start_result=dura_meter_start_batch(10);return NULL;}
static void *home_worker(void*p){stop_thread=true;stop_result=dura_meter_set_screen_home();return NULL;}
static void pending(const char*t){
 pthread_t a,b;block_commit=true;CHECK(!pthread_create(&a,NULL,start_worker,NULL));
 pthread_mutex_lock(&gate);while(!commit_entered)pthread_cond_wait(&cond,&gate);pthread_mutex_unlock(&gate);
 CHECK(dura_board_sync_outputs()==ESP_OK);off();CHECK(!snap().pump_enabled);
 if(!strcmp(t,"pending_home")){CHECK(!pthread_create(&b,NULL,home_worker,NULL));pthread_mutex_lock(&gate);while(!stop_waiting)pthread_cond_wait(&cond,&gate);pthread_mutex_unlock(&gate);}
 else if(!strcmp(t,"pending_defaults"))CHECK(dura_meter_factory_defaults()==ESP_OK);
 else CHECK(dura_meter_load()==ESP_OK);
 pthread_mutex_lock(&gate);release_commit=true;pthread_cond_broadcast(&cond);pthread_mutex_unlock(&gate);CHECK(!pthread_join(a,NULL));
 if(!strcmp(t,"pending_home")){CHECK(!pthread_join(b,NULL));CHECK(stop_result==ESP_OK);}
 CHECK(start_result!=ESP_OK);CHECK(!snap().pump_enabled);off();
}
static void cal_wait(void){CHECK(dura_meter_start_calibration()==ESP_OK);CHECK(dura_meter_record_pulse(250)==ESP_OK);CHECK(dura_meter_continue_calibration()==ESP_OK);}
static void cal_test(const char*t){
 if(!strcmp(t,"cal_stale_batch_cancel")){start_batch();CHECK(dura_meter_record_pulse(snap().preset_batch_counts)==ESP_OK);}
 cal_wait();dura_meter_snapshot_t before=snap();
 if(!strcmp(t,"cal_units")){CHECK(dura_meter_set_units(DURA_UNITS_LITER)==ESP_ERR_INVALID_STATE);CHECK(snap().selected_units==before.selected_units);}
 else if(!strcmp(t,"cal_coefficients")){CHECK(dura_meter_apply_quick_cal(1)==ESP_ERR_INVALID_STATE);CHECK(dura_meter_select_precal_profile(0)==ESP_ERR_INVALID_STATE);}
 else if(!strcmp(t,"cal_manual_stop"))CHECK(dura_meter_stop_manual()==ESP_ERR_INVALID_STATE);
 else CHECK(dura_meter_cancel_batch()==ESP_ERR_INVALID_STATE);
 CHECK(snap().calibration_mode==DURA_CAL_WAIT_MEASURED_AMOUNT);CHECK(dura_meter_cancel_calibration()==ESP_OK);CHECK(dura_meter_start_manual()==ESP_OK);
}
static void owner_test(bool aba){
#ifdef DURA_METER_HAS_SYNC_OUTPUT_HANDLER
 uint32_t a,b;CHECK(dura_meter_claim_recipe(&a)==ESP_OK);CHECK(a);
 if(aba){CHECK(dura_meter_factory_defaults()==ESP_OK);CHECK(dura_meter_claim_recipe(&b)==ESP_OK);CHECK(a!=b);CHECK(dura_meter_start_owned_batch(1,a)==ESP_ERR_INVALID_STATE);CHECK(dura_meter_release_recipe(a)==ESP_ERR_INVALID_STATE);CHECK(snap().recipe_owner_id==b);CHECK(dura_meter_release_recipe(b)==ESP_OK);}
 else {CHECK(dura_meter_start_manual()==ESP_ERR_INVALID_STATE);CHECK(dura_meter_start_batch(1)==ESP_ERR_INVALID_STATE);CHECK(dura_meter_start_calibration()==ESP_ERR_INVALID_STATE);CHECK(dura_meter_set_units(DURA_UNITS_LITER)==ESP_ERR_INVALID_STATE);CHECK(dura_meter_start_owned_batch(1,a)==ESP_OK);CHECK(dura_meter_release_recipe(a)==ESP_OK);off();CHECK(dura_meter_resume_batch()==ESP_ERR_INVALID_STATE);}
#else
 CHECK(false && "No meter ownership contract in baseline");
#endif
}
static void *sync_worker(void*p){for(int i=0;i<10000;++i)CHECK(dura_board_sync_outputs()==ESP_OK);return NULL;}
static void threaded(void){pthread_t b;CHECK(!pthread_create(&b,NULL,sync_worker,NULL));for(int i=0;i<500;++i){CHECK(dura_meter_start_manual()==ESP_OK);CHECK(dura_meter_stop_manual()==ESP_OK);off();}CHECK(!pthread_join(b,NULL));off();}
int main(int argc,char**argv){CHECK(argc==2);fresh();const char*t=argv[1];
 if(!strncmp(t,"start_",6))start_failure(t);
 else if(!strcmp(t,"resume_failure"))resume_failure();
 else if(!strncmp(t,"pending_",8))pending(t);
 else if(!strncmp(t,"cal_",4))cal_test(t);
 else if(!strcmp(t,"stale_snapshot")){CHECK(dura_meter_start_manual()==ESP_OK);CHECK(dura_board_sync_outputs()==ESP_OK);snapshot_fault=true;CHECK(dura_board_sync_outputs()==ESP_OK);if(snapshot_fault){snapshot_fault=false;CHECK(dura_meter_set_fault(7)==ESP_OK);CHECK(dura_board_sync_outputs()==ESP_OK);}CHECK(snap().fault_latched);off();}
 else if(!strcmp(t,"sleep_prepare_guard")){CHECK(dura_meter_start_manual()==ESP_OK);CHECK(dura_meter_prepare_for_sleep()==ESP_OK);off();CHECK(dura_meter_start_manual()==ESP_ERR_INVALID_STATE);CHECK(dura_meter_factory_defaults()==ESP_OK);CHECK(dura_meter_start_manual()==ESP_ERR_INVALID_STATE);
#ifdef DURA_METER_HAS_SLEEP_FENCE
 CHECK(dura_meter_cancel_sleep_prepare()==ESP_OK);CHECK(dura_meter_start_manual()==ESP_OK);
#endif
 }
 else if(!strcmp(t,"owner_exclusion"))owner_test(false);
 else if(!strcmp(t,"owner_aba"))owner_test(true);
 else if(!strcmp(t,"happy_recirc")){CHECK(dura_meter_start_batch_mode(10,DURA_OPERATION_RECIRC)==ESP_OK);CHECK(dura_board_sync_outputs()==ESP_OK);CHECK(gpio[0]&&gpio[1]&&!gpio[2]);CHECK(dura_meter_pause_batch()==ESP_OK);CHECK(dura_board_sync_outputs()==ESP_OK);off();CHECK(dura_meter_resume_batch()==ESP_OK);CHECK(dura_board_sync_outputs()==ESP_OK);CHECK(gpio[0]&&gpio[1]&&!gpio[2]);}
 else if(!strcmp(t,"redundant_sync")){start_batch();int n=on_edges;for(int i=0;i<100;++i)CHECK(dura_board_sync_outputs()==ESP_OK);CHECK(on_edges==n);}
 else if(!strcmp(t,"threaded_output_serialization"))threaded();
 else shutdown_test(t);
 printf("PASS %s\n",t);return 0;
}
