#pragma once

#include "dura_battery_math.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t dura_battery_init(void);
void dura_battery_poll(void);
dura_battery_measurement_t dura_battery_get_measurement(void);

#ifdef __cplusplus
}
#endif
