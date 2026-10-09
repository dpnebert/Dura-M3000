#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dura_legacy_protocol.h"

#define EXPECT_ERR(vector, expected) do { \
    dura_legacy_action_t a = {0}; \
    assert(dura_legacy_parse((vector), sizeof(vector), &a) == (expected)); \
} while (0)

int main(void)
{
    const uint8_t connect[] = {0x01, 0x0a, 0xf5};
    const uint8_t poll[] = {0x01, 0x1c, 0xe3};
    const uint8_t b1[] = {0x02, 0x1e, 0x01, 0xe0};
    const uint8_t b2[] = {0x02, 0x1e, 0x02, 0xe3};
    const uint8_t b3[] = {0x02, 0x1e, 0x04, 0xe5};
    const uint8_t b4[] = {0x02, 0x1e, 0x08, 0xe9};
    const uint8_t bad_count[] = {0x02, 0x0a, 0xf5};
    const uint8_t extra[] = {0x02, 0x0a, 0x00, 0xf5};
    const uint8_t bad_checksum[] = {0x01, 0x1c, 0xe2};
    const uint8_t zero_button[] = {0x02, 0x1e, 0x00, 0xe1};
    const uint8_t multi_button[] = {0x02, 0x1e, 0x03, 0xe2};
    const uint8_t unknown[] = {0x01, 0x55, 0xaa};
    const uint8_t short_frame[] = {0x01, 0x0a};

    dura_legacy_action_t a = {0};
    assert(dura_legacy_parse(connect, sizeof(connect), &a) == ESP_OK && a.type == DURA_LEGACY_ACTION_CONNECT);
    assert(dura_legacy_parse(poll, sizeof(poll), &a) == ESP_OK && a.type == DURA_LEGACY_ACTION_POLL);
    const uint8_t *buttons[] = {b1, b2, b3, b4};
    const uint8_t values[] = {1, 2, 4, 8};
    for (size_t i = 0; i < 4; ++i) {
        assert(dura_legacy_parse(buttons[i], 4, &a) == ESP_OK);
        assert(a.type == DURA_LEGACY_ACTION_BUTTON && a.button_position == values[i]);
    }
    EXPECT_ERR(bad_count, ESP_ERR_INVALID_SIZE);
    EXPECT_ERR(extra, ESP_ERR_INVALID_SIZE);
    EXPECT_ERR(bad_checksum, ESP_ERR_INVALID_CRC);
    EXPECT_ERR(zero_button, ESP_ERR_INVALID_ARG);
    EXPECT_ERR(multi_button, ESP_ERR_INVALID_ARG);
    EXPECT_ERR(unknown, ESP_ERR_NOT_SUPPORTED);
    EXPECT_ERR(short_frame, ESP_ERR_INVALID_SIZE);
    assert(dura_legacy_parse(NULL, 0, &a) == ESP_ERR_INVALID_ARG);

    uint8_t frame[DURA_LEGACY_NOTIFICATION_MAX_LEN];
    size_t len = 99;

    dura_legacy_snapshot_t s = {
        .screen_id = DURA_LEGACY_SCREEN_HOME,
        .menu_id = DURA_LEGACY_MENU_HOME,
        .meter_total = 0.0f,
        .selected_units = 1u,
    };
    const uint8_t home_expected[] = {
        0x0a, 0x1d, 0x1e, 0x02, 0x00, 0x00, 0x00, 0x00,
        0x64, 0x01, 0x01, 0x9a
    };
    assert(dura_legacy_serialize_snapshot(&s, frame, sizeof(frame), &len) == ESP_OK);
    assert(len == sizeof(home_expected) && memcmp(frame, home_expected, len) == 0);
    assert(dura_legacy_serialize_snapshot(&s, frame, sizeof(home_expected) - 1u, &len) == ESP_ERR_INVALID_SIZE && len == 0);

    s = (dura_legacy_snapshot_t){
        .screen_id = DURA_LEGACY_SCREEN_EDIT,
        .menu_id = DURA_LEGACY_MENU_BACKLIGHT_TIMEOUT,
        .backlight_timeout_sec = 10u,
    };
    const uint8_t backlight_expected[] = {0x04, 0x1d, 0x00, 0x01, 0x0a, 0xe9};
    assert(dura_legacy_serialize_snapshot(&s, frame, sizeof(frame), &len) == ESP_OK);
    assert(len == sizeof(backlight_expected) && memcmp(frame, backlight_expected, len) == 0);

    s = (dura_legacy_snapshot_t){
        .screen_id = DURA_LEGACY_SCREEN_EDIT,
        .menu_id = DURA_LEGACY_MENU_SHOW_BATCH_TOTAL,
        .show_batch_total = false,
    };
    const uint8_t batch_total_expected[] = {0x04, 0x1d, 0x00, 0x02, 0x00, 0xe0};
    assert(dura_legacy_serialize_snapshot(&s, frame, sizeof(frame), &len) == ESP_OK);
    assert(len == sizeof(batch_total_expected) && memcmp(frame, batch_total_expected, len) == 0);

    s = (dura_legacy_snapshot_t){.screen_id = 0x5a, .menu_id = 0x30, .remaining_batch = 1.5f};
    const uint8_t batch_expected[] = {0x07, 0x1d, 0x5a, 0x30, 0x00, 0x00, 0xc0, 0x3f, 0x77};
    assert(dura_legacy_serialize_snapshot(&s, frame, sizeof(frame), &len) == ESP_OK);
    assert(len == sizeof(batch_expected) && memcmp(frame, batch_expected, len) == 0);

    s = (dura_legacy_snapshot_t){.screen_id = 0x50, .menu_id = 0x2c, .calibration_counts = 0x12345678};
    const uint8_t cal_expected[] = {0x07, 0x1d, 0x50, 0x2c, 0x78, 0x56, 0x34, 0x12, 0x96};
    assert(dura_legacy_serialize_snapshot(&s, frame, sizeof(frame), &len) == ESP_OK);
    assert(len == sizeof(cal_expected) && memcmp(frame, cal_expected, len) == 0);

    s = (dura_legacy_snapshot_t){.screen_id = 0x00, .menu_id = 0x38, .batch_amount = 10.0f};
    const uint8_t edit_expected[] = {0x07, 0x1d, 0x00, 0x38, 0x00, 0x00, 0x20, 0x41, 0xbb};
    assert(dura_legacy_serialize_snapshot(&s, frame, sizeof(frame), &len) == ESP_OK);
    assert(len == sizeof(edit_expected) && memcmp(frame, edit_expected, len) == 0);

    s = (dura_legacy_snapshot_t){.screen_id = 0x5a, .menu_id = 0x38,
                                 .batch_amount = 10.0f, .remaining_batch = 2.0f};
    const uint8_t batch_edit_expected[] = {0x07, 0x1d, 0x5a, 0x38, 0x00, 0x00, 0x20, 0x41, 0xe1};
    assert(dura_legacy_serialize_snapshot(&s, frame, sizeof(frame), &len) == ESP_OK);
    assert(len == sizeof(batch_edit_expected) && memcmp(frame, batch_edit_expected, len) == 0);

    s = (dura_legacy_snapshot_t){.screen_id = DURA_LEGACY_SCREEN_HOME, .menu_id = 0};
    assert(dura_legacy_serialize_snapshot(&s, frame, sizeof(frame), &len) == ESP_ERR_NOT_SUPPORTED && len == 0);
    s = (dura_legacy_snapshot_t){.screen_id = 0, .menu_id = 3};
    assert(dura_legacy_serialize_snapshot(&s, frame, sizeof(frame), &len) == ESP_ERR_NOT_SUPPORTED && len == 0);
    s = (dura_legacy_snapshot_t){.screen_id = 0x5a, .menu_id = 0x100};
    assert(dura_legacy_serialize_snapshot(&s, frame, sizeof(frame), &len) == ESP_ERR_NOT_SUPPORTED && len == 0);

    puts("DURA_LEGACY_PROTOCOL_TEST_PASS");
    return 0;
}
