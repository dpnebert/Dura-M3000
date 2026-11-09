#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DURA_UI_HOLD_ACCELERATION_LIMIT_MS 10000u

/* Held amount-edit step stages (selected volume unit). Taps use the finest
 * count (0.01 when decimals==2). Repeats use 0.1 / 1.0 / 10.0 by hold age. */
#define DURA_UI_HOLD_STEP_TENTHS_MS 800u
#define DURA_UI_HOLD_STEP_ONES_MS   2000u

/* Returns true when the percentage text should be drawn. elapsed_ms is measured
 * from entry into the low-battery state so every warning begins with its ON phase. */
bool dura_ui_battery_percent_visible(bool measurement_valid,
                                     uint8_t battery_percent,
                                     uint64_t elapsed_ms,
                                     uint8_t low_battery_threshold_percent,
                                     uint32_t blink_on_ms,
                                     uint32_t blink_off_ms);

/* The held-button rate follows max_rate_x * x/(1+x), where x is held seconds,
 * capped at DURA_UI_HOLD_ACCELERATION_LIMIT_MS. The initial edge is dispatched
 * separately; UINT32_MAX means no repeat yet. */
uint32_t dura_ui_button_repeat_interval_ms(uint32_t held_ms,
                                           uint8_t max_rate_x);

/* Adjust an editable batch amount. direction is +1 or -1. is_repeat false keeps
 * the finest step (1 count at the configured decimals). is_repeat true uses
 * 0.1 / 1.0 / 10.0 unit stages from held_ms, snaps to the stage grid when
 * entering the 1.0 or 10.0 stage, and clamps to [0, 9999999]. */
float dura_ui_adjust_editable_amount(float amount,
                                     int direction,
                                     bool is_repeat,
                                     uint32_t held_ms,
                                     int decimals);

#ifdef __cplusplus
}
#endif
