/* M3000 fix cycle 2 (items 1, 2, 3): real board handlers/renderers, real meter,
 * genuine font assets and LCD pixel code. Only GPIO/NVS/clock are host seams
 * (safe_output.c). The generator adds coordinate/touch observers around the
 * production text and number draws; every element of a frame is re-drawn alone
 * to prove it lies on-screen, is intact in the final frame and does not overlap
 * another element. PGM evidence goes to the case cwd.
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

typedef struct{char orient;int x,y;char text[64];}tlog_t;
static tlog_t s_log[64];static int s_nlog;static bool s_log_off;
static void text_log(char orient,int x,int y,const char *text){
    if(s_log_off||!text)return;CHECK(s_nlog<64);
    s_log[s_nlog].orient=orient;s_log[s_nlog].x=x;s_log[s_nlog].y=y;
    snprintf(s_log[s_nlog].text,sizeof(s_log[s_nlog].text),"%s",text);s_nlog++;
}
typedef struct{int x0,y0,x1,y1;}box_t;
typedef struct{uint8_t fb[1024];int n;tlog_t log[64];int akind,ax,ay;char amount[32];box_t aink,atouch;unsigned aclip;}frame_t;
static frame_t F;
static bool fb_pixel(const uint8_t *fb,int x,int y){int py=63-y;return (fb[(py/8)*128+x]>>(py%8))&1;}
static void begin_render(void){s_nlog=0;rendered_text[0]=0;memset(labels,0,sizeof(labels));amt_kind=0;}
static void end_render(void){
    memcpy(F.fb,s_fb,sizeof(F.fb));F.n=s_nlog;memcpy(F.log,s_log,sizeof(s_log));F.akind=amt_kind;
    if(amt_kind){F.ax=amt_x;F.ay=amt_y;snprintf(F.amount,sizeof(F.amount),"%s",rendered_amount);
        F.aink=(box_t){qminx,qminy,qmaxx,qmaxy};F.atouch=(box_t){tminx,tminy,tmaxx,tmaxy};F.aclip=clipped_foreground;}
}
static void refresh(void){begin_render();CHECK(dura_board_render_debug_screen()==ESP_OK);end_render();}
static void press(dura_button_t b){dura_board_handle_button(b);}
static void label_is(int slot,const char *l){if(strcmp(labels[slot],l))fprintf(stderr,"slot %d label [%s] expected [%s]\n",slot,labels[slot],l);CHECK(!strcmp(labels[slot],l));}
static void labels4(const char *a,const char *b,const char *c,const char *d){label_is(0,a);label_is(1,b);label_is(2,c);label_is(3,d);}
static void pgm(const char *name){
    FILE *f=fopen(name,"wb");CHECK(f);CHECK(fputs("P5\n128 64\n255\n",f)>=0);
    for(int y=0;y<64;y++)for(int x=0;x<128;x++)CHECK(fputc(fb_pixel(F.fb,x,y)?0:0xff,f)!=EOF);
    CHECK(fclose(f)==0);
}
static const tlog_t *find_row(const char *text){for(int i=0;i<F.n;i++)if(!strcmp(F.log[i].text,text))return &F.log[i];return NULL;}
static const tlog_t *find_prefix(const char *p){for(int i=0;i<F.n;i++)if(!strncmp(F.log[i].text,p,strlen(p)))return &F.log[i];return NULL;}
static void row_at(const char *text,int y){const tlog_t *t=find_row(text);
    if(!t||t->y!=y){fprintf(stderr,"row [%s] y=%d missing; drawn:\n",text,y);for(int i=0;i<F.n;i++)fprintf(stderr,"  y=%d [%s]\n",F.log[i].y,F.log[i].text);}
    CHECK(t&&t->y==y);}
static void no_text(const char *s){if(strstr(rendered_text,s))fprintf(stderr,"unexpected [%s]\n",s);CHECK(!strstr(rendered_text,s));}
static bool overlap(box_t a,box_t b){return !(a.x1<b.x0||b.x1<a.x0||a.y1<b.y0||b.y1<a.y0);}
static void print_frame(void){
    for(int i=0;i<F.n;i++)printf("  ROW %c x=%d y=%d [%s]\n",F.log[i].orient,F.log[i].x,F.log[i].y,F.log[i].text);
    if(F.akind)printf("  AMOUNT num%d at (%d,%d) [%s] ink=(%d,%d)-(%d,%d) touch=(%d,%d)-(%d,%d)\n",F.akind,F.ax,F.ay,F.amount,
        F.aink.x0,F.aink.y0,F.aink.x1,F.aink.y1,F.atouch.x0,F.atouch.y0,F.atouch.x1,F.atouch.y1);
}
/* Re-draw one element alone; every ink pixel must be on in the final frame. */
static unsigned missing_after(void){unsigned miss=0;for(int y=0;y<64;y++)for(int x=0;x<128;x++)if(fb_pixel(s_fb,x,y)&&!fb_pixel(F.fb,x,y))miss++;return miss;}
static void frame_check(void){
    static const int x0[]={1,32,63,94};
    uint8_t save[1024];memcpy(save,s_fb,sizeof(save));
    box_t ink[64];bool seen_button=false;
    for(int i=0;i<F.n;i++){
        const tlog_t *t=&F.log[i];
        dura_lcd_clear();quantity_begin(t->text);s_log_off=true;const size_t rl=strlen(rendered_text);
        const int end=draw_legacy_text_aligned(t->orient,t->x,t->y,t->text,false);
        rendered_text[rl]=0;s_log_off=false;quantity_pixels=false;
        ink[i]=(box_t){qminx,qminy,qmaxx,qmaxy};const box_t touch={tminx,tminy,tmaxx,tmaxy};
        const unsigned miss=missing_after();
        /* x=128 may receive only the cleanup column's background write (dropped
         * by dura_lcd_set_pixel); foreground ink must never be clipped. */
        if(clipped_foreground||miss||touch.x0<0||touch.x1>128||touch.y0<0||touch.y1>63||end>128)
            {fprintf(stderr,"row [%s] y=%d clipped=%u missing=%u touch=(%d,%d)-(%d,%d) end=%d\n",t->text,t->y,clipped_foreground,miss,touch.x0,touch.y0,touch.x1,touch.y1,end);print_frame();CHECK(false);}
        CHECK(quantity_foreground>0);
        if(t->y==53){ /* softkey label: ink and cleanup inside its own rectangle */
            seen_button=true;int s=-1;for(int k=0;k<4;k++)if(t->x==x0[k]+15)s=k;CHECK(s>=0);
            CHECK(ink[i].x0>x0[s]&&ink[i].x1<x0[s]+30&&ink[i].y0>52&&ink[i].y1<63);
            CHECK(touch.x0>=x0[s]&&touch.x1<=x0[s]+30);
        }else{
            CHECK(!seen_button);             /* body text is drawn before softkeys */
            CHECK(ink[i].y1<=51&&touch.y1<=52);
        }
        for(int j=0;j<i;j++)if(overlap(ink[i],ink[j])){fprintf(stderr,"ink overlap [%s] / [%s]\n",t->text,F.log[j].text);print_frame();CHECK(false);}
        if(F.akind&&overlap(ink[i],F.atouch)){fprintf(stderr,"text [%s] overlaps amount\n",t->text);print_frame();CHECK(false);}
    }
    if(F.akind){
        dura_lcd_clear();
        if(F.akind==24)draw_num24_text((uint8_t)F.ax,(uint8_t)F.ay,F.amount);else draw_num40_right((uint8_t)F.ax,(uint8_t)F.ay,F.amount);
        const unsigned miss=missing_after();
        if(miss||F.aclip||F.atouch.x0<0||F.atouch.x1>127||F.atouch.y1>41){fprintf(stderr,"amount missing=%u clipped=%u\n",miss,F.aclip);print_frame();CHECK(false);}
    }
    for(int s=0;s<4;s++)if(labels[s][0]){
        for(int x=x0[s];x<=x0[s]+30;x++){CHECK(fb_pixel(F.fb,x,52));CHECK(fb_pixel(F.fb,x,63));}
        for(int y=52;y<=63;y++){CHECK(fb_pixel(F.fb,x0[s],y));CHECK(fb_pixel(F.fb,x0[s]+30,y));}
    }
    memcpy(s_fb,save,sizeof(save));
}
static const char *FLOW_UNIT[4]={"","GPM","LPM","OPM"};
static const char *UNIT_NAME[4]={"","Gallons","Liters","Ounces"};
static void fmt(char *s,size_t n,float v,int u){format_legacy_batch_amount(s,n,v,(dura_units_t)u);}
static void logo_check(bool expect_logo){
    uint8_t logo[1024];memcpy(logo,s_fb,sizeof(logo));dura_lcd_clear();
    dura_lcd_draw_bitmap_1bpp(0,0,dura_asset_durametertitle_bmp_glcd_bmp,128,8,true);
    bool same=true;for(int y=0;y<8;y++)for(int x=0;x<128;x++)if(fb_pixel(s_fb,x,y)!=fb_pixel(F.fb,x,y))same=false;
    memcpy(s_fb,logo,sizeof(logo));CHECK(same==expect_logo);
}
static void flow_row_check(bool on,float rate,int u){
    if(!on){no_text("Flow:");return;}
    float r=rate;if(!isfinite(r)||r<0)r=0;if(r>99999.9f)r=99999.9f;
    char e[40];snprintf(e,sizeof(e),"Flow: %.1f %s",(double)r,FLOW_UNIT[u]);row_at(e,42);
    CHECK(find_row(e)->orient=='L'&&find_row(e)->x==1);
}
/* ---- Item 2: Manual operating screen matrix (direct renderer) ---- */
static void manual_layout(void){
    const float amounts[]={0,.25f,12.46f,199.89f,250,9999.9f,123456.7f};
    const float rates[]={13.7f,NAN,-2,1e9f};
    const float totals[][2]={{1,3.5f},{15.62f,1280.4f},{123456.7f,9876543}};
    unsigned frames=0;
    for(int u=1;u<=3;u++)for(int bt=0;bt<2;bt++)for(int fl=0;fl<2;fl++)
    for(unsigned a=0;a<sizeof(amounts)/sizeof(*amounts);a++)for(unsigned r=0;r<4;r++)for(unsigned t=0;t<3;t++){
        if(!fl&&r)continue;if(!bt&&t)continue;
        dura_meter_snapshot_t m={0};m.selected_units=(dura_units_t)u;m.meter_total=amounts[a];m.show_batch_total=bt;
        m.show_flow_rate=fl;m.flow_rate=rates[r];m.batch_total=totals[t][0];m.total_total=totals[t][1];
        begin_render();dura_lcd_clear();render_manual_run(&m);end_render();frames++;
        labels4("Home","Reset","Stop","Start");frame_check();no_text("Press");no_text("Power");
        char e[40];fmt(e,sizeof(e),amounts[a],u);CHECK(F.akind==24&&!strcmp(F.amount,e));
        CHECK(F.atouch.x1<=127&&F.aink.x1>=120); /* right aligned */
        row_at(UNIT_NAME[u],42);CHECK(find_row(UNIT_NAME[u])->orient=='R');
        flow_row_check(fl,rates[r],u);
        logo_check(!bt);
        if(bt){
            const tlog_t *b=find_prefix("Batch: "),*tt=find_prefix("Total: ");CHECK(b&&tt&&b->y==0&&tt->y==0&&b->orient=='L'&&tt->orient=='R');
            if(t<2){char v[24],x[40];fmt(v,sizeof(v),totals[t][0],u);snprintf(x,sizeof(x),"Batch: %s",v);row_at(x,0);
                    fmt(v,sizeof(v),totals[t][1],u);snprintf(x,sizeof(x),"Total: %s",v);row_at(x,0);}
            CHECK(F.aink.y0>10);  /* below the header row */
        }else{no_text("Batch:");no_text("Total:");CHECK(F.aink.y0>=8);}
        if(u==1&&a==2&&r==0&&t==0){
            if(!bt&&fl){row_at("Flow: 13.7 GPM",42);print_frame();pgm("manual-logo-flow.pgm");}
            if(bt&&fl){row_at("Batch: 1.00",0);row_at("Total: 3.50",0);print_frame();pgm("manual-batchtotal-flow.pgm");}
            if(!bt&&!fl)pgm("manual-logo-noflow.pgm");
        }
        if(u==3&&a==6&&bt&&fl&&r==3&&t==2){print_frame();pgm("manual-worst-case.pgm");}
    }
    printf("MANUAL_FRAMES %u\n",frames);
}
static void seed_counts(void){
    CHECK(dura_meter_start_manual()==ESP_OK);CHECK(dura_meter_record_pulse(300)==ESP_OK);CHECK(dura_meter_stop_manual()==ESP_OK);
    CHECK(dura_meter_reset_batch_total()==ESP_OK);
    CHECK(dura_meter_start_manual()==ESP_OK);CHECK(dura_meter_record_pulse(200)==ESP_OK);CHECK(dura_meter_stop_manual()==ESP_OK);
    CHECK(dura_meter_reset_meter_total()==ESP_OK);
    CHECK(dura_meter_start_manual()==ESP_OK);CHECK(dura_meter_record_pulse(50)==ESP_OK);CHECK(dura_meter_stop_manual()==ESP_OK);
    CHECK(dura_board_sync_outputs()==ESP_OK);off();
    const dura_meter_snapshot_t s=snap();
    CHECK(s.meter_total_counts!=s.batch_total_counts&&s.batch_total_counts!=s.total_total_counts&&s.meter_total_counts>0);
}
static void same_counts(const dura_meter_snapshot_t *a,const dura_meter_snapshot_t *b){
    CHECK(a->meter_total_counts==b->meter_total_counts&&a->batch_total_counts==b->batch_total_counts);
    CHECK(a->total_total_counts==b->total_total_counts&&a->odometer_counts==b->odometer_counts);
}
/* ---- Item 2: Manual through the real handler ---- */
static void manual_ui_flow(void){
    CHECK(dura_meter_set_show_batch_total(true)==ESP_OK);CHECK(dura_meter_set_show_flow_rate(true)==ESP_OK);
    s_ui_screen=DURA_UI_MANUAL;refresh();labels4("Home","Reset","Stop","Start");frame_check();
    press(DURA_BUTTON_BACK);CHECK(snap().operation_mode==DURA_OPERATION_MANUAL&&snap().pump_enabled);
    CHECK(dura_meter_record_pulse(321)==ESP_OK);refresh();CHECK(s_ui_screen==DURA_UI_MANUAL);
    const dura_meter_snapshot_t m=snap();char e[24];fmt(e,sizeof(e),m.meter_total,m.selected_units);
    CHECK(!strcmp(F.amount,e)&&F.akind==24);CHECK(find_prefix("Batch: ")&&find_prefix("Total: ")&&find_prefix("Flow: "));
    frame_check();print_frame();pgm("manual-ui-running.pgm");
    press(DURA_BUTTON_SELECT);CHECK(snap().operation_mode==DURA_OPERATION_IDLE);CHECK(dura_board_sync_outputs()==ESP_OK);off();
    CHECK(dura_meter_set_show_batch_total(false)==ESP_OK);CHECK(dura_meter_set_show_flow_rate(false)==ESP_OK);
    refresh();logo_check(true);no_text("Flow:");row_at("Gallons",42);frame_check();
}
/* ---- Item 2: Auto and Recirc operating screens through the real handler ---- */
static void operating_screen_check(int u,bool fl,dura_ui_screen_t run){
    CHECK(s_ui_screen==run);labels4("Back","","Stop","Start");
    no_text("Press Start to begin.");no_text("Press Stop to end.");no_text("Batch:");no_text("Total:");
    const dura_meter_snapshot_t m=snap();char e[24];fmt(e,sizeof(e),m.remaining_batch,u);
    CHECK(F.akind&&!strcmp(F.amount,e));CHECK(F.aink.y0>=1&&F.aink.y1<=41);
    if(F.akind==24){CHECK(num40_text_width(e)>128);printf("NUM24_FALLBACK [%s]\n",e);}  /* too wide for 40 pt */
    row_at(UNIT_NAME[u],42);flow_row_check(fl,m.flow_rate,u);frame_check();
}
static void operating_layout(void){
    CHECK(dura_meter_set_show_batch_total(true)==ESP_OK); /* Batch/Total never appears on Auto/Recirc */
    CHECK(dura_meter_set_auto_batch(true)==ESP_OK);
    for(int recirc=0;recirc<2;recirc++)for(int u=1;u<=3;u++)for(int fl=0;fl<2;fl++){
        CHECK(dura_meter_set_units((dura_units_t)u)==ESP_OK);CHECK(dura_meter_set_show_flow_rate(fl)==ESP_OK);
        const dura_ui_screen_t edit=recirc?DURA_UI_RECIRC_EDIT:DURA_UI_AUTO_BATCH_EDIT,run=recirc?DURA_UI_RECIRC_RUN:DURA_UI_AUTO_BATCH_RUN;
        s_ui_screen=edit;refresh();press(DURA_BUTTON_BACK);
        CHECK(snap().batch_mode==DURA_BATCH_RUNNING);refresh();operating_screen_check(u,fl,run);
        if(u==1&&fl){print_frame();pgm(recirc?"recirc-running-flow.pgm":"auto-running-flow.pgm");}
        CHECK(dura_meter_record_pulse(37)==ESP_OK);refresh();operating_screen_check(u,fl,run);
        press(DURA_BUTTON_SELECT);CHECK(snap().batch_mode==DURA_BATCH_PAUSED);refresh();operating_screen_check(u,fl,run);
        if(u==1&&!fl)pgm(recirc?"recirc-paused.pgm":"auto-paused.pgm");
        press(DURA_BUTTON_BACK);CHECK(snap().batch_mode==DURA_BATCH_RUNNING);
        press(DURA_BUTTON_UP);CHECK(s_ui_screen==edit&&snap().batch_mode!=DURA_BATCH_RUNNING);
        CHECK(dura_board_sync_outputs()==ESP_OK);off();
    }
}
/* ---- Item 2: non-auto Batch run keeps its softkeys with the same layout ---- */
static void batch_run_layout(void){
    for(int u=1;u<=3;u++)for(int fl=0;fl<2;fl++){
        dura_meter_snapshot_t m={0};m.selected_units=(dura_units_t)u;m.remaining_batch=u==3?640:12.5f;m.show_flow_rate=fl;m.flow_rate=7.25f;m.show_batch_total=true;
        begin_render();dura_lcd_clear();render_batch_run(&m,false);end_render();
        labels4("Stop","","","Start");no_text("Press");no_text("Batch:");CHECK(F.akind==40);row_at(UNIT_NAME[u],42);flow_row_check(fl,7.25f,u);frame_check();
        begin_render();dura_lcd_clear();render_batch_run(&m,true);end_render();labels4("Back","","Stop","Start");frame_check();
        begin_render();dura_lcd_clear();render_recirc_run(&m);end_render();labels4("Back","","Stop","Start");frame_check();
        const float wide[]={32000,99999.9f,1234567.8f};
        for(int w=0;w<3;w++){m.remaining_batch=wide[w];begin_render();dura_lcd_clear();render_batch_run(&m,true);end_render();
            CHECK(F.akind==24&&F.aink.y0>=9&&F.aink.y1<=31);row_at(UNIT_NAME[u],42);frame_check();if(u==3&&w==0&&fl)pgm("auto-wide-ounces.pgm");}
        m.remaining_batch=999.9f;begin_render();dura_lcd_clear();render_batch_run(&m,true);end_render();CHECK(F.akind==40);frame_check();
    }
}
/* ---- Item 3: Batch/Total off -> Reset zeroes Amount directly (unchanged) ---- */
static void reset_direct_off(void){
    seed_counts();CHECK(!snap().show_batch_total);
    s_ui_screen=DURA_UI_MANUAL;refresh();const dura_meter_snapshot_t b=snap();
    press(DURA_BUTTON_DOWN);const dura_meter_snapshot_t a=snap();
    CHECK(s_ui_screen==DURA_UI_MANUAL&&a.meter_total_counts==0);
    CHECK(a.batch_total_counts==b.batch_total_counts&&a.total_total_counts==b.total_total_counts&&a.odometer_counts==b.odometer_counts);
    press(DURA_BUTTON_BACK);CHECK(dura_meter_record_pulse(40)==ESP_OK);press(DURA_BUTTON_DOWN);
    CHECK(snap().meter_total_counts==0&&snap().pump_enabled&&s_ui_screen==DURA_UI_MANUAL);
    press(DURA_BUTTON_SELECT);CHECK(dura_board_sync_outputs()==ESP_OK);off();
}
static const char *const TITLES[3]={"Amount Metered","Batch Metered","Total Metered"};
static void reset_page_check(int item){
    CHECK(s_ui_screen==DURA_UI_RESET_TOTALS&&s_ui_reset_item==item);
    refresh();labels4("Man","Reset","Help","Next");
    const dura_meter_snapshot_t m=snap();const float v=item==0?m.meter_total:item==1?m.batch_total:m.total_total;
    char a[24],e[48];fmt(a,sizeof(a),v,m.selected_units);snprintf(e,sizeof(e),"%s %s",a,UNIT_NAME[m.selected_units]);
    row_at(TITLES[item],2);row_at(e,15);row_at("Press 'Reset' to set to 0.00",30);
    CHECK(F.n==7);frame_check();
    for(int x=14;x<=113;x++){CHECK(fb_pixel(F.fb,x,13));CHECK(fb_pixel(F.fb,x,26));}
    for(int y=13;y<=26;y++){CHECK(fb_pixel(F.fb,14,y));CHECK(fb_pixel(F.fb,113,y));}
    /* value ink strictly inside the box */
    dura_lcd_clear();quantity_begin(e);s_log_off=true;draw_legacy_text_aligned('C',64,15,e,false);s_log_off=false;quantity_pixels=false;
    CHECK(qminx>14&&qmaxx<113&&qminy>13&&qmaxy<26);
}
/* ---- Item 3: Batch/Total on -> Reset opens the Metered pages; navigation never clears ---- */
static void reset_pages(void){
    CHECK(dura_meter_set_show_batch_total(true)==ESP_OK);seed_counts();
    for(int u=1;u<=3;u++){
        CHECK(dura_meter_set_units((dura_units_t)u)==ESP_OK);
        s_ui_screen=DURA_UI_MANUAL;s_ui_reset_item=2;refresh();CHECK(find_prefix("Batch: ")&&find_prefix("Total: "));
        const dura_meter_snapshot_t before=snap();
        press(DURA_BUTTON_DOWN);reset_page_check(0);
        if(u==1){print_frame();pgm("reset-amount.pgm");}
        for(int i=0;i<3;i++){
            reset_page_check(i);
            press(DURA_BUTTON_SELECT);CHECK(s_ui_screen==DURA_UI_RESET_HELP);refresh();label_is(0,"Back");frame_check();
            if(u==1){char f[32];snprintf(f,sizeof(f),"reset-help-%d.pgm",i);pgm(f);}
            press(DURA_BUTTON_UP);reset_page_check(i);
            if(u==1&&i){char f[32];snprintf(f,sizeof(f),"reset-%s.pgm",i==1?"batch":"total");pgm(f);}
            press(DURA_BUTTON_BACK);
            {const dura_meter_snapshot_t now=snap();same_counts(&now,&before);}
        }
        reset_page_check(0); /* Next wraps Total -> Amount */
        press(DURA_BUTTON_BACK);reset_page_check(1);
        press(DURA_BUTTON_UP);CHECK(s_ui_screen==DURA_UI_MANUAL); /* Man */
        refresh();CHECK(s_ui_screen==DURA_UI_MANUAL);labels4("Home","Reset","Stop","Start");
        const dura_meter_snapshot_t now=snap();same_counts(&now,&before);
        CHECK(now.operation_mode==DURA_OPERATION_IDLE&&!now.pump_enabled);CHECK(dura_board_sync_outputs()==ESP_OK);off();
    }
}
/* ---- Item 3: each page's Reset scope (accounting unchanged from dura_meter) ---- */
static void reset_scopes(void){
    CHECK(dura_meter_set_show_batch_total(true)==ESP_OK);
    for(int item=0;item<3;item++){
        seed_counts();s_ui_screen=DURA_UI_MANUAL;press(DURA_BUTTON_DOWN);
        for(int i=0;i<item;i++)press(DURA_BUTTON_BACK);
        const dura_meter_snapshot_t b=snap();press(DURA_BUTTON_DOWN);const dura_meter_snapshot_t a=snap();
        printf("SCOPE %s before M=%u B=%u T=%u O=%u after M=%u B=%u T=%u O=%u\n",TITLES[item],
            (unsigned)b.meter_total_counts,(unsigned)b.batch_total_counts,(unsigned)b.total_total_counts,(unsigned)b.odometer_counts,
            (unsigned)a.meter_total_counts,(unsigned)a.batch_total_counts,(unsigned)a.total_total_counts,(unsigned)a.odometer_counts);
        CHECK(a.meter_total_counts==0&&a.odometer_counts==b.odometer_counts);
        CHECK(a.batch_total_counts==(item==1?0:b.batch_total_counts));
        CHECK(a.total_total_counts==(item==2?0:b.total_total_counts));
        reset_page_check(item);
        char e[48],v[24];fmt(v,sizeof(v),0,1);snprintf(e,sizeof(e),"%s Gallons",v);
        if(item==0||item==1||item==2){const tlog_t *t=find_prefix(v);CHECK(t&&!strcmp(t->text,e));}
        press(DURA_BUTTON_UP);CHECK(s_ui_screen==DURA_UI_MANUAL);off();
    }
}
/* ---- Item 3: while Manual dispenses, Reset neither navigates nor clears ---- */
static void reset_while_running(void){
    CHECK(dura_meter_set_show_batch_total(true)==ESP_OK);seed_counts();
    s_ui_screen=DURA_UI_MANUAL;press(DURA_BUTTON_BACK);CHECK(dura_meter_record_pulse(100)==ESP_OK);
    const dura_meter_snapshot_t b=snap();CHECK(b.pump_enabled);
    press(DURA_BUTTON_DOWN);refresh();const dura_meter_snapshot_t a=snap();
    CHECK(s_ui_screen==DURA_UI_MANUAL&&a.pump_enabled&&a.operation_mode==DURA_OPERATION_MANUAL);same_counts(&a,&b);
    press(DURA_BUTTON_SELECT);CHECK(dura_board_sync_outputs()==ESP_OK);off();
    press(DURA_BUTTON_DOWN);CHECK(s_ui_screen==DURA_UI_RESET_TOTALS&&s_ui_reset_item==0); /* after Stop it opens */
}
/* ---- Item 1: Factory Reset page 8 wording + reachable Help, Back returns ---- */
static void factory_help_nav(void){
    CHECK(dura_meter_set_units(DURA_UNITS_LITER)==ESP_OK);CHECK(dura_meter_set_show_batch_total(true)==ESP_OK);seed_counts();
    const dura_meter_snapshot_t before=snap();
    s_ui_screen=DURA_UI_SETUP;s_ui_setup_item=8;refresh();
    row_at("Press 'Yes' to restore the",2);row_at("meter to the settings",12);
    row_at("programmed at the factory.",22);row_at("(Custom settings are replaced)",32);
    labels4("Home","Yes","Help","Next");frame_check();print_frame();pgm("factory-page8.pgm");
    for(int round=0;round<2;round++){
        press(DURA_BUTTON_SELECT);CHECK(s_ui_screen==DURA_UI_SETUP_HELP&&s_ui_setup_item==8);
        refresh();labels4("Back","","","");row_at("Factory Reset",2);row_at("Custom settings are replaced,",12);
        row_at("including saved calibration",22);row_at("values. Are you sure?",32);frame_check();
        if(!round)pgm("factory-help.pgm");
        press(DURA_BUTTON_DOWN);press(DURA_BUTTON_SELECT);press(DURA_BUTTON_BACK); /* blank keys inert */
        CHECK(s_ui_screen==DURA_UI_SETUP_HELP&&s_ui_setup_item==8);
        press(DURA_BUTTON_UP);CHECK(s_ui_screen==DURA_UI_SETUP&&s_ui_setup_item==8);
        refresh();labels4("Home","Yes","Help","Next");
        const dura_meter_snapshot_t now=snap();same_counts(&now,&before);
        CHECK(now.selected_units==before.selected_units&&now.show_batch_total==before.show_batch_total);
        CHECK(!memcmp(&now.working_coefficients,&before.working_coefficients,sizeof(now.working_coefficients)));
    }
    press(DURA_BUTTON_DOWN);CHECK(s_ui_setup_item==9);refresh();labels4("Home","Yes","","Home");
    press(DURA_BUTTON_UP);CHECK(s_ui_screen==DURA_UI_MAIN_MENU);
    {const dura_meter_snapshot_t now=snap();same_counts(&now,&before);CHECK(now.selected_units==DURA_UNITS_LITER);}
}
/* ---- PreCal softkeys "Fluid N" fit their 31 px buttons ---- */
static void precal_labels(void){
    s_ui_screen=DURA_UI_PRECAL_1;refresh();labels4("Water","Fluid 1","Fluid 2","Next");row_at("Current Water",22);frame_check();pgm("precal-1.pgm");
    s_ui_screen=DURA_UI_PRECAL_2;refresh();labels4("Fluid 3","Fluid 4","Fluid 5","Cancel");frame_check();pgm("precal-2.pgm");
    const dura_meter_snapshot_t m=snap();
    for(int page=0;page<2;page++){
        s_ui_screen=page?DURA_UI_CAL_SAVE_2:DURA_UI_CAL_SAVE_1;begin_render();dura_lcd_clear();render_precal(&m,(uint8_t)page);end_render();
        if(page)labels4("Fluid 3","Fluid 4","Fluid 5","Cancel");else labels4("Water","Fluid 1","Fluid 2","Next");frame_check();
    }
    no_text("Fluid1");no_text("Fluid5");
}
int main(int argc,char **argv){
    CHECK(argc==2);fresh();s_provisioning_qr_dismissed=true;const char *n=argv[1];
    if(!strcmp(n,"manual_layout"))manual_layout();
    else if(!strcmp(n,"manual_ui_flow"))manual_ui_flow();
    else if(!strcmp(n,"operating_layout"))operating_layout();
    else if(!strcmp(n,"batch_run_layout"))batch_run_layout();
    else if(!strcmp(n,"reset_direct_off"))reset_direct_off();
    else if(!strcmp(n,"reset_pages"))reset_pages();
    else if(!strcmp(n,"reset_scopes"))reset_scopes();
    else if(!strcmp(n,"reset_while_running"))reset_while_running();
    else if(!strcmp(n,"factory_help_nav"))factory_help_nav();
    else if(!strcmp(n,"precal_labels"))precal_labels();
    else CHECK(false);
    printf("PASS %s\n",n);return 0;
}
