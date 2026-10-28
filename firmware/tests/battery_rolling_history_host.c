/* Black-box tests of the separately compiled, exact production battery module.
 * The oracle keeps all successful samples and resums the last 16 (not a ring).
 * IDF functions below simulate peripherals/time only. Conversion wrappers call
 * the real battery math; no production state, reset hook, or future symbol is used.
 */
#include "dura_battery.h"
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *scenario;
static unsigned step, divider_calls, percent_calls;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "ASSERT %s step=%u line=%d: %s\n", scenario, step, __LINE__, #condition); \
    exit(1); } } while (0)

uint32_t __real_dura_battery_apply_divider_mv(uint32_t pad);
uint8_t __real_dura_battery_percent_from_mv(uint32_t battery);
uint32_t __wrap_dura_battery_apply_divider_mv(uint32_t pad)
{
    ++divider_calls;
    return __real_dura_battery_apply_divider_mv(pad);
}
uint8_t __wrap_dura_battery_percent_from_mv(uint32_t battery)
{
    ++percent_calls;
    return __real_dura_battery_percent_from_mv(battery);
}

static void unavailable(void)
{
    const dura_battery_measurement_t m = dura_battery_get_measurement();
    CHECK(!m.valid);
    CHECK(m.millivolts == 0);
    CHECK(m.percent == 0);
}

#if CONFIG_DURA_BOARD_ENABLE_HARDWARE_TASK
#include "dura_board.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali_scheme.h"

static int64_t clock_us = 1234567;
static int64_t due_us;
static unsigned reads, calibrations, reads_this_poll, fail_read;
static unsigned init_calls, deleted_units;
static int requested_mv;
static bool fail_calibration;
static const char *init_fault = "";
static int unit_token, cali_token;
static uint32_t successes[4096];
static unsigned success_count;

const char *esp_err_to_name(esp_err_t err) { (void)err; return "injected failure"; }
void test_log(const char *tag, const char *format, ...)
{
    (void)tag;
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    putchar('\n');
    va_end(args);
}
int64_t esp_timer_get_time(void) { return clock_us; }

esp_err_t adc_oneshot_io_to_channel(int gpio, adc_unit_t *unit, adc_channel_t *channel)
{
    ++init_calls;
    CHECK(gpio == 1);
    *unit = strcmp(init_fault, "unit_route") == 0 ? ADC_UNIT_2 : ADC_UNIT_1;
    *channel = strcmp(init_fault, "channel_route") == 0 ? 1 : ADC_CHANNEL_0;
    return strcmp(init_fault, "io") == 0 ? ESP_FAIL : ESP_OK;
}
esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *cfg,
                               adc_oneshot_unit_handle_t *unit)
{
    ++init_calls;
    CHECK(cfg->unit_id == ADC_UNIT_1);
    CHECK(cfg->ulp_mode == ADC_ULP_MODE_DISABLE);
    if (strcmp(init_fault, "new_unit") == 0) return ESP_FAIL;
    *unit = &unit_token;
    return ESP_OK;
}
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t unit, adc_channel_t channel,
                                     const adc_oneshot_chan_cfg_t *cfg)
{
    ++init_calls;
    CHECK(unit == &unit_token && channel == ADC_CHANNEL_0);
    CHECK(cfg->atten == ADC_ATTEN_DB_12 && cfg->bitwidth == ADC_BITWIDTH_DEFAULT);
    return strcmp(init_fault, "channel") == 0 ? ESP_FAIL : ESP_OK;
}
esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t unit)
{
    CHECK(unit == &unit_token);
    ++deleted_units;
    return ESP_OK;
}
esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *cfg,
                                               adc_cali_handle_t *cali)
{
    ++init_calls;
    CHECK(cfg->unit_id == ADC_UNIT_1 && cfg->chan == ADC_CHANNEL_0);
    CHECK(cfg->atten == ADC_ATTEN_DB_12 && cfg->bitwidth == ADC_BITWIDTH_DEFAULT);
    if (strcmp(init_fault, "calibration") == 0) return ESP_FAIL;
    *cali = &cali_token;
    return ESP_OK;
}
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t unit, adc_channel_t channel, int *raw)
{
    CHECK(unit == &unit_token && channel == ADC_CHANNEL_0);
    ++reads;
    ++reads_this_poll;
    /* Priming values are deliberately different from the retained sample. */
    *raw = reads_this_poll <= 4 ? 4000 + (int)reads_this_poll : 1234;
    return reads_this_poll == fail_read ? ESP_FAIL : ESP_OK;
}
esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t cali, int raw, int *mv)
{
    CHECK(cali == &cali_token);
    CHECK(raw == 1234); /* Neither discard readings nor raw ADC counts are averaged. */
    ++calibrations;
    *mv = requested_mv;
    return fail_calibration ? ESP_FAIL : ESP_OK;
}

