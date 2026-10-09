/* Real UI/render/meter behavior. Reuse hardware-only stubs from safety suite. */
#define main safety_fixture_main
#include "safe_output.c"
#undef main
#include "volume_config.h"
static bool reject_cancel;
static int cancel_for_ui(void) {
    return reject_cancel ? ESP_ERR_INVALID_STATE : dura_meter_cancel_batch();
}
#define dura_meter_cancel_batch cancel_for_ui
#include "board_home_volume.inc"
#undef dura_meter_cancel_batch

/* Expected defaults are requirements, not fallback definitions fed to production. */
#ifndef CONFIG_DURA_BOARD_VOLUME_GALLON_DECIMALS
#define EXPECT_GAL 2
#else
#define EXPECT_GAL CONFIG_DURA_BOARD_VOLUME_GALLON_DECIMALS
#endif
#ifndef CONFIG_DURA_BOARD_VOLUME_LITER_DECIMALS
#define EXPECT_LITER 2
#else
#define EXPECT_LITER CONFIG_DURA_BOARD_VOLUME_LITER_DECIMALS
#endif
#ifndef CONFIG_DURA_BOARD_VOLUME_OUNCE_DECIMALS
#define EXPECT_OUNCE 2
#else
#define EXPECT_OUNCE CONFIG_DURA_BOARD_VOLUME_OUNCE_DECIMALS
#endif
#ifndef CONFIG_DURA_BOARD_VOLUME_2DP_LIMIT_TENTHS
#define EXPECT_CUTOFF 1999
#else
#define EXPECT_CUTOFF CONFIG_DURA_BOARD_VOLUME_2DP_LIMIT_TENTHS
#endif

