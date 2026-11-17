#include "dura_ui_behavior.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stddef.h>

bool dura_ui_battery_percent_visible(bool measurement_valid,
                                     uint8_t battery_percent,
                                     uint64_t elapsed_ms,
                                     uint8_t low_battery_threshold_percent,
                                     uint32_t blink_on_ms,
                                     uint32_t blink_off_ms)
{
    if (!measurement_valid || low_battery_threshold_percent == 0u ||
        battery_percent >= low_battery_threshold_percent) {
        return true;
    }
    if (blink_on_ms == 0u || blink_off_ms == 0u) {
        return true;
    }

    const uint64_t cycle_ms = (uint64_t)blink_on_ms + (uint64_t)blink_off_ms;
    return ((uint64_t)elapsed_ms % cycle_ms) < (uint64_t)blink_on_ms;
}

uint32_t dura_ui_button_repeat_interval_ms(uint32_t held_ms,
                                           uint8_t max_rate_x)
{
    if (held_ms == 0u || max_rate_x == 0u) {
        return UINT32_MAX;
    }

    const uint32_t capped_held_ms = held_ms > DURA_UI_HOLD_ACCELERATION_LIMIT_MS
                                        ? DURA_UI_HOLD_ACCELERATION_LIMIT_MS
                                        : held_ms;

    /* rate_hz = max_rate_x * held_ms / (1000 + held_ms)
     * interval_ms = ceil(1000 / rate_hz).  Use 64-bit arithmetic so the
     * UINT32_MAX hold-duration boundary remains defined. */
    const uint64_t interval_numerator = 1000ULL * (1000ULL + (uint64_t)capped_held_ms);
    const uint64_t interval_denominator = (uint64_t)max_rate_x * (uint64_t)capped_held_ms;
    uint64_t interval_ms = (interval_numerator + interval_denominator - 1ULL) /
                           interval_denominator;
    if (interval_ms == 0u) {
        interval_ms = 1u;
    }
    return interval_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)interval_ms;
}

float dura_ui_adjust_editable_amount(float amount, int direction, int decimals)
{
    if (direction != 1 && direction != -1) return amount;
    if (!isfinite(amount) || amount < 0.0f) amount = 0.0f;
    const double scale = decimals >= 2 ? 100.0 : decimals == 1 ? 10.0 : 1.0;
    double counts = round((double)amount * scale) + direction;
    if (counts < 0.0) counts = 0.0;
    if (counts > 9999999.0 * scale) counts = 9999999.0 * scale;
    return (float)(counts / scale);
}

float dura_ui_held_amount(float anchor, int direction, uint32_t held_ms,
                          int decimals, const dura_ui_hold_ramp_t *ramp)
{
    if ((direction != 1 && direction != -1) || ramp == NULL ||
        ramp->start_rate_centi == 0u || ramp->max_rate_centi == 0u ||
        ramp->doubling_ms == 0u || held_ms <= ramp->delay_ms) return anchor;
    if (!isfinite(anchor) || anchor < 0.0f) anchor = 0.0f;
    const double start = (double)ramp->start_rate_centi / 100.0;
    const double cap = (double)ramp->max_rate_centi / 100.0;
    const double t = (double)(held_ms - ramp->delay_ms) / 1000.0;
    double movement;
    if (start >= cap) {
        movement = cap * t;
    } else {
        const double k = log(2.0) * 1000.0 / (double)ramp->doubling_ms;
        const double cap_time = log(cap / start) / k;
        /* Clamp before evaluating exp: arbitrarily long holds cannot overflow
         * the exponential. The remainder is a constant-speed tail. */
        const double ramp_time = t < cap_time ? t : cap_time;
        movement = start * expm1(k * ramp_time) / k;
        if (t > cap_time) movement += cap * (t - cap_time);
    }
    const double scale = decimals >= 2 ? 100.0 : decimals == 1 ? 10.0 : 1.0;
    const double whole_counts = floor(movement * scale);
    double counts = round((double)anchor * scale) + direction * whole_counts;
    if (counts < 0.0) counts = 0.0;
    if (counts > 9999999.0 * scale) counts = 9999999.0 * scale;
    return (float)(counts / scale);
}
