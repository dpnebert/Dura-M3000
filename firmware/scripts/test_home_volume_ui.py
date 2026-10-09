#!/usr/bin/env python3
"""Real board handlers/renderers, meter and LCD pixel code; genuine font assets.
Only hardware/NVS/battery/QR services are shimmed. Generated evidence stays in --out.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
from test_safe_output_ownership import function
from test_calibration_save_recovery import enum

ROOT = Path(__file__).resolve().parents[1]
UNITS = ('GALLON', 'LITER', 'OUNCE')


def config_defines(root=ROOT):
    return '\n'.join('#define ' + line.replace('=', ' ', 1)
                     for line in (root/'sdkconfig').read_text().splitlines()
                     if line.startswith('CONFIG_DURA_BOARD_VOLUME_')) + '\n'


def generate(root, out):
    board = (root/'components/dura_board/dura_board.c').read_text()
    lcd = (root/'components/dura_board/dura_st7567.c').read_text()
    prelude = r'''
#include "dura_lcd_assets.h"
#include "dura_battery.h"
#include "esp_log.h"
#define RTC_DATA_ATTR
#define DURA_LCD_WIDTH 128
#define DURA_LCD_HEIGHT 64
#define DURA_AUTO_BATCH_DEFAULT_GAL 10.0f
#define DURA_RECIRC_BATCH_DEFAULT_GAL 250.0f
#define CONFIG_DURA_BOARD_NAME "HOST"
#ifndef CONFIG_DURA_APP_M3000_LEGACY_ONLY
#define CONFIG_DURA_APP_M3000_LEGACY_ONLY 1
#endif
static uint8_t s_fb[1024];
static bool s_provisioning_qr_dismissed, s_provisioning_qr_rendered;
static char s_provisioning_qr_payload[80];
static char labels[4][32], rendered_amount[32];
static bool quantity_pixels;
static unsigned clipped_foreground, quantity_foreground;
static int qminx, qmaxx, qminy, qmaxy;
static dura_battery_measurement_t dura_battery_get_measurement(void) {
    return (dura_battery_measurement_t){.valid=true,.percent=75};
}
static bool battery_percentage_is_visible(const dura_battery_measurement_t *b) {(void)b;return true;}
static bool dura_battery_format_voltage(char *s,size_t n,const dura_battery_measurement_t *b) {
    (void)b;snprintf(s,n,"Voltage: 3.60 V");return true;
}
int dura_lcd_flush(void){return ESP_OK;}
'''.replace('static dura_battery_measurement_t dura_battery_get_measurement',
            'dura_battery_measurement_t dura_battery_get_measurement').replace(
            'static bool dura_battery_format_voltage', 'bool dura_battery_format_voltage')
    prelude += enum((root/'components/dura_board/include/dura_board.h').read_text(), 'dura_button_t')+'\n'
    prelude += board[board.index('typedef enum {\n    DURA_UI_MAIN_MENU'):board.index('static bool s_low_battery_blink_active;')]
    prelude += function(lcd, 'dura_lcd_clear')+'\n'
    # Observe pixel requests before the actual clipping implementation, not a replacement renderer.
    prelude += '#define dura_lcd_set_pixel actual_set_pixel\n'+function(lcd, 'dura_lcd_set_pixel')+'\n#undef dura_lcd_set_pixel\n'
    prelude += r'''
void dura_lcd_set_pixel(int x,int y,bool on) {
    if(quantity_pixels && on) {
        ++quantity_foreground;
        if(x<0||x>=128||y<0||y>=64)++clipped_foreground;
        if(x<qminx)qminx=x;if(x>qmaxx)qmaxx=x;
        if(y<qminy)qminy=y;if(y>qmaxy)qmaxy=y;
    }
    actual_set_pixel(x,y,on);
}
'''
    for name in ('dura_lcd_draw_hline','dura_lcd_draw_vline','dura_lcd_draw_rect',
                 'dura_lcd_fill_rect','dura_lcd_draw_bitmap_1bpp'):
        prelude += function(lcd,name)+'\n'
    # Production numeric drawing chain, intact; wrap only top-level calls for observations.
    numbers = board[board.index('static int16_t num40_desc_index'):board.index('typedef enum {\n    DURA_UI_MAIN_MENU')]
    prelude += '#define draw_num40_right actual_num40_right\n#define draw_num24_text actual_num24_text\n'+numbers
    prelude += '\n#undef draw_num40_right\n#undef draw_num24_text\n'
    prelude += r'''
static void quantity_begin(const char *text) {
    snprintf(rendered_amount,sizeof(rendered_amount),"%s",text);
    quantity_pixels=true;clipped_foreground=quantity_foreground=0;
    qminx=qminy=999;qmaxx=qmaxy=-1;
}
static void draw_num40_right(uint8_t x,uint8_t y,const char *text) {
    quantity_begin(text);actual_num40_right(x,y,text);quantity_pixels=false;
}
static void draw_num24_text(uint8_t x,uint8_t y,const char *text) {
    quantity_begin(text);actual_num24_text(x,y,text);quantity_pixels=false;
}
'''
    # Whole render region includes formatting selectors added in production later.
    render = board[board.index('static uint8_t legacy_font_char_width'):board.index('typedef struct {\n    int x;')]
    prelude += '#define draw_legacy_button actual_legacy_button\n'+render+'\n#undef draw_legacy_button\n'
    # Macro was active for callers above: use a forward-declared observer instead.
    prelude = prelude.replace('#define draw_legacy_button actual_legacy_button\n',
        'static void observed_button(int slot,const char *label);\n')
    # Rename definition only and redirect calls using a macro after its definition.
    original = function(board,'draw_legacy_button')
    replacement = original.replace('draw_legacy_button(', 'actual_legacy_button(',1)
    prelude = prelude.replace(original, replacement+'\n#define draw_legacy_button observed_button\n')
    prelude += r'''
static void observed_button(int slot,const char *label) {
    snprintf(labels[slot],sizeof(labels[slot]),"%s",label?label:"");
    actual_legacy_button(slot,label);
}
'''
    # Real QR failure render path; QR/peer SDK services alone are injected.
    prelude += r'''
typedef void *esp_qrcode_handle_t;
typedef struct {void (*display_func)(esp_qrcode_handle_t);int max_qrcode_version,qrcode_ecc_level;} esp_qrcode_config_t;
#define ESP_QRCODE_CONFIG_DEFAULT() ((esp_qrcode_config_t){0})
#define ESP_QRCODE_ECC_LOW 0
#define ESP_LOG_WARN 2
static int qr_error;
static int format_provisioning_qr_payload(char *p,size_t n){snprintf(p,n,"host");return qr_error==1?ESP_FAIL:ESP_OK;}
static void draw_dynamic_qr_to_lcd(esp_qrcode_handle_t q){(void)q;}
static void esp_log_level_set(const char*s,int l){(void)s;(void)l;}
static int esp_qrcode_generate(const esp_qrcode_config_t*c,const char*p){(void)c;(void)p;return ESP_FAIL;}
static size_t strlcpy(char*d,const char*s,size_t n){snprintf(d,n,"%s",s);return strlen(s);}
'''
    prelude += function(board,'render_provisioning_qr')+'\n'
    prelude += function(board,'should_show_provisioning_qr')+'\n'
    names = ('dura_board_render_debug_screen','ui_go_main','ui_go_home',
             'ui_cancel_calibration_to_start')
    prelude += '\n'.join(function(board,n) for n in names)+'\n'
    cancel = 'ui_cancel_batch_to_home' if 'ui_cancel_batch_to_home' in board else 'ui_cancel_batch_to_edit'
    prelude += function(board,cancel)+'\n'
    for n in ('next_setup_item','reset_current_total','handle_setup_button',
              'choose_precal_fluid','store_precal_fluid','dura_board_handle_button'):
        prelude += function(board,n)+'\n'
    # Reuse real board output chain and mature hardware safety fixture.
    output = (root/'scripts/test_calibration_save_recovery.py').read_text()
    output = output[output.index('#define CONFIG_DURA_BOARD_ENABLE_PUMP_OUTPUT'):output.index("'''",output.index('#define CONFIG_DURA_BOARD_ENABLE_PUMP_OUTPUT'))]
    prelude += output+'\n'+function(board,'apply_meter_outputs')+'\n'+function(board,'dura_board_sync_outputs')
    prelude += '\nvoid test_board_bind(void){CHECK(dura_meter_set_output_handler(apply_meter_outputs)==ESP_OK);}\n'
    (out/'board_home_volume.inc').write_text(prelude)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,default=ROOT.parent/'evidence/home-volume-latest')
    p.add_argument('--only',choices=('home','volume','fit'))
    args = p.parse_args()
    out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    generate(ROOT,out)
    normal=config_defines()
    # Enum coverage cannot silently ignore a future unit.
    units=re.findall(r'DURA_UNITS_(\w+)\s*=',(ROOT/'components/dura_meter/include/dura_meter.h').read_text())
    assert set(units)==set(UNITS),f'extend test matrix for units {units}'
    configs={'normal':normal}
    for unit in UNITS:
        for decimals in (0,1):
            macros={f'CONFIG_DURA_BOARD_VOLUME_{u}_DECIMALS':2 for u in UNITS}
            macros[f'CONFIG_DURA_BOARD_VOLUME_{unit}_DECIMALS']=decimals
            macros['CONFIG_DURA_BOARD_VOLUME_2DP_LIMIT_TENTHS']=1999
            configs[f'{unit.lower()}-{decimals}']='\n'.join(f'#define {k} {v}' for k,v in macros.items())+'\n'
    configs['cutoff-99.9']='\n'.join(f'#define CONFIG_DURA_BOARD_VOLUME_{u}_DECIMALS 2' for u in UNITS)+'\n#define CONFIG_DURA_BOARD_VOLUME_2DP_LIMIT_TENTHS 999\n'
    configs['qr-failures']=normal+'#define CONFIG_DURA_APP_M3000_LEGACY_ONLY 0\n'
    cases=['home_auto_running','home_auto_paused','home_auto_done','home_auto_done_noauto',
           'home_recirc_running','home_recirc_paused','home_recirc_done','home_manual',
           'home_flow_error','home_labels','other_buttons','cancel_rejected']
    cases += [f'home_{mode}_{state}_{failure}' for mode in ('auto','recirc')
              for state in ('running','paused','done') for failure in ('open','write','commit')]
    cases += ['volume','fit']
    results={}
    for config, defines in configs.items():
        if args.only=='home' and config not in ('normal','qr-failures'):continue
        if args.only in ('volume','fit') and config=='qr-failures':continue
        d=out/config;d.mkdir(exist_ok=True)
        (d/'volume_config.h').write_text(defines)
        inputs=[ROOT/'tests/home_volume_ui.c', ROOT/'components/dura_meter/dura_meter.c',
                ROOT/'components/dura_meter/dura_control_policy.c',ROOT/'components/dura_board/dura_lcd_assets.c']
        cmd=['cc','-std=c11','-g','-pthread','-fsanitize=undefined','-Werror=implicit-function-declaration',
             '-I'+str(d),'-I'+str(out),'-I'+str(ROOT/'tests/recipe_ownership/include'),
             '-I'+str(ROOT/'components/dura_meter/include'),'-I'+str(ROOT/'components/dura_board/include'),
             *map(str,inputs),'-lm','-o',str(d/'test')]
        r=subprocess.run(cmd,capture_output=True,text=True)
        (d/'compile.log').write_text(' '.join(cmd)+'\n'+r.stdout+r.stderr)
        if r.returncode:print(r.stderr);return r.returncode
        selected=cases if config=='normal' else (['qr'] if config=='qr-failures' else ['volume','fit'])
        if args.only:selected=[c for c in selected if c.startswith(args.only) or (args.only=='home' and c in ('other_buttons','cancel_rejected','qr'))]
        for case in selected:
            r=subprocess.run([str(d/'test'),case],capture_output=True,text=True,timeout=20)
            (d/(case+'.log')).write_text(r.stdout+r.stderr)
            results[config+'/'+case]=r.returncode
            print(('PASS ' if not r.returncode else 'FAIL ')+config+'/'+case)
            if r.returncode:print(r.stdout+r.stderr)
    (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    paths=[ROOT/'components/dura_board/dura_board.c',ROOT/'sdkconfig',Path(__file__),*inputs,out/'board_home_volume.inc']
    (out/'INPUT_SHA256.json').write_text(json.dumps({str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},indent=2)+'\n')
    print(f'{sum(v==0 for v in results.values())}/{len(results)} passed')
    return int(any(results.values()))

if __name__=='__main__':sys.exit(main())
