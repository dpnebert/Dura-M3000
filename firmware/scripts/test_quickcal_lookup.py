#!/usr/bin/env python3
"""Candidate47 actual meter/board TDD, typed durable NVS and real LCD pixels.
Oracle: automation/ralph-loop/quickcal-contract.md, not candidate lookup.
Host reboot is not target power-loss verification.
"""
import argparse, json, os, subprocess, re
from pathlib import Path
from test_legacy_batch_conversion import build_host
from test_precal_profile_paths import board_extract
from test_home_volume_ui import generate, config_defines
ROOT=Path(__file__).resolve().parents[1]
BODY=r'''
static const float factors[21]={1.03f,1.02f,1.01f,1.f,.99f,.98f,.97f,.96f,.95f,.94f,.93f,.92f,.91f,.90f,.89f,.88f,.87f,.86f,.85f,.84f,.83f};
static void same(dura_cal_coefficients_t a,dura_cal_coefficients_t b){CHECK(!memcmp(&a,&b,sizeof(a)));}
static void seed(void){
 for(int i=0;i<6;i++) s_meter.precal_profiles[i]=(dura_cal_coefficients_t){.021f+i*.001f,.079f+i*.001f,2.69f+i*.1f};
 s_meter.working_coef_gal=.031f;s_meter.working_coef_liter=.118f;s_meter.working_coef_ounce=3.97f;
 CHECK(dura_meter_save()==ESP_OK);
}
static void reboot(void){CHECK(dura_meter_factory_defaults()==ESP_OK);CHECK(dura_meter_load()==ESP_OK);}
static void preserved(dura_meter_snapshot_t a){dura_meter_snapshot_t b=snap();same(a.working_coefficients,b.working_coefficients);CHECK(!memcmp(a.precal_profiles,b.precal_profiles,sizeof(a.precal_profiles)));CHECK(a.selected_precal_profile==b.selected_precal_profile);off();}
static void mapping(unsigned r,unsigned u){
 seed();CHECK(dura_meter_set_units(u)==ESP_OK);dura_meter_snapshot_t before=snap();
 CHECK(dura_meter_apply_quick_cal(r)==ESP_OK);
 dura_cal_coefficients_t w=before.precal_profiles[0], expected={w.gallon_per_count*factors[r-1],w.liter_per_count*factors[r-1],w.ounce_per_count*factors[r-1]};
 same(snap().working_coefficients,expected);CHECK(!memcmp(before.precal_profiles,snap().precal_profiles,sizeof(before.precal_profiles)));
 CHECK(snap().quick_cal_reference==r);reboot();same(snap().working_coefficients,expected);CHECK(snap().quick_cal_reference==r);
 CHECK(dura_meter_apply_quick_cal(r)==ESP_OK);same(snap().working_coefficients,expected);
 CHECK(dura_meter_apply_quick_cal(4)==ESP_OK);same(snap().working_coefficients,w);off();
}
static void migration(unsigned old){
 seed();dura_meter_snapshot_t before=snap();
 /* Construct legacy record: no v2 key, exact saved working/profile byte values. */
 for(unsigned i=0;i<40;i++) if(!strcmp(durable[i].key,"quick_ref_v2")) memset(&durable[i],0,sizeof(durable[i]));
 nvs_handle_t h;CHECK(nvs_open(DURA_METER_NVS_NAMESPACE,NVS_READWRITE,&h)==ESP_OK);CHECK(nvs_set_u8(h,"quick_ref",old)==ESP_OK);CHECK(nvs_commit(h)==ESP_OK);
 reboot();preserved(before);CHECK(snap().quick_cal_reference==(old==21?0:21-old));
 s_ui_screen=DURA_UI_CAL_START;ui_button(DURA_BUTTON_BACK);CHECK(s_ui_quick_ref==(old==21?4:21-old));ui_button(DURA_BUTTON_UP);preserved(before);
 CHECK(dura_meter_save()==ESP_OK);uint8_t value=255;CHECK(nvs_get_u8(0,"quick_ref_v2",&value)==ESP_OK);CHECK(value==(old==21?0:21-old));
 reboot();preserved(before);CHECK(snap().quick_cal_reference==value);
}
static void failures(unsigned f,unsigned cancel){
 seed();dura_meter_snapshot_t before=snap();s_ui_screen=DURA_UI_CAL_START;ui_button(DURA_BUTTON_BACK);s_ui_quick_ref=6;
 if(f==0)fail_open=1;else if(f==1)fail_write=1;else if(f==2)fail_commit=1;else fail_key="quick_ref_v2";
 ui_button(DURA_BUTTON_BACK);CHECK(s_ui_screen==DURA_UI_QUICK_CAL);CHECK(!strcmp(s_ui_notice_line1,"Save failed"));preserved(before);
 CHECK(dura_meter_start_manual()==ESP_ERR_INVALID_STATE);fail_open=fail_write=fail_commit=0;fail_key=NULL;
 if(cancel){ui_button(DURA_BUTTON_UP);CHECK(dura_meter_save()==ESP_OK);reboot();preserved(before);}
 else {ui_button(DURA_BUTTON_BACK);CHECK(!strcmp(s_ui_notice_line1,"Calibration saved"));CHECK(snap().quick_cal_reference==6);reboot();CHECK(snap().quick_cal_reference==6);CHECK(snap().working_coefficients.gallon_per_count==before.precal_profiles[0].gallon_per_count*.98f);}
}
static void guards(void){seed();dura_meter_snapshot_t before=snap();CHECK(dura_meter_apply_quick_cal(0)==ESP_ERR_INVALID_ARG);CHECK(dura_meter_apply_quick_cal(22)==ESP_ERR_INVALID_ARG);CHECK(dura_meter_apply_quick_cal(255)==ESP_ERR_INVALID_ARG);preserved(before);uint32_t owner;CHECK(dura_meter_claim_recipe(&owner)==ESP_OK);CHECK(dura_meter_apply_quick_cal(4)==ESP_ERR_INVALID_STATE);}
int main(int argc,char **argv){CHECK(argc==2);fresh();unsigned r,u;const char*n=argv[1];
 if(sscanf(n,"map_%u_%u",&r,&u)==2)mapping(r,u);
 else if(sscanf(n,"migration_%u",&r)==1)migration(r);
 else if(sscanf(n,"failure_%u_%u",&r,&u)==2)failures(r,u);
 else if(!strcmp(n,"default"))CHECK(snap().quick_cal_reference==4);
 else if(!strcmp(n,"guards"))guards();else if(!strcmp(n,"selftest"))selftest();else CHECK(false);
 printf("PASS %s\n",n);return 0;}
'''
UI=r'''
#define main safety_fixture_main
#include "safe_output.c"
#undef main
#include "volume_config.h"
#include "board_home_volume.inc"
static void press(dura_button_t b){hardware_button_sample(b);hardware_button_sample(DURA_BUTTON_NONE);}
static bool pixel(int x,int y){return (s_fb[(y/8)*128+x]>>(y%8))&1;}
static void frame(unsigned r){
 rendered_text[0]=0;CHECK(dura_board_render_debug_screen()==ESP_OK);
 CHECK(strstr(rendered_text,"Product names for"));CHECK(strstr(rendered_text,"reference only!"));CHECK(strstr(rendered_text,"Pending"));
 if(r==4){CHECK(strstr(rendered_text,"Water"));CHECK(strstr(rendered_text,"66 F"));}
 if(r==6){CHECK(strstr(rendered_text,"Kerosene"));CHECK(strstr(rendered_text,"69 F"));}
 if(r==10){CHECK(strstr(rendered_text,"Anti-freeze"));CHECK(strstr(rendered_text,"71 F"));}
 if(r==14){CHECK(strstr(rendered_text,"10W Oil"));CHECK(strstr(rendered_text,"71 F"));}
 uint8_t actual[1024];memcpy(actual,s_fb,sizeof(actual));
 dura_lcd_clear();draw_legacy_buttons("Cancel","Dn","Up","Select");
 for(int y=0;y<12;y++)for(int x=0;x<128;x++){bool a=(actual[(y/8)*128+x]>>(y%8))&1;if(a!=pixel(x,y))fprintf(stderr,"button mismatch x=%d raw_y=%d actual=%d expected=%d\\n",x,y,a,pixel(x,y));CHECK(a==pixel(x,y));}
 dura_lcd_clear();dura_lcd_draw_bitmap_1bpp(0,2*r,dura_asset_thick_pointer,7,5,true);
 for(int y=13;y<64;y++)for(int x=0;x<7;x++)CHECK(((actual[(y/8)*128+x]>>(y%8))&1)==pixel(x,y));
 memcpy(s_fb,actual,sizeof(actual));char path[64];snprintf(path,sizeof(path),"quick-%02u.pbm",r);FILE*f=fopen(path,"wb");CHECK(f);fprintf(f,"P1\n128 64\n");
 /* ST7567 maps logical y to 63-y; x is unchanged. */
 for(int y=0;y<64;y++){for(int x=0;x<128;x++)fprintf(f,"%d ",pixel(x,63-y));fputc('\n',f);}fclose(f);
}
int main(int argc,char**argv){CHECK(argc==2);fresh();s_ui_screen=DURA_UI_CAL_START;press(DURA_BUTTON_BACK);unsigned r=atoi(argv[1]);
 CHECK(s_ui_quick_ref==4);
 while(s_ui_quick_ref>r)press(DURA_BUTTON_DOWN);while(s_ui_quick_ref<r)press(DURA_BUTTON_SELECT);
 frame(r);if(r==1){press(DURA_BUTTON_DOWN);CHECK(s_ui_quick_ref==1);}if(r==21){press(DURA_BUTTON_SELECT);CHECK(s_ui_quick_ref==21);}
 dura_meter_snapshot_t before=snap();press(DURA_BUTTON_UP);CHECK(s_ui_screen==DURA_UI_CAL_START);CHECK(!memcmp(&before.working_coefficients,&snap().working_coefficients,0));off();return 0;}
'''.replace('CHECK(!memcmp(&before.working_coefficients,&snap().working_coefficients,0));','dura_meter_snapshot_t after=snap();CHECK(!memcmp(&before.working_coefficients,&after.working_coefficients,sizeof(before.working_coefficients)));')