static void refresh(void) {
    memset(labels,0,sizeof(labels));
    CHECK(dura_board_render_debug_screen()==ESP_OK);
}
static void press(dura_button_t b) {dura_board_handle_button(b);}
static void label_is(int slot,const char *label) {CHECK(!strcmp(labels[slot],label));}
static void main_menu(void) {
    CHECK(s_ui_screen==DURA_UI_MAIN_MENU);
    dura_meter_snapshot_t m=snap();
    CHECK(m.operation_mode==DURA_OPERATION_IDLE && !m.pump_enabled && !m.recipe_owner_id);
    CHECK(m.batch_mode!=DURA_BATCH_RUNNING && m.batch_mode!=DURA_BATCH_PAUSED && m.batch_mode!=DURA_BATCH_DONE);
    off();
    /* Reconciliation may not drag Home back onto Run/Complete. */
    for(int i=0;i<3;i++){refresh();CHECK(s_ui_screen==DURA_UI_MAIN_MENU);off();}
    label_is(0,"Man");label_is(1,"Calibr");label_is(2,"Recirc");label_is(3,"Auto");
}
static void home_case(const char *name) {
    bool recirc=strstr(name,"recirc")!=NULL;
    if(strstr(name,"manual")) {
        press(DURA_BUTTON_UP);press(DURA_BUTTON_BACK);CHECK(gpio[0]&&gpio[2]);
    } else {
        press(recirc?DURA_BUTTON_SELECT:DURA_BUTTON_BACK);
        CHECK(s_ui_batch_amount==(recirc?250.0f:10.0f));
        press(DURA_BUTTON_BACK);
        CHECK(snap().batch_mode==DURA_BATCH_RUNNING);
        if(strstr(name,"paused"))press(DURA_BUTTON_SELECT);
        if(strstr(name,"done"))CHECK(dura_meter_record_pulse(snap().preset_batch_counts)==ESP_OK);
        if(strstr(name,"noauto"))CHECK(dura_meter_set_auto_batch(false)==ESP_OK);
        if(strstr(name,"flow_error"))CHECK(dura_meter_set_fault(7)==ESP_OK);
    }
    refresh();
    if(strstr(name,"flow_error")) {s_ui_screen=DURA_UI_FLOW_ERROR;render_popup_confirm("FLOW DETECTION","ERROR");}
    label_is(0,"Home");
    safe_required=true;
    if(strstr(name,"open"))fail_open=1;
    if(strstr(name,"write"))fail_write=1;
    if(strstr(name,"commit"))fail_commit=1;
    press(DURA_BUTTON_UP);
    off();main_menu();
    if(strstr(name,"flow_error"))CHECK(!snap().fault_latched);
}
static void all_home_labels(void) {
    /* Exhaustively dispatch every screen and setup page with an idle meter;
     * collect Home from actual renderer arguments, not screen-name guesses. */
    unsigned total=0;
    for(int screen=0;screen<DURA_UI_SCREEN_COUNT;screen++) {
        for(int item=0;item<(screen==DURA_UI_SETUP?54:1);item++) {
            s_ui_screen=screen;s_ui_setup_item=item;refresh();
            char saved[4][32];memcpy(saved,labels,sizeof(saved));
            for(int slot=0;slot<4;slot++)if(!strcmp(saved[slot],"Home")) {
                s_ui_screen=screen;s_ui_setup_item=item;
                press((dura_button_t)(slot+1));
                printf("HOME screen=%d setup=%d slot=%d destination=%d\n",screen,item,slot,s_ui_screen);
                main_menu();++total;
            }
        }
    }
    CHECK(total>20);
}
static void other_buttons(void) {
    s_ui_screen=DURA_UI_RESET_HELP;refresh();label_is(0,"Back");press(DURA_BUTTON_UP);
    CHECK(s_ui_screen==DURA_UI_RESET_TOTALS);
    s_ui_screen=DURA_UI_BATCH_SET;s_ui_batch_amount=99;refresh();label_is(0,"Reset");press(DURA_BUTTON_UP);
    CHECK(s_ui_screen==DURA_UI_BATCH_SET && s_ui_batch_amount==10);
    for(int item=3;item<=7;item+=4) {
        s_ui_screen=DURA_UI_SETUP;s_ui_setup_item=item;refresh();CHECK(strcmp(labels[0],"Home"));
        press(DURA_BUTTON_UP);CHECK(s_ui_screen==DURA_UI_SETUP);
    }
    ui_go_main();press(DURA_BUTTON_BACK);press(DURA_BUTTON_BACK);refresh();
    label_is(1,"");label_is(2,"Stop");label_is(3,"Start");
    press(DURA_BUTTON_DOWN);CHECK(snap().batch_mode==DURA_BATCH_RUNNING);
    press(DURA_BUTTON_SELECT);CHECK(snap().batch_mode==DURA_BATCH_PAUSED);off();
    press(DURA_BUTTON_BACK);CHECK(snap().batch_mode==DURA_BATCH_RUNNING);CHECK(gpio[0]&&gpio[2]);
    press(DURA_BUTTON_UP);main_menu();
}
static void cancellation_rejected(void) {
    press(DURA_BUTTON_BACK);press(DURA_BUTTON_BACK);refresh();
    int before=s_ui_screen;reject_cancel=true;press(DURA_BUTTON_UP);
    CHECK(s_ui_screen==before && snap().batch_mode==DURA_BATCH_RUNNING);
    reject_cancel=false;press(DURA_BUTTON_UP);main_menu();
}
static int expected_precision(float amount,int unit) {
    const int values[]={0,EXPECT_GAL,EXPECT_LITER,EXPECT_OUNCE};
    int p=values[unit];
    if(amount>(float)EXPECT_CUTOFF/10.0f && p>1)p=1;
    return p;
}
static void expected_text(char *text,float amount,int unit,bool editing) {
    int p=expected_precision(amount,unit);
    const double scales[]={1,10,100};
    double a=amount;
    if(!editing)a=trunc(a*scales[p])/scales[p];
    snprintf(text,32,"%.*f",p,a);
}
static void expect_amount(float amount,int unit,bool editing) {
    char expected[32];expected_text(expected,amount,unit,editing);
    if(strcmp(expected,rendered_amount)) {
        fprintf(stderr,"amount=%.9g unit=%d editing=%d actual=%s expected=%s\n",amount,unit,editing,rendered_amount,expected);
        CHECK(false);
    }
    CHECK(quantity_foreground>0);
}
static void quantity_screens(void) {
    const float cutoff=(float)EXPECT_CUTOFF/10.0f;
    const float amounts[]={0,1,5.099f,10,12.999f,99.9f,199.89f,
        nextafterf(cutoff,-INFINITY),cutoff,nextafterf(cutoff,INFINITY),199.99f,200,250,999.99f,9999.99f};
    for(int u=DURA_UNITS_GALLON;u<=DURA_UNITS_OUNCE;u++) {
        for(size_t i=0;i<sizeof(amounts)/sizeof(*amounts);i++) {
            dura_meter_snapshot_t m={.selected_units=u,.meter_total=amounts[i],.remaining_batch=amounts[i]};
            dura_meter_snapshot_t before=m;
            s_ui_batch_amount=amounts[i];
            render_manual_run(&m);expect_amount(amounts[i],u,false);
            render_batch_edit(&m,true);expect_amount(amounts[i],u,true);
            render_recirc_edit(&m);expect_amount(amounts[i],u,true);
            render_batch_run(&m,true);expect_amount(amounts[i],u,false);
            render_recirc_run(&m);expect_amount(amounts[i],u,false);
            CHECK(!memcmp(&before,&m,sizeof(m)));CHECK(s_ui_batch_amount==amounts[i]);
        }
        for(int recirc=0;recirc<2;recirc++) {
            CHECK(dura_meter_set_units(u)==ESP_OK);
            ui_go_main();press(recirc?DURA_BUTTON_SELECT:DURA_BUTTON_BACK);
            s_ui_batch_amount=12.5f;refresh();expect_amount(12.5f,u,true);
            press(DURA_BUTTON_BACK);refresh();
            CHECK(snap().remaining_batch==12.5f);expect_amount(12.5f,u,false);
            uint32_t counts=snap().preset_batch_counts;
            CHECK(dura_meter_record_pulse(1)==ESP_OK);refresh();
            CHECK(snap().preset_batch_counts==counts-1);expect_amount(snap().remaining_batch,u,false);
            press(DURA_BUTTON_SELECT);refresh();expect_amount(snap().remaining_batch,u,false);off();
            /* This matrix isolates precision from the separate Home contract. */
            ui_go_main();main_menu();
        }
    }
    CHECK(adjust_batch_amount_tenths(10,-1)==9.9f);
    CHECK(adjust_batch_amount_tenths(10,1)==10.1f);
    CHECK(adjust_batch_amount_tenths(1,-1)==1);
}
static bool fb_pixel(const uint8_t *fb,int x,int y) {
    int py=63-y;return (fb[(py/8)*128+x]>>(py%8))&1;
}
static void genuine_font_fit(void) {
    const float amounts[]={0,10,99.9f,199.89f,199.9f,199.91f,250,999.9f,9999.9f,99999.9f};
    for(int unit=1;unit<=3;unit++)for(int large=0;large<2;large++)for(size_t i=0;i<sizeof(amounts)/sizeof(*amounts);i++) {
        char text[32];format_legacy_batch_amount(text,sizeof(text),amounts[i],unit);
        dura_lcd_clear();
        if(large)draw_num40_right(128,12,text);else draw_num24_text(2,26,text);
        uint8_t numeric[1024];memcpy(numeric,s_fb,sizeof(numeric));
        /* Independent asset decode oracle, using the fixed production placement.
         * Deliberately keep x wide here so width/uint8 wrap limits stay visible. */
        const uint16_t *desc=large?dura_asset_arialNarrow40ptCharDescriptors:dura_asset_arialNarrow24ptCharDescriptors;
        const uint8_t *bits=large?dura_asset_arialNarrow40ptCharBitmaps:dura_asset_arialNarrow24ptCharBitmaps;
        int width=0;for(const char*p=text;*p;p++)width+=desc[(*p-'.')*2]+1;
        int x=large?(width<128?128-width:0):2,y=large?12:26,height=large?40:23;
        uint8_t expected[1024]={0};unsigned clipped=0;
        for(const char*p=text;*p;p++) {
            int index=(*p-'.')*2,w=desc[index],offset=desc[index+1],stride=(w+7)/8;
            for(int row=0;row<height;row++)for(int col=0;col<w;col++)if(bits[offset+row*stride+col/8]&(0x80>>(col%8))) {
                if(x+col>=128)++clipped;
                else {int py=63-y-row;expected[(py/8)*128+x+col]|=1<<(py%8);}
            }
            x+=w+1;
        }
        if(width<256)CHECK(!memcmp(numeric,expected,sizeof(numeric)));
        CHECK(clipped==clipped_foreground);
        unsigned erased=0;
        dura_meter_snapshot_t m={.selected_units=unit,.meter_total=amounts[i],.remaining_batch=amounts[i]};
        dura_lcd_clear();if(large)render_manual_run(&m);else render_batch_run(&m,true);
        for(int yy=0;yy<64;yy++)for(int xx=0;xx<128;xx++)if(fb_pixel(numeric,xx,yy)&&!fb_pixel(s_fb,xx,yy))++erased;
        printf("FIT unit=%d font=%s amount=%.9g text=%s width=%d ink=(%d,%d)-(%d,%d) clipped=%u erased_by_other_fields=%u\n",
               unit,large?"genuine40":"genuine24",amounts[i],text,width,qminx,qminy,qmaxx,qmaxy,clipped,erased);
        /* Default operating examples must fit horizontally; large limits are evidence. */
        if(amounts[i]<=250)CHECK(clipped==0);
    }
}
static void qr_failures(void) {
    for(int err=1;err<=2;err++) {
        qr_error=err;s_provisioning_qr_dismissed=false;s_ui_screen=DURA_UI_MAIN_MENU;
        refresh();label_is(0,"Home");press(DURA_BUTTON_UP);CHECK(s_provisioning_qr_dismissed);main_menu();
    }
}
int main(int argc,char **argv) {
    CHECK(argc==2);fresh();s_provisioning_qr_dismissed=true;
    const char *name=argv[1];
    if(!strcmp(name,"home_labels"))all_home_labels();
    else if(!strncmp(name,"home_",5))home_case(name);
    else if(!strcmp(name,"other_buttons"))other_buttons();
    else if(!strcmp(name,"cancel_rejected"))cancellation_rejected();
    else if(!strcmp(name,"volume"))quantity_screens();
    else if(!strcmp(name,"fit"))genuine_font_fit();
    else if(!strcmp(name,"qr"))qr_failures();
    else CHECK(false);
    printf("PASS %s\n",name);return 0;
}
