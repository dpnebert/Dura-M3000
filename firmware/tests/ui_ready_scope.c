/* Actual board UI + meter, genuine LCD/font pixels. Hardware/NVS injected only. */
#define main safety_fixture_main
#include "safe_output.c"
#undef main
#include "volume_config.h"
static int64_t ui_now=1000000;
static int64_t ui_clock(void){return ui_now;}
#define esp_timer_get_time ui_clock
#include "board_home_volume.inc"
#undef esp_timer_get_time
static void press(dura_button_t b){dura_board_handle_button(b);}
static void refresh(void){rendered_text[0]=0;memset(labels,0,sizeof(labels));CHECK(dura_board_render_debug_screen()==ESP_OK);}
static void near(float a,float b){if(fabsf(a-b)>0.00002f*fmaxf(1,fabsf(b)))fprintf(stderr,"actual %.9g expected %.9g\n",a,b);CHECK(fabsf(a-b)<=0.00002f*fmaxf(1,fabsf(b)));}
static float factor(int u){return u==2?3.8f:u==3?128.0f:1.0f;}
static float step(int u){const int p[]={0,CONFIG_DURA_BOARD_VOLUME_GALLON_DECIMALS,CONFIG_DURA_BOARD_VOLUME_LITER_DECIMALS,CONFIG_DURA_BOARD_VOLUME_OUNCE_DECIMALS};return p[u]==2?.01f:p[u]==1?.1f:1.f;}
static void precision(void){
 for(int u=1;u<=3;u++)for(int rec=0;rec<2;rec++){
  ui_go_main();CHECK(dura_meter_set_units(u)==ESP_OK);press(rec?DURA_BUTTON_SELECT:DURA_BUTTON_BACK);
  s_ui_batch_amount=10;press(DURA_BUTTON_SELECT);near(s_ui_batch_amount,10+step(u));
  press(DURA_BUTTON_DOWN);near(s_ui_batch_amount,10);
  s_ui_batch_amount=0;press(DURA_BUTTON_DOWN);near(s_ui_batch_amount,0);
  press(DURA_BUTTON_SELECT);near(s_ui_batch_amount,step(u));refresh();CHECK(quantity_foreground>0 && clipped_foreground==0);
 }
}
static void hold_precision(void){
 for(int u=1;u<=3;u++)for(int editor=0;editor<3;editor++)for(int sign=-1;sign<=1;sign+=2){
  hardware_button_sample(DURA_BUTTON_NONE);ui_go_main();CHECK(dura_meter_set_units(u)==ESP_OK);
  press(editor==1?DURA_BUTTON_SELECT:DURA_BUTTON_BACK);
  if(editor==2)s_ui_screen=DURA_UI_BATCH_SET;
  s_ui_batch_amount=10;float tap=step(u),held=tap<.1f?.1f:tap;
  dura_button_t b=sign>0?DURA_BUTTON_SELECT:DURA_BUTTON_DOWN;
  hardware_button_sample(b);near(s_ui_batch_amount,10+sign*tap);
  ui_now+=1000000;hardware_button_sample(b);near(s_ui_batch_amount,10+sign*(tap+held));
  ui_now+=1000000;hardware_button_sample(b);near(s_ui_batch_amount,10+sign*(tap+2*held));
  hardware_button_sample(DURA_BUTTON_NONE);near(s_ui_batch_amount,10+sign*(tap+2*held));
  hardware_button_sample(b);near(s_ui_batch_amount,10+sign*(2*tap+2*held));
  hardware_button_sample(DURA_BUTTON_NONE);
  /* Queued/ordinary dispatch after repeat must remain a fine tap. */
  dispatch_ui_button(b);near(s_ui_batch_amount,10+sign*(3*tap+2*held));off();
 }
}
static void hold_calibration(void){
 for(int u=1;u<=3;u++)for(int quickcal=0;quickcal<2;quickcal++){
  CHECK(dura_meter_set_units(u)==ESP_OK);hardware_button_sample(DURA_BUTTON_NONE);
  s_ui_screen=quickcal?DURA_UI_QUICK_CAL:DURA_UI_CAL_ADJUST;
  s_ui_cal_measured_amount=10;s_ui_cal_measured_amount_valid=true;s_ui_quick_ref=17;
  hardware_button_sample(DURA_BUTTON_SELECT);
  if(quickcal)CHECK(s_ui_quick_ref==18);else near(s_ui_cal_measured_amount,10+(u==3?.1f:.01f));
  ui_now+=1000000;hardware_button_sample(DURA_BUTTON_SELECT);
  if(quickcal)CHECK(s_ui_quick_ref==19);else near(s_ui_cal_measured_amount,10+2*(u==3?.1f:.01f));
  hardware_button_sample(DURA_BUTTON_NONE);hardware_button_sample(DURA_BUTTON_DOWN);
  ui_now+=1000000;hardware_button_sample(DURA_BUTTON_DOWN);
  if(quickcal)CHECK(s_ui_quick_ref==17);else near(s_ui_cal_measured_amount,10);
  hardware_button_sample(DURA_BUTTON_NONE);off();
 }
}
static void live_units(void){
 for(int rec=0;rec<2;rec++)for(int u=1;u<=3;u++)for(int v=1;v<=3;v++)for(int render=0;render<2;render++){
  ui_go_main();CHECK(dura_meter_set_units(u)==ESP_OK);press(rec?DURA_BUTTON_SELECT:DURA_BUTTON_BACK);
  s_ui_batch_amount=2*factor(u);press(DURA_BUTTON_SELECT);float gallons=s_ui_batch_amount/factor(u);
  CHECK(dura_meter_set_units(v)==ESP_OK);if(render)refresh();press(DURA_BUTTON_SELECT);
  near(s_ui_batch_amount,roundf(gallons*factor(v)/step(v))*step(v)+step(v));
  float saved=s_ui_batch_amount/factor(v);press(DURA_BUTTON_UP);CHECK(dura_meter_set_units(1)==ESP_OK);
  press(rec?DURA_BUTTON_SELECT:DURA_BUTTON_BACK);near(s_ui_batch_amount,saved);off();
 }
}
static void retention(void){
 for(int rec=0;rec<2;rec++){
  ui_go_main();CHECK(dura_meter_set_units(1)==ESP_OK);press(rec?DURA_BUTTON_SELECT:DURA_BUTTON_BACK);
  s_ui_batch_amount=rec?24.5f:3.5f;press(DURA_BUTTON_SELECT);float entered=s_ui_batch_amount;
  press(DURA_BUTTON_UP);CHECK(s_ui_screen==DURA_UI_MAIN_MENU);
  for(int u=1;u<=3;u++){
   CHECK(dura_meter_set_units(u)==ESP_OK);press(rec?DURA_BUTTON_SELECT:DURA_BUTTON_BACK);near(s_ui_batch_amount,entered*factor(u));
   press(DURA_BUTTON_UP);
  }
 }
 CHECK(dura_meter_set_units(1)==ESP_OK);press(DURA_BUTTON_BACK);near(s_ui_batch_amount,3.51f);
 press(DURA_BUTTON_UP);press(DURA_BUTTON_SELECT);near(s_ui_batch_amount,24.51f);
}
static void navigation(void){
 for(int rec=0;rec<2;rec++)for(int state=0;state<3;state++)for(int fail=0;fail<4;fail++){
  fail_open=fail_write=fail_commit=0;
  ui_go_main();press(rec?DURA_BUTTON_SELECT:DURA_BUTTON_BACK);s_ui_batch_amount=2.5f;press(DURA_BUTTON_SELECT);float entered=s_ui_batch_amount;
  press(DURA_BUTTON_BACK);CHECK(snap().batch_mode==DURA_BATCH_RUNNING);
  if(state==1)press(DURA_BUTTON_SELECT);
  if(state==2)CHECK(dura_meter_record_pulse(snap().preset_batch_counts)==ESP_OK);
  refresh();CHECK(!strcmp(labels[0],"Back"));
  if(state==2){
   CHECK(s_ui_screen==DURA_UI_BATCH_COMPLETE);off();
   for(int key=DURA_BUTTON_DOWN;key<=DURA_BUTTON_BACK;key++){
    press(key);refresh();CHECK(s_ui_screen==DURA_UI_BATCH_COMPLETE);
    CHECK(snap().batch_mode==DURA_BATCH_DONE);off();
   }
  }
  safe_required=true;fail_open=fail==1;fail_write=fail==2;fail_commit=fail==3;
  press(DURA_BUTTON_UP);off();safe_required=false;
  CHECK(s_ui_screen==(rec?DURA_UI_RECIRC_EDIT:DURA_UI_AUTO_BATCH_EDIT));near(s_ui_batch_amount,entered);
  for(int i=0;i<3;i++){refresh();off();CHECK(s_ui_screen==(rec?DURA_UI_RECIRC_EDIT:DURA_UI_AUTO_BATCH_EDIT));}
  CHECK(!strcmp(labels[0],"Home"));press(DURA_BUTTON_UP);CHECK(s_ui_screen==DURA_UI_MAIN_MENU);
 }
}
static void external(void){
 press(DURA_BUTTON_SELECT);s_ui_batch_amount=1.5f;press(DURA_BUTTON_SELECT);press(DURA_BUTTON_UP);
 CHECK(dura_meter_set_units(2)==ESP_OK);handle_external_recirc_press();
 CHECK(s_ui_screen==DURA_UI_RECIRC_RUN && snap().operation_mode==DURA_OPERATION_RECIRC);near(snap().remaining_batch,1.51f*3.8f);
 uint32_t counts=snap().preset_batch_counts;handle_external_recirc_press();CHECK(snap().preset_batch_counts==counts);
 press(DURA_BUTTON_UP);press(DURA_BUTTON_UP);
 for(int i=1;i<DURA_UI_SCREEN_COUNT;i++){s_ui_screen=i;handle_external_recirc_press();off();CHECK(s_ui_screen==i);}
 ui_go_main();CHECK(dura_meter_start_manual()==ESP_OK);handle_external_recirc_press();CHECK(snap().operation_mode==DURA_OPERATION_MANUAL);CHECK(gpio[0]&&gpio[2]);
 CHECK(dura_meter_stop_manual()==ESP_OK);ui_go_main();uint32_t owner;CHECK(dura_meter_claim_recipe(&owner)==ESP_OK);handle_external_recirc_press();CHECK(snap().recipe_owner_id==owner);off();CHECK(dura_meter_release_recipe(owner)==ESP_OK);
 for(int fail=1;fail<=3;fail++){ui_go_main();fail_open=fail==1;fail_write=fail==2;fail_commit=fail==3;handle_external_recirc_press();off();CHECK(snap().operation_mode==DURA_OPERATION_IDLE);CHECK(s_ui_screen==DURA_UI_MAIN_MENU);}
}
static void range_test(void){
 const float bad[]={NAN,INFINITY,-INFINITY,-1.0f};
 for(int rec=0;rec<2;rec++)for(int i=0;i<4;i++){
  ui_go_main();press(rec?DURA_BUTTON_SELECT:DURA_BUTTON_BACK);s_ui_batch_amount=bad[i];
  press(DURA_BUTTON_BACK);off();CHECK(s_ui_screen==(rec?DURA_UI_RECIRC_EDIT:DURA_UI_AUTO_BATCH_EDIT));
  press(DURA_BUTTON_SELECT);CHECK(isfinite(s_ui_batch_amount)&&s_ui_batch_amount>=0);
 }
 for(int u=1;u<=3;u++){
  ui_go_main();CHECK(dura_meter_set_units(u)==ESP_OK);press(DURA_BUTTON_BACK);
  s_ui_batch_amount=0;press(DURA_BUTTON_BACK);off();CHECK(s_ui_screen==DURA_UI_AUTO_BATCH_EDIT);
  s_ui_batch_amount=.00000001f;press(DURA_BUTTON_BACK);off();CHECK(s_ui_screen==DURA_UI_AUTO_BATCH_EDIT);
  s_ui_batch_amount=1e30f;press(DURA_BUTTON_SELECT);CHECK(isfinite(s_ui_batch_amount));refresh();CHECK(clipped_foreground==0);
 }
}
static void has(const char*s){if(!strstr(rendered_text,s))fprintf(stderr,"Missing [%s] in [%s]\n",s,rendered_text);CHECK(strstr(rendered_text,s));}
static void pixels(const char *name){FILE*f=fopen(name,"wb");CHECK(f);fprintf(f,"P1\n128 64\n");for(int y=0;y<64;y++){for(int x=0;x<128;x++){int py=63-y;fprintf(f,"%d ",(s_fb[(py/8)*128+x]>>(py%8))&1);}fputc('\n',f);}fclose(f);}
static void popup_pixels(int mode){
 /* Real meter completion and production renderer/font/framebuffer; no label oracle. */
 ui_go_main();CHECK(dura_meter_set_units(1)==ESP_OK);
 if(mode==3){s_ui_screen=DURA_UI_FLOW_ERROR;}
 else {
  press(mode==1?DURA_BUTTON_SELECT:DURA_BUTTON_BACK);
  if(mode==2)s_ui_screen=DURA_UI_BATCH_SET;
  s_ui_batch_amount=2.5f;press(DURA_BUTTON_BACK);
  CHECK(snap().batch_mode==DURA_BATCH_RUNNING);
  CHECK(dura_meter_record_pulse(snap().preset_batch_counts)==ESP_OK);
  CHECK(snap().batch_mode==DURA_BATCH_DONE);off();
  /* Both Auto and Recirc must naturally reach the finished popup.
   * Never assign BATCH_COMPLETE in this test: that masks a missing transition. */
  refresh();CHECK(s_ui_screen==DURA_UI_BATCH_COMPLETE);off();
 }
 for(int repeat=0;repeat<3;repeat++){
  refresh();CHECK(s_ui_screen==(mode==3?DURA_UI_FLOW_ERROR:DURA_UI_BATCH_COMPLETE));
  uint8_t actual[1024];memcpy(actual,s_fb,sizeof(actual));pixels("popup-actual.pbm");
  dura_lcd_clear();
  draw_legacy_menu_screen(mode==3?"FLOW DETECTION":"BATCH PRESET",mode==3?"ERROR":"FINISHED",
                          NULL,NULL,NULL,mode==3?"Home":"Back",NULL,NULL,NULL);
  pixels("popup-expected.pbm");
  unsigned different=0;
  for(int y=0;y<64;y++)for(int x=0;x<128;x++){
   int py=63-y;unsigned mask=1u<<(py%8);int i=(py/8)*128+x;
   if((actual[i]^s_fb[i])&mask){fprintf(stderr,"pixel mismatch x=%d y=%d\n",x,y);different++;}
  }
  fprintf(stderr,"mode=%d refresh=%d differing_pixels=%u\n",mode,repeat,different);
  CHECK(!memcmp(actual,s_fb,sizeof(actual)));
 }
}
static void manual_pixels(void){
 for(int u=1;u<=3;u++)for(int i=0;i<3;i++){
  dura_meter_snapshot_t m={.selected_units=u,.meter_total=i==0?.25f:i==1?10:199.89f};
  dura_lcd_clear();render_manual_run(&m);uint8_t actual[1024];memcpy(actual,s_fb,1024);pixels("manual.pbm");
  /* Independent narrow golden: baseline composition minus only Power draw,
   * NOT erasing a band (which would destroy previously covered numeric ink). */
  dura_lcd_clear();char text[24];format_legacy_batch_amount(text,sizeof(text),m.meter_total,u);
  dura_lcd_draw_bitmap_1bpp(0,0,dura_asset_durametertitle_bmp_glcd_bmp,128,8,true);draw_num40_right(128,12,text);
  draw_legacy_text_aligned('R',128,42,unit_label(u),false);draw_legacy_buttons("Home","Reset","Stop","Start");
  CHECK(!memcmp(actual,s_fb,1024));
 }
}
static void settings(void){CHECK(next_setup_item(5)==8);s_ui_screen=DURA_UI_SETUP;s_ui_setup_item=7;refresh();CHECK(!strstr(rendered_text,"Language"));press(DURA_BUTTON_DOWN);CHECK(snap().selected_language==DURA_LANGUAGE_ENGLISH);}
static void lifetime(void){for(int u=1;u<=3;u++){
 dura_meter_snapshot_t m={.selected_units=u,.total_total=0,.odometer_counts=100,.working_coefficients={.gallon_per_count=.5f,.liter_per_count=1.9f,.ounce_per_count=64}};
 rendered_text[0]=0;render_swv(&m);char s[40];snprintf(s,sizeof(s),"%.0f %s",50.0*factor(u),unit_label(u));has(s);
}}
static void cal_help(void){
 for(int liters=0;liters<2;liters++){
  s_ui_screen=liters?DURA_UI_CAL_HELP_LITER:DURA_UI_CAL_HELP_GAL;s_ui_help_page=0;refresh();has(liters?"using from 10.0 to 30.0":"using from 2.5 to 7.5");pixels(liters?"help-liter.pbm":"help-gallon.pbm");
  press(DURA_BUTTON_DOWN);refresh();has(liters?"going below 8.0 LPM and":"going below 2.0 GPM and");has("press 'Cont'");
  press(DURA_BUTTON_DOWN);refresh();has("Press 'Save'");
  press(DURA_BUTTON_DOWN);refresh();has("Remember!!!");CHECK(!strcmp(labels[1],""));press(DURA_BUTTON_DOWN);CHECK(s_ui_help_page==3);
 }
 CHECK(dura_meter_start_calibration()==ESP_OK);CHECK(dura_meter_record_pulse(12)==ESP_OK);s_ui_screen=DURA_UI_CAL_HELP_GAL;press(DURA_BUTTON_UP);CHECK(snap().calibration_counts==12 && snap().calibration_mode==DURA_CAL_RUNNING);CHECK(dura_meter_cancel_calibration()==ESP_OK);
}
static void cal_units(void){for(int u=1;u<=3;u++){
 dura_meter_snapshot_t m={.selected_units=u};s_ui_cal_measured_amount=5.25f;rendered_text[0]=0;dura_lcd_clear();render_cal_adjust(&m);has("Enter the amount measured");has(unit_label(u));pixels("cal-amount.pbm");
}}
static void precal(void){
 s_ui_screen=DURA_UI_PRECAL_1;refresh();has("Recall");uint8_t recall[1024];memcpy(recall,s_fb,1024);pixels("precal-recall.pbm");
 s_ui_screen=DURA_UI_CAL_SAVE_1;refresh();has("save your fluid calibration #");CHECK(memcmp(recall,s_fb,1024));pixels("precal-save.pbm");
 s_ui_screen=DURA_UI_PRECAL_1;press(DURA_BUTTON_DOWN);refresh();has("No saved calibration");off();CHECK(s_ui_screen==DURA_UI_MAIN_MENU);
 ui_now+=3000000;refresh();CHECK(!strstr(rendered_text,"No saved calibration"));
 s_ui_screen=DURA_UI_PRECAL_1;fail_commit=1;press(DURA_BUTTON_UP);refresh();has("Save failed");CHECK(!strstr(rendered_text,"Calibration saved"));CHECK(s_ui_screen==DURA_UI_PRECAL_1);off();
}
static void cal_feedback(void){
 press(DURA_BUTTON_DOWN);press(DURA_BUTTON_DOWN);CHECK(snap().calibration_mode==DURA_CAL_RUNNING);
 press(DURA_BUTTON_BACK);refresh();has("Not enough flow");CHECK(snap().calibration_mode==DURA_CAL_RUNNING);
 /* Overlay never blocks Cancel while pump is on. */
 press(DURA_BUTTON_UP);off();CHECK(snap().calibration_mode==DURA_CAL_IDLE);
 press(DURA_BUTTON_DOWN);CHECK(dura_meter_record_pulse(DURA_MIN_FLOW_COUNTS)==ESP_OK);press(DURA_BUTTON_BACK);refresh();has("Cal Counts");char n[16];snprintf(n,sizeof(n),"%u",DURA_MIN_FLOW_COUNTS);has(n);off();pixels("cal-counts.pbm");
 ui_now+=3000000;refresh();CHECK(!strstr(rendered_text,"Cal Counts"));CHECK(s_ui_screen==DURA_UI_CAL_ADJUST);
 press(DURA_BUTTON_BACK);CHECK(s_ui_screen==DURA_UI_CAL_SAVE_1);
 for(int f=1;f<=3;f++){fail_open=f==1;fail_write=f==2;fail_commit=f==3;press(DURA_BUTTON_DOWN);refresh();has("Save failed");CHECK(!strstr(rendered_text,"Calibration saved"));CHECK(s_ui_screen==DURA_UI_CAL_SAVE_1);off();}
 fail_open=fail_write=fail_commit=0;press(DURA_BUTTON_DOWN);refresh();has("Calibration saved");has("Fluid1");off();CHECK(s_ui_screen==DURA_UI_MAIN_MENU);
}
static void quick(void){
 s_ui_screen=DURA_UI_QUICK_CAL;refresh();has("Product names for");has("reference only!");pixels("quick.pbm");
 s_ui_quick_ref=DURA_QUICK_CAL_REFERENCE_MIN;press(DURA_BUTTON_DOWN);refresh();has("Reference limit");CHECK(s_ui_quick_ref==DURA_QUICK_CAL_REFERENCE_MIN);
 s_ui_quick_ref=DURA_QUICK_CAL_REFERENCE_MAX;press(DURA_BUTTON_SELECT);refresh();has("Reference limit");CHECK(s_ui_quick_ref==DURA_QUICK_CAL_REFERENCE_MAX);
 fail_commit=1;press(DURA_BUTTON_BACK);refresh();has("Save failed");CHECK(!strstr(rendered_text,"Calibration saved"));off();CHECK(s_ui_screen==DURA_UI_QUICK_CAL);
 fail_commit=0;press(DURA_BUTTON_BACK);refresh();has("Calibration saved");off();CHECK(s_ui_screen==DURA_UI_MAIN_MENU);
}
static void notice_fit(void){
 const char *lines[]={"Calibration recalled","Ready","No saved calibration","Choose another fluid","Save failed","Retry or Cancel","Calibration saved","Water","Fluid5","Not enough flow","Cal Counts","4294967295","Reference limit","Use manual calibration","Quick Cal","Calibration error","Counts reset; retry"};
 for(unsigned i=0;i<sizeof(lines)/sizeof(*lines);i++)for(int row=0;row<2;row++){
  dura_lcd_clear();quantity_begin(lines[i]);draw_legacy_text_aligned('C',64,row?29:15,lines[i],false);quantity_pixels=false;
  printf("notice [%s] row %d bounds %d,%d..%d,%d clipped %u\n",lines[i],row,qminx,qminy,qmaxx,qmaxy,clipped_foreground);
  CHECK(quantity_foreground && !clipped_foreground);CHECK(qminx>1 && qmaxx<126);CHECK(qminy>13 && qmaxy<41);
  CHECK(row?qminy>27:qmaxy<29);
 }
}
int main(int argc,char**argv){CHECK(argc==2);fresh();s_provisioning_qr_dismissed=true;
 if(!strncmp(argv[1],"popup_",6)){
  int mode=!strcmp(argv[1],"popup_auto")?0:!strcmp(argv[1],"popup_recirc")?1:!strcmp(argv[1],"popup_batch")?2:3;
  popup_pixels(mode);printf("PASS %s\n",argv[1]);return 0;
 }
 if(!strcmp(argv[1],"hold_precision")){hold_precision();puts("PASS hold_precision");return 0;}
 if(!strcmp(argv[1],"hold_calibration")){hold_calibration();puts("PASS hold_calibration");return 0;}
 if(!strcmp(argv[1],"notice_fit")){notice_fit();return 0;}
 if(!strcmp(argv[1],"manual_pixels"))manual_pixels();else if(!strcmp(argv[1],"settings"))settings();else if(!strcmp(argv[1],"lifetime"))lifetime();else if(!strcmp(argv[1],"cal_help"))cal_help();else if(!strcmp(argv[1],"cal_units"))cal_units();else if(!strcmp(argv[1],"precal"))precal();else if(!strcmp(argv[1],"cal_feedback"))cal_feedback();else if(!strcmp(argv[1],"quick"))quick();
 else if(!strcmp(argv[1],"precision"))precision();else if(!strcmp(argv[1],"live_units"))live_units();else if(!strcmp(argv[1],"retention"))retention();else if(!strcmp(argv[1],"navigation"))navigation();else if(!strcmp(argv[1],"external"))external();else if(!strcmp(argv[1],"range"))range_test();else CHECK(false);
 printf("PASS %s\n",argv[1]);return 0;}
