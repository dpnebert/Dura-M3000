#include "dura_board.h"
#include "dura_legacy_protocol.h"
#include "gama_blefob.h"

#include "esp_log.h"

static const char *TAG = "dura_legacy_ble";

static esp_err_t notify_snapshot(const dura_board_ui_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const dura_legacy_snapshot_t legacy = {
        .screen_id = snapshot->legacy_screen_id,
        .menu_id = snapshot->legacy_menu_id,
        .calibration_counts = snapshot->calibration_counts,
        .meter_total = snapshot->meter_total,
        .batch_total = snapshot->batch_total,
        .total_total = snapshot->total_total,
        .remaining_batch = snapshot->remaining_batch,
        .batch_amount = snapshot->batch_amount,
        .calibration_measured_amount = snapshot->calibration_measured_amount,
        .selected_units = snapshot->selected_units,
        .selected_language = snapshot->selected_language,
        .meter_timeout_sec = snapshot->meter_timeout_sec,
        .backlight_timeout_sec = snapshot->backlight_timeout_sec,
        .show_flow_rate = snapshot->show_flow_rate,
        .show_auto_batch = snapshot->show_auto_batch,
        .show_batch_total = snapshot->show_batch_total,
        .oval_gear_meter = snapshot->oval_gear_meter,
    };
    uint8_t frame[DURA_LEGACY_NOTIFICATION_MAX_LEN];
    size_t frame_len = 0;
    esp_err_t err = dura_legacy_serialize_snapshot(&legacy, frame, sizeof(frame), &frame_len);
    if (err != ESP_OK) {
        return err;
    }
    return gama_blefob_notify_binary(frame, frame_len);
}

void dura_legacy_ble_ui_change_handler(const dura_board_ui_snapshot_t *snapshot,
                                       void *context)
{
    (void)context;
    esp_err_t err = notify_snapshot(snapshot);
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "legacy UI notification failed 0x%x", err);
    }
}

esp_err_t dura_legacy_ble_binary_write_handler(const uint8_t *data, size_t size,
                                               void *user_ctx)
{
    (void)user_ctx;
    dura_legacy_action_t action;
    esp_err_t err = dura_legacy_parse(data, size, &action);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "legacy binary frame rejected len=%u: %s",
                 (unsigned)size, esp_err_to_name(err));
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, size, ESP_LOG_WARN);
        return err;
    }

    if (action.type == DURA_LEGACY_ACTION_BUTTON) {
        dura_button_t button;
        switch (action.button_position) {
        case DURA_LEGACY_BUTTON_POSITION_1: button = DURA_BUTTON_UP; break;
        case DURA_LEGACY_BUTTON_POSITION_2: button = DURA_BUTTON_DOWN; break;
        case DURA_LEGACY_BUTTON_POSITION_3: button = DURA_BUTTON_SELECT; break;
        case DURA_LEGACY_BUTTON_POSITION_4: button = DURA_BUTTON_BACK; break;
        default: return ESP_ERR_INVALID_ARG;
        }
        return dura_board_enqueue_button(button);
    }

    dura_board_ui_snapshot_t snapshot;
    err = dura_board_get_ui_snapshot(&snapshot);
    if (err != ESP_OK) {
        return err;
    }
    return notify_snapshot(&snapshot);
}
