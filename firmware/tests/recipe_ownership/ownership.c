/* Behavioral host tests: compile real coordinator, recipe and meter C unchanged.
 * Only RTOS/NVS/peer boundaries are stubbed. One fresh process per test. */
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dura_meter.h"
#include "dura_recipe.h"
#include "dura_coordinator.h"
#include "dura_peers.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__func__,__LINE__,#x); exit(1); } } while (0)
static int fail_commit, commits, active_commits, aborts, starts, completions, acks;
static int peer_start_error, peer_ack_error, peer_complete_error;
static bool peer_available;
static int64_t now_us=1000000;
static dura_peer_event_cb_t peer_cb;
static dura_peer_event_t sent;
static void (*coord_task)(void*);
static _Thread_local jmp_buf task_return;
static _Thread_local int delay_calls;
_Thread_local unsigned host_critical_depth;
static _Thread_local int mutex_depth;
static bool require_mutex;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static bool block_commit, commit_entered, release_commit, abort_waiting;
static _Thread_local bool is_abort_thread;
static int threaded_start_result, threaded_abort_result;
static bool pause_claim, claim_entered, release_claim;
static bool pause_release, release_entered, allow_release;
esp_err_t __real_dura_meter_release_recipe(uint32_t owner);
esp_err_t __wrap_dura_meter_release_recipe(uint32_t owner){
 pthread_mutex_lock(&gate);
 if(pause_release && is_abort_thread){release_entered=true;pthread_cond_broadcast(&condition);while(!allow_release)pthread_cond_wait(&condition,&gate);pause_release=false;}
 pthread_mutex_unlock(&gate);return __real_dura_meter_release_recipe(owner);
}
static int output_on_publications;
#ifdef DURA_METER_HAS_SYNC_OUTPUT_HANDLER
static void observe_outputs(dura_output_intent_t intent){if(intent.pump)++output_on_publications;}
#endif
esp_err_t __real_dura_meter_claim_recipe(uint32_t *owner);
esp_err_t __wrap_dura_meter_claim_recipe(uint32_t *owner){
 esp_err_t err=__real_dura_meter_claim_recipe(owner);
 pthread_mutex_lock(&gate);
 if(pause_claim && err==ESP_OK){claim_entered=true;pthread_cond_broadcast(&condition);while(!release_claim)pthread_cond_wait(&condition,&gate);pause_claim=false;}
 pthread_mutex_unlock(&gate);return err;
}

