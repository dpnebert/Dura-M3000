#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Stable values recovered from the legacy Dura app protocol. */
#define DURA_LEGACY_COMMAND_CONNECT       0x0Au
#define DURA_LEGACY_COMMAND_SCREEN_POLL   0x1Cu
#define DURA_LEGACY_COMMAND_NOTIFICATION  0x1Du
#define DURA_LEGACY_COMMAND_BUTTON        0x1Eu

#define DURA_LEGACY_SCREEN_EDIT           0x00u
#define DURA_LEGACY_SCREEN_HOME           0x1Eu
#define DURA_LEGACY_SCREEN_CALIBRATION    0x50u
#define DURA_LEGACY_SCREEN_BATCH          0x5Au

#define DURA_LEGACY_MENU_HOME             0x02u
#define DURA_LEGACY_MENU_METER_TIMEOUT    0x00u
#define DURA_LEGACY_MENU_BACKLIGHT_TIMEOUT 0x01u
#define DURA_LEGACY_MENU_SHOW_BATCH_TOTAL 0x02u
#define DURA_LEGACY_MENU_CAL_AMOUNT       0x2Cu
#define DURA_LEGACY_MENU_BATCH_EDIT       0x38u

#define DURA_LEGACY_BUTTON_POSITION_1     0x01u
#define DURA_LEGACY_BUTTON_POSITION_2     0x02u
#define DURA_LEGACY_BUTTON_POSITION_3     0x04u
#define DURA_LEGACY_BUTTON_POSITION_4     0x08u

#define DURA_LEGACY_NOTIFICATION_MAX_LEN  12u

typedef enum {
    DURA_LEGACY_ACTION_CONNECT = 0,
    DURA_LEGACY_ACTION_POLL,
    DURA_LEGACY_ACTION_BUTTON,
} dura_legacy_action_type_t;

typedef struct {
    dura_legacy_action_type_t type;
    uint8_t button_position;
} dura_legacy_action_t;

typedef struct {
    uint16_t screen_id;
    uint16_t menu_id;
    uint32_t calibration_counts;
    float meter_total;
    float remaining_batch;
    float batch_amount;
    float calibration_measured_amount;
    uint8_t selected_units;
    uint8_t backlight_timeout_sec;
    bool show_batch_total;
} dura_legacy_snapshot_t;

/* Parse exactly one complete app write. byte[0] counts command + payload and
 * the final byte is ~(command XOR payload...). */
esp_err_t dura_legacy_parse(const uint8_t *data, size_t len,
                            dura_legacy_action_t *action);

/* Serialize only live-captured or recovered 0x1D layouts. byte[0] counts
 * command + payload and the final byte is ~(command XOR payload...). */
esp_err_t dura_legacy_serialize_snapshot(const dura_legacy_snapshot_t *snapshot,
                                         uint8_t *frame, size_t capacity,
                                         size_t *frame_len);

#ifdef __cplusplus
}
#endif
