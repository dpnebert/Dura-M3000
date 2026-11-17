/* M3000 fix cycle 1 (items 1, 4, 6): real board handlers/renderers, real meter,
 * genuine font assets and LCD pixel code. Only GPIO/NVS/clock are host seams
 * (safe_output.c). Framebuffer evidence is written as PGM into the case cwd.
 * NOT device, flash, power-loss or pump verification. */
#define main safety_fixture_main
#include "safe_output.c"
#undef main
#include "volume_config.h"
static int64_t ui_now=1000000;
static int64_t ui_clock(void){return ui_now;}
#define esp_timer_get_time ui_clock
#include "board_home_volume.inc"
#undef esp_timer_get_time

static bool fb_pixel(const uint8_t *fb,int x,int y){int py=63-y;return (fb[(py/8)*128+x]>>(py%8))&1;}
static void press(dura_button_t b){dura_board_handle_button(b);}
static void refresh(void){rendered_text[0]=0;memset(labels,0,sizeof(labels));CHECK(dura_board_render_debug_screen()==ESP_OK);}
static void label_is(int slot,const char *l){if(strcmp(labels[slot],l))fprintf(stderr,"slot %d label [%s] expected [%s]\n",slot,labels[slot],l);CHECK(!strcmp(labels[slot],l));}
static void has(const char *s){if(!strstr(rendered_text,s))fprintf(stderr,"missing [%s] in [\n%s]\n",s,rendered_text);CHECK(strstr(rendered_text,s)!=NULL);}
static void no_null(void){CHECK(strstr(rendered_text,"(null)")==NULL);}
static void pgm(const char *name){
    FILE *f=fopen(name,"wb");CHECK(f);CHECK(fputs("P5\n128 64\n255\n",f)>=0);
    for(int y=0;y<64;y++)for(int x=0;x<128;x++)CHECK(fputc(fb_pixel(s_fb,x,y)?0:0xff,f)!=EOF);
    CHECK(fclose(f)==0);
}
/* One production text row, measured with the production renderer and the
 * genuine font: foreground ink box, clipping and the cursor/cleanup column. */
typedef struct {int start,end,minx,maxx,miny,maxy;unsigned ink,clipped;} row_t;
static row_t measure_row(int y,const char *text){
    row_t r;dura_lcd_clear();quantity_begin(text);
    const int w=legacy_text_width(&dura_font_ui_small,text);
    r.start=64-(w/2)+1;
    r.end=draw_legacy_text_aligned('C',64,y,text,false);
    quantity_pixels=false;
    r.minx=qminx;r.maxx=qmaxx;r.miny=qminy;r.maxy=qmaxy;r.ink=quantity_foreground;r.clipped=clipped_foreground;
    printf("ROW y=%d [%s] start=%d end_excl=%d ink=(%d,%d)-(%d,%d) clipped=%u\n",y,text,r.start,r.end,r.minx,r.miny,r.maxx,r.maxy,r.clipped);
    return r;
}
/* Notice popup: border rect x=1..126, y=13..41; rows at y=15 and y=29. */
static void notice_rows_fit(const char *l1,const char *l2){
    const char *rows[2]={l1,l2};
    for(int i=0;i<2;i++){
        if(!rows[i][0])continue;
        row_t r=measure_row(i?29:15,rows[i]);
        CHECK(r.ink>0&&r.clipped==0);
        CHECK(r.start>=2&&r.minx>=2&&r.maxx<=125);
        CHECK(r.end<=125); /* cleanup column must not erase the x=126 border */
        CHECK(r.miny>13&&r.maxy<41);
        CHECK(i?r.miny>27:r.maxy<29);
    }
}
/* Every row the production renderer actually drew on this frame fits the
 * 128 px display (bound to rendered_text, not to the test's expected rows). */
