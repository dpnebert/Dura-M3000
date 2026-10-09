/* Real dura_board.h and extracted production code. Only SDK/unrelated leaves
 * are stubbed. Never define DURA_GPIO_* or DURA_*_ACTIVE_LEVEL in this fixture.
 * GPIO47 checks use uint64_t masks; GPIO21 is now exclusively a flow input.
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

/* Hardware seams record actual production operations, not expected behavior. */
static int levels[64], writes[64], reads[64], modes[64], pullups[64], pulldowns[64];
static int sequence[256], sequence_count;
static uint64_t configured_mask, output_mask_seen, input_mask_seen, isr_mask;
static dura_meter_output_handler_t installed_output_handler;
int gpio_set_level(gpio_num_t pin, int level)
{
    if (pin < 0 || pin >= 64 || sequence_count >= 256) abort();
    levels[pin] = level;
    ++writes[pin];
    sequence[sequence_count++] = pin;
    return ESP_OK;
}
int gpio_get_level(gpio_num_t pin) { ++reads[pin]; return levels[pin]; }
int gpio_config(const gpio_config_t *cfg)
{
    configured_mask |= cfg->pin_bit_mask;
    if (cfg->mode == GPIO_MODE_OUTPUT) output_mask_seen |= cfg->pin_bit_mask;
    if (cfg->mode == GPIO_MODE_INPUT) input_mask_seen |= cfg->pin_bit_mask;
    for (int pin = 0; pin < 64; ++pin) {
        if (cfg->pin_bit_mask & BIT(pin)) {
            modes[pin] = cfg->mode;
            pullups[pin] = cfg->pull_up_en;
            pulldowns[pin] = cfg->pull_down_en;
        }
    }
    return ESP_OK;
}
static void reset_writes(void)
{
    memset(writes, 0, sizeof(writes));
    memset(levels, -1, sizeof(levels));
    memset(sequence, -1, sizeof(sequence));
    sequence_count = 0;
}

/* Board-init's unrelated battery/LCD/queue/ISR/RTOS dependencies. The complete
 * dura_board_init body and apply_meter_outputs are included verbatim below. */
typedef void *button_handle_t;
static void *s_button_queue, *s_flow_event_queue;
static bool s_ignore_buttons_until_release;
static uint8_t s_last_valid_reed;
static int s_ui_screen;
static float s_ui_batch_amount;
static int home_calls, activity_calls;
static bool recirc_was_pressed;
static int64_t s_last_activity_us;
#define ESP_RETURN_ON_ERROR(expr, ...) do { esp_err_t e_ = (expr); if (e_ != ESP_OK) return e_; } while (0)
#define ESP_RETURN_ON_FALSE(expr, err, ...) do { if (!(expr)) return (err); } while (0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_SLEEP_WAKEUP_UNDEFINED 0
#define ESP_INTR_FLAG_IRAM 1
#define DURA_BOOT_LOGO_HOLD_MS 500u
#define DURA_BOOT_POWERING_UP_HOLD_MS 2000u
#define pdMS_TO_TICKS(ms) (ms)
#define portENTER_CRITICAL(...) ((void)0)
#define portEXIT_CRITICAL(...) ((void)0)
static int esp_sleep_get_wakeup_cause(void) { return 0; }
static int64_t esp_timer_get_time(void) { return 123456; }
static void *xQueueCreate(unsigned count, unsigned size) { return (void *)1; }
esp_err_t dura_board_init_power_sense(void) { return ESP_OK; }
static esp_err_t dura_battery_init(void) { return ESP_OK; }
static esp_err_t rtc_gpio_hold_dis(int pin) { return ESP_OK; }
static uint32_t flow_boundary(bool enter) { return 0; }
esp_err_t dura_meter_set_flow_handler(dura_meter_flow_handler_t handler) { return ESP_OK; }
static esp_err_t dura_board_init_buttons(void) { return ESP_OK; }
static esp_err_t gpio_set_intr_type(int pin, int type) { return ESP_OK; }
static esp_err_t gpio_install_isr_service(int flags) { return ESP_OK; }
static void flow_isr(void *arg) {}
static esp_err_t gpio_isr_handler_add(int pin, void (*handler)(void *), void *arg)
{
    isr_mask |= BIT(pin);
    return ESP_OK;
}
static esp_err_t dura_lcd_init(void) { return ESP_OK; }
static esp_err_t dura_lcd_draw_splash(void) { return ESP_OK; }
static void update_backlight(void) {}
static esp_err_t draw_boot_powering_up_popup(void) { return ESP_OK; }
static void vTaskDelay(unsigned ticks) {}
esp_err_t dura_board_render_debug_screen(void) { return ESP_OK; }
esp_err_t dura_meter_get_snapshot(dura_meter_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot)); return ESP_OK;
}
static void publish_ui_snapshot(const dura_meter_snapshot_t *snapshot) {}
void dura_board_note_user_activity(void) { ++activity_calls; }
esp_err_t dura_meter_set_screen_home(void) { ++home_calls; return ESP_OK; }
esp_err_t dura_meter_set_output_handler(dura_meter_output_handler_t handler)
{
    installed_output_handler = handler;
    return ESP_OK;
}

