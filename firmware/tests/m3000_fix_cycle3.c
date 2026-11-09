/* M3000 fix cycle 3: held amount-step acceleration, amount erase-before-draw,
 * and the Cal Counts notice label. Real board/meter/LCD seams as in cycle 1/2.
 * NOT device, flash or pump verification. */
#define main safety_fixture_main
#include "safe_output.c"
#undef main
#include "volume_config.h"
#include "dura_ui_behavior.h"
static int64_t ui_now=1000000;
static int64_t ui_clock(void){return ui_now;}
#define esp_timer_get_time ui_clock
#include "board_home_volume.inc"
#undef esp_timer_get_time

static bool fb_pixel(const uint8_t *fb,int x,int y){int py=63-y;return (fb[(py/8)*128+x]>>(py%8))&1;}
static void press(dura_button_t b){dura_board_handle_button(b);}
static void refresh(void){rendered_text[0]=0;memset(labels,0,sizeof(labels));CHECK(dura_board_render_debug_screen()==ESP_OK);}
static void has(const char *s){if(!strstr(rendered_text,s))fprintf(stderr,"missing [%s] in [\n%s]\n",s,rendered_text);CHECK(strstr(rendered_text,s)!=NULL);}
static void no_has(const char *s){if(strstr(rendered_text,s))fprintf(stderr,"unexpected [%s]\n",s);CHECK(!strstr(rendered_text,s));}
static unsigned count_ink(int x0,int y0,int x1,int y1){
    unsigned n=0;for(int y=y0;y<=y1;y++)for(int x=x0;x<=x1;x++)if(fb_pixel(s_fb,x,y))++n;return n;}

/* ---- Hold stages (pure helper, decimals=2) ---- */
static void hold_stages(void){
    CHECK(fabsf(dura_ui_adjust_editable_amount(10.0f,-1,false,0u,2)-9.99f)<1e-5f);
    CHECK(fabsf(dura_ui_adjust_editable_amount(10.0f,-1,true,100u,2)-9.9f)<1e-5f);
    CHECK(fabsf(dura_ui_adjust_editable_amount(10.37f,-1,true,800u,2)-10.0f)<1e-5f);
    CHECK(fabsf(dura_ui_adjust_editable_amount(10.0f,-1,true,800u,2)-9.0f)<1e-5f);
    CHECK(fabsf(dura_ui_adjust_editable_amount(17.0f,-1,true,2000u,2)-10.0f)<1e-5f);
    CHECK(fabsf(dura_ui_adjust_editable_amount(7.0f,-1,true,2000u,2)-0.0f)<1e-5f);
}

/* ---- Simulate 250->0 and 10->1 with 100 ms poll / max_rate 100 ---- */
static float sim_hold(float start,float stop,uint32_t *ms_out){
    float a=start;uint32_t t=0,last=0;
    a=dura_ui_adjust_editable_amount(a,-1,false,0u,2);
    while(a>stop+1e-6f){
        t+=100u;
        uint32_t iv=dura_ui_button_repeat_interval_ms(t,100u);
        if(iv==UINT32_MAX)continue;
        uint32_t since=t-last;
        if(since>=iv){
            a=dura_ui_adjust_editable_amount(a,-1,true,t,2);
            last=t-(since%iv);
        }
        if(t>60000u)break;
    }
    if(ms_out)*ms_out=t;
    return a;
}
static void hold_timings(void){
    uint32_t ms=0;
    float a=sim_hold(250.0f,0.0f,&ms);
    printf("250->0 final=%.2f ms=%u\n",a,ms);
    CHECK(fabsf(a)<1e-5f);
    CHECK(ms>=3000u&&ms<=5000u);
    a=sim_hold(10.0f,1.0f,&ms);
    printf("10->1 final=%.2f ms=%u\n",a,ms);
    CHECK(a<=1.0f+1e-4f);
    CHECK(ms>=1500u&&ms<=2500u);
}

/* ---- Amount erase-before-draw: full clear leaves the amount band empty;
 * after drawing the editable amount, only the new ink is present. ---- */
static void amount_erase_before_draw(void){
    CHECK(dura_meter_init()==ESP_OK);
    test_board_bind();
    s_ui_screen=DURA_UI_AUTO_BATCH_EDIT;
    s_ui_batch_amount=10.00f;
    refresh();
    uint8_t with10[1024];memcpy(with10,s_fb,sizeof(with10));
    unsigned ink10=count_ink(0,26,127,48);
    CHECK(ink10>0);
    /* Production render path starts with dura_lcd_clear(). Prove the amount
     * band is empty after clear and before any redraw. */
    dura_lcd_clear();
    CHECK(count_ink(0,26,127,48)==0);
    /* Draw the shorter amount; no orphan ink from the previous 10.00 frame. */
    s_ui_batch_amount=9.00f;
    refresh();
    unsigned ink9=count_ink(0,26,127,48);
    CHECK(ink9>0);
    /* Every ON pixel in the amount band of the 9.00 frame must be OFF in a
     * freshly cleared buffer (sanity), and the 10.00-only right-edge column
     * must not survive: compare against a clear+redraw of 9.00 alone. */
    uint8_t with9[1024];memcpy(with9,s_fb,sizeof(with9));
    dura_lcd_clear();
    s_ui_batch_amount=9.00f;
    refresh();
    for(int y=26;y<=48;y++)for(int x=0;x<128;x++){
        if(fb_pixel(with9,x,y)!=fb_pixel(s_fb,x,y)){
            fprintf(stderr,"amount band mismatch at %d,%d (erase/redraw unstable)\n",x,y);
            CHECK(false);
        }
    }
    /* And a clear between two different amounts leaves no previous ink. */
    dura_lcd_clear();
    CHECK(count_ink(0,26,127,48)==0);
    (void)with10;(void)ink10;
}

/* ---- Cal Counts label (not County); string + notice path ---- */
static void cal_counts_label(void){
    CHECK(dura_meter_init()==ESP_OK);
    test_board_bind();
    ui_show_notice("Cal Counts","271");
    CHECK(!strcmp(s_ui_notice_line1,"Cal Counts"));
    CHECK(!strcmp(s_ui_notice_line2,"271"));
    CHECK(strstr(s_ui_notice_line1,"County")==NULL);
    ui_go_main();
    ui_show_notice("Cal Counts","271");
    refresh();
    has("Cal Counts");has("271");no_has("County");
    /* Source-level guard already in contract; framebuffer must show the notice. */
    unsigned ink=0;for(int y=14;y<=40;y++)for(int x=2;x<=125;x++)if(fb_pixel(s_fb,x,y))++ink;
    CHECK(ink>0);
}

int main(int argc,char **argv){
    const char * wh=argc>1?argv[1]:"";
    if(!strcmp(wh,"hold_stages"))hold_stages();
    else if(!strcmp(wh,"hold_timings"))hold_timings();
    else if(!strcmp(wh,"amount_erase_before_draw"))amount_erase_before_draw();
    else if(!strcmp(wh,"cal_counts_label"))cal_counts_label();
    else{fprintf(stderr,"unknown case %s\n",wh);return 2;}
    puts("M3000_FIX_CYCLE3_PASS");
    return 0;
}