#include "dura_ui_behavior.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>

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

float dura_ui_adjust_editable_amount(float amount,
                                     int direction,
                                     bool is_repeat,
                                     uint32_t held_ms,
                                     int decimals)
{
    if (direction != 1 && direction != -1) {
        return amount;
    }
    if (!isfinite((double)amount) || amount < 0.0f) {
        amount = 0.0f;
    }
    if (decimals < 0) {
        decimals = 0;
    }
    if (decimals > 2) {
        decimals = 2;
    }

    const double scale = decimals == 2 ? 100.0 : decimals == 1 ? 10.0 : 1.0;
    double counts = round((double)amount * scale);
    const double max_counts = 9999999.0 * scale;

    int step_counts;
    if (!is_repeat) {
        step_counts = 1; /* finest: 0.01 when decimals==2 */
    } else {
        double unit_step;
        if (held_ms < DURA_UI_HOLD_STEP_TENTHS_MS) {
            unit_step = 0.1;
        } else if (held_ms < DURA_UI_HOLD_STEP_ONES_MS) {
            unit_step = 1.0;
        } else {
            unit_step = 10.0;
        }
        step_counts = (int)lround(unit_step * scale);
        if (step_counts < 1) {
            step_counts = 1;
        }

        /* Entering the 1.0 or 10.0 stage: snap onto the stage grid in the
         * direction of travel so the display lands on a clean multiple. */
        if (held_ms >= DURA_UI_HOLD_STEP_TENTHS_MS) {
            const long grid = (long)step_counts;
            const long cur = (long)counts;
            if (grid > 0 && (cur % grid) != 0L) {
                if (direction < 0) {
                    counts = floor((double)cur / (double)grid) * (double)grid;
                } else {
                    counts = ceil((double)cur / (double)grid) * (double)grid;
                }
                if (counts < 0.0) {
                    counts = 0.0;
                }
                if (counts > max_counts) {
                    counts = max_counts;
                }
                return (float)(counts / scale);
            }
        }
    }

    counts += (double)direction * (double)step_counts;
    if (counts < 0.0) {
        counts = 0.0;
    }
    if (counts > max_counts) {
        counts = max_counts;
    }
    return (float)(counts / scale);
}