#include "production.inc"

static void check_recirc_behavior(int pin, int active)
{
    memset(reads, 0, sizeof(reads));
    recirc_was_pressed = false;
    s_ui_screen = DURA_UI_MAIN_MENU;
    s_ui_batch_amount = 0;
    home_calls = activity_calls = 0;
    levels[pin] = !active;
    production_recirc_tick();
    CHECK("Recirc idle does not open menu", home_calls, 0);
    CHECK("Recirc idle keeps Main Menu", s_ui_screen, DURA_UI_MAIN_MENU);
    levels[pin] = active;
    production_recirc_tick();
    CHECK("Recirc press opens editor", s_ui_screen, DURA_UI_RECIRC_EDIT);
    CHECK("Recirc press resets meter home once", home_calls, 1);
    CHECK("Recirc accepted activity once", activity_calls, 1);
    CHECK("Recirc default batch retained", (int)s_ui_batch_amount, 250);
    CHECK("Recirc reads assigned input", reads[pin], 2);
    s_ui_screen = DURA_UI_MAIN_MENU;
    production_recirc_tick();
    CHECK("Recirc held input does not retrigger", home_calls, 1);
    CHECK("Recirc held input keeps menu", s_ui_screen, DURA_UI_MAIN_MENU);
    levels[pin] = !active;
    production_recirc_tick();
    levels[pin] = active;
    production_recirc_tick();
    CHECK("Recirc release/repress retriggers", home_calls, 2);
    /* HOME aliases MAIN_MENU; use every actual non-menu state instead. */
    for (int screen = DURA_UI_MANUAL; screen < DURA_UI_SCREEN_COUNT; ++screen) {
        levels[pin] = !active;
        production_recirc_tick();
        s_ui_screen = screen;
        levels[pin] = active;
        production_recirc_tick();
        CHECK("Recirc outside Main Menu rejected", home_calls, 2);
        CHECK("Recirc outside Main Menu keeps screen", s_ui_screen, screen);
    }
}

static void check_drive(dura_operation_mode_t mode, bool enabled,
                        int pump, int recirc, int inject)
{
    reset_writes();
    /* Execute the real operation policy, then the real production GPIO sink. */
    apply_meter_outputs(dura_control_output_intent(mode, enabled));
    char label[120];
#define DRIVE_CHECK(suffix, actual, expected) do { \
    snprintf(label, sizeof(label), "drive mode=%d enabled=%d %s", mode, enabled, suffix); \
    CHECK(label, actual, expected); \
} while (0)
    DRIVE_CHECK("pump47 level", levels[47], pump);
    DRIVE_CHECK("RecircEV17 level", levels[17], recirc);
    DRIVE_CHECK("InjectEV18 level", levels[18], inject);
    DRIVE_CHECK("no writes to FlowA21", writes[21], 0);
    DRIVE_CHECK("pump47 written exactly once", writes[47], 1);
    DRIVE_CHECK("three output writes", sequence_count, 3);
    DRIVE_CHECK("pump sequence safety", sequence[pump ? 2 : 0], 47);
    DRIVE_CHECK("no spare38 write", writes[38], 0);
    DRIVE_CHECK("no spare48 write", writes[48], 0);
#undef DRIVE_CHECK
}