static void init_ok(void)
{
    unavailable();
    dura_battery_poll();
    CHECK(reads == 0 && calibrations == 0);
    CHECK(dura_battery_init() == ESP_OK);
    CHECK(init_calls == 4);
    CHECK(DURA_BATTERY_ADC_WAKE_SETTLE_MS == 250);
    due_us = clock_us + 250000;
    unavailable();
}

static void poll_due(int pad_mv)
{
    requested_mv = pad_mv;
    reads_this_poll = 0;
    clock_us = due_us;
    dura_battery_poll();
    due_us = clock_us + 5000000;
    ++step;
}

static void append_expected(int pad_mv)
{
    CHECK(pad_mv >= 0);
    CHECK(success_count < sizeof(successes) / sizeof(successes[0]));
    successes[success_count++] = (uint32_t)pad_mv;
}

static void check_expected(void)
{
    CHECK(success_count != 0);
    const unsigned first = success_count > 16 ? success_count - 16 : 0;
    uint64_t sum = 0;
    for (unsigned i = first; i < success_count; ++i) sum += successes[i];
    const uint32_t mean = (uint32_t)(sum / (success_count - first));
    const uint32_t battery = mean * 2u; /* nonnegative int pad: max is INT_MAX */
    const unsigned percent = battery <= 1300 ? 0 : battery >= 3200 ? 100 :
                             (battery - 1300u) * 100u / 1900u;
    const dura_battery_measurement_t m = dura_battery_get_measurement();
    CHECK(m.valid);
    if (m.millivolts != battery || m.percent != percent) {
        fprintf(stderr, "ASSERT %s step=%u successes=%u populated=%u sum=%" PRIu64
                " expected_mv=%" PRIu32 " actual_mv=%" PRIu32
                " expected_percent=%u actual_percent=%u\n", scenario, step,
                success_count, success_count - first, sum, battery, m.millivolts,
                percent, m.percent);
        exit(1);
    }
}

static void good_sample(int pad_mv)
{
    const unsigned d = divider_calls, p = percent_calls;
    poll_due(pad_mv);
    append_expected(pad_mv);
    check_expected();
    CHECK(divider_calls == d + 1 && percent_calls == p + 1);
}

static void assert_pure_observation(int64_t at)
{
    const dura_battery_measurement_t before = dura_battery_get_measurement();
    const unsigned r = reads, c = calibrations, d = divider_calls, p = percent_calls;
    clock_us = at;
    for (unsigned i = 0; i < 32; ++i) {
        dura_battery_poll();
        const dura_battery_measurement_t after = dura_battery_get_measurement();
        CHECK(after.valid == before.valid && after.millivolts == before.millivolts &&
              after.percent == before.percent);
    }
    CHECK(reads == r && calibrations == c && divider_calls == d && percent_calls == p);
}

static void error_case(const char *fault, unsigned filled)
{
    init_ok();
    /* Do not fail on setup means: each RED case must reach its fault/recovery. */
    for (unsigned i = 0; i < filled; ++i) {
        poll_due(600 + (int)i * 71);
        append_expected(600 + (int)i * 71);
    }
    if (strncmp(fault, "discard", 7) == 0) fail_read = (unsigned)(fault[7] - '0');
    else if (strcmp(fault, "read") == 0) fail_read = 5;
    else if (strcmp(fault, "calibration") == 0) fail_calibration = true;
    const int bad = strcmp(fault, "negative1") == 0 ? -1 :
                    strcmp(fault, "negative_min") == 0 ? INT_MIN : INT_MAX;
    /* Consecutive faults also must not advance or clear the retained history. */
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        const unsigned c = calibrations, d = divider_calls, p = percent_calls;
        poll_due(bad);
        CHECK(reads_this_poll == (fail_read ? fail_read : 5));
        CHECK(calibrations == c + (fail_read ? 0 : 1));
        CHECK(divider_calls == d && percent_calls == p);
        unavailable();
        const unsigned inits = init_calls;
        CHECK(dura_battery_init() == ESP_OK);
        CHECK(init_calls == inits);
        unavailable();
        assert_pure_observation(due_us - 1);
    }
    fail_read = 0;
    fail_calibration = false;
    /* Nonuniform recovery through a full wrap exposes changed count/head/sum. */
    for (unsigned i = 0; i < 35; ++i) good_sample(1700 + (int)i * 13);
}

static void init_error_case(const char *fault)
{
    init_fault = fault;
    const bool route = strstr(fault, "route") != NULL;
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        CHECK(dura_battery_init() == (route ? ESP_ERR_INVALID_STATE : ESP_FAIL));
        clock_us += 10000000;
        dura_battery_poll();
        unavailable();
        CHECK(reads == 0 && calibrations == 0 && divider_calls == 0 && percent_calls == 0);
    }
    const bool allocates_then_fails = strcmp(fault, "channel") == 0 || strcmp(fault, "calibration") == 0;
    CHECK(deleted_units == (allocates_then_fails ? 2u : 0u));
    init_fault = "";
    CHECK(dura_battery_init() == ESP_OK);
    due_us = clock_us + 250000;
    assert_pure_observation(due_us - 1);
    good_sample(1456);
}

