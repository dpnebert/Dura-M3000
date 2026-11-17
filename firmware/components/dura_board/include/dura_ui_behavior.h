#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DURA_UI_HOLD_ACCELERATION_LIMIT_MS 10000u

/* Build-time settings for one selected volume unit. Centi = 0.01 units/s.
 * Runtime/NVS repeat settings do not override this quantity-speed policy. */
typedef struct {
    uint32_t start_rate_centi;
    uint32_t doubling_ms;
    uint32_t max_rate_centi;
    uint32_t delay_ms;
} dura_ui_hold_ramp_t;

bool dura_ui_battery_percent_visible(bool measurement_valid,
                                     uint8_t battery_percent,
                                     uint64_t elapsed_ms,
                                     uint8_t low_battery_threshold_percent,
                                     uint32_t blink_on_ms,
                                     uint32_t blink_off_ms);

/* Retained repeat timing for non-batch editors (setup/calibration only). */
uint32_t dura_ui_button_repeat_interval_ms(uint32_t held_ms,
                                           uint8_t max_rate_x);

/* One fine tap, unchanged resolution and bounds [0, 9999999]. */
float dura_ui_adjust_editable_amount(float amount, int direction, int decimals);

/* Absolute quantity after a hold, anchored immediately after the initial tap.
 * For t=(held_ms-delay_ms)/1000 > 0:
 * speed(t) = min(max_rate, start_rate * 2^(t / doubling_seconds)).
 * Integrates that speed, then truncates movement to whole editable counts.
 * Absolute elapsed time avoids dropped repeats, accumulated rounding error,
 * and dependence on UI-loop cadence. No movement before the hold delay.
 * Invalid/zero settings cause no repeat; start > cap means constant cap speed.
 * The caller resets the anchor on each press and cancels on context changes. */
float dura_ui_held_amount(float anchor, int direction, uint32_t held_ms,
                          int decimals, const dura_ui_hold_ramp_t *ramp);

#ifdef __cplusplus
}
#endif