int main(void)
{
    memset(levels, -1, sizeof(levels));
    memset(modes, -1, sizeof(modes));
    memset(pullups, -1, sizeof(pullups));
    memset(pulldowns, -1, sizeof(pulldowns));
    CHECK("production board init succeeds", dura_board_init(), ESP_OK);
#ifdef TEST_POLARITY_ONLY
    CHECK("Recirc header active level", DURA_RECIRC_INPUT_ACTIVE_LEVEL, TEST_ACTIVE_LEVEL);
    CHECK("Recirc header pullup", CONFIG_DURA_BOARD_RECIRC_INPUT_PULLUP, TEST_PULLUP);
#if CONFIG_DURA_BOARD_ENABLE_RECIRC_INPUT
    CHECK("Recirc actual input mode", modes[DURA_GPIO_RECIRC_INPUT], GPIO_MODE_INPUT);
    CHECK("Recirc actual pullup", pullups[DURA_GPIO_RECIRC_INPUT], TEST_PULLUP);
    CHECK("Recirc actual pulldown", pulldowns[DURA_GPIO_RECIRC_INPUT], GPIO_PULLDOWN_DISABLE);
    check_recirc_behavior(DURA_GPIO_RECIRC_INPUT, TEST_ACTIVE_LEVEL);
#else
    CHECK("Recirc disabled unconfigured", modes[DURA_GPIO_RECIRC_INPUT], -1);
    production_recirc_tick();
    CHECK("Recirc disabled not read", reads[DURA_GPIO_RECIRC_INPUT], 0);
#endif
#elif defined(TEST_GATES_ONLY)
    const int pump = CONFIG_DURA_BOARD_ENABLE_PUMP_OUTPUT;
    const int ev = CONFIG_DURA_BOARD_ENABLE_EV_OUTPUTS;
    const int recirc = CONFIG_DURA_BOARD_ENABLE_RECIRC_INPUT;
    CHECK("gate output mask", output_mask_seen,
          (pump ? BIT(47) : 0) | (ev ? BIT(17)|BIT(18) : 0));
    CHECK("gate pump init", writes[47], pump);
    CHECK("gate RecircEV init", writes[17], ev);
    CHECK("gate InjectEV init", writes[18], ev);
    CHECK("gate Recirc input mode", modes[35], recirc ? GPIO_MODE_INPUT : -1);
    for (int mask = 0; mask < 8; ++mask) {
        reset_writes();
        dura_output_intent_t intent = { .pump = (mask & 1) != 0,
            .recirc_ev = (mask & 2) != 0, .inject_ev = (mask & 4) != 0 };
        apply_meter_outputs(intent);
        CHECK("gate pump drive", levels[47], pump ? intent.pump : -1);
        CHECK("gate RecircEV drive", levels[17], ev ? intent.recirc_ev : -1);
        CHECK("gate InjectEV drive", levels[18], ev ? intent.inject_ev : -1);
        CHECK("gate write count", sequence_count, pump + 2*ev);
        CHECK("gate FlowA not driven", writes[21], 0);
        if (pump) CHECK("gate pump ordering", sequence[intent.pump ? sequence_count-1 : 0], 47);
    }
    if (recirc) check_recirc_behavior(35, 1);
    else {
        production_recirc_tick();
        CHECK("gate disabled Recirc not read", reads[35], 0);
    }
#else
#define PIN(name, expected) CHECK("header GPIO " #name, DURA_GPIO_##name, expected)
    PIN(FLOW_A, 21); PIN(FLOW_B, 2); PIN(FIELD_SENSE_ADC, 8);
    PIN(RECIRC_INPUT, 35); PIN(PUMP_OUTPUT, 47);
    PIN(RECIRC_EV_OUTPUT, 17); PIN(INJECT_EV_OUTPUT, 18); PIN(LCD_BACKLIGHT, 14);
    PIN(KEY_1, 6); PIN(KEY_2, 7); PIN(KEY_3, 5); PIN(KEY_4, 4);
#undef PIN
    CHECK("header Recirc active-high", DURA_RECIRC_INPUT_ACTIVE_LEVEL, 1);
    CHECK("header Recirc no pullup", CONFIG_DURA_BOARD_RECIRC_INPUT_PULLUP, 0);
    CHECK("header Recirc no pulldown", CONFIG_DURA_BOARD_RECIRC_INPUT_PULLDOWN, 0);
    CHECK("header backlight active-high", DURA_LCD_BACKLIGHT_ACTIVE_LEVEL, 1);
    CHECK("header backlight inactive-low", DURA_LCD_BACKLIGHT_INACTIVE_LEVEL, 0);
    CHECK("header Recirc input enabled", CONFIG_DURA_BOARD_ENABLE_RECIRC_INPUT, 1);
    CHECK("header EV outputs enabled", CONFIG_DURA_BOARD_ENABLE_EV_OUTPUTS, 1);
    CHECK("init output mask includes GPIO47", output_mask_seen, BIT(47)|BIT(17)|BIT(18));
    CHECK("init pump47 is output", modes[47], GPIO_MODE_OUTPUT);
    CHECK("init pump47 inactive", levels[47], 0);
    CHECK("init pump47 written once", writes[47], 1);
    CHECK("init RecircEV17 inactive", levels[17], 0);
    CHECK("init InjectEV18 inactive", levels[18], 0);
    CHECK("init no writes to FlowA21", writes[21], 0);
    CHECK("init FlowA21 input", modes[21], GPIO_MODE_INPUT);
    CHECK("init FlowB2 input", modes[2], GPIO_MODE_INPUT);
    CHECK("init Recirc35 input", modes[35], GPIO_MODE_INPUT);
    CHECK("init Recirc35 no pullup", pullups[35], GPIO_PULLUP_DISABLE);
    CHECK("init Recirc35 no pulldown", pulldowns[35], GPIO_PULLDOWN_DISABLE);
    CHECK("init spare38 unconfigured", (configured_mask & BIT(38)) != 0, 0);
    CHECK("init spare48 unconfigured", (configured_mask & BIT(48)) != 0, 0);
    CHECK("init actual flow ISR pins", isr_mask, BIT(21)|BIT(2));
    CHECK("init installed production output handler", installed_output_handler == apply_meter_outputs, 1);
    const uint64_t keys = BIT(4)|BIT(5)|BIT(6)|BIT(7);
    CHECK("production button wake mask", dura_board_button_wake_mask(), keys);
    CHECK("production flow wake mask", dura_board_flow_wake_mask(), BIT(21)|BIT(2));
    CHECK("production combined wake mask", dura_board_combined_wake_mask(), keys|BIT(21)|BIT(2));
    CHECK("GPIO21 wake classified as flow", dura_board_wake_group_from_ext1_status(BIT(21)), DURA_WAKE_GROUP_FLOW);
    CHECK("GPIO2 wake classified as flow", dura_board_wake_group_from_ext1_status(BIT(2)), DURA_WAKE_GROUP_FLOW);
    CHECK("GPIO47 not in wake mask", dura_board_wake_group_from_ext1_status(BIT(47)), DURA_WAKE_GROUP_NONE);
    CHECK("GPIO35 not incorrectly used as EXT1", dura_board_wake_group_from_ext1_status(BIT(35)), DURA_WAKE_GROUP_NONE);
    check_drive(DURA_OPERATION_IDLE, false, 0, 0, 0);
    check_drive(DURA_OPERATION_MANUAL, true, 1, 0, 1);
    check_drive(DURA_OPERATION_CALIBRATION, true, 1, 1, 0);
    check_drive(DURA_OPERATION_RECIRC, true, 1, 1, 0);
    check_drive(DURA_OPERATION_AUTO, true, 1, 0, 1);
    check_drive(DURA_OPERATION_MANUAL, false, 0, 0, 0);
    check_drive(DURA_OPERATION_CALIBRATION, false, 0, 0, 0);
    check_drive(DURA_OPERATION_RECIRC, false, 0, 0, 0);
    check_drive(DURA_OPERATION_AUTO, false, 0, 0, 0);
    reset_writes();
    CHECK("backlight on call succeeds", dura_lcd_set_backlight(true), ESP_OK);
    CHECK("backlight14 on is high", levels[14], 1);
    CHECK("backlight off call succeeds", dura_lcd_set_backlight(false), ESP_OK);
    CHECK("backlight14 off is low", levels[14], 0);
    CHECK("backlight never writes FlowA21", writes[21], 0);
    check_recirc_behavior(35, 1);
#endif
    printf("HARNESS_COMPLETE checks=%d failures=%d\n", check_count, failures);
    return failures ? 1 : 0;
}
