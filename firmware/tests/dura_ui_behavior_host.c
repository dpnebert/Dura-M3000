#include "dura_ui_behavior.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    /* The low-battery rule is strictly below the configured threshold. */
    assert(dura_ui_battery_percent_visible(false, 0u, 1000u, 20u, 1000u, 1000u));
    assert(dura_ui_battery_percent_visible(true, 20u, 1000u, 20u, 1000u, 1000u));
    assert(dura_ui_battery_percent_visible(true, 19u, 0u, 20u, 1000u, 1000u));
    assert(dura_ui_battery_percent_visible(true, 19u, 999u, 20u, 1000u, 1000u));
    assert(!dura_ui_battery_percent_visible(true, 19u, 1000u, 20u, 1000u, 1000u));
    assert(!dura_ui_battery_percent_visible(true, 19u, 1999u, 20u, 1000u, 1000u));
    assert(dura_ui_battery_percent_visible(true, 19u, 2000u, 20u, 1000u, 1000u));
    assert(dura_ui_battery_percent_visible(true, 0u, 1500u, 0u, 1000u, 1000u));

    /* Rate = max_rate * x/(1+x), with x measured in held seconds. */
    assert(dura_ui_button_repeat_interval_ms(0u, 10u) == UINT32_MAX);
    assert(dura_ui_button_repeat_interval_ms(1000u, 10u) == 200u);  /* 5.0 actions/s */
    assert(dura_ui_button_repeat_interval_ms(3000u, 10u) == 134u);  /* 7.5 actions/s */
    assert(dura_ui_button_repeat_interval_ms(9000u, 10u) == 112u);  /* 9.0 actions/s */
    assert(dura_ui_button_repeat_interval_ms(10000u, 10u) == 110u);
    assert(dura_ui_button_repeat_interval_ms(20000u, 10u) == 110u); /* capped at 10 s */
    assert(dura_ui_button_repeat_interval_ms(UINT32_MAX, 10u) == 110u);
    assert(dura_ui_button_repeat_interval_ms(1000u, 1u) == 2000u);  /* 0.5 actions/s */
    assert(dura_ui_button_repeat_interval_ms(1000u, 0u) == UINT32_MAX);

    assert(fabsf(dura_ui_adjust_editable_amount(10.0f, -1, 2) - 9.99f) < 1e-5f);
    assert(fabsf(dura_ui_adjust_editable_amount(10.0f, 1, 2) - 10.01f) < 1e-5f);
    assert(dura_ui_adjust_editable_amount(0.0f, -1, 2) == 0.0f);
    assert(dura_ui_adjust_editable_amount(10.0f, 0, 2) == 10.0f);
    const dura_ui_hold_ramp_t r = {100, 1000, 400, 400};
    assert(dura_ui_held_amount(0,1,400,2,&r) == 0);
    assert(fabsf(dura_ui_held_amount(0,1,1400,2,&r)-1.44f) < 1e-5f);
    assert(fabsf(dura_ui_held_amount(0,1,2400,2,&r)-4.32f) < 1e-5f);
    assert(fabsf(dura_ui_held_amount(0,1,3400,2,&r)-8.32f) < 1e-5f);
    assert(fabsf(dura_ui_held_amount(0,1,3400,1,&r)-8.3f) < 1e-5f);
    assert(dura_ui_held_amount(0,1,3400,0,&r) == 8);
    assert(dura_ui_held_amount(7,-1,3400,2,&r) == 0);
    assert(dura_ui_held_amount(9999999,1,UINT32_MAX,2,&r) == 9999999);
    assert(dura_ui_held_amount(10,-1,UINT32_MAX,2,&r) == 0);
    assert(dura_ui_held_amount(10,0,3400,2,&r) == 10);
    assert(dura_ui_held_amount(10,1,3400,2,NULL) == 10);
    dura_ui_hold_ramp_t invalid = r;
    invalid.doubling_ms=0; assert(dura_ui_held_amount(10,1,3400,2,&invalid)==10);
    invalid=r; invalid.start_rate_centi=0; assert(dura_ui_held_amount(10,1,3400,2,&invalid)==10);
    invalid=r; invalid.max_rate_centi=0; assert(dura_ui_held_amount(10,1,3400,2,&invalid)==10);
    invalid=r; invalid.start_rate_centi=800; assert(dura_ui_held_amount(0,1,1400,2,&invalid)==4);
    assert(isfinite(dura_ui_held_amount(NAN,1,1400,2,&r)));
    float previous=0;
    for (unsigned t=0;t<=30000;t+=17) {
        float next=dura_ui_held_amount(0,1,t,2,&r);
        assert(next>=previous); previous=next;
    }

    puts("DURA_UI_BEHAVIOR_HOST_PASS");
    return 0;
}