/* Actual board press/hold/release branch and real meter, inert hardware/NVS.
 * Independent midpoint-integrated rate oracle (not the production integral). */
#define main safety_fixture_main
#include "safe_output.c"
#undef main
#include "volume_config.h"
static int64_t ui_now = 1000000;
static int64_t ui_clock(void) { return ui_now; }
#define esp_timer_get_time ui_clock
#include "board_home_volume.inc"
#undef esp_timer_get_time

static double oracle(unsigned ms, int unit) {
    const double starts[] = {CONFIG_DURA_BOARD_HOLD_GALLON_START_CENTI,
        CONFIG_DURA_BOARD_HOLD_LITER_START_CENTI, CONFIG_DURA_BOARD_HOLD_OUNCE_START_CENTI};
    const double caps[] = {CONFIG_DURA_BOARD_HOLD_GALLON_MAX_CENTI,
        CONFIG_DURA_BOARD_HOLD_LITER_MAX_CENTI, CONFIG_DURA_BOARD_HOLD_OUNCE_MAX_CENTI};
    const double doubling[] = {CONFIG_DURA_BOARD_HOLD_GALLON_DOUBLING_MS,
        CONFIG_DURA_BOARD_HOLD_LITER_DOUBLING_MS, CONFIG_DURA_BOARD_HOLD_OUNCE_DOUBLING_MS};
    double sum = 0;
    for (unsigned t = CONFIG_DURA_BOARD_HOLD_DELAY_MS; t < ms; ++t) {
        double rate = starts[unit] / 100.0 * pow(2.0, (t + 0.5 - CONFIG_DURA_BOARD_HOLD_DELAY_MS) / doubling[unit]);
        sum += fmin(caps[unit] / 100.0, rate) / 1000.0;
    }
    return sum;
}
static void sample(dura_button_t button, unsigned delta_ms) {
    ui_now += (int64_t)delta_ms * 1000;
    hardware_button_sample(button);
}
static void enter(int unit, int mode, float amount) {
    sample(DURA_BUTTON_NONE, 100);
    CHECK(dura_meter_set_units((dura_units_t)(unit+1)) == ESP_OK);
    ui_enter_batch_editor(mode != 0, (dura_units_t)(unit+1));
    s_ui_batch_amount = amount;
}
static float held(int unit, int mode, int dir, unsigned duration, unsigned period) {
    enter(unit, mode, 10000.0f);
    dura_button_t key = dir < 0 ? DURA_BUTTON_DOWN : DURA_BUTTON_SELECT;
    sample(key, 100);
    const float anchor = s_ui_batch_amount;
    unsigned t = 0;
    while (t < duration) {
        unsigned step = period < duration-t ? period : duration-t;
        sample(key, step); t += step;
    }
    double expected = anchor + dir * floor(oracle(duration,unit) * 100.0 + 1e-7) / 100.0;
    if (fabs(s_ui_batch_amount - expected) > 0.011) {
        fprintf(stderr,"rate unit=%d mode=%d dir=%d ms=%u poll=%u expected=%.5f got=%.5f\n",
            unit,mode,dir,duration,period,expected,(double)s_ui_batch_amount);
        CHECK(false);
    }
    const float final = s_ui_batch_amount;
    sample(DURA_BUTTON_NONE, 1000);
    CHECK(s_ui_batch_amount == final); /* no catch-up on release */
    CHECK(!snap().pump_enabled); off();
    return final;
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    CHECK(dura_meter_init() == ESP_OK); test_board_bind();
    if (!strcmp(argv[1],"rates")) {
        unsigned checks=0;
        const unsigned periods[]={10,100,333,1000};
        const unsigned times[]={100,400,800,1000,2000,5000,10000};
        for(int u=0;u<3;++u) for(int mode=0;mode<2;++mode) for(int dir=-1;dir<=1;dir+=2)
            for(unsigned p=0;p<4;++p) for(unsigned t=0;t<7;++t) {
                held(u,mode,dir,times[t],periods[p]); ++checks;
            }
        printf("ACTUAL_CALLER_RATE_CASES=%u\n",checks);
    } else if (!strcmp(argv[1],"runtime_override")) {
        for(int u=0;u<3;++u)for(int mode=0;mode<2;++mode)for(int dir=-1;dir<=1;dir+=2) {
            test_repeat_rate=1;held(u,mode,dir,10000,100);
            test_repeat_rate=100;held(u,mode,dir,10000,100);
            test_ui_config_fail=true;held(u,mode,dir,10000,100);
            test_ui_config_fail=false;
        }
    } else if (!strcmp(argv[1],"lifecycle")) {
        for(int u=0;u<3;++u) for(int mode=0;mode<2;++mode) {
            enter(u,mode,10.0f);
            sample(DURA_BUTTON_SELECT,100); CHECK(fabs(s_ui_batch_amount-10.01f)<1e-5);
            sample(DURA_BUTTON_SELECT,100); CHECK(fabs(s_ui_batch_amount-10.01f)<1e-5);
            sample(DURA_BUTTON_NONE,100); sample(DURA_BUTTON_DOWN,100);
            CHECK(fabs(s_ui_batch_amount-10.0f)<1e-5);
            sample(DURA_BUTTON_DOWN,10000);
            float end=s_ui_batch_amount;
            /* Direction reversal starts a fresh tap and delay, not the old ramp. */
            sample(DURA_BUTTON_SELECT,100); CHECK(fabs(s_ui_batch_amount-(end+0.01f))<1e-5);
            end=s_ui_batch_amount; sample(DURA_BUTTON_SELECT,100); CHECK(s_ui_batch_amount==end);
            sample(DURA_BUTTON_NONE,100); sample(DURA_BUTTON_SELECT,100);
            end=s_ui_batch_amount; sample(DURA_BUTTON_SELECT,100); CHECK(s_ui_batch_amount==end);
            /* Unit change cancels the current ramp until a fresh press. */
            CHECK(dura_meter_set_units((dura_units_t)((u+1)%3+1))==ESP_OK);
            ui_sync_batch_units((dura_units_t)((u+1)%3+1));
            end=s_ui_batch_amount;sample(DURA_BUTTON_SELECT,2000);CHECK(s_ui_batch_amount==end);
            sample(DURA_BUTTON_NONE,100);
            enter(u,mode,0.05f); sample(DURA_BUTTON_DOWN,100);sample(DURA_BUTTON_DOWN,60000);
            CHECK(s_ui_batch_amount==0.0f);
            /* Remote/navigation action cancels a held quantity gesture. */
            enter(u,mode,10.0f);sample(DURA_BUTTON_SELECT,100);sample(DURA_BUTTON_SELECT,5000);
            dura_board_handle_button(DURA_BUTTON_UP); /* Home */
            CHECK(s_ui_screen==DURA_UI_MAIN_MENU);
            dura_board_handle_button(mode?DURA_BUTTON_SELECT:DURA_BUTTON_BACK);
            end=s_ui_batch_amount;sample(DURA_BUTTON_SELECT,1000);CHECK(s_ui_batch_amount==end);
            sample(DURA_BUTTON_NONE,100);off();
        }
    } else return 2;
    puts("HELD_VOLUME_RAMP_PASS"); return 0;
}