static void rendered_rows_fit(void){
    char copy[sizeof(rendered_text)];snprintf(copy,sizeof(copy),"%s",rendered_text);
    for(char *line=strtok(copy,"\n");line;line=strtok(NULL,"\n")){
        row_t r=measure_row(20,line);
        CHECK(r.clipped==0&&r.start>=0&&r.end<=128&&r.minx>=0&&r.maxx<=127);
    }
}
static void popup_frame(const char *l1,const char *l2,const char *file){
    /* Geometry of what production stored, before comparing wording. */
    notice_rows_fit(s_ui_notice_line1,s_ui_notice_line2);
    CHECK(!strcmp(s_ui_notice_line1,l1));
    CHECK(!strcmp(s_ui_notice_line2,l2));
    refresh();no_null();has(l1);if(l2[0])has(l2);
    uint8_t with[1024];memcpy(with,s_fb,sizeof(with));pgm(file);
    /* Actual framebuffer coverage: popup border fully intact. */
    for(int x=1;x<=126;x++){CHECK(fb_pixel(with,x,13));CHECK(fb_pixel(with,x,41));}
    for(int y=13;y<=41;y++){CHECK(fb_pixel(with,1,y));CHECK(fb_pixel(with,126,y));}
    /* Interior ink is only the notice text and lies inside the body. */
    unsigned ink=0;
    for(int y=14;y<=40;y++)for(int x=2;x<=125;x++)if(fb_pixel(with,x,y))++ink;
    CHECK(ink>0);
    /* Outside the popup, the frame equals the same screen once the notice expires. */
    const int64_t saved=ui_now;ui_now+=3000000;refresh();
    CHECK(strstr(rendered_text,l1)==NULL||!strcmp(l1,""));
    for(int y=0;y<64;y++)for(int x=0;x<128;x++){
        if(x>=1&&x<=126&&y>=13&&y<=41)continue;
        if(fb_pixel(with,x,y)!=fb_pixel(s_fb,x,y)){fprintf(stderr,"pixel outside popup changed at %d,%d\n",x,y);CHECK(false);}
    }
    ui_now=saved;
    notice_rows_fit(l1,l2);
}
static void joined_is(const char *expected){
    char shown[80];snprintf(shown,sizeof(shown),"%s%s%s",s_ui_notice_line1,s_ui_notice_line2[0]?" ":"",s_ui_notice_line2);
    if(strcmp(shown,expected))fprintf(stderr,"notice [%s] expected [%s]\n",shown,expected);
    CHECK(!strcmp(shown,expected));
}
static const char *const NAMES[6]={"Water","Fluid 1","Fluid 2","Fluid 3","Fluid 4","Fluid 5"};

