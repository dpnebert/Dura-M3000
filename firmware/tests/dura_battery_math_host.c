#include "dura_battery_math.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static void test_divider(void)
{
    CHECK(dura_battery_apply_divider_mv(1365u) == 2730u);
    CHECK(dura_battery_apply_divider_mv(0u) == 0u);
    CHECK(dura_battery_apply_divider_mv(UINT32_MAX) == UINT32_MAX);
    CHECK(dura_battery_apply_divider_mv((UINT32_MAX / 2u) + 1u) == UINT32_MAX);
}

static void test_percent(void)
{
    CHECK(dura_battery_percent_from_mv(0u) == 0u);
    CHECK(dura_battery_percent_from_mv(1299u) == 0u);
    CHECK(dura_battery_percent_from_mv(1300u) == 0u);
    for (uint32_t percent = 1u; percent <= 99u; ++percent) {
        const uint32_t boundary = 1300u + (percent * 19u);
        CHECK(dura_battery_percent_from_mv(boundary - 1u) == (uint8_t)(percent - 1u));
        CHECK(dura_battery_percent_from_mv(boundary) == (uint8_t)percent);
    }
    CHECK(dura_battery_percent_from_mv(3199u) == 99u);
    CHECK(dura_battery_percent_from_mv(3200u) == 100u);
    CHECK(dura_battery_percent_from_mv(3201u) == 100u);
    CHECK(dura_battery_percent_from_mv(UINT32_MAX) == 100u);
}

static void test_format(void)
{
    char text[32];
    dura_battery_measurement_t value = { .valid = false, .millivolts = 0u, .percent = 0u };
    CHECK(dura_battery_format_voltage(text, sizeof(text), &value));
    CHECK(strcmp(text, "Voltage: --.-- V") == 0);

    value.valid = true;
    value.millivolts = 2730u;
    value.percent = 100u;
    CHECK(dura_battery_format_voltage(text, sizeof(text), &value));
    CHECK(strcmp(text, "Voltage: 2.73 V") == 0);

    value.millivolts = 2735u;
    CHECK(dura_battery_format_voltage(text, sizeof(text), &value));
    CHECK(strcmp(text, "Voltage: 2.74 V") == 0);

    CHECK(!dura_battery_format_voltage(NULL, 0u, &value));
    CHECK(!dura_battery_format_voltage(text, 5u, &value));
    CHECK(!dura_battery_format_voltage(text, sizeof(text), NULL));
}

int main(void)
{
    test_divider();
    test_percent();
    test_format();
    if (failures != 0) return 1;
    puts("DURA_BATTERY_MATH_HOST_PASS");
    return 0;
}
