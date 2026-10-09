#include "dura_legacy_protocol.h"

#include <string.h>

static void put_u32_le(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

static void put_float_le(uint8_t *dst, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    put_u32_le(dst, bits);
}

static uint8_t complement_xor(const uint8_t *data, size_t first, size_t last)
{
    uint8_t xor_value = 0u;
    for (size_t i = first; i < last; ++i) xor_value ^= data[i];
    return (uint8_t)~xor_value;
}

esp_err_t dura_legacy_parse(const uint8_t *data, size_t len,
                            dura_legacy_action_t *action)
{
    if (data == NULL || action == NULL) return ESP_ERR_INVALID_ARG;
    if (len < 3u) return ESP_ERR_INVALID_SIZE;

    const size_t counted = data[0];
    if (counted + 2u != len) return ESP_ERR_INVALID_SIZE;

    size_t expected_len;
    switch (data[1]) {
    case DURA_LEGACY_COMMAND_CONNECT:
    case DURA_LEGACY_COMMAND_SCREEN_POLL:
        expected_len = 3u;
        break;
    case DURA_LEGACY_COMMAND_BUTTON:
        expected_len = 4u;
        break;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (len != expected_len || counted != expected_len - 2u) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (data[len - 1u] != complement_xor(data, 1u, len - 1u)) {
        return ESP_ERR_INVALID_CRC;
    }

    action->button_position = 0u;
    if (data[1] == DURA_LEGACY_COMMAND_CONNECT) {
        action->type = DURA_LEGACY_ACTION_CONNECT;
    } else if (data[1] == DURA_LEGACY_COMMAND_SCREEN_POLL) {
        action->type = DURA_LEGACY_ACTION_POLL;
    } else {
        const uint8_t button = data[2];
        if (button != DURA_LEGACY_BUTTON_POSITION_1 &&
            button != DURA_LEGACY_BUTTON_POSITION_2 &&
            button != DURA_LEGACY_BUTTON_POSITION_3 &&
            button != DURA_LEGACY_BUTTON_POSITION_4) {
            return ESP_ERR_INVALID_ARG;
        }
        action->type = DURA_LEGACY_ACTION_BUTTON;
        action->button_position = button;
    }
    return ESP_OK;
}

esp_err_t dura_legacy_serialize_snapshot(const dura_legacy_snapshot_t *snapshot,
                                         uint8_t *frame, size_t capacity,
                                         size_t *frame_len)
{
    if (snapshot == NULL || frame == NULL || frame_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *frame_len = 0u;
    if (snapshot->screen_id > UINT8_MAX || snapshot->menu_id > UINT8_MAX) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    size_t payload_len;
    switch (snapshot->screen_id) {
    case DURA_LEGACY_SCREEN_HOME:
        if (snapshot->menu_id != DURA_LEGACY_MENU_HOME) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        payload_len = 7u;
        break;
    case DURA_LEGACY_SCREEN_BATCH:
    case DURA_LEGACY_SCREEN_CALIBRATION:
        payload_len = 4u;
        break;
    case DURA_LEGACY_SCREEN_EDIT:
        if (snapshot->menu_id == DURA_LEGACY_MENU_BACKLIGHT_TIMEOUT ||
            snapshot->menu_id == DURA_LEGACY_MENU_SHOW_BATCH_TOTAL) {
            payload_len = 1u;
        } else if (snapshot->menu_id == DURA_LEGACY_MENU_CAL_AMOUNT ||
                   snapshot->menu_id == DURA_LEGACY_MENU_BATCH_EDIT) {
            payload_len = 4u;
        } else {
            return ESP_ERR_NOT_SUPPORTED;
        }
        break;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }

    const size_t len = 5u + payload_len;
    if (capacity < len) return ESP_ERR_INVALID_SIZE;

    frame[0] = (uint8_t)(len - 2u);
    frame[1] = DURA_LEGACY_COMMAND_NOTIFICATION;
    frame[2] = (uint8_t)snapshot->screen_id;
    frame[3] = (uint8_t)snapshot->menu_id;

    switch (snapshot->screen_id) {
    case DURA_LEGACY_SCREEN_HOME:
        put_float_le(&frame[4], snapshot->meter_total);
        /* These trailing bytes reproduce the live customer home frame. M3000
         * has no accepted battery-percentage contract yet. */
        frame[8] = 100u;
        frame[9] = snapshot->selected_units;
        frame[10] = 1u;
        break;
    case DURA_LEGACY_SCREEN_BATCH:
        put_float_le(&frame[4],
                     snapshot->menu_id == DURA_LEGACY_MENU_BATCH_EDIT
                         ? snapshot->batch_amount
                         : snapshot->remaining_batch);
        break;
    case DURA_LEGACY_SCREEN_CALIBRATION:
        put_u32_le(&frame[4], snapshot->calibration_counts);
        break;
    case DURA_LEGACY_SCREEN_EDIT:
        if (snapshot->menu_id == DURA_LEGACY_MENU_BACKLIGHT_TIMEOUT) {
            frame[4] = snapshot->backlight_timeout_sec;
        } else if (snapshot->menu_id == DURA_LEGACY_MENU_SHOW_BATCH_TOTAL) {
            frame[4] = snapshot->show_batch_total ? 1u : 0u;
        } else if (snapshot->menu_id == DURA_LEGACY_MENU_CAL_AMOUNT) {
            put_float_le(&frame[4], snapshot->calibration_measured_amount);
        } else {
            put_float_le(&frame[4], snapshot->batch_amount);
        }
        break;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }

    frame[len - 1u] = complement_xor(frame, 1u, len - 1u);
    *frame_len = len;
    return ESP_OK;
}