def main():
 p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
 board,inputs=board_extract(ROOT)
 app=ROOT/'main/app_main.c';text=app.read_text()
 expressions=[re.search(r'DURA_SELF_TEST_CHECK\("'+key+r'",\s*(.*?)\);',text,re.S).group(1) for key in ('CFG-DEFAULT','CFG-QUICK-APPLY')]
 selftest='static void selftest(void){dura_meter_snapshot_t meter=snap();esp_err_t err=ESP_OK;CHECK('+expressions[0]+');err=dura_meter_apply_quick_cal(21);CHECK('+expressions[1]+');}\n'
 exe,hashes=build_host(ROOT,out/'meter',board+selftest+BODY,[*inputs,app,Path(__file__)])
 cases=[f'map_{r}_{u}' for r in range(1,22) for u in range(1,4)]+[f'migration_{r}' for r in range(1,22)]+[f'failure_{f}_{c}' for f in range(4) for c in range(2)]+['default','guards','selftest']
 results={}
 for n in cases:
  run=subprocess.run([str(exe),n],capture_output=True,text=True);(out/'meter'/f'{n}.log').write_text(run.stdout+run.stderr);results[n]=run.returncode
 ui=out/'ui';ui.mkdir(exist_ok=True);generate(ROOT,ui);(ui/'volume_config.h').write_text(config_defines());(ui/'quick.c').write_text(UI)
 cmd=['cc','-std=c11','-g','-pthread','-fsanitize=undefined',*[f'-I{x}' for x in (ui,ROOT/'tests',ROOT/'tests/recipe_ownership/include',ROOT/'components/dura_meter/include',ROOT/'components/dura_board/include')],str(ui/'quick.c'),str(ROOT/'components/dura_meter/dura_meter.c'),str(ROOT/'components/dura_meter/dura_control_policy.c'),str(ROOT/'components/dura_board/dura_lcd_assets.c'),'-lm','-o',str(ui/'quick')]
 run=subprocess.run(cmd,capture_output=True,text=True);(ui/'compile.log').write_text(' '.join(cmd)+'\n'+run.stdout+run.stderr);results['ui_compile']=run.returncode
 if not run.returncode:
  for r in range(1,22):
   run=subprocess.run([str(ui/'quick'),str(r)],cwd=ui,capture_output=True,text=True);(ui/f'{r}.log').write_text(run.stdout+run.stderr);results[f'ui_{r}']=run.returncode
 (out/'results.json').write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(results));print(f'{sum(v==0 for v in results.values())}/{len(results)} passed');return int(any(results.values()))
if __name__=='__main__':raise SystemExit(main())
