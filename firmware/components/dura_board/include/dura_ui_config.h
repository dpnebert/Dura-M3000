#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DURA_UI_CONFIG_LOW_BATTERY_THRESHOLD_PERCENT_MAX 100u
#define DURA_UI_CONFIG_BLINK_MS_MIN 100u
#define DURA_UI_CONFIG_BLINK_MS_MAX 60000u
#define DURA_UI_CONFIG_BUTTON_HOLD_MAX_RATE_X_MIN 1u
#define DURA_UI_CONFIG_BUTTON_HOLD_MAX_RATE_X_MAX 100u

typedef struct {
    uint8_t low_battery_threshold_percent;
    uint8_t button_hold_max_rate_x;
    uint16_t reserved;
    uint32_t low_battery_blink_on_ms;
    uint32_t low_battery_blink_off_ms;
} dura_ui_runtime_config_t;

esp_err_t dura_ui_config_init(void);
esp_err_t dura_ui_config_get(dura_ui_runtime_config_t *config);
esp_err_t dura_ui_config_set(const dura_ui_runtime_config_t *config);
esp_err_t dura_ui_config_validate(const dura_ui_runtime_config_t *config);
esp_err_t dura_ui_config_format_json(char *response, size_t response_len);

#ifdef __cplusplus
}
#endif
