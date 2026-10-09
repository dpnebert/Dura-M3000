#!/usr/bin/env python3
"""Full real board owner/render/button paths + meter, with injected hardware/drawing.
Promoted from the independent F04 owner_render_review.py reproduction.
No physical LCD, controller-final-race, or rejected-deep-sleep rollback proof.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
from test_safe_output_ownership import function


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=root/'verification/F04/owner-ui-latest')
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    board = root/'components/dura_board/dura_board.c'
    fixture = root/'tests/sleep_prepare_recovery.c'
    text = board.read_text()
    names = ['dura_board_set_ble_connected', 'dura_board_note_activity',
             'dura_board_note_user_activity', 'update_backlight',
             'config_rtc_wake_input', 'prepare_deep_sleep_inputs',
             'restore_deep_sleep_inputs', 'sleep_attempt_current', 'dura_board_enter_deep_sleep']
    base = fixture.read_text().split('static void decision(void)')[0]
    base = base.replace('/* BOARD_FUNCTIONS */', '\n'.join(function(text, n) for n in names))
    base = base.replace('void *xTaskGetCurrentTaskHandle(void){return s_task;}',
                        'static void *current_task=(void*)2;\nvoid *xTaskGetCurrentTaskHandle(void){return current_task;}')
    base = base.replace('static void outputs(dura_output_intent_t i)',
                        'static unsigned on_intents;\nstatic void outputs(dura_output_intent_t i)')
    base = base.replace('CHECK(!i.pump&&!i.recirc_ev&&!i.inject_ev);',
                        'if(s_sleep_recovery_pending)CHECK(!i.pump&&!i.recirc_ev&&!i.inject_ev);'
                        'if(i.pump||i.recirc_ev||i.inject_ev)++on_intents;')
    base = base.replace('inject("save");return fail_open?',
                        'inject("save");if(s_sleep_recovery_pending){'
                        'CHECK(!output[0]&&!output[1]&&!output[2]);'
                        'CHECK(dura_meter_resume_batch()==ESP_ERR_INVALID_STATE);}'
                        'return fail_open?')
    base = base.replace('int dura_board_render_debug_screen(void){return ESP_OK;}',
                        'int dura_board_render_debug_screen(void);')
    base += function(fixture.read_text(), 'restored')
    ui = text[text.index('typedef enum {\n    DURA_UI_MAIN_MENU'):text.index('static bool s_low_battery_blink_active;')]
    render = function(text, 'dura_board_render_debug_screen')
    leaves = sorted(set(re.findall(r'\b(render_\w+|draw_legacy_home)\(', render)))
    preamble = r'''
#include "esp_log.h"
#define RTC_DATA_ATTR
#define DURA_AUTO_BATCH_DEFAULT_GAL 10
#define DURA_RECIRC_BATCH_DEFAULT_GAL 10
#define DURA_BUTTON_UP 1
#define DURA_BUTTON_DOWN 2
#define DURA_BUTTON_SELECT 3
#define DURA_BUTTON_BACK 4
#define CONFIG_DURA_BOARD_SETTINGS_HOLD_MS 1000
#define CONFIG_DURA_BOARD_ENABLE_DEEP_SLEEP 1
#define DURA_BOARD_UI_PERIOD_MS 50
#define DURA_BOARD_SAVE_PERIOD_MS 30000
#define pdMS_TO_TICKS(x) (x)
typedef int dura_button_t;
typedef struct { unsigned button_hold_max_rate_x; } dura_ui_runtime_config_t;
static bool s_provisioning_qr_rendered, s_provisioning_qr_dismissed;
static char s_provisioning_qr_payload[80];
static jmp_buf owner_done;
void dura_battery_poll(void){}
static int queued_test_button;
int xQueueReceive(void*q,void*v,int t){if(!queued_test_button)return 0;*(int*)v=queued_test_button;queued_test_button=0;return pdTRUE;}
int dura_ui_config_get(dura_ui_runtime_config_t*c){return ESP_FAIL;}
uint32_t dura_ui_button_repeat_interval_ms(uint32_t t,unsigned r){return UINT32_MAX;}
void vTaskDelay(unsigned t){longjmp(owner_done,1);}
bool should_show_provisioning_qr(const dura_meter_snapshot_t*m){return false;}
void dura_lcd_clear(void){}
int dura_lcd_flush(void){return ESP_OK;}
/* Unvisited unrelated menu helpers fail loudly if accidentally dispatched. */
#define adjust_batch_amount_tenths(...) (CHECK_UNVISITED(), 0.0f)
#define handle_setup_button(...) CHECK_UNVISITED()
#define reset_current_total(...) CHECK_UNVISITED()
#define choose_precal_fluid(...) CHECK_UNVISITED()
#define store_precal_fluid(...) CHECK_UNVISITED()
#define ui_cancel_calibration_to_start(...) CHECK_UNVISITED()

#define CHECK_UNVISITED() abort()
'''
    preamble += '\n'.join('#define '+name+'(...) ((void)0)' for name in leaves)+'\n'
    helpers = '\n'.join(function(text, n) for n in ['ui_go_main', 'ui_go_home', 'ui_cancel_batch_to_home',
                          'dura_board_handle_button', 'ui_button_is_repeatable', 'dispatch_ui_button'])
    cmain = r'''
static void owner_iteration(void){if(!setjmp(owner_done))board_task(NULL);}
static void owner_button(int b){CHECK(!queued_test_button);queued_test_button=b;owner_iteration();CHECK(!queued_test_button);}
int main(int argc,char**argv){
 CHECK(argc==2);test=argv[1];
 CHECK(dura_meter_init()==ESP_OK);
 /* This fixture bypasses board_init; reproduce its initial activity seed. */
 dura_board_note_user_activity();
 CHECK(dura_meter_set_output_handler(outputs)==ESP_OK);
 bool recirc=strstr(test,"recirc")!=NULL;
 if(recirc){
   CHECK(dura_meter_start_batch_mode(10,DURA_OPERATION_RECIRC)==ESP_OK);
   if(strstr(test,"paused"))CHECK(dura_meter_pause_batch()==ESP_OK);
   s_ui_screen=DURA_UI_RECIRC_RUN;
 }else{s_ui_screen=DURA_UI_SETUP;s_ui_setup_item=7;s_ui_cal_measured_amount=23.5f;s_ui_cal_measured_amount_valid=true;}
 CHECK(dura_board_render_debug_screen()==ESP_OK);
 const int before=s_ui_screen;
 dura_meter_snapshot_t initial;CHECK(dura_meter_get_snapshot(&initial)==ESP_OK);
 if(strstr(test,"save"))fail_open=1;
 if(strstr(test,"rtc"))fail_step=27;
 CHECK(dura_board_enter_deep_sleep()==ESP_OK);
 CHECK(s_sleep_requested&&!s_sleep_recovery_pending&&!sleep_calls&&!step);
 current_task=s_task;
 if(!setjmp(slept))owner_iteration();
 if(strstr(test,"success")){CHECK(sleep_calls==1);CHECK(!s_sleep_requested);CHECK(!output[0]&&!output[1]&&!output[2]);puts("PASS permitted queued request executed by full board_task");return 0;}
 CHECK(!sleep_calls&&!s_sleep_requested&&!s_sleep_recovery_pending);
 restored();
 dura_meter_snapshot_t m;CHECK(dura_meter_get_snapshot(&m)==ESP_OK);
 CHECK(!m.pump_enabled&&!m.recipe_owner_id);
 printf("before_screen=%d after_screen=%d batch_mode=%d operation=%d backlight=%d\n",before,s_ui_screen,m.batch_mode,m.operation_mode,light);
 if(recirc)CHECK(m.batch_mode==DURA_BATCH_PAUSED);
 CHECK(s_ui_screen==before);
 if(recirc){
   CHECK(m.operation_mode==DURA_OPERATION_RECIRC);
   CHECK(m.preset_batch_counts==initial.preset_batch_counts);
   /* Repeated owner/render iterations cannot drift or restart the operation. */
   const unsigned before_idle_intents=on_intents;
   for(int i=0;i<3;i++){now+=100000;owner_iteration();CHECK(s_ui_screen==before);restored();CHECK(on_intents==before_idle_intents);}
   fail_open=0;fail_step=0;
   if(strstr(test,"resume")){
     /* Only a new explicit operator action may energize a recovered operation. */
     owner_button(DURA_BUTTON_BACK);
     CHECK(dura_meter_get_snapshot(&m)==ESP_OK);
     CHECK(m.batch_mode==DURA_BATCH_RUNNING&&m.operation_mode==DURA_OPERATION_RECIRC);
     CHECK(output[0]&&output[1]&&!output[2]);
     owner_button(DURA_BUTTON_SELECT);
     CHECK(dura_meter_get_snapshot(&m)==ESP_OK);CHECK(m.batch_mode==DURA_BATCH_PAUSED);
     restored();
   }
   owner_button(DURA_BUTTON_UP);
   CHECK(s_ui_screen==DURA_UI_MAIN_MENU);
   CHECK(dura_meter_get_snapshot(&m)==ESP_OK);
   CHECK(m.operation_mode==DURA_OPERATION_IDLE&&m.batch_mode!=DURA_BATCH_PAUSED);
   CHECK(dura_board_render_debug_screen()==ESP_OK);CHECK(s_ui_screen==DURA_UI_MAIN_MENU);restored();
 }else CHECK(s_ui_setup_item==7&&s_ui_cal_measured_amount==23.5f&&s_ui_cal_measured_amount_valid);
 CHECK(dura_meter_start_manual()==ESP_OK);
 puts("PASS owner recovery, safe UI/control context and navigation retained");return 0;
}
'''
    c = out/'owner_ui_recovery.c'
    c.write_text(base+preamble+ui+render+'\n'+helpers+'\n'+function(text,'board_task')+'\n'+cmain)
    inputs = [c,root/'components/dura_meter/dura_meter.c',root/'components/dura_meter/dura_control_policy.c']
    cmd = ['cc','-std=c11','-g','-pthread','-fsanitize=undefined',
           '-I'+str(root/'tests/recipe_ownership/include'),'-I'+str(root/'components/dura_meter/include'),
           *map(str,inputs),'-lm','-o',str(out/'owner-ui-test')]
    r = subprocess.run(cmd,capture_output=True,text=True)
    (out/'compile.log').write_text(r.stdout+r.stderr)
    if r.returncode:
        print(r.stderr)
        return r.returncode
    cases = ['owner_success','owner_recirc_success','owner_setup_save','owner_setup_rtc',
             'owner_recirc_save','owner_recirc_rtc','owner_recirc_save_resume','owner_recirc_rtc_resume',
             'owner_recirc_paused_save','owner_recirc_paused_rtc']
    results = {}
    for case in cases:
        r = subprocess.run([str(out/'owner-ui-test'),case],capture_output=True,text=True,timeout=10)
        results[case] = r.returncode
        (out/(case+'.log')).write_text(r.stdout+r.stderr)
        print(('PASS ' if r.returncode==0 else 'FAIL ')+case+' '+r.stdout+r.stderr)
    (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    (out/'INPUT_SHA256.json').write_text(json.dumps({str(p):hashlib.sha256(p.read_bytes()).hexdigest()
        for p in inputs+[board,fixture,Path(__file__)]},indent=2)+'\n')
    return int(any(results.values()))

if __name__ == '__main__':
    sys.exit(main())