static void enabled_case(void)
{
    if (strncmp(scenario, "error_", 6) == 0) {
        char fault[32];
        const char *suffix = strrchr(scenario, '_');
        CHECK(suffix != NULL && suffix > scenario + 6);
        const size_t length = (size_t)(suffix - (scenario + 6));
        CHECK(length < sizeof(fault));
        memcpy(fault, scenario + 6, length);
        fault[length] = '\0';
        const unsigned filled = (unsigned)strtoul(suffix + 1, NULL, 10);
        error_case(fault, filled);
        return;
    }
    if (strncmp(scenario, "init_", 5) == 0) {
        init_error_case(scenario + 5);
        return;
    }
    init_ok();
    if (strcmp(scenario, "acquisition_counts") == 0) {
        for (unsigned i = 0; i < 40; ++i) {
            const unsigned r = reads, c = calibrations;
            good_sample(1500);
            CHECK(reads == r + 5); /* four priming reads + ONE new sample */
            CHECK(calibrations == c + 1);
        }
    } else if (strcmp(scenario, "startup_1_to_16") == 0) {
        for (unsigned i = 0; i < 16; ++i) good_sample(701 + (int)i * 57);
    } else if (strcmp(scenario, "overwrite_17") == 0) {
        for (unsigned i = 0; i < 16; ++i) good_sample(1500);
        good_sample(0); /* retain exactly previous 15, drop oldest */
    } else if (strcmp(scenario, "multiple_wraps") == 0) {
        for (unsigned i = 0; i < 16; ++i) good_sample(0);
        for (unsigned i = 0; i < 160; ++i) good_sample((int)((i * 137 + 1001) % 2503));
    } else if (strcmp(scenario, "extreme_values") == 0) {
        CHECK(INT_MAX == 2147483647); /* target's int width, including max * 16 sum */
        for (unsigned i = 0; i < 16; ++i) good_sample(INT_MAX);
        for (unsigned i = 0; i < 16; ++i) good_sample(0);
        for (unsigned i = 0; i < 128; ++i) good_sample(i % 3 == 0 ? INT_MAX : i % 3 == 1 ? 0 : 1);
    } else if (strcmp(scenario, "mean_before_conversion") == 0) {
        good_sample(0);
        good_sample(2000); /* percent(mean voltage)=36, mean(percent)=50 */
        CHECK(dura_battery_get_measurement().percent == 36);
        good_sample(1005); /* pad mean truncates BEFORE divider */
    } else if (strcmp(scenario, "timing_getter_purity") == 0) {
        assert_pure_observation(due_us - 250000);
        assert_pure_observation(due_us - 1);
        good_sample(1432);
        assert_pure_observation(due_us - 5000000);
        assert_pure_observation(due_us - 1);
        /* Getters even when due must never acquire/update. */
        clock_us = due_us;
        const unsigned r = reads, c = calibrations, d = divider_calls, p = percent_calls;
        for (unsigned i = 0; i < 32; ++i) check_expected();
        CHECK(reads == r && calibrations == c && divider_calls == d && percent_calls == p);
        good_sample(1432);
        /* A late poll acquires one sample, not catch-up samples. */
        due_us += 20000000;
        const unsigned r2 = reads, c2 = calibrations;
        good_sample(1432);
        CHECK(reads == r2 + 5 && calibrations == c2 + 1);
        assert_pure_observation(due_us - 1);
    } else if (strcmp(scenario, "idempotent_init") == 0) {
        for (unsigned i = 0; i < 12; ++i) good_sample(1000);
        clock_us = due_us - 2000000;
        for (unsigned i = 0; i < 3; ++i) CHECK(dura_battery_init() == ESP_OK);
        CHECK(init_calls == 4 && deleted_units == 0);
        check_expected();
        assert_pure_observation(due_us - 1);
        good_sample(2200);
    } else {
        CHECK(!"unknown test scenario");
    }
}
#endif

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    scenario = argv[1];
#if CONFIG_DURA_BOARD_ENABLE_HARDWARE_TASK
    enabled_case();
#else
    CHECK(strcmp(scenario, "disabled") == 0);
    unavailable();
    for (unsigned i = 0; i < 100; ++i) {
        CHECK(dura_battery_init() == ESP_ERR_NOT_SUPPORTED);
        dura_battery_poll();
        unavailable();
    }
    CHECK(divider_calls == 0 && percent_calls == 0);
#endif
    printf("PASS %s: successful_history_checked=%u\n", scenario, step);
    return 0;
}
