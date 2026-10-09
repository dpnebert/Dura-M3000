#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DURA_UI_HOLD_ACCELERATION_LIMIT_MS 10000u

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

#ifdef __cplusplus
}
#endif
