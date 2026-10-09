/* Real production functions are inserted by test_backlight_policy.py. */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "dura_meter.h"
#include "freertos/semphr.h"
#include "nvs.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %s:%d %s\n",__func__,__LINE__,#x);exit(1);}}while(0)
#define ESP_RETURN_ON_ERROR(x,...) do{int e=(x);if(e)return e;}while(0)
#define ESP_RETURN_ON_FALSE(x,e,...) do{if(!(x))return e;}while(0)
#define ESP_ERROR_CHECK(x) CHECK((x)==ESP_OK)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_ERR_TIMEOUT 0x107
#define RTC_DATA_ATTR
#define IRAM_ATTR
#define portENTER_CRITICAL_ISR(x) portENTER_CRITICAL(x)
#define portEXIT_CRITICAL_ISR(x) portEXIT_CRITICAL(x)
#define portYIELD_FROM_ISR() ((void)0)
#define CONFIG_DURA_BOARD_LCD_HOLD_SPLASH_ON_BOOT 0
#define CONFIG_DURA_BOARD_ENABLE_DEEP_SLEEP 1
#define CONFIG_DURA_BOARD_DEEP_SLEEP_DELAY_SEC 60
#define CONFIG_DURA_BOARD_SETTINGS_HOLD_MS 1000
#define CONFIG_DURA_BOARD_FLOW_PULLUP 1
#define CONFIG_DURA_BOARD_FLOW_PULLDOWN 0
#define CONFIG_DURA_BOARD_RECIRC_INPUT_PULLUP 1
#define CONFIG_DURA_BOARD_RECIRC_INPUT_PULLDOWN 0
#define DURA_BOARD_UI_PERIOD_MS 50
#define DURA_BOARD_SAVE_PERIOD_MS 30000
#define DURA_BOOT_LOGO_HOLD_MS 500
#define DURA_BOOT_POWERING_UP_HOLD_MS 2000
#define DURA_AUTO_BATCH_DEFAULT_GAL 10.0f
#define DURA_RECIRC_BATCH_DEFAULT_GAL 250.0f
#define pdMS_TO_TICKS(x) (x)
#define DURA_BUTTON_NONE 0
#define DURA_BUTTON_UP 1
#define DURA_BUTTON_DOWN 2
#define DURA_BUTTON_SELECT 3
#define DURA_BUTTON_BACK 4
#define DURA_GPIO_FLOW_A 4
#define DURA_GPIO_FLOW_B 5
#define DURA_GPIO_RECIRC_INPUT 8
#define DURA_RECIRC_INPUT_ACTIVE_LEVEL 1
#define DURA_FLOW_ACTIVE_LEVEL 1
#define GPIO_MODE_INPUT 1
#define GPIO_MODE_OUTPUT 2
#define GPIO_PULLUP_ENABLE 1
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_ENABLE 1
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
#define GPIO_INTR_ANYEDGE 3
#define ESP_INTR_FLAG_IRAM 1
#define ESP_SLEEP_WAKEUP_UNDEFINED 0
#define DURA_GPIO_LCD_SPI_CLK 11
#define DURA_GPIO_LCD_SPI_MOSI 12
#define DURA_GPIO_LCD_CS 13
#define DURA_GPIO_LCD_A0_DC 14
#define DURA_GPIO_LCD_RST 15
#define DURA_GPIO_LCD_BACKLIGHT 16
#define DURA_LCD_BACKLIGHT_ACTIVE_LEVEL 1
#define DURA_LCD_BACKLIGHT_INACTIVE_LEVEL 0
#define DURA_LCD_CS_INACTIVE_LEVEL 1
#define DURA_LCD_RST_ACTIVE_LEVEL 0
#define DURA_LCD_RST_INACTIVE_LEVEL 1