static void boundary(void) { if (require_mutex) CHECK(mutex_depth > 0); }
int64_t esp_timer_get_time(void){return now_us;}
SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    pthread_mutex_t *m=malloc(sizeof(*m)); CHECK(m); CHECK(!pthread_mutex_init(m,NULL)); return m;
}
int xSemaphoreTake(SemaphoreHandle_t m,unsigned t){(void)t; CHECK(!host_critical_depth); if(is_abort_thread){pthread_mutex_lock(&gate);abort_waiting=true;pthread_cond_broadcast(&condition);pthread_mutex_unlock(&gate);} CHECK(!pthread_mutex_lock(m)); ++mutex_depth; return pdTRUE;}
void xSemaphoreGive(SemaphoreHandle_t m){--mutex_depth; CHECK(!pthread_mutex_unlock(m));}
int nvs_open(const char*s,int m,nvs_handle_t*h){(void)s; *h=1; return m==NVS_READONLY?ESP_ERR_NVS_NOT_FOUND:ESP_OK;}
int nvs_get_u32(int h,const char*k,uint32_t*v){return ESP_ERR_NVS_NOT_FOUND;}
int nvs_get_u8(int h,const char*k,uint8_t*v){return ESP_ERR_NVS_NOT_FOUND;}
int nvs_get_blob(int h,const char*k,void*v,size_t*l){return ESP_ERR_NVS_NOT_FOUND;}
int nvs_set_u32(int h,const char*k,uint32_t v){return ESP_OK;}
int nvs_set_u8(int h,const char*k,uint8_t v){return ESP_OK;}
int nvs_set_blob(int h,const char*k,const void*v,size_t l){return ESP_OK;}
int nvs_commit(int h){dura_meter_snapshot_t m; CHECK(dura_meter_get_snapshot(&m)==ESP_OK); ++commits; active_commits+=m.pump_enabled; pthread_mutex_lock(&gate);if(block_commit){commit_entered=true;pthread_cond_broadcast(&condition);while(!release_commit)pthread_cond_wait(&condition,&gate);block_commit=false;}pthread_mutex_unlock(&gate);return fail_commit?ESP_FAIL:ESP_OK;}
void nvs_close(int h){}
int xTaskCreate(void (*fn)(void*),const char*n,unsigned s,void*a,unsigned p,TaskHandle_t*h){coord_task=fn; *h=(void*)1; return pdPASS;}
void vTaskDelay(unsigned t){if(delay_calls++>0)longjmp(task_return,1);}
static void tick(void){delay_calls=0; now_us+=100000; if(setjmp(task_return)==0)coord_task(NULL); CHECK(mutex_depth==0);}
int dura_peers_register_event_callback(dura_peer_event_cb_t cb,void*ctx){peer_cb=cb;return ESP_OK;}
int dura_peers_get_snapshot(dura_peers_snapshot_t*p){boundary(); memset(p,0,sizeof(*p)); if(peer_available){p->peer_count=1; strcpy(p->peers[0].liquid_name,"Remote"); p->peers[0].last_seen_ms=now_us/1000;} return ESP_OK;}
int dura_peers_send_abort(void){boundary(); ++aborts; return ESP_ERR_NOT_SUPPORTED;}
int dura_peers_send_recipe_ingredient_start(const char*r,const dura_recipe_ingredient_t*i,uint32_t t){boundary(); ++starts; sent=(dura_peer_event_t){.type=DURA_PEER_MSG_RECIPE_COMPLETE,.sequence=i->sequence,.claim_id=t}; strcpy(sent.recipe_name,r); strcpy(sent.target_liquid,i->liquid_name); return peer_start_error;}
int dura_peers_send_recipe_ack(const char*r,const char*l,uint8_t s,uint32_t t){boundary(); ++acks; return peer_ack_error;}
int dura_peers_send_recipe_complete(const char*r,const char*l,uint8_t s,uint32_t t){boundary(); ++completions; return peer_complete_error;}
static dura_meter_snapshot_t meter(void){dura_meter_snapshot_t m;CHECK(dura_meter_get_snapshot(&m)==ESP_OK);return m;}
static dura_coordinator_snapshot_t coord(void){dura_coordinator_snapshot_t c;CHECK(dura_coordinator_get_snapshot(&c)==ESP_OK);return c;}
static void fresh(void){CHECK(dura_meter_init()==ESP_OK);CHECK(dura_coordinator_init()==ESP_OK);CHECK(dura_recipe_init()==ESP_OK);CHECK(dura_recipe_select_liquid_profile(17)==ESP_OK);CHECK(dura_meter_set_auto_batch(true)==ESP_OK);}
static void recipe(dura_recipe_order_t order){CHECK(dura_recipe_reset_default("proof",order)==ESP_OK);}
static void ingredient(const char *name,float amount,int seq){CHECK(dura_recipe_add_default_ingredient(name,amount,seq)==ESP_OK);}
static void two_step(void){recipe(DURA_RECIPE_ORDER_CONFIGURED);ingredient("Water 66F",1,1);ingredient("Water 66F",2,2);}
static void start(void){CHECK(dura_coordinator_start_recipe_as_leader("proof")==ESP_OK);}
static void done(void){CHECK(dura_meter_record_pulse(meter().preset_batch_counts)==ESP_OK);CHECK(!meter().pump_enabled);}
static void unrelated_done(void){CHECK(dura_meter_start_batch(.5f)==ESP_OK);done();}
static void invalidate(const char *kind){
 if(!strcmp(kind,"cancel")) CHECK(dura_meter_cancel_batch()==ESP_OK);
 else if(!strcmp(kind,"home")){CHECK(dura_meter_cancel_batch()==ESP_OK);CHECK(dura_meter_set_screen_home()==ESP_OK);}
 else if(!strcmp(kind,"fault")){CHECK(dura_meter_set_fault(7)==ESP_OK);CHECK(dura_meter_clear_fault()==ESP_OK);}
 else if(!strcmp(kind,"defaults"))CHECK(dura_meter_factory_defaults()==ESP_OK);
 else if(!strcmp(kind,"load"))CHECK(dura_meter_load()==ESP_OK);
 else if(!strcmp(kind,"sleep")){
  CHECK(dura_meter_prepare_for_sleep()==ESP_OK);
#ifdef DURA_METER_HAS_SLEEP_FENCE
  /* Model board preparation being abandoned without sleeping/rebooting. */
  CHECK(dura_meter_cancel_sleep_prepare()==ESP_OK);
#endif
 }
 else CHECK(false);
}
static dura_peer_event_t follower(void){dura_peer_event_t e={.type=DURA_PEER_MSG_RECIPE_START,.amount=1,.sequence=1,.claim_id=42,.src_mac={1}};strcpy(e.recipe_name,"follower");strcpy(e.target_liquid,"Water 66F");return e;}
static void test_invalidation(const char *kind){two_step();start();invalidate(kind);/* Legacy fault/sleep pause; resolve that state before unrelated work. */if(meter().batch_mode==DURA_BATCH_PAUSED)CHECK(dura_meter_cancel_batch()==ESP_OK);unrelated_done();tick();CHECK(!meter().pump_enabled);CHECK(coord().state!=DURA_COORDINATOR_LEADER_RUNNING);}
static void test_duplicate(int order,bool later){recipe(order);if(later)ingredient("Water 66F",3,1);ingredient("Water 66F",1,later?2:1);ingredient("water 66f",order?1:2,later?2:1);commits=active_commits=0;CHECK(dura_coordinator_start_recipe_as_leader("proof")!=ESP_OK);CHECK(!meter().pump_enabled);CHECK(active_commits==0);CHECK(commits==0);CHECK(starts==0);}
static void test_happy(void){two_step();start();CHECK(meter().pump_enabled);CHECK(dura_meter_pause_batch()==ESP_OK);tick();CHECK(meter().batch_mode==DURA_BATCH_PAUSED);CHECK(dura_meter_resume_batch()==ESP_OK);done();tick();CHECK(meter().pump_enabled && meter().batch_preset_gal==2);done();tick();CHECK(coord().state==DURA_COORDINATOR_IDLE);CHECK(dura_meter_start_manual()==ESP_OK);}
static void test_abort(void){two_step();start();CHECK(dura_coordinator_abort()==ESP_OK);CHECK(!meter().pump_enabled);CHECK(meter().batch_mode!=DURA_BATCH_PAUSED);CHECK(dura_meter_resume_batch()!=ESP_OK);start();}
static void test_unrelated_abort(bool peer){CHECK(dura_meter_start_batch(5)==ESP_OK);if(peer){dura_peer_event_t e={.type=DURA_PEER_MSG_RECIPE_ABORT};peer_cb(&e,NULL);}else CHECK(dura_coordinator_abort()==ESP_OK);CHECK(meter().pump_enabled);}
static void mixed(bool remote_first){peer_available=true;recipe(DURA_RECIPE_ORDER_CONFIGURED);if(remote_first)ingredient("Remote",1,1);ingredient("Water 66F",1,1);if(!remote_first)ingredient("Remote",1,1);}
static void test_peer_failure(void){mixed(false);peer_start_error=ESP_FAIL;CHECK(dura_coordinator_start_recipe_as_leader("proof")!=ESP_OK);CHECK(!meter().pump_enabled);CHECK(meter().batch_mode!=DURA_BATCH_PAUSED);CHECK(aborts>0);CHECK(dura_meter_start_manual()==ESP_OK);}
static void test_save_failure(void){two_step();fail_commit=1;CHECK(dura_coordinator_start_recipe_as_leader("proof")!=ESP_OK);CHECK(!meter().pump_enabled);fail_commit=0;CHECK(dura_meter_start_manual()==ESP_OK);}
static void test_timeout(bool unrelated){mixed(false);start();if(unrelated){CHECK(dura_meter_cancel_batch()==ESP_OK);CHECK(dura_meter_start_batch(5)==ESP_OK);}now_us+=6000000;tick();CHECK(meter().pump_enabled==unrelated);CHECK(aborts>0);CHECK(coord().state!=DURA_COORDINATOR_LEADER_RUNNING);if(!unrelated)CHECK(coord().last_failed_transaction_id!=0);}
static void test_follower_cancel(bool replay){dura_peer_event_t e=follower();peer_cb(&e,NULL);CHECK(meter().pump_enabled);CHECK(dura_meter_cancel_batch()==ESP_OK);if(replay){tick();peer_cb(&e,NULL);CHECK(!meter().pump_enabled);}else{unrelated_done();tick();CHECK(completions==0);}}
static void test_follower_ack_failure(void){peer_ack_error=ESP_FAIL;dura_peer_event_t e=follower();peer_cb(&e,NULL);CHECK(!meter().pump_enabled);CHECK(aborts>0);}
static void test_follower_happy(void){dura_peer_event_t e=follower();peer_cb(&e,NULL);CHECK(acks==1);done();tick();CHECK(completions==1);CHECK(dura_meter_start_manual()==ESP_OK);}
static void test_late_complete(void){mixed(false);start();dura_peer_event_t e=sent;CHECK(dura_meter_cancel_batch()==ESP_OK);CHECK(dura_meter_start_batch(5)==ESP_OK);peer_cb(&e,NULL);tick();CHECK(meter().pump_enabled);CHECK(coord().state!=DURA_COORDINATOR_LEADER_RUNNING);CHECK(aborts>0);}
static void test_reinit(void){two_step();start();CHECK(dura_coordinator_init()==ESP_OK);done();tick();CHECK(meter().pump_enabled && meter().batch_preset_gal==2);}
static void test_lock(void){mixed(false);require_mutex=true;start();tick();dura_peer_event_t e=sent;peer_cb(&e,NULL);CHECK(dura_coordinator_abort()==ESP_OK);}
static void *start_worker(void*p){threaded_start_result=dura_coordinator_start_recipe_as_leader("proof");return NULL;}
static void *abort_worker(void*p){is_abort_thread=true;threaded_abort_result=dura_coordinator_abort();return NULL;}
static void test_pending_abort(void){
 two_step();pthread_t start_thread,abort_thread;block_commit=true;
 CHECK(!pthread_create(&start_thread,NULL,start_worker,NULL));
 pthread_mutex_lock(&gate);while(!commit_entered)pthread_cond_wait(&condition,&gate);pthread_mutex_unlock(&gate);
 CHECK(!pthread_create(&abort_thread,NULL,abort_worker,NULL));
 pthread_mutex_lock(&gate);while(!abort_waiting)pthread_cond_wait(&condition,&gate);release_commit=true;pthread_cond_broadcast(&condition);pthread_mutex_unlock(&gate);
 CHECK(!pthread_join(start_thread,NULL));CHECK(!pthread_join(abort_thread,NULL));
 CHECK(threaded_start_result!=ESP_OK);CHECK(threaded_abort_result==ESP_OK);CHECK(!meter().pump_enabled);CHECK(coord().state==DURA_COORDINATOR_IDLE);
}
static void *follower_worker(void*p){dura_peer_event_t e=follower();peer_cb(&e,NULL);return NULL;}
static void test_unpublished_claim(bool replay){
#ifdef DURA_METER_HAS_SYNC_OUTPUT_HANDLER
 CHECK(dura_meter_set_output_handler(observe_outputs)==ESP_OK);
#endif
 pthread_t follower_thread,abort_thread;pause_claim=true;
 CHECK(!pthread_create(&follower_thread,NULL,follower_worker,NULL));
 pthread_mutex_lock(&gate);while(!claim_entered)pthread_cond_wait(&condition,&gate);pthread_mutex_unlock(&gate);
 CHECK(!pthread_create(&abort_thread,NULL,abort_worker,NULL));
 pthread_mutex_lock(&gate);while(!abort_waiting)pthread_cond_wait(&condition,&gate);release_claim=true;pthread_cond_broadcast(&condition);pthread_mutex_unlock(&gate);
 CHECK(!pthread_join(follower_thread,NULL));CHECK(!pthread_join(abort_thread,NULL));
 CHECK(output_on_publications==0);CHECK(!meter().pump_enabled);CHECK(coord().state==DURA_COORDINATOR_IDLE);
 if(replay){dura_peer_event_t e=follower();peer_cb(&e,NULL);tick();CHECK(output_on_publications==0);CHECK(!meter().pump_enabled);}
}
static void test_reserved_cancel(void){
#ifdef DURA_METER_HAS_SYNC_OUTPUT_HANDLER
 uint32_t owner;CHECK(dura_meter_claim_recipe(&owner)==ESP_OK);CHECK(dura_meter_cancel_batch()==ESP_OK);CHECK(dura_meter_start_owned_batch(1,owner)==ESP_ERR_INVALID_STATE);CHECK(meter().recipe_owner_id==0);
#else
 CHECK(false);
#endif
}
static void test_remote_first_cancel(void){peer_available=true;recipe(DURA_RECIPE_ORDER_CONFIGURED);ingredient("Remote",1,1);ingredient("Water 66F",1,2);start();CHECK(!meter().pump_enabled);CHECK(dura_meter_cancel_batch()==ESP_OK);dura_peer_event_t e=sent;peer_cb(&e,NULL);tick();CHECK(!meter().pump_enabled);CHECK(coord().state==DURA_COORDINATOR_IDLE);}
static void test_abort_newer_owner(void){
 two_step();start();pause_release=true;pthread_t abort_thread;CHECK(!pthread_create(&abort_thread,NULL,abort_worker,NULL));
 pthread_mutex_lock(&gate);while(!release_entered)pthread_cond_wait(&condition,&gate);pthread_mutex_unlock(&gate);
 CHECK(dura_meter_set_screen_home()==ESP_OK);tick();start();CHECK(meter().pump_enabled);
 pthread_mutex_lock(&gate);allow_release=true;pthread_cond_broadcast(&condition);pthread_mutex_unlock(&gate);
 CHECK(!pthread_join(abort_thread,NULL));CHECK(threaded_abort_result==ESP_OK);CHECK(meter().pump_enabled);CHECK(coord().state==DURA_COORDINATOR_LEADER_RUNNING);
}
int main(int argc,char **argv){CHECK(argc==2);fresh();const char*t=argv[1];
 if(!strncmp(t,"invalidate_",11))test_invalidation(t+11);
 else if(!strcmp(t,"duplicate_configured"))test_duplicate(0,false);
 else if(!strcmp(t,"duplicate_largest"))test_duplicate(1,false);
 else if(!strcmp(t,"duplicate_smallest"))test_duplicate(2,false);
 else if(!strcmp(t,"duplicate_later"))test_duplicate(0,true);
 else if(!strcmp(t,"happy_pause_resume"))test_happy();
 else if(!strcmp(t,"abort_revokes"))test_abort();
 else if(!strcmp(t,"abort_unrelated"))test_unrelated_abort(false);
 else if(!strcmp(t,"peer_abort_unrelated"))test_unrelated_abort(true);
 else if(!strcmp(t,"peer_failure"))test_peer_failure();
 else if(!strcmp(t,"save_failure"))test_save_failure();
 else if(!strcmp(t,"ack_timeout"))test_timeout(false);
 else if(!strcmp(t,"timeout_unrelated"))test_timeout(true);
 else if(!strcmp(t,"follower_cancel"))test_follower_cancel(false);
 else if(!strcmp(t,"follower_replay"))test_follower_cancel(true);
 else if(!strcmp(t,"follower_ack_failure"))test_follower_ack_failure();
 else if(!strcmp(t,"follower_happy"))test_follower_happy();
 else if(!strcmp(t,"late_complete"))test_late_complete();
 else if(!strcmp(t,"reinit_preserves_owner"))test_reinit();
 else if(!strcmp(t,"serialized_boundaries"))test_lock();
 else if(!strcmp(t,"pending_abort"))test_pending_abort();
 else if(!strcmp(t,"unpublished_follower_abort"))test_unpublished_claim(false);
 else if(!strcmp(t,"unpublished_follower_replay"))test_unpublished_claim(true);
 else if(!strcmp(t,"abort_preserves_newer_owner"))test_abort_newer_owner();
 else if(!strcmp(t,"reserved_cancel"))test_reserved_cancel();
 else if(!strcmp(t,"remote_first_cancel"))test_remote_first_cancel();
 else CHECK(false);
 printf("PASS %s\n",t);return 0;
}
