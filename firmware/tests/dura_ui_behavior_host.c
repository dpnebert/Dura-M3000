#include "dura_ui_behavior.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

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

    puts("DURA_UI_BEHAVIOR_HOST_PASS");
    return 0;
}
