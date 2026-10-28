#!/usr/bin/env python3
"""Actual meter + verbatim board calibration paths, deterministic host NVS reload.

Tests independent defects, NOT attribution of the underspecified customer symptom.
NVS seam is not target flash/brownout or physical LCD/pump verification.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from test_legacy_batch_conversion import build_host, sha
from test_safe_output_ownership import function


def board_extract(root):
    path=root/'components/dura_board/dura_board.c'
    header=root/'components/dura_board/include/dura_board.h'
    board=path.read_text()
    def enum(text,name):
        return re.search(r'typedef enum\s*\{[^}]*\}\s*'+name+r';',text).group()
    result=enum(header.read_text(),'dura_button_t')+'\n'+enum(board,'dura_ui_screen_t')+'\n'
    result+=r'''
static dura_ui_screen_t s_ui_screen;
static bool s_ui_cal_measured_amount_valid;
static float s_ui_cal_measured_amount;
static uint8_t s_ui_quick_ref, s_ui_help_page;
static char s_ui_notice_line1[32], s_ui_notice_line2[32];
static int64_t s_ui_notice_until_us;
'''
    result+='\n'.join(function(board,name) for name in (
        'ui_show_notice','ui_go_main','ui_go_home','ui_cancel_calibration_to_start',
        'choose_precal_fluid','store_precal_fluid'))
    handle=function(board,'dura_board_handle_button')
    cases=handle[handle.index('    case DURA_UI_CAL_START:'):handle.index('    case DURA_UI_BATCH_SET:')]
    result+='\nstatic void ui_button(dura_button_t b) { dura_meter_snapshot_t meter=snap(); switch(s_ui_screen) {\n'+cases+'\ndefault: CHECK(false); } }\n'
    render=function(board,'dura_board_render_debug_screen')
    transitions=render[render.index('    if (meter.operation_mode'):render.index('    if (should_show_provisioning_qr')]
    result+='static void ui_refresh(void) { dura_meter_snapshot_t meter=snap();\n'+transitions+'}\n'
    return result,[path,header,root/'scripts/test_safe_output_ownership.py']


TEST_BODY=r'''
static dura_meter_snapshot_t before;
static bool invalidate_at_boundary;
static bool check_unpublished;
static unsigned observations;
static void same(dura_cal_coefficients_t a,dura_cal_coefficients_t b) {
    CHECK(a.gallon_per_count==b.gallon_per_count);
    CHECK(a.liter_per_count==b.liter_per_count);
    CHECK(a.ounce_per_count==b.ounce_per_count);
}
static void unchanged(void) {
    dura_meter_snapshot_t s=snap();
    same(s.working_coefficients,before.working_coefficients);
    CHECK(s.selected_precal_profile==before.selected_precal_profile);
    CHECK(!memcmp(s.precal_profiles,before.precal_profiles,sizeof(s.precal_profiles)));
    off();
}
static void bucket(unsigned slot,unsigned units,float amount) {
    CHECK(dura_meter_set_units((dura_units_t)units)==ESP_OK);
    s_ui_screen=DURA_UI_CAL_START;
    ui_button(DURA_BUTTON_DOWN);
    CHECK(snap().calibration_mode==DURA_CAL_RUNNING);
    CHECK(dura_meter_record_pulse(250)==ESP_OK);
    ui_button(DURA_BUTTON_BACK); ui_refresh();
    CHECK(s_ui_screen==DURA_UI_CAL_ADJUST);
    /* Real measured editor entry seam: chosen value, not a copied calculation. */
    s_ui_cal_measured_amount=amount;
    ui_button(DURA_BUTTON_BACK);
    CHECK(snap().calibration_mode==DURA_CAL_WAIT_SAVE);
    CHECK(s_ui_screen==DURA_UI_CAL_SAVE_1);
    if(slot>=3) ui_button(DURA_BUTTON_BACK);
    ui_button((dura_button_t)(1+slot%3));
    CHECK(s_ui_screen==DURA_UI_MAIN_MENU);
    CHECK(!strcmp(s_ui_notice_line1,"Calibration saved"));
    CHECK(snap().selected_precal_profile==slot); off();
}
static void recall_ui(unsigned slot) {
    s_ui_screen=DURA_UI_CAL_START;
    ui_button(DURA_BUTTON_SELECT); CHECK(s_ui_screen==DURA_UI_PRECAL_1);
    if(slot>=3) { ui_button(DURA_BUTTON_BACK); CHECK(s_ui_screen==DURA_UI_PRECAL_2); }
    ui_button((dura_button_t)(1+slot%3));
}
static void roundtrip(unsigned slot,unsigned unit) {
    dura_meter_snapshot_t initial=snap();
    float amount=unit==1 ? 5.0f : unit==2 ? 19.0f : 640.0f;
    bucket(slot,unit,amount);
    dura_meter_snapshot_t saved=snap();
    /* Independent exact expected coefficients for these exactly representable
     * collection operands, tolerance only for derived unit multiplications. */
    CHECK(fabsf(saved.working_coefficients.gallon_per_count-0.02f)<1e-8f);
    CHECK(fabsf(saved.working_coefficients.liter_per_count-0.076f)<1e-8f);
    CHECK(fabsf(saved.working_coefficients.ounce_per_count-2.56f)<1e-6f);
    for(unsigned i=0;i<6;i++) if(i!=slot) same(saved.precal_profiles[i],initial.precal_profiles[i]);
    CHECK(dura_meter_load()==ESP_OK);
    same(snap().working_coefficients,saved.working_coefficients);
    CHECK(snap().selected_precal_profile==slot);
    CHECK(dura_meter_apply_quick_cal(1)==ESP_OK);
    CHECK(snap().working_coefficients.gallon_per_count!=saved.working_coefficients.gallon_per_count);
    recall_ui(slot); CHECK(s_ui_screen==DURA_UI_MAIN_MENU);
    CHECK(!strcmp(s_ui_notice_line1,"Calibration recalled"));
    same(snap().working_coefficients,saved.working_coefficients);
    CHECK(dura_meter_load()==ESP_OK);
    same(snap().working_coefficients,saved.working_coefficients);
    CHECK(!memcmp(snap().precal_profiles,saved.precal_profiles,sizeof(saved.precal_profiles))); off();
}
static void empty_slot(unsigned slot) {
    before=snap();
    unsigned writes_before=write_calls;
    CHECK(dura_meter_select_precal_profile(slot)==ESP_ERR_NOT_FOUND);
    CHECK(write_calls==writes_before); unchanged();
    recall_ui(slot);
    CHECK(s_ui_screen==DURA_UI_MAIN_MENU); /* Retained reference also goes Home. */
    CHECK(!strcmp(s_ui_notice_line1,"No saved calibration")); unchanged();
    CHECK(dura_meter_load()==ESP_OK); unchanged();
}
static void failure_setup(bool store) {
    bucket(4,1,5.0f);
    CHECK(dura_meter_select_precal_profile(0)==ESP_OK);
    if(store) CHECK(dura_meter_apply_quick_cal(1)==ESP_OK);
    before=snap();
}
static void failed_save(const char *name,bool store) {
    failure_setup(store);
    if(strstr(name,"open")) fail_open=1;
    else if(strstr(name,"late")) fail_key="coef_ounce";
    else if(strstr(name,"write")) fail_write=1;
    else fail_commit=1;
    if(store) CHECK(dura_meter_store_working_precal_profile(4)==ESP_FAIL);
    else {
        recall_ui(4); CHECK(s_ui_screen==DURA_UI_PRECAL_2);
        CHECK(!strcmp(s_ui_notice_line1,"Save failed"));
    }
    unchanged();
    fail_open=fail_write=fail_commit=0; fail_key=NULL;
    if(strstr(name,"cancel")) {
        if(!store) { ui_button(DURA_BUTTON_BACK); CHECK(s_ui_screen==DURA_UI_CAL_START); }
        CHECK(dura_meter_set_screen_home()==ESP_OK); /* Later unrelated save must not publish failed operation. */
        CHECK(dura_meter_load()==ESP_OK); unchanged();
    } else {
        if(store) CHECK(dura_meter_store_working_precal_profile(4)==ESP_OK);
        else { ui_button(DURA_BUTTON_DOWN); CHECK(!strcmp(s_ui_notice_line1,"Calibration recalled")); }
        dura_meter_snapshot_t s=snap();
        CHECK(s.selected_precal_profile==4);
        same(s.working_coefficients,store ? before.working_coefficients : before.precal_profiles[4]);
        same(s.precal_profiles[4],s.working_coefficients);
        CHECK(dura_meter_load()==ESP_OK); same(snap().working_coefficients,s.working_coefficients); off();
    }
}
static void observer(void) {
    ++observations;
    if(invalidate_at_boundary) {
        save_observer=NULL;
        CHECK(dura_meter_factory_defaults()==ESP_OK);
        return;
    }
    if(check_unpublished) unchanged();
    CHECK(dura_meter_start_manual()==ESP_ERR_INVALID_STATE);
    CHECK(dura_meter_start_batch(5)==ESP_ERR_INVALID_STATE);
    CHECK(dura_meter_start_calibration()==ESP_ERR_INVALID_STATE);
    CHECK(dura_meter_select_precal_profile(0)==ESP_ERR_INVALID_STATE);
    CHECK(dura_meter_store_working_precal_profile(0)==ESP_ERR_INVALID_STATE);
    CHECK(dura_meter_load()==ESP_ERR_INVALID_STATE);
}
static void pending_save(bool store,bool invalidate,bool unpublished) {
    failure_setup(store); observations=0; invalidate_at_boundary=invalidate;
    check_unpublished=unpublished; save_observer=observer;
    int err=store ? dura_meter_store_working_precal_profile(4) : dura_meter_select_precal_profile(4);
    save_observer=NULL; CHECK(observations>0);
    if(invalidate) {
        CHECK(err==ESP_ERR_INVALID_STATE);
        CHECK(snap().selected_precal_profile==0);
        CHECK(snap().precal_profiles[4].gallon_per_count==0);
        same(snap().working_coefficients,snap().precal_profiles[0]);
    } else { CHECK(err==ESP_OK); CHECK(snap().selected_precal_profile==4); }
    off(); CHECK(dura_meter_start_manual()==ESP_OK); CHECK(dura_meter_stop_manual()==ESP_OK);
}
static void store_reload(unsigned slot) {
    CHECK(dura_meter_apply_quick_cal(1)==ESP_OK); dura_meter_snapshot_t s=snap();
    CHECK(dura_meter_store_working_precal_profile(slot)==ESP_OK);
    CHECK(dura_meter_load()==ESP_OK); CHECK(snap().selected_precal_profile==slot);
    same(snap().precal_profiles[slot],s.working_coefficients);
    same(snap().working_coefficients,s.working_coefficients); off();
}
static void invalid_slot(void) {
    before=snap();
    CHECK(dura_meter_select_precal_profile(6)==ESP_ERR_INVALID_ARG);
    CHECK(dura_meter_store_working_precal_profile(255)==ESP_ERR_INVALID_ARG); unchanged();
    s_meter.precal_profiles[1]=(dura_cal_coefficients_t){0.01f,0.0f,1.0f};
    CHECK(dura_meter_select_precal_profile(1)==ESP_ERR_NOT_FOUND);
    s_meter.precal_profiles[1]=(dura_cal_coefficients_t){NAN,0.03f,1.0f};
    CHECK(dura_meter_select_precal_profile(1)==ESP_ERR_NOT_FOUND);
    CHECK(dura_meter_save()==ESP_OK); CHECK(dura_meter_load()==ESP_ERR_INVALID_ARG); off();
}
static void navigation(void) {
    before=snap(); s_ui_screen=DURA_UI_CAL_START;
    ui_button(DURA_BUTTON_SELECT); CHECK(s_ui_screen==DURA_UI_PRECAL_1);
    ui_button(DURA_BUTTON_BACK); CHECK(s_ui_screen==DURA_UI_PRECAL_2);
    ui_button(DURA_BUTTON_BACK); CHECK(s_ui_screen==DURA_UI_CAL_START); unchanged();
}
int main(int argc,char **argv) {
    CHECK(argc==2); fresh(); const char *name=argv[1]; unsigned slot,unit;
    if(sscanf(name,"roundtrip_%u_%u",&slot,&unit)==2) roundtrip(slot,unit);
    else if(sscanf(name,"empty_%u",&slot)==1) empty_slot(slot);
    else if(sscanf(name,"store_reload_%u",&slot)==1) store_reload(slot);
    else if(strstr(name,"_fail_")) failed_save(name,!strncmp(name,"store",5));
    else if(strstr(name,"_pending_")) pending_save(!strncmp(name,"store",5),strstr(name,"defaults")!=NULL,strstr(name,"observe")!=NULL);
    else if(!strcmp(name,"invalid_slot")) invalid_slot();
    else if(!strcmp(name,"navigation")) navigation();
    else CHECK(false);
    printf("PASS %s\n",name); return 0;
}
'''
TESTS=[f'roundtrip_{slot}_{unit}' for slot in range(6) for unit in (1,2,3)]
TESTS += [f'empty_{slot}' for slot in range(1,6)]
TESTS += [f'store_reload_{slot}' for slot in range(6)]
TESTS += [f'{op}_fail_{failure}_{action}' for op in ('recall','store')
          for failure in ('open','write','late','commit') for action in ('retry','cancel')]
TESTS += [f'{op}_pending_{action}' for op in ('recall','store') for action in ('observe','exclusion','defaults')]
TESTS += ['invalid_slot','navigation']


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-root',type=Path,default=Path(__file__).resolve().parents[1])
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--test',action='append',choices=TESTS)
    args=p.parse_args()
    board,inputs=board_extract(args.source_root)
    exe,hashes=build_host(args.source_root,args.out,board+TEST_BODY,[*inputs,Path(__file__)])
    results={}
    for name in args.test or TESTS:
        run=subprocess.run([str(exe),name],capture_output=True,text=True,timeout=10,
                           env=dict(os.environ,UBSAN_OPTIONS='halt_on_error=1'))
        (args.out/(name+'.log')).write_text(run.stdout+run.stderr)
        results[name]=run.returncode
        print(('PASS ' if not run.returncode else 'FAIL ')+name)
    stable=all(sha(Path(path))==digest for path,digest in hashes.items())
    summary=dict(passed=sum(v==0 for v in results.values()),total=len(results),input_hashes_stable=stable)
    (args.out/'results.json').write_text(json.dumps(dict(summary=summary,cases=results),indent=2)+'\n')
    print(json.dumps(summary,sort_keys=True))
    return int(any(results.values()) or not stable)


if __name__=='__main__':
    sys.exit(main())
