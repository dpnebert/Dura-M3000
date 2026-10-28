/* Help-only fixture. Real meter, UI dispatcher/render, LCD pixels and fonts. */
#define main safety_fixture_main
#include "safe_output.c"
#undef main
#include "volume_config.h"
#include "board_home_volume.inc"
#include "help_reference_oracle.h"
#include "help_reference_ids.inc"

static void press(dura_button_t b) { dura_board_handle_button(b); }
static void refresh(void) {
    memset(labels,0,sizeof(labels)); rendered_text[0]=0;
    CHECK(dura_board_render_debug_screen()==ESP_OK);
}
static void save_frame(const char *name) {
    char path[96];snprintf(path,sizeof(path),"%s.fb",name);
    FILE *f=fopen(path,"wb");CHECK(f);CHECK(fwrite(s_fb,1,1024,f)==1024);CHECK(!fclose(f));
}
static void frame_is(const char *name,const char *const *p,
                     const char *b0,const char *b1,const char *b2,const char *b3) {
    uint8_t actual[1024];memcpy(actual,s_fb,sizeof(actual));save_frame(name);
    dura_lcd_clear();draw_legacy_menu_screen(p[0],p[1],p[2],p[3],p[4],b0,b1,b2,b3);
    char expected[96];snprintf(expected,sizeof(expected),"%s-reference",name);save_frame(expected);
    if(memcmp(actual,s_fb,1024)) {
        fprintf(stderr,"FRAME mismatch %s: reference-derived title/button pixels differ\n",name);
        CHECK(false);
    }
}
static void no_side_effects(dura_meter_snapshot_t before) {
    dura_meter_snapshot_t after=snap();CHECK(!memcmp(&before,&after,sizeof(before)));off();
}
static void setup_help(int item) {
    s_ui_screen=DURA_UI_SETUP;s_ui_setup_item=item;refresh();
    CHECK(!strcmp(labels[2],"Help"));
    uint8_t parent[1024];memcpy(parent,s_fb,1024);
    dura_meter_snapshot_t before=snap();press(DURA_BUTTON_SELECT);refresh();
    char name[32];snprintf(name,sizeof(name),"setup%d-help",item);
    frame_is(name,reference_contact[0],"Back",NULL,NULL,NULL);no_side_effects(before);
    /* All unlabelled keys inert; leftmost Back alone restores the exact parent. */
    for(int b=DURA_BUTTON_DOWN;b<=DURA_BUTTON_BACK;b++) {
        press(b);refresh();frame_is(name,reference_contact[0],"Back",NULL,NULL,NULL);no_side_effects(before);
    }
    press(DURA_BUTTON_UP);refresh();
    CHECK(s_ui_screen==DURA_UI_SETUP && s_ui_setup_item==item);
    CHECK(!memcmp(parent,s_fb,1024));no_side_effects(before);
}
static void setup_traversal(void) {
    s_ui_screen=DURA_UI_SETUP;s_ui_setup_item=0;
    const int items[]={0,1,2,3,4,5,8,48,50,53};
    for(unsigned i=0;i<sizeof(items)/sizeof(*items);i++) {
        CHECK(s_ui_screen==DURA_UI_SETUP && s_ui_setup_item==items[i]);
        refresh();press(DURA_BUTTON_BACK);off();
    }
    CHECK(s_ui_screen==DURA_UI_MAIN_MENU);
}
static void scan(void) {
    for(int b=DURA_BUTTON_UP;b<=DURA_BUTTON_BACK;b++) {
        s_ui_screen=DURA_UI_INFO_START;refresh();CHECK(!strcmp(labels[1],"Scan"));
        dura_meter_snapshot_t before=snap();press(DURA_BUTTON_DOWN);refresh();save_frame("scan");
        /* Legacy drawbitmap reads page bytes LSB-first (Display.mbas:359-374).
         * Compare all logical pixels using this board's established physical
         * Y flip, not raw source bytes against controller framebuffer bytes.
         * Oracle data comes only from legacy Graphics.mbas, never candidate QR. */
        for (int y=0;y<64;y++) for (int x=0;x<128;x++) {
            const bool expected=(reference_qr[(y/8)*128+x] & (1u<<(y%8)))!=0;
            const int physical_y=63-y;
            const bool actual=(s_fb[(physical_y/8)*128+x] & (1u<<(physical_y%8)))!=0;
            CHECK(actual==expected);
        }
        no_side_effects(before);
        CHECK(!strcmp(labels[0],"")&&!strcmp(labels[1],"")&&!strcmp(labels[2],"")&&!strcmp(labels[3],""));
        press(b);refresh();CHECK(s_ui_screen==DURA_UI_INFO_START);no_side_effects(before);
    }
}
static void info_frame(int page) {
    s_ui_screen=DURA_UI_INFO_HELP;s_ui_info_page=page;refresh();
    char name[32];snprintf(name,sizeof(name),"info%d",page);
    frame_is(name,reference_info[page],"Home",NULL,"More","Info");
}
static void info_navigation(void) {
    s_ui_screen=DURA_UI_INFO_START;press(DURA_BUTTON_SELECT);
    for(int i=0;i<8;i++) {
        CHECK(s_ui_screen==DURA_UI_INFO_HELP && s_ui_info_page==i);
        press(DURA_BUTTON_DOWN);CHECK(s_ui_screen==DURA_UI_INFO_HELP && s_ui_info_page==i);
        press(DURA_BUTTON_SELECT);
    }
    CHECK(s_ui_screen==DURA_UI_INFO_START);
    for(int i=0;i<8;i++) {
        s_ui_screen=DURA_UI_INFO_HELP;s_ui_info_page=i;press(DURA_BUTTON_BACK);CHECK(s_ui_screen==DURA_UI_INFO_START);
        s_ui_screen=DURA_UI_INFO_HELP;s_ui_info_page=i;press(DURA_BUTTON_UP);CHECK(s_ui_screen==DURA_UI_MAIN_MENU);off();
    }
}
static void info_entry(void) {
    const int parents[]={DURA_UI_INFO_START,DURA_UI_COMPANY,DURA_UI_SWV,DURA_UI_BATTERY};
    for(unsigned i=0;i<sizeof(parents)/sizeof(*parents);i++) {
        s_ui_screen=parents[i];s_ui_info_page=7;refresh();CHECK(!strcmp(labels[2],"Help"));
        press(DURA_BUTTON_SELECT);CHECK(s_ui_screen==DURA_UI_INFO_HELP && s_ui_info_page==0);
        info_frame(0);off();
    }
}
static void reset_help(int item) {
    s_ui_screen=DURA_UI_RESET_TOTALS;s_ui_reset_item=item;
    dura_meter_snapshot_t before=snap();press(DURA_BUTTON_SELECT);refresh();
    char name[32];snprintf(name,sizeof(name),"reset%d-help",item);
    frame_is(name,reference_reset[item],"Back",NULL,NULL,NULL);
    press(DURA_BUTTON_UP);CHECK(s_ui_screen==DURA_UI_RESET_TOTALS && s_ui_reset_item==item);no_side_effects(before);
}
static void held_calibration(void) {
    for(int unit=DURA_UNITS_GALLON;unit<=DURA_UNITS_LITER;unit++) {
        CHECK(dura_meter_set_units(unit)==ESP_OK);s_ui_screen=DURA_UI_CAL_START;
        press(DURA_BUTTON_DOWN);CHECK(snap().calibration_mode==DURA_CAL_RUNNING);
        CHECK(dura_meter_record_pulse(100)==ESP_OK);press(DURA_BUTTON_DOWN);
        for(int page=0;page<4;page++) {
            refresh();CHECK(snap().calibration_counts==100);CHECK(snap().pump_enabled);
            CHECK(!strcmp(labels[0],"Return")&&!strcmp(labels[3],"Cal"));
            press(DURA_BUTTON_DOWN);
        }
        CHECK(s_ui_help_page==3);press(DURA_BUTTON_UP);refresh();
        CHECK(snap().calibration_counts==100 && snap().calibration_mode==DURA_CAL_RUNNING);
        press(DURA_BUTTON_DOWN);CHECK(s_ui_help_page==0);
        CHECK(dura_meter_record_pulse(25)==ESP_OK);press(DURA_BUTTON_BACK);refresh();
        CHECK(snap().calibration_counts==125 && snap().calibration_mode==DURA_CAL_RUNNING);
        press(DURA_BUTTON_UP);off();
    }
}
static void safety(void) {
    CHECK(dura_meter_start_batch(10)==ESP_OK);
    s_ui_screen=DURA_UI_SETUP;s_ui_setup_item=48;press(DURA_BUTTON_SELECT);refresh();
    CHECK(s_ui_screen==DURA_UI_AUTO_BATCH_RUN || s_ui_screen==DURA_UI_BATCH_RUN);
    CHECK(snap().batch_mode==DURA_BATCH_RUNNING);
    CHECK(dura_meter_cancel_batch()==ESP_OK);off();
    CHECK(dura_meter_start_calibration()==ESP_OK);
    s_ui_screen=DURA_UI_INFO_START;press(DURA_BUTTON_DOWN);refresh();
    CHECK(s_ui_screen==DURA_UI_BUCKET_GAL);CHECK(snap().calibration_mode==DURA_CAL_RUNNING);
    CHECK(dura_meter_cancel_calibration()==ESP_OK);off();
}
static void blank_buttons(void) {
    for(int i=0;i<8;i++) {
        s_ui_screen=DURA_UI_INFO_HELP;s_ui_info_page=i;press(DURA_BUTTON_DOWN);
        CHECK(s_ui_screen==DURA_UI_INFO_HELP && s_ui_info_page==i);
    }
    const int items[]={48,50,53};
    for(unsigned i=0;i<3;i++) {
        s_ui_screen=DURA_UI_SETUP;s_ui_setup_item=items[i];press(DURA_BUTTON_DOWN);
        CHECK(s_ui_screen==DURA_UI_SETUP && s_ui_setup_item==items[i]);
        CHECK(!ui_button_is_repeatable(DURA_BUTTON_SELECT));
    }
}
static void wire_ids(void) {
    uint16_t screen,menu;
    const int parents[]={48,50,53}, help_menus[]={50,52,54};
    for(unsigned i=0;i<3;i++) {
        s_ui_screen=DURA_UI_SETUP;s_ui_setup_item=parents[i];press(DURA_BUTTON_SELECT);
        ui_legacy_ids(&screen,&menu);CHECK(screen==DURA_UI_LEGACY_SCREEN_EDIT && menu==help_menus[i]);
    }
    s_ui_screen=DURA_UI_INFO_START;press(DURA_BUTTON_DOWN);
    ui_legacy_ids(&screen,&menu);CHECK(screen==DURA_UI_LEGACY_SCREEN_SCAN && menu==DURA_UI_LEGACY_MENU_INFO_START);
}
int main(int argc,char **argv) {
    CHECK(argc==2);fresh();s_provisioning_qr_dismissed=true;
    const char *name=argv[1];
    if(!strcmp(name,"setup-traversal"))setup_traversal();
    else if(!strncmp(name,"setup",5))setup_help(atoi(name+5));
    else if(!strcmp(name,"scan"))scan();
    else if(!strcmp(name,"info-navigation"))info_navigation();
    else if(!strcmp(name,"info-entry"))info_entry();
    else if(!strncmp(name,"info",4))info_frame(atoi(name+4));
    else if(!strncmp(name,"reset",5))reset_help(atoi(name+5));
    else if(!strcmp(name,"held-calibration"))held_calibration();
    else if(!strcmp(name,"safety"))safety();
    else if(!strcmp(name,"wire-ids"))wire_ids();
    else if(!strcmp(name,"blank-buttons"))blank_buttons();
    else CHECK(false);
    printf("PASS %s\n",name);return 0;
}
