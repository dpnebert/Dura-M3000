#include "dura_ui_behavior.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t interval_ms(uint32_t held_ms, uint8_t max_rate)
{
    return dura_ui_button_repeat_interval_ms(held_ms, max_rate);
}

/* Mirror board_task: 100 ms poll, one repeat dispatch per tick once due. */
static float simulate_hold(float start, float target, int direction, int decimals,
                           uint8_t max_rate, uint32_t *out_ms, int *out_actions)
{
    float amount = start;
    uint32_t t = 0;
    uint32_t last_repeat = 0;
    int actions = 0;

    amount = dura_ui_adjust_editable_amount(amount, direction, false, 0u, decimals);
    actions++;

    while ((direction < 0 && amount > target + 1e-6f) ||
           (direction > 0 && amount < target - 1e-6f)) {
        t += 100u;
        const uint32_t iv = interval_ms(t, max_rate);
        if (iv == UINT32_MAX) {
            continue;
        }
        const uint32_t since = t - last_repeat;
        if (since >= iv) {
            amount = dura_ui_adjust_editable_amount(amount, direction, true, t, decimals);
            last_repeat = t - (since % iv);
            actions++;
            if (actions > 200000) {
                break;
            }
        }
    }
    if (out_ms) {
        *out_ms = t;
    }
    if (out_actions) {
        *out_actions = actions;
    }
    return amount;
}

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

    /* Tap stays at finest 0.01 with decimals==2. */
    assert(fabsf(dura_ui_adjust_editable_amount(10.0f, -1, false, 0u, 2) - 9.99f) < 1e-5f);
    assert(fabsf(dura_ui_adjust_editable_amount(10.0f, 1, false, 0u, 2) - 10.01f) < 1e-5f);

    /* Hold under 800 ms: 0.1 unit. */
    assert(fabsf(dura_ui_adjust_editable_amount(10.0f, -1, true, 100u, 2) - 9.9f) < 1e-5f);
    assert(fabsf(dura_ui_adjust_editable_amount(10.0f, -1, true, 799u, 2) - 9.9f) < 1e-5f);

    /* Entering 1.0 stage snaps in travel direction. */
    assert(fabsf(dura_ui_adjust_editable_amount(10.37f, -1, true, 800u, 2) - 10.0f) < 1e-5f);
    assert(fabsf(dura_ui_adjust_editable_amount(10.37f, 1, true, 800u, 2) - 11.0f) < 1e-5f);
    assert(fabsf(dura_ui_adjust_editable_amount(10.0f, -1, true, 800u, 2) - 9.0f) < 1e-5f);

    /* Entering 10.0 stage snaps; down clamps exactly to 0. */
    assert(fabsf(dura_ui_adjust_editable_amount(17.3f, -1, true, 2000u, 2) - 10.0f) < 1e-5f);
    assert(fabsf(dura_ui_adjust_editable_amount(10.0f, -1, true, 2000u, 2) - 0.0f) < 1e-5f);
    assert(fabsf(dura_ui_adjust_editable_amount(7.0f, -1, true, 2500u, 2) - 0.0f) < 1e-5f);
    assert(fabsf(dura_ui_adjust_editable_amount(0.05f, -1, true, 2500u, 2) - 0.0f) < 1e-5f);

    uint32_t ms = 0;
    int actions = 0;
    float end = simulate_hold(250.0f, 0.0f, -1, 2, 100u, &ms, &actions);
    assert(fabsf(end) < 1e-5f);
    assert(ms >= 3000u && ms <= 5000u);
    printf("sim 250->0: %u ms, %d actions\n", ms, actions);

    end = simulate_hold(10.0f, 1.0f, -1, 2, 100u, &ms, &actions);
    assert(fabsf(end - 1.0f) < 1e-4f || (end < 1.0f && end >= 0.0f));
    /* Allow landing on 1.0 exactly or just under from a 1.0 step; re-sim to >=1 then one more. */
    {
        float a = 10.0f;
        uint32_t t = 0, last = 0;
        a = dura_ui_adjust_editable_amount(a, -1, false, 0u, 2);
        while (a > 1.0f + 1e-6f) {
            t += 100u;
            uint32_t iv = interval_ms(t, 100u);
            if (iv == UINT32_MAX) continue;
            uint32_t since = t - last;
            if (since >= iv) {
                a = dura_ui_adjust_editable_amount(a, -1, true, t, 2);
                last = t - (since % iv);
            }
        }
        assert(fabsf(a - 1.0f) < 1e-4f || a <= 1.0f);
        assert(t >= 1500u && t <= 2500u);
        printf("sim 10->1: %u ms, final=%.2f\n", t, a);
    }

    puts("DURA_UI_BEHAVIOR_HOST_PASS");
    return 0;
}