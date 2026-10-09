#include "dura_battery_math.h"

#include <inttypes.h>
#include <stdio.h>

uint32_t dura_battery_apply_divider_mv(uint32_t adc_pad_mv)
{
    if (adc_pad_mv > UINT32_MAX / 2u) return UINT32_MAX;
    return adc_pad_mv * 2u;
}

uint8_t dura_battery_percent_from_mv(uint32_t battery_mv)
{
    if (battery_mv <= 1300u) return 0u;
    if (battery_mv >= 3200u) return 100u;
    const uint32_t linear_percent = ((battery_mv - 1300u) * 100u) / 1900u;
    return (uint8_t)linear_percent;
}

bool dura_battery_format_voltage(char *text, size_t text_len,
                                 const dura_battery_measurement_t *measurement)
{
    if (text == NULL || text_len == 0u || measurement == NULL) return false;
    int written;
    if (!measurement->valid) {
        written = snprintf(text, text_len, "Voltage: --.-- V");
    } else {
        const uint64_t hundredths = ((uint64_t)measurement->millivolts + 5u) / 10u;
        written = snprintf(text, text_len, "Voltage: %" PRIu64 ".%02" PRIu64 " V",
                           hundredths / 100u, hundredths % 100u);
    }
    return written >= 0 && (size_t)written < text_len;
}
