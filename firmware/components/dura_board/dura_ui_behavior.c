#include "dura_ui_behavior.h"

#include <limits.h>
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