/* ---- Item 1 / issue 1: NULL-safe notice ---- */
static void notice_null_safe(void){
    ui_show_notice("Only line",NULL);CHECK(!strcmp(s_ui_notice_line2,""));
    ui_show_notice(NULL,NULL);CHECK(!strcmp(s_ui_notice_line1,"")&&!strcmp(s_ui_notice_line2,""));
    ui_go_main();ui_show_notice("Only line",NULL);refresh();no_null();has("Only line");
}
/* ---- Item 4: empty-slot recall notices, every slot ---- */
static void precal_empty_notices(void){
    for(int slot=1;slot<6;slot++){
        const dura_meter_snapshot_t before=snap();
        s_ui_screen=DURA_UI_CAL_START;press(DURA_BUTTON_SELECT);CHECK(s_ui_screen==DURA_UI_PRECAL_1);
        if(slot>=3){press(DURA_BUTTON_BACK);CHECK(s_ui_screen==DURA_UI_PRECAL_2);}
        press((dura_button_t)(1+slot%3));
        CHECK(s_ui_screen==DURA_UI_MAIN_MENU);
        char expected[64],l1[32];snprintf(expected,sizeof(expected),"%s has no saved value",NAMES[slot]);
        joined_is(expected);snprintf(l1,sizeof(l1),"%s has",NAMES[slot]);
        char file[48];snprintf(file,sizeof(file),"notice-empty-slot%d.pgm",slot);
        popup_frame(l1,"no saved value",file);
        const dura_meter_snapshot_t after=snap();
        CHECK(!memcmp(&after.working_coefficients,&before.working_coefficients,sizeof(after.working_coefficients)));
        CHECK(after.selected_precal_profile==before.selected_precal_profile);off();
    }
}
static void run_bucket_to_save(float amount){
    s_ui_screen=DURA_UI_CAL_START;press(DURA_BUTTON_DOWN);
    CHECK(snap().calibration_mode==DURA_CAL_RUNNING);
    CHECK(dura_meter_record_pulse(250)==ESP_OK);press(DURA_BUTTON_BACK);
    refresh(); /* production render reconciliation moves to the measured editor */
    CHECK(s_ui_screen==DURA_UI_CAL_ADJUST);off();
    s_ui_cal_measured_amount=amount;press(DURA_BUTTON_BACK);
    CHECK(snap().calibration_mode==DURA_CAL_WAIT_SAVE);CHECK(s_ui_screen==DURA_UI_CAL_SAVE_1);
}
/* ---- Item 4: successful save notices (Water + every Fluid), failure never claims success ---- */
static void precal_save_notices(void){
    for(int slot=0;slot<6;slot++){
        run_bucket_to_save(5.0f);
        /* An injected commit failure must not show the success text. */
        fail_commit=1;
        if(slot>=3)press(DURA_BUTTON_BACK);
        press((dura_button_t)(1+slot%3));
        fail_commit=0;
        CHECK(strstr(s_ui_notice_line1,"has")==NULL);
        CHECK(!strcmp(s_ui_notice_line1,"Save failed"));
        CHECK(s_ui_screen==(slot>=3?DURA_UI_CAL_SAVE_2:DURA_UI_CAL_SAVE_1));
        press((dura_button_t)(1+slot%3));
        CHECK(s_ui_screen==DURA_UI_MAIN_MENU);
        char expected[64],l1[32];snprintf(expected,sizeof(expected),"%s has a new saved value",NAMES[slot]);
        joined_is(expected);snprintf(l1,sizeof(l1),"%s has",NAMES[slot]);
        char file[48];snprintf(file,sizeof(file),"notice-saved-slot%d.pgm",slot);
        popup_frame(l1,"a new saved value",file);
        CHECK(snap().selected_precal_profile==slot);off();
        ui_now+=3000000;
    }
}
static void to_bucket(void){
    s_ui_screen=DURA_UI_CAL_START;press(DURA_BUTTON_DOWN);
    CHECK(s_ui_screen==DURA_UI_BUCKET_GAL||s_ui_screen==DURA_UI_BUCKET_LITER);
    CHECK(snap().calibration_mode==DURA_CAL_RUNNING&&snap().calibration_counts==0);
    CHECK(snap().pump_enabled&&gpio[0]);
}
/* ---- Item 6 / CA-03: early Cont at 0, below, at and above threshold ---- */
static void insufficient_flow(void){
    const uint32_t below[]={0,1,DURA_MIN_FLOW_COUNTS-1};
    for(unsigned i=0;i<3;i++){
        to_bucket();
        if(below[i])CHECK(dura_meter_record_pulse(below[i])==ESP_OK);
        CHECK(snap().calibration_counts==below[i]);
        press(DURA_BUTTON_BACK);
        CHECK(snap().calibration_mode==DURA_CAL_RUNNING);CHECK(snap().calibration_counts==0);
        notice_rows_fit(s_ui_notice_line1,s_ui_notice_line2); /* out-of-bounds span guard */
        joined_is("Not enough fluid flow detected");
        popup_frame("Not enough fluid","flow detected",i==2?"notice-insufficient-flow.pgm":"notice-insufficient-flow-tmp.pgm");
        /* Retry cannot inherit the rejected counts. */
        CHECK(dura_meter_record_pulse(DURA_MIN_FLOW_COUNTS)==ESP_OK);press(DURA_BUTTON_BACK);
        CHECK(snap().calibration_mode==DURA_CAL_WAIT_MEASURED_AMOUNT);
        CHECK(snap().calibration_counts==DURA_MIN_FLOW_COUNTS);
        char n[16];snprintf(n,sizeof(n),"%u",DURA_MIN_FLOW_COUNTS);
        CHECK(!strcmp(s_ui_notice_line1,"Cal Counts")&&!strcmp(s_ui_notice_line2,n));off();
        ui_now+=3000000;ui_cancel_calibration_to_start();CHECK(s_ui_screen==DURA_UI_CAL_START);off();
    }
    const uint32_t ok[]={DURA_MIN_FLOW_COUNTS,DURA_MIN_FLOW_COUNTS+1};
    for(unsigned i=0;i<2;i++){
        to_bucket();CHECK(dura_meter_record_pulse(ok[i])==ESP_OK);press(DURA_BUTTON_BACK);
        CHECK(snap().calibration_mode==DURA_CAL_WAIT_MEASURED_AMOUNT&&snap().calibration_counts==ok[i]);
        CHECK(strcmp(s_ui_notice_line1,"Not enough fluid"));off();
        ui_now+=3000000;ui_cancel_calibration_to_start();off();
    }
}
/* ---- Item 6 / CA-01, CA-02: Help/Return with real pulses, both units ---- */
static void cal_help_pulses(void){
    for(int liters=0;liters<2;liters++){
        CHECK(dura_meter_set_units(liters?DURA_UNITS_LITER:DURA_UNITS_GALLON)==ESP_OK);
        to_bucket();
        const dura_ui_screen_t bucket=s_ui_screen,help=liters?DURA_UI_CAL_HELP_LITER:DURA_UI_CAL_HELP_GAL;
        CHECK(bucket==(liters?DURA_UI_BUCKET_LITER:DURA_UI_BUCKET_GAL));
        CHECK(dura_meter_record_pulse(180)==ESP_OK);CHECK(snap().calibration_counts==180);
        int edges=on_edges,pump=gpio[0];
        press(DURA_BUTTON_DOWN);                      /* Help */
        CHECK(s_ui_screen==help);CHECK(snap().calibration_counts==0);
        CHECK(snap().calibration_mode==DURA_CAL_RUNNING);
        CHECK(on_edges==edges&&gpio[0]==pump&&snap().pump_enabled); /* no restart, no stop */
        refresh();label_is(0,"Return");no_null();
        CHECK(dura_meter_record_pulse(30)==ESP_OK);    /* pulses while Help is open */
        CHECK(snap().calibration_counts==30);
        press(DURA_BUTTON_DOWN);CHECK(s_ui_screen==help&&s_ui_help_page==1);
        press(DURA_BUTTON_UP);                        /* Return */
        CHECK(s_ui_screen==bucket);
        CHECK(snap().calibration_counts==30);         /* old 180 not restored */
        CHECK(on_edges==edges&&gpio[0]==pump);
        CHECK(dura_meter_record_pulse(20)==ESP_OK);CHECK(snap().calibration_counts==50);
        /* Repeated Help; leave via the Cal key this time. */
        press(DURA_BUTTON_DOWN);CHECK(s_ui_screen==help&&snap().calibration_counts==0&&s_ui_help_page==0);
        press(DURA_BUTTON_BACK);CHECK(s_ui_screen==bucket&&snap().calibration_counts==0);
        CHECK(on_edges==edges&&gpio[0]==pump);
        /* Post-Help collection is exactly what arrived after Help. */
        CHECK(dura_meter_record_pulse(DURA_MIN_FLOW_COUNTS)==ESP_OK);press(DURA_BUTTON_BACK);
        CHECK(snap().calibration_mode==DURA_CAL_WAIT_MEASURED_AMOUNT&&snap().calibration_counts==DURA_MIN_FLOW_COUNTS);
        off();
        /* Cancel then a new attempt starts clean with no stale count. */
        press(DURA_BUTTON_UP);CHECK(s_ui_screen==DURA_UI_CAL_START);off();
        CHECK(snap().calibration_mode==DURA_CAL_IDLE);
        to_bucket();CHECK(snap().calibration_counts==0);
        CHECK(dura_meter_record_pulse(90)==ESP_OK);press(DURA_BUTTON_DOWN);CHECK(snap().calibration_counts==0);
        press(DURA_BUTTON_UP);press(DURA_BUTTON_UP);  /* Return, then Cancel */
        CHECK(s_ui_screen==DURA_UI_CAL_START&&snap().calibration_mode==DURA_CAL_IDLE);off();
        /* No saved-profile overwrite from Help/Cancel. */
        for(int p=1;p<6;p++)CHECK(dura_meter_select_precal_profile(p)==ESP_ERR_NOT_FOUND);
    }
}
/* ---- Item 1 / FR-06: Factory Reset Help rows bounded ---- */
static void factory_help_frame(void){
    static const char *const rows[]={"Factory Reset","Custom settings are replaced,","including saved calibration","values. Are you sure?"};
    for(int item=8;item<=9;item++){
        s_ui_screen=DURA_UI_SETUP_HELP;s_ui_setup_item=item;refresh();no_null();
        char drawn[sizeof(rendered_text)];snprintf(drawn,sizeof(drawn),"%s",rendered_text);
        if(item==8)pgm("factory-reset-help.pgm"); /* before row probes reuse the framebuffer */
        for(int y=44;y<52;y++)for(int x=0;x<128;x++)CHECK(!fb_pixel(s_fb,x,y)); /* text clear of softkeys */
        rendered_rows_fit();
        for(int i=0;i<4;i++){char row[64];snprintf(row,sizeof(row),"%s\n",rows[i]);CHECK(strstr(drawn,row)!=NULL);}
        label_is(0,"Back");label_is(1,"");label_is(2,"");label_is(3,""); /* cycle 2: B1 Back returns to page 8 */
        for(int i=0;i<4;i++){
            row_t r=measure_row(2+10*i,rows[i]);
            CHECK(r.ink>0&&r.clipped==0&&r.start>=0&&r.end<=128&&r.minx>=0&&r.maxx<=127);
            CHECK(r.maxy<52); /* above the softkey band */
        }
    }
    /* The whole sentence as one row is the defect this guards against. */
    row_t whole=measure_row(12,"Custom settings are replaced, including saved calibration values. Are you sure?");
    CHECK(whole.clipped>0||whole.start<0||whole.end>128);
}
/* ---- Item 1 / FR-02, FR-04 and default scopes via the real handler ---- */
static dura_meter_snapshot_t s_factory;
static float s_seed_auto=33.0f,s_seed_recirc=44.0f;
static void seed(void){
    CHECK(dura_meter_set_units(DURA_UNITS_LITER)==ESP_OK);
    CHECK(dura_meter_set_show_flow_rate(!snap().show_flow_rate)==ESP_OK);
    CHECK(dura_meter_set_show_batch_total(true)==ESP_OK);
    CHECK(dura_meter_set_auto_batch(!snap().show_auto_batch)==ESP_OK);
    CHECK(dura_meter_set_meter_timeout((uint8_t)(s_factory.meter_timeout_sec+5))==ESP_OK);
    CHECK(dura_meter_set_backlight_timeout((uint8_t)(s_factory.backlight_timeout_sec+5))==ESP_OK);
    CHECK(snap().meter_timeout_sec!=s_factory.meter_timeout_sec&&snap().backlight_timeout_sec!=s_factory.backlight_timeout_sec);
    CHECK(snap().selected_units!=s_factory.selected_units);
    CHECK(dura_meter_apply_quick_cal(5)==ESP_OK);
    CHECK(dura_meter_store_working_precal_profile(3)==ESP_OK);
    CHECK(dura_meter_start_manual()==ESP_OK);CHECK(dura_meter_record_pulse(777)==ESP_OK);CHECK(dura_meter_stop_manual()==ESP_OK);
    CHECK(dura_board_sync_outputs()==ESP_OK);off();
    s_ui_auto_amount_gal=s_seed_auto;s_ui_recirc_amount_gal=s_seed_recirc;s_ui_batch_amount=s_seed_auto;
}
static void same_settings(const dura_meter_snapshot_t *a,const dura_meter_snapshot_t *b){
    CHECK(a->selected_units==b->selected_units&&a->show_flow_rate==b->show_flow_rate);
    CHECK(a->show_batch_total==b->show_batch_total&&a->show_auto_batch==b->show_auto_batch);
    CHECK(a->meter_timeout_sec==b->meter_timeout_sec&&a->backlight_timeout_sec==b->backlight_timeout_sec);
    CHECK(a->quick_cal_reference==b->quick_cal_reference&&a->selected_precal_profile==b->selected_precal_profile);
    CHECK(!memcmp(&a->working_coefficients,&b->working_coefficients,sizeof(a->working_coefficients)));
    CHECK(!memcmp(a->precal_profiles,b->precal_profiles,sizeof(a->precal_profiles)));
    CHECK(a->odometer_counts==b->odometer_counts&&a->meter_total_counts==b->meter_total_counts);
    CHECK(a->batch_total_counts==b->batch_total_counts&&a->total_total_counts==b->total_total_counts);
}
static void seeded_cancel(void){
    seed();const dura_meter_snapshot_t before=snap();
    /* Each exit that must not mutate: page 8 Home, page 9 Home (B0), page 9 Home (B3). */
    const struct {int item;dura_button_t b;} exits[]={{8,DURA_BUTTON_UP},{9,DURA_BUTTON_UP},{9,DURA_BUTTON_BACK},{9,DURA_BUTTON_SELECT}};
    for(unsigned i=0;i<4;i++){
        ui_go_main();s_ui_screen=DURA_UI_SETUP;s_ui_setup_item=8;refresh();
        label_is(0,"Home");label_is(1,"Yes");label_is(3,"Next");
        if(exits[i].item==9){press(DURA_BUTTON_DOWN);CHECK(s_ui_setup_item==9);refresh();label_is(0,"Home");label_is(1,"Yes");label_is(2,"");label_is(3,"Home");}
        press(exits[i].b);
        if(exits[i].b==DURA_BUTTON_SELECT)CHECK(s_ui_screen==DURA_UI_SETUP&&s_ui_setup_item==9); /* blank key inert */
        else CHECK(s_ui_screen==DURA_UI_MAIN_MENU);
        const dura_meter_snapshot_t now=snap();same_settings(&now,&before);
        CHECK(s_ui_auto_amount_gal==s_seed_auto&&s_ui_recirc_amount_gal==s_seed_recirc);off();
    }
    /* Page 8 Next leaves page 8 without mutation. */
    s_ui_screen=DURA_UI_SETUP;s_ui_setup_item=8;press(DURA_BUTTON_BACK);CHECK(s_ui_setup_item!=8&&s_ui_setup_item!=9);
    {const dura_meter_snapshot_t now=snap();same_settings(&now,&before);}
}
static void expect_defaults(uint32_t odometer){
    const dura_meter_snapshot_t m=snap();
    CHECK(m.selected_units==s_factory.selected_units&&m.selected_units==DURA_UNITS_GALLON);
    /* Factory Water = fresh-boot defaults with empty storage, not a copied constant. */
    CHECK(!memcmp(&m.working_coefficients,&s_factory.working_coefficients,sizeof(m.working_coefficients)));
    CHECK(!memcmp(&m.precal_profiles[0],&s_factory.precal_profiles[0],sizeof(m.precal_profiles[0])));
    CHECK(m.show_flow_rate==s_factory.show_flow_rate&&m.show_batch_total==s_factory.show_batch_total&&m.show_auto_batch==s_factory.show_auto_batch);
    for(int p=1;p<6;p++)CHECK(dura_meter_select_precal_profile(p)==ESP_ERR_NOT_FOUND);
    CHECK(m.selected_precal_profile==0&&m.quick_cal_reference==DURA_QUICK_CAL_DEFAULT);
    CHECK(m.meter_timeout_sec==s_factory.meter_timeout_sec&&m.backlight_timeout_sec==s_factory.backlight_timeout_sec);
    CHECK(m.odometer_counts==odometer);                 /* lifetime history preserved */
    CHECK(s_ui_auto_amount_gal==DURA_AUTO_BATCH_DEFAULT_GAL&&s_ui_recirc_amount_gal==DURA_RECIRC_BATCH_DEFAULT_GAL);
    CHECK(!m.pump_enabled&&m.operation_mode==DURA_OPERATION_IDLE);off();
    printf("DEFAULT_SCOPE batch_total_counts=%u total_total_counts=%u meter_total_counts=%u odometer=%u show_flow=%d show_batch_total=%d auto_batch=%d\n",
           (unsigned)m.batch_total_counts,(unsigned)m.total_total_counts,(unsigned)m.meter_total_counts,(unsigned)m.odometer_counts,
           m.show_flow_rate,m.show_batch_total,m.show_auto_batch);
}
static void factory_failures(void){
    const char *kinds[]={"open","write","commit"};
    for(int k=0;k<3;k++){
        fresh();seed();const uint32_t odo=snap().odometer_counts;CHECK(odo>=777);
        s_ui_screen=DURA_UI_SETUP;s_ui_setup_item=8;press(DURA_BUTTON_DOWN);CHECK(s_ui_setup_item==9);
        fail_open=k==0;fail_write=k==1;fail_commit=k==2;safe_required=true;
        press(DURA_BUTTON_DOWN);                         /* Yes */
        fail_open=fail_write=fail_commit=0;safe_required=false;
        printf("FAILURE %s screen=%d item=%d notice=[%s|%s]\n",kinds[k],s_ui_screen,s_ui_setup_item,s_ui_notice_line1,s_ui_notice_line2);
        CHECK(s_ui_screen==DURA_UI_SETUP&&s_ui_setup_item==9);   /* no success navigation */
        CHECK(!strcmp(s_ui_notice_line1,"Reset failed")&&!strcmp(s_ui_notice_line2,"Retry or Cancel"));
        popup_frame("Reset failed","Retry or Cancel","factory-reset-failed.pgm");
        off();
        /* Characterized RAM outcome (no atomicity claim): defaults are already in RAM. */
        expect_defaults(odo);
        /* Retry succeeds and only then goes Home. */
        press(DURA_BUTTON_DOWN);CHECK(s_ui_screen==DURA_UI_MAIN_MENU);expect_defaults(odo);
    }
}
static void factory_success(void){
    seed();const uint32_t odo=snap().odometer_counts;
    s_ui_screen=DURA_UI_SETUP;s_ui_setup_item=8;press(DURA_BUTTON_DOWN);press(DURA_BUTTON_DOWN);
    CHECK(s_ui_screen==DURA_UI_MAIN_MENU);expect_defaults(odo);
}
int main(int argc,char **argv){
    CHECK(argc==2);fresh();s_factory=snap();s_provisioning_qr_dismissed=true;const char *n=argv[1];
    if(!strcmp(n,"notice_null_safe"))notice_null_safe();
    else if(!strcmp(n,"precal_empty_notices"))precal_empty_notices();
    else if(!strcmp(n,"precal_save_notices"))precal_save_notices();
    else if(!strcmp(n,"insufficient_flow"))insufficient_flow();
    else if(!strcmp(n,"cal_help_pulses"))cal_help_pulses();
    else if(!strcmp(n,"factory_help_frame"))factory_help_frame();
    else if(!strcmp(n,"factory_seeded_cancel"))seeded_cancel();
    else if(!strcmp(n,"factory_failures"))factory_failures();
    else if(!strcmp(n,"factory_success"))factory_success();
    else CHECK(false);
    printf("PASS %s\n",n);return 0;
}
