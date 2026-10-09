/* Driver injection only; board transaction and policy decision are extracted verbatim. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "dura_meter.h"
#include "freertos/semphr.h"
#include "nvs.h"
#define CHECK(x) do { if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);} }while(0)
#define ESP_RETURN_ON_ERROR(x,...) do{int e=(x);if(e)return e;}while(0)
#define ESP_RETURN_ON_FALSE(x,e,...) do{if(!(x))return e;}while(0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_ERR_NOT_SUPPORTED 0x106
#define CONFIG_DURA_BOARD_BUTTON_PULLUP 1
#define CONFIG_DURA_BOARD_BUTTON_PULLDOWN 0
#define CONFIG_DURA_BOARD_FLOW_PULLUP 1
#define CONFIG_DURA_BOARD_FLOW_PULLDOWN 0
#define CONFIG_DURA_BOARD_BUTTON_ACTIVE_LOW 1
#define CONFIG_DURA_BOARD_ENABLE_PUMP_OUTPUT 1
#define CONFIG_DURA_BOARD_ENABLE_EV_OUTPUTS 1
#define CONFIG_DURA_BOARD_DEEP_SLEEP_DELAY_SEC 300
#define DURA_BUTTON_ACTIVE_LEVEL 0
#define DURA_FLOW_ACTIVE_LEVEL 0
#define DURA_GPIO_PUMP_OUTPUT 8
#define DURA_GPIO_RECIRC_EV_OUTPUT 9
#define DURA_GPIO_INJECT_EV_OUTPUT 10
#define DURA_PUMP_OUTPUT_INACTIVE_LEVEL 0
#define DURA_RECIRC_EV_INACTIVE_LEVEL 0
#define DURA_INJECT_EV_INACTIVE_LEVEL 0
#define RTC_GPIO_MODE_INPUT_ONLY 1
#define GPIO_MODE_INPUT 1
#define GPIO_PULLUP_ENABLE 1
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_ENABLE 1
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
#define GPIO_INTR_ANYEDGE 3
#define ESP_EXT1_WAKEUP_ANY_LOW 1
#define ESP_EXT1_WAKEUP_ANY_HIGH 2
#define ESP_SLEEP_WAKEUP_EXT1 3
#define DURA_BUTTON_NONE 0
#define CONFIG_DURA_BOARD_LCD_HOLD_SPLASH_ON_BOOT 0
#define CONFIG_DURA_BOARD_ENABLE_RECIRC_INPUT 0
typedef int gpio_num_t;
typedef struct {uint64_t pin_bit_mask;int mode,pull_up_en,pull_down_en,intr_type;} gpio_config_t;
static struct {int gpio;const char *name;} s_buttons[]={{0,"a"},{1,"b"},{2,"c"},{3,"d"}},s_flow_inputs[]={{4,"a"},{5,"b"}};
static bool s_ble_connected,s_ignore_buttons_until_release,s_sleep_requested,s_sleep_recovery_pending;
static int64_t s_last_activity_us,s_backlight_activity_us,now=1000000000;
static uint64_t s_activity_generation,s_sleep_touched_mask;
static bool s_sleep_ext1_attempted;
static portMUX_TYPE s_ui_snapshot_mux=portMUX_INITIALIZER_UNLOCKED;
static void *s_task=(void*)1;
static bool light=true,ext1,rtc[6],hold[6],output[3];
static bool expected_policy_light=true,expected_recovery_light=true;
static unsigned light_on_calls;
static int intr[6]={0,0,0,0,3,3},step,fail_step,fail_open,fail_write,fail_commit,sleep_calls,restore_error,screen=17;
static const char *test;
static jmp_buf slept;
_Thread_local unsigned host_critical_depth;
void dura_board_note_activity(void);
esp_err_t dura_board_set_ble_connected(bool);
static void inject(const char *phase){
 if(!strstr(test,phase))return;
 if(strstr(test,"activity"))dura_board_note_activity();
 if(strstr(test,"ble")){dura_board_set_ble_connected(true);if(strstr(test,"aba"))dura_board_set_ble_connected(false);}
}
int64_t esp_timer_get_time(void){return now;}
void *xTaskGetCurrentTaskHandle(void){return s_task;}
SemaphoreHandle_t xSemaphoreCreateMutex(void){pthread_mutex_t*m=malloc(sizeof(*m));pthread_mutex_init(m,0);return m;}
int xSemaphoreTake(SemaphoreHandle_t m,unsigned t){CHECK(!host_critical_depth);pthread_mutex_lock(m);return pdTRUE;}
void xSemaphoreGive(SemaphoreHandle_t m){pthread_mutex_unlock(m);}
int nvs_open(const char*s,int mode,nvs_handle_t*h){*h=1;if(mode==NVS_READONLY)return ESP_ERR_NVS_NOT_FOUND;inject("save");return fail_open?ESP_FAIL:ESP_OK;}
int nvs_get_u32(int h,const char*k,uint32_t*v){return ESP_ERR_NVS_NOT_FOUND;}
int nvs_get_u8(int h,const char*k,uint8_t*v){return ESP_ERR_NVS_NOT_FOUND;}
int nvs_get_blob(int h,const char*k,void*v,size_t*l){return ESP_ERR_NVS_NOT_FOUND;}
int nvs_set_u32(int h,const char*k,uint32_t v){return fail_write?ESP_FAIL:ESP_OK;}
int nvs_set_u8(int h,const char*k,uint8_t v){return nvs_set_u32(h,k,v);}
int nvs_set_blob(int h,const char*k,const void*v,size_t n){return nvs_set_u32(h,k,0);}
int nvs_commit(int h){return fail_commit?ESP_FAIL:ESP_OK;}
void nvs_close(int h){}
static void outputs(dura_output_intent_t i){CHECK(!i.pump&&!i.recirc_ev&&!i.inject_ev);output[0]=i.pump;output[1]=i.recirc_ev;output[2]=i.inject_ev;}
int dura_board_sync_outputs(void){return dura_meter_sync_outputs();}
uint64_t dura_board_button_wake_mask(void){return 15;}
uint64_t dura_board_flow_wake_mask(void){return 48;}
int dura_lcd_set_backlight(bool b){light=b;if(b)++light_on_calls;return ESP_OK;}
int gpio_set_level(int p,int v){CHECK(!v);output[p-8]=v;return ESP_OK;}
static int prep_step(void){CHECK(light==expected_policy_light);CHECK(!output[0]&&!output[1]&&!output[2]);inject("rtc");++step;return step==fail_step?ESP_FAIL:ESP_OK;}
int rtc_gpio_init(int p){rtc[p]=true;return prep_step();}
int rtc_gpio_set_direction(int p,int m){return prep_step();}
int rtc_gpio_pullup_en(int p){return prep_step();}
int rtc_gpio_pullup_dis(int p){return prep_step();}
int rtc_gpio_pulldown_en(int p){return prep_step();}
int rtc_gpio_pulldown_dis(int p){return prep_step();}
int rtc_gpio_hold_en(int p){hold[p]=true;return prep_step();}
static void recovery_check(void){CHECK(light==expected_recovery_light);CHECK(dura_meter_start_manual()==ESP_ERR_INVALID_STATE);}
int rtc_gpio_hold_dis(int p){recovery_check();hold[p]=false;return ESP_OK;}
int rtc_gpio_deinit(int p){recovery_check();if(restore_error)return ESP_FAIL;rtc[p]=false;return ESP_OK;}
int gpio_config(const gpio_config_t*c){recovery_check();for(int i=0;i<6;i++)if(c->pin_bit_mask&(1ULL<<i)){CHECK(!rtc[i]);CHECK(c->pull_up_en&&!c->pull_down_en);intr[i]=c->intr_type;}return ESP_OK;}
int gpio_intr_disable(int p){intr[p]=0;return ESP_OK;}
int gpio_intr_enable(int p){return ESP_OK;}
int gpio_set_intr_type(int p,int t){intr[p]=t;return ESP_OK;}
void gpio_dump_io_configuration(FILE*f,uint64_t m){}
int esp_sleep_enable_ext1_wakeup(uint64_t m,int mode){ext1=true;return prep_step();}
int esp_sleep_disable_wakeup_source(int source){recovery_check();ext1=false;return ESP_OK;}
void esp_deep_sleep_start(void){++sleep_calls;longjmp(slept,1);}
/* Preparation/owner tests isolate BLE; lifecycle coverage lives in ble_sleep. */
int gama_blefob_sleep_shutdown(void){return ESP_OK;}
int gama_blefob_sleep_resume(void){return ESP_OK;}
int esp_deep_sleep_try_to_start(void){esp_deep_sleep_start();return ESP_FAIL;}
int dura_board_poll_button(void){return DURA_BUTTON_NONE;}
int rtc_gpio_get_level(int p){return 1;}
unsigned uxQueueMessagesWaiting(void*q){return 0;}
static void *s_flow_event_queue,*s_button_queue;
int dura_board_render_debug_screen(void){return ESP_OK;}
static void publish_ui_snapshot(const dura_meter_snapshot_t*m){}
/* BOARD_FUNCTIONS */
static void decision(void){
 int last_button=0;int64_t button_pressed_us=0,last_button_repeat_us=0;
 bool hold_repeat_enabled=false,main_outside_hold_pending=false,settings_hold_fired=false;
 /* BOARD_DECISION */
}
static void restored(void){CHECK(light==expected_policy_light&&screen==17);CHECK(!ext1);for(int i=0;i<6;i++)CHECK(!rtc[i]&&!hold[i]&&intr[i]==(i<4?0:3));CHECK(!output[0]&&!output[1]&&!output[2]);}
int main(int argc,char**argv){CHECK(argc==2);test=argv[1];CHECK(dura_meter_init()==ESP_OK);CHECK(dura_meter_set_output_handler(outputs)==ESP_OK);
 /* Board initialization is bypassed by this fixture. */
 dura_board_note_user_activity();update_backlight();
 if(!strcmp(test,"save_open"))fail_open=1;if(!strcmp(test,"save_write"))fail_write=1;if(!strcmp(test,"save_commit"))fail_commit=1;
 if(!strncmp(test,"gpio_",5))fail_step=atoi(test+5);
 if(!strcmp(test,"restore_fail")){fail_step=4;restore_error=1;}
 if(!strcmp(test,"known_ble"))dura_board_set_ble_connected(true);
 if(!strcmp(test,"decision_stale")){dura_board_note_activity();decision();CHECK(!sleep_calls);return 0;}
 if(!strcmp(test,"retry"))fail_step=4;
 int e=0;if(!setjmp(slept))e=dura_board_enter_deep_sleep();
 CHECK(!sleep_calls);CHECK(e!=ESP_OK);
 if(restore_error){CHECK(dura_meter_start_manual()==ESP_ERR_INVALID_STATE);CHECK(light);return 0;}
 restored();
 if(strcmp(test,"known_ble"))CHECK(s_last_activity_us==now);
 if(!strcmp(test,"retry")){fail_step=0;step=0;now+=400000000;test="success";if(!setjmp(slept))dura_board_enter_deep_sleep();CHECK(sleep_calls==1);CHECK(!light);}
 return 0;
}
