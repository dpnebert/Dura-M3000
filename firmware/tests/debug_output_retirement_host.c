/* Real board header + verbatim production output setter/init block.
 * GPIO calls are observations, not hardware/electrical acceptance. No aliases
 * or level macros are reimplemented here; expected pins/polarities are external
 * test inputs. Retired names below are negative-input regression coverage only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dura_board.h"
#include "dura_meter.h"

#define BIT(pin) (UINT64_C(1) << (pin))
static int failures, check_count;
#define CHECK(name, actual, expected) do { \
    long long a_ = (long long)(actual), e_ = (long long)(expected); \
    printf("%s %s actual=%lld expected=%lld\n", a_ == e_ ? "PASS" : "FAIL", name, a_, e_); \
    ++check_count; if (a_ != e_) ++failures; \
} while (0)
static int levels[64], writes[64], sequence[64], sequence_count, config_calls;
static uint64_t configured_mask;
static int mode_seen;
int gpio_set_level(gpio_num_t pin, int level)
{
    if (pin < 0 || pin >= 64 || sequence_count >= 64) abort();
    levels[pin] = level;
    ++writes[pin];
    sequence[sequence_count++] = pin;
    return ESP_OK;
}
int gpio_config(const gpio_config_t *cfg)
{
    ++config_calls;
    configured_mask |= cfg->pin_bit_mask;
    mode_seen = cfg->mode;
    return ESP_OK;
}
#define ESP_RETURN_ON_ERROR(expr, ...) do { esp_err_t e_ = (expr); if (e_ != ESP_OK) return e_; } while (0)
#include "production.inc"

static void reset_observations(int seed)
{
    for (int pin = 0; pin < 64; ++pin) levels[pin] = seed;
    memset(writes, 0, sizeof(writes));
    memset(sequence, -1, sizeof(sequence));
    sequence_count = 0;
}
static void check_outputs(const char *phase, int seed, bool pump_on, bool recirc_on, bool inject_on)
{
    char label[160];
#define OUTPUT_CHECK(suffix, actual, expected) do { \
    snprintf(label, sizeof(label), "%s seed=%d %s", phase, seed, suffix); \
    CHECK(label, actual, expected); \
} while (0)
    const int pump = TEST_PUMP, ev = TEST_EV;
    OUTPUT_CHECK("pump47 writes only by production gate", writes[47], pump);
    OUTPUT_CHECK("pump47 final level", levels[47], pump ? (pump_on != TEST_PUMP_LOW) : seed);
    OUTPUT_CHECK("RecircEV17 writes", writes[17], ev);
    OUTPUT_CHECK("InjectEV18 writes", writes[18], ev);
    OUTPUT_CHECK("RecircEV17 final level", levels[17], ev ? recirc_on : seed);
    OUTPUT_CHECK("InjectEV18 final level", levels[18], ev ? inject_on : seed);
    OUTPUT_CHECK("no extra debug writes", sequence_count, pump + 2*ev);
    int unexpected_writes = 0, unexpected_changes = 0;
    for (int pin = 0; pin < 64; ++pin) {
        if (pin != 47 && pin != 17 && pin != 18) {
            unexpected_writes += writes[pin];
            unexpected_changes += levels[pin] != seed;
        }
    }
    OUTPUT_CHECK("all unrelated pins untouched", unexpected_writes, 0);
    OUTPUT_CHECK("all unrelated levels unchanged", unexpected_changes, 0);
    int pos = 0;
    /* OFF: pump before EVs. ON: EVs before pump, independent of polarity. */
    if (pump && !pump_on) OUTPUT_CHECK("OFF pump first", sequence[pos++], 47);
    if (ev) {
        OUTPUT_CHECK("RecircEV write order", sequence[pos++], 17);
        OUTPUT_CHECK("InjectEV write order", sequence[pos++], 18);
    }
    if (pump && pump_on) OUTPUT_CHECK("ON pump last", sequence[pos++], 47);
#undef OUTPUT_CHECK
}
int main(void)
{
    CHECK("real header pump pin", DURA_GPIO_PUMP_OUTPUT, 47);
    CHECK("real header RecircEV pin", DURA_GPIO_RECIRC_EV_OUTPUT, 17);
    CHECK("real header InjectEV pin", DURA_GPIO_INJECT_EV_OUTPUT, 18);
    CHECK("real header pump gate", CONFIG_DURA_BOARD_ENABLE_PUMP_OUTPUT, TEST_PUMP);
    CHECK("real header EV gate", CONFIG_DURA_BOARD_ENABLE_EV_OUTPUTS, TEST_EV);
    CHECK("real header pump active level", DURA_PUMP_OUTPUT_ACTIVE_LEVEL, !TEST_PUMP_LOW);
    CHECK("real header pump inactive level", DURA_PUMP_OUTPUT_INACTIVE_LEVEL, TEST_PUMP_LOW);
#ifdef DURA_GPIO_TICK_METER_DEBUG
    CHECK("retired header GPIO alias absent", 1, 0);
#else
    CHECK("retired header GPIO alias absent", 0, 0);
#endif
#if defined(DURA_TICK_METER_DEBUG_ACTIVE_LEVEL) || defined(DURA_TICK_METER_DEBUG_INACTIVE_LEVEL)
    CHECK("retired header level aliases absent", 1, 0);
#else
    CHECK("retired header level aliases absent", 0, 0);
#endif
    /* Either previously stored physical level must be untouched when disabled. */
    for (int seed = 0; seed <= 1; ++seed) {
        reset_observations(seed);
        configured_mask = 0;
        config_calls = 0;
        mode_seen = -1;
        CHECK("production output init succeeds", production_output_init(), ESP_OK);
        CHECK("init only production output mask", configured_mask,
              (TEST_PUMP ? BIT(47) : 0) | (TEST_EV ? BIT(17)|BIT(18) : 0));
        CHECK("init config only when production outputs enabled", config_calls, !!(TEST_PUMP || TEST_EV));
        CHECK("init output mode", mode_seen, (TEST_PUMP || TEST_EV) ? GPIO_MODE_OUTPUT : -1);
        check_outputs("init inactive", seed, false, false, false);
        for (int mask = 0; mask < 8; ++mask) {
            reset_observations(seed);
            dura_output_intent_t intent = { .pump = (mask & 1) != 0,
                .recirc_ev = (mask & 2) != 0, .inject_ev = (mask & 4) != 0 };
            apply_meter_outputs(intent);
            char phase[64];
            snprintf(phase, sizeof(phase), "intent=%d", mask);
            check_outputs(phase, seed, intent.pump, intent.recirc_ev, intent.inject_ev);
        }
    }
    printf("HARNESS_COMPLETE checks=%d failures=%d\n", check_count, failures);
    return failures ? 1 : 0;
}
