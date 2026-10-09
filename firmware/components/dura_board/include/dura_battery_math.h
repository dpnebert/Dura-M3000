#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;
    uint32_t millivolts;
    uint8_t percent;
} dura_battery_measurement_t;

uint32_t dura_battery_apply_divider_mv(uint32_t adc_pad_mv);
uint8_t dura_battery_percent_from_mv(uint32_t battery_mv);
bool dura_battery_format_voltage(char *text, size_t text_len,
                                 const dura_battery_measurement_t *measurement);

#ifdef __cplusplus
}
#endif