typedef int dura_button_t;
typedef struct {uint64_t pin_bit_mask;int mode,pull_up_en,pull_down_en,intr_type;} gpio_config_t;
#include "dura_ui_config.h"
typedef struct {unsigned rd,wr,cap,size; unsigned char data[2048];} queue_t;
static queue_t queues[2]; static unsigned queue_count;
static void *s_button_queue,*s_flow_event_queue;
static portMUX_TYPE s_ui_snapshot_mux=portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_flow_mux=portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_flow_boundary_mux=portMUX_INITIALIZER_UNLOCKED;
_Thread_local unsigned host_critical_depth;
static int64_t now=1000000, origin, s_last_activity_us, s_backlight_activity_us;
static uint64_t s_activity_generation;
static uint32_t s_flow_pulses;
static uint8_t s_last_valid_reed;
static bool s_ble_connected,s_ignore_buttons_until_release,s_sleep_requested;
static bool s_provisioning_qr_dismissed;
static bool light,in_owner,off_setting; static unsigned on_writes, sleeps, stage;
static int button, levels[32], wake;
static const char *test;
static unsigned timeout_sec=10;
static jmp_buf done;
static struct {int gpio;} s_flow_inputs[]={{4},{5}};
static void checkpoint(void);
int64_t esp_timer_get_time(void){return now;}
SemaphoreHandle_t xSemaphoreCreateMutex(void){pthread_mutex_t*m=malloc(sizeof(*m));CHECK(m);CHECK(!pthread_mutex_init(m,0));return m;}
int xSemaphoreTake(SemaphoreHandle_t m,unsigned t){CHECK(!host_critical_depth);CHECK(!pthread_mutex_lock(m));return pdTRUE;}
void xSemaphoreGive(SemaphoreHandle_t m){CHECK(!pthread_mutex_unlock(m));}
static bool persisted_nvs, inject_load_tick, invalid_marker, invalid_units, read_error;
static uint8_t stored_backlight=9;
static int open_error=ESP_OK;
static unsigned load_ticks;
static void load_boundary_tick(void);
int nvs_open(const char*s,int mode,nvs_handle_t*h){
    *h=1;
    if(mode!=NVS_READONLY)return ESP_OK;
    if(inject_load_tick)load_boundary_tick();
    return persisted_nvs?open_error:ESP_ERR_NVS_NOT_FOUND;
}
int nvs_get_u32(int h,const char*k,uint32_t*v){
    if(persisted_nvs&&!strcmp(k,"ee_check")){*v=invalid_marker?0:20240921U;return ESP_OK;}
    if(persisted_nvs&&invalid_units&&!strcmp(k,"units")){*v=UINT32_MAX;return ESP_OK;}
    return ESP_ERR_NVS_NOT_FOUND;
}
int nvs_get_u8(int h,const char*k,uint8_t*v){
    if(persisted_nvs&&!strcmp(k,"backlight")){
        if(inject_load_tick)load_boundary_tick();
        if(read_error)return ESP_FAIL;
        *v=stored_backlight;return ESP_OK;
    }
    return ESP_ERR_NVS_NOT_FOUND;
}
int nvs_get_blob(int h,const char*k,void*v,size_t*l){return ESP_ERR_NVS_NOT_FOUND;}
int nvs_set_u32(int h,const char*k,uint32_t v){return ESP_OK;}
int nvs_set_u8(int h,const char*k,uint8_t v){return ESP_OK;}
int nvs_set_blob(int h,const char*k,const void*v,size_t n){return ESP_OK;}
int nvs_commit(int h){return ESP_OK;}
void nvs_close(int h){if(inject_load_tick)load_boundary_tick();}
void *xQueueCreate(unsigned cap,unsigned size){CHECK(queue_count<2);queue_t*q=&queues[queue_count++];q->cap=cap;q->size=size;CHECK(cap*size<=sizeof(q->data));return q;}
int xQueueSend(void*v,const void*e,int t){queue_t*q=v;if(q->wr-q->rd==q->cap)return pdFALSE;memcpy(q->data+(q->wr++%q->cap)*q->size,e,q->size);return pdTRUE;}
int xQueueSendFromISR(void*v,const void*e,int*w){return xQueueSend(v,e,0);}
int xQueueReceive(void*v,void*e,int t){queue_t*q=v;if(!q||q->rd==q->wr)return pdFALSE;memcpy(e,q->data+(q->rd++%q->cap)*q->size,q->size);return pdTRUE;}
int gpio_get_level(int pin){return levels[pin];}
int gpio_set_level(int pin,int level){levels[pin]=level;if(pin==DURA_GPIO_LCD_BACKLIGHT){light=level;on_writes+=light;if(off_setting)CHECK(!light);}return ESP_OK;}
int gpio_config(const gpio_config_t*c){return ESP_OK;}
int rtc_gpio_hold_dis(int pin){return ESP_OK;}
int gpio_set_intr_type(int pin,int t){return ESP_OK;}
int gpio_install_isr_service(int flags){return ESP_OK;}
int gpio_isr_handler_add(int pin,void(*fn)(void*),void*arg){return ESP_OK;}
int esp_sleep_get_wakeup_cause(void){return wake;}
int dura_board_poll_button(void){return button;}
int dura_board_init_power_sense(void){return ESP_OK;}
int dura_board_init_buttons(void){return ESP_OK;}
void apply_meter_outputs(dura_output_intent_t intent){}
int dura_board_sync_outputs(void){return dura_meter_sync_outputs();}
int dura_battery_init(void){return ESP_OK;}
void dura_battery_poll(void){}
bool sleep_attempt_current(uint64_t g){return !s_ble_connected&&!button&&g==s_activity_generation;}
int dura_board_enter_deep_sleep(void){++sleeps;longjmp(done,1);return ESP_OK;}
void publish_ui_snapshot(const dura_meter_snapshot_t*m){}
int dura_board_render_debug_screen(void){return ESP_OK;}
bool should_show_provisioning_qr(const dura_meter_snapshot_t*m){return false;}
int dura_ui_config_get(dura_ui_runtime_config_t*c){c->button_hold_max_rate_x=1;return ESP_OK;}
uint32_t dura_ui_button_repeat_interval_ms(uint32_t t,unsigned r){return t>=1000?100:UINT32_MAX;}
void dura_lcd_clear(void){}
int dura_lcd_flush(void){return ESP_OK;}
int dura_lcd_set_backlight(bool on);
void lcd_cmd(uint8_t b){}
void lcd_delay_us(uint32_t t){}
int dura_lcd_draw_splash(void){CHECK(light==!off_setting);return ESP_OK;}
int draw_boot_powering_up_popup(void){CHECK(light==!off_setting);return ESP_OK;}
void vTaskDelay(unsigned t){if(in_owner){checkpoint();++stage;}else{CHECK(light==!off_setting);now+=(int64_t)t*1000;}}
/* Unvisited non-policy UI leaves are fatal, not successful behavior substitutes. */
#define handle_setup_button(...) abort()
#define reset_current_total(...) abort()
#define choose_precal_fluid(...) abort()
#define store_precal_fluid(...) abort()
#define ui_cancel_calibration_to_start(...) abort()
#define ui_cancel_batch_to_edit(...) abort()
#include "board_backlight.inc"

static void finish(void){longjmp(done,1);}
static void edge(unsigned reed,bool a,bool b){levels[4]=a;levels[5]=b;flow_isr((void*)(uintptr_t)reed);}
static bool is(const char*s){return !strcmp(test,s);}
/* A legal owner policy tick during unlocked NVS, using the real meter and GPIO path. */
static void load_boundary_tick(void){
    CHECK(!host_critical_depth);
    dura_meter_snapshot_t m;CHECK(dura_meter_get_snapshot(&m)==ESP_OK);
    printf("LOAD_BOUNDARY timeout=%u light=%d on_writes=%u\n",m.backlight_timeout_sec,light,on_writes);fflush(stdout);
    update_backlight(); /* RED must expose the attempted ON GPIO write. */
    CHECK(m.backlight_timeout_sec==9);
    CHECK(!m.pump_enabled&&m.operation_mode==DURA_OPERATION_IDLE);
    CHECK(!light&&on_writes==0);++load_ticks;
}
static void persisted_case(void){
    persisted_nvs=true;off_setting=true;
    CHECK(dura_meter_init()==ESP_OK);
    dura_meter_snapshot_t m;CHECK(dura_meter_get_snapshot(&m)==ESP_OK);CHECK(m.backlight_timeout_sec==9);
    wake=is("persisted_wake");CHECK(dura_board_init()==ESP_OK);CHECK(!light&&on_writes==0);
    if(is("persisted_cal")){CHECK(dura_meter_start_calibration()==ESP_OK);update_backlight();CHECK(!light&&on_writes==0);}
    if(!strncmp(test,"load_",5)){
        CHECK(dura_meter_start_manual()==ESP_OK);
        CHECK(dura_meter_get_snapshot(&m)==ESP_OK);CHECK(m.pump_enabled);
        stored_backlight=(is("load_timed")||is("load_invalid_timed"))?30:9;
        if(is("load_invalid_backlight"))stored_backlight=31;
        invalid_units=is("load_invalid_timed");invalid_marker=is("load_invalid_marker");
        read_error=is("load_read_error");
        open_error=is("load_open_error")?ESP_FAIL:(is("load_missing")?ESP_ERR_NVS_NOT_FOUND:ESP_OK);
        const int expected=(invalid_units||read_error||is("load_invalid_backlight"))?ESP_ERR_INVALID_ARG:
                           (is("load_open_error")?ESP_FAIL:ESP_OK);
        const int64_t activity=s_backlight_activity_us;
        inject_load_tick=true;CHECK(dura_meter_load()==expected);inject_load_tick=false;
        CHECK(load_ticks==((is("load_missing")||is("load_open_error"))?1U:(invalid_marker?2U:3U)));
        CHECK(dura_meter_get_snapshot(&m)==ESP_OK);CHECK(!m.pump_enabled&&m.operation_mode==DURA_OPERATION_IDLE);
        CHECK(m.backlight_timeout_sec==(is("load_timed")?30:9));
        CHECK(!light&&on_writes==0); /* No publication-driven ON before owner tick. */
        off_setting=!is("load_timed");update_backlight();
        CHECK(light==is("load_timed"));CHECK(on_writes==(is("load_timed")?1U:0U));
        CHECK(s_backlight_activity_us==activity);
    }
    printf("PASS %s\n",test);
}
static void checkpoint(void){
    if(!strncmp(test,"timeout_",8)||is("cal_timeout")||is("cal_wait_timeout")){
        if(stage==0){CHECK(light);now=origin+(int64_t)timeout_sec*1000000-1;}
        else if(stage==1){CHECK(light);now++;}
        else {CHECK(!light);CHECK(!sleeps);finish();} return;
    }
    if(is("sleep_60")||is("ble_veto")){
        if(stage==0){origin=now;now=origin+59999999;if(is("ble_veto"))dura_board_set_ble_connected(true);}
        else if(stage==1){CHECK(!sleeps);now=origin+60000000;}
        else if(is("ble_veto")){CHECK(!sleeps);CHECK(!light);finish();}
        else CHECK(false);
        return;
    }
    if(stage==0){
        /* Darken at the driver observation seam; expiry has independent cases. */
        light=false; levels[DURA_GPIO_LCD_BACKLIGHT]=0; now=origin+40000000;
        if(is("local_dark_action")||is("wake_release")){s_ui_screen=DURA_UI_MANUAL;button=DURA_BUTTON_BACK;}
        if(is("local_hold")){s_ui_screen=DURA_UI_MANUAL;button=DURA_BUTTON_DOWN;}
        if(is("local_repeat")){s_ui_screen=DURA_UI_RECIRC_EDIT;s_ui_batch_amount=10;button=DURA_BUTTON_SELECT;}
        if(is("remote"))CHECK(dura_board_enqueue_button(DURA_BUTTON_DOWN)==ESP_OK);
        if(is("remote_invalid"))CHECK(dura_board_enqueue_button(99)==ESP_ERR_INVALID_ARG);
        if(is("remote_full")){
            queue_t*q=s_button_queue;q->wr=q->cap;q->rd=0;
            int64_t before=s_last_activity_us;
            CHECK(dura_board_enqueue_button(DURA_BUTTON_DOWN)==ESP_ERR_TIMEOUT);
            CHECK(s_last_activity_us==before);
            q->rd=q->wr; /* No accepted events in this rejection test. */
        }
        if(is("flow")||is("flow_boundary")){edge(1,true,false);edge(2,false,true);if(is("flow_boundary"))CHECK(dura_meter_start_manual()==ESP_OK);}
        if(is("flow_bounce")){edge(1,false,false);edge(1,true,true);edge(1,true,false);edge(1,true,false); /* first A qualifies */ CHECK(dura_meter_drain_flow()>0); /* consume first before the test interval */ s_last_valid_reed=1;light=false;now+=20000000;edge(1,true,false);edge(2,false,false);}
        if(is("recirc")||is("recirc_rejected")){s_ui_screen=is("recirc")?DURA_UI_MAIN_MENU:DURA_UI_MANUAL;levels[8]=1;}
        if(is("ble_connect"))dura_board_set_ble_connected(true);
        if(is("ble_disconnect"))dura_board_set_ble_connected(false);
        if(is("sleep_only_activity"))dura_board_note_activity();
        if(is("off_events")){button=DURA_BUTTON_DOWN;edge(1,true,false);CHECK(dura_board_enqueue_button(DURA_BUTTON_SELECT)==ESP_OK);}
        if(is("setting_off")){light=true;CHECK(dura_meter_set_backlight_timeout(9)==ESP_OK);off_setting=true;}
        if(is("setting_on")){CHECK(dura_meter_set_backlight_timeout(30)==ESP_OK);button=DURA_BUTTON_DOWN;off_setting=false;}
        return;
    }
    if(is("local_dark_action")){dura_meter_snapshot_t m;CHECK(dura_meter_get_snapshot(&m)==ESP_OK);CHECK(m.pump_enabled&&light);finish();}
    if(is("local_hold")||is("local_repeat")){
        if(stage<4)CHECK(light);
        if(stage==1){now+=20000000;}
        else if(stage==2){if(is("local_repeat"))CHECK(s_ui_batch_amount>10.1f);button=0;}
        else if(stage==3){now+=10000000;}
        else {CHECK(!light);finish();} return;
    }
    if(is("wake_release")){
        dura_meter_snapshot_t m;CHECK(dura_meter_get_snapshot(&m)==ESP_OK);
        if(stage==1){CHECK(!m.pump_enabled);button=0;}
        else if(stage==2){CHECK(!m.pump_enabled);button=DURA_BUTTON_BACK;}
        else {CHECK(m.pump_enabled&&light);finish();}return;
    }
    bool expected=is("remote")||is("flow")||is("flow_boundary")||is("setting_on")||(is("recirc")&&CONFIG_DURA_BOARD_ENABLE_RECIRC_INPUT);
    CHECK(light==expected);
    if(is("remote"))CHECK(s_ui_screen==DURA_UI_CAL_START);
    if(is("recirc"))CHECK(s_ui_screen==(CONFIG_DURA_BOARD_ENABLE_RECIRC_INPUT?DURA_UI_RECIRC_EDIT:DURA_UI_MAIN_MENU));
    finish();
}
int main(int argc,char**argv){
    CHECK(argc==2);test=argv[1];
    if(!strncmp(test,"load_",5)||!strncmp(test,"persisted_",10)){persisted_case();return 0;}
    CHECK(dura_meter_init()==ESP_OK);
    dura_meter_snapshot_t initial;CHECK(dura_meter_get_snapshot(&initial)==ESP_OK);CHECK(initial.backlight_timeout_sec==10);
    if(!strncmp(test,"timeout_",8))timeout_sec=(unsigned)atoi(test+8);
    off_setting=is("boot_off")||is("wake_off")||is("cal_off")||is("off_events")||is("setting_on");
    CHECK(dura_meter_set_backlight_timeout(off_setting?9:timeout_sec)==ESP_OK);
    wake=!strncmp(test,"wake_",5);
    origin=now;
    CHECK(dura_board_init()==ESP_OK);
    CHECK(light==!off_setting);
    if(is("cal_off")||is("cal_timeout")||is("cal_wait_timeout")){
        CHECK(dura_meter_start_calibration()==ESP_OK);
        if(is("cal_wait_timeout")){CHECK(dura_meter_record_pulse(250)==ESP_OK);CHECK(dura_meter_continue_calibration()==ESP_OK);}
    }
    if(is("boot_off")||is("wake_off")||is("cal_off"))CHECK(on_writes==0);
    if(is("boot_off")||is("wake_off")||is("boot_on")||is("wake_on")||is("cal_off")){printf("PASS %s\n",test);return 0;}
    in_owner=true;
    if(is("wake_release"))button=DURA_BUTTON_BACK;
    if(!setjmp(done))board_task(NULL);
    if(is("sleep_60"))CHECK(sleeps==1&&now==origin+60000000);
    else CHECK(!sleeps);
    printf("PASS %s\n",test);return 0;
}
