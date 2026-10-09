#include "dura_board.h"
#include "gama_blefob.h"
#include "dura_battery.h"
#include "dura_st7567.h"
#include "dura_meter.h"
#include "dura_coordinator.h"
#include "dura_peers.h"
#include "dura_recipe.h"
#include "dura_lcd_assets.h"
#include "dura_ui_behavior.h"
#include "dura_ui_config.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "button_gpio.h"
#include "iot_button.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "qrcode.h"

static const char *TAG = "dura_board";
#define DURA_BOOT_LOGO_HOLD_MS 500u
#define DURA_BOOT_POWERING_UP_HOLD_MS 2000u
#define DURA_AUTO_BATCH_DEFAULT_GAL 10.0f
#define DURA_RECIRC_BATCH_DEFAULT_GAL 250.0f

static volatile uint32_t s_flow_pulses;
static portMUX_TYPE s_flow_mux = portMUX_INITIALIZER_UNLOCKED;
/* DRAM spinlock shared with the IRAM ISR; never acquire the meter from ISR. */
static portMUX_TYPE s_flow_boundary_mux = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_flow_event_queue;
static portMUX_TYPE s_ui_snapshot_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;
static QueueHandle_t s_button_queue;
static dura_board_ui_snapshot_t s_ui_snapshot;
static bool s_ui_snapshot_valid;
static dura_board_ui_change_cb_t s_ui_change_cb;
static void *s_ui_change_context;
static bool s_ble_connected;
static int64_t s_last_activity_us;
/* Illumination is independent of sleep admission/connection bookkeeping. */
static int64_t s_backlight_activity_us;
static uint64_t s_activity_generation;
/* Only board_task touches preparation/recovery state and input muxes. */
static bool s_sleep_requested;
static bool s_sleep_recovery_pending;
static uint64_t s_sleep_touched_mask;
static bool s_sleep_ext1_attempted;
static bool s_ignore_buttons_until_release;
static void publish_ui_snapshot(const dura_meter_snapshot_t *meter);

esp_err_t dura_board_get_ui_snapshot(dura_board_ui_snapshot_t *snapshot)
{
    if (snapshot == NULL) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_ui_snapshot_mux);
    if (!s_ui_snapshot_valid) {
        portEXIT_CRITICAL(&s_ui_snapshot_mux);
        return ESP_ERR_INVALID_STATE;
    }
    *snapshot = s_ui_snapshot;
    portEXIT_CRITICAL(&s_ui_snapshot_mux);
    return ESP_OK;
}

esp_err_t dura_board_set_ui_change_callback(dura_board_ui_change_cb_t callback, void *context)
{
    portENTER_CRITICAL(&s_ui_snapshot_mux);
    s_ui_change_cb = callback;
    s_ui_change_context = context;
    portEXIT_CRITICAL(&s_ui_snapshot_mux);
    return ESP_OK;
}

esp_err_t dura_board_set_ble_connected(bool connected)
{
    portENTER_CRITICAL(&s_ui_snapshot_mux);
    s_ble_connected = connected;
    ++s_activity_generation;
    s_last_activity_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_ui_snapshot_mux);
    return ESP_OK;
}

void dura_board_note_activity(void)
{
    const int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_ui_snapshot_mux);
    s_last_activity_us = now_us;
    ++s_activity_generation;
    portEXIT_CRITICAL(&s_ui_snapshot_mux);
}
void dura_board_note_user_activity(void)
{
    const int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_ui_snapshot_mux);
    s_last_activity_us = now_us;
    s_backlight_activity_us = now_us;
    ++s_activity_generation;
    portEXIT_CRITICAL(&s_ui_snapshot_mux);
}

/* Board owner only: callers on other tasks record activity, never drive GPIO.
 * No calibration exception. OFF (9) wins over every activity source. */
static void update_backlight(void)
{
    dura_meter_snapshot_t meter;
    if (dura_meter_get_snapshot(&meter) != ESP_OK) {
        (void)dura_lcd_set_backlight(false);
        return;
    }
    portENTER_CRITICAL(&s_ui_snapshot_mux);
    const int64_t activity_us = s_backlight_activity_us;
    portEXIT_CRITICAL(&s_ui_snapshot_mux);
    const int64_t now_us = esp_timer_get_time();
    const int64_t elapsed_us = now_us >= activity_us ? now_us - activity_us : 0;
    const bool on = meter.backlight_timeout_sec >= DURA_BACKLIGHT_TIMEOUT_MIN_SEC &&
                    meter.backlight_timeout_sec <= DURA_BACKLIGHT_TIMEOUT_MAX_SEC &&
                    elapsed_us < (int64_t)meter.backlight_timeout_sec * 1000000LL;
    (void)dura_lcd_set_backlight(on);
}

RTC_DATA_ATTR static bool s_provisioning_qr_dismissed;
static bool s_provisioning_qr_rendered;
static bool s_boost_enabled;
static char s_provisioning_qr_payload[80];

typedef struct {
    dura_button_t id;
    gpio_num_t gpio;
    const char *name;
    button_handle_t handle;
} dura_button_def_t;

static dura_button_def_t s_buttons[] = {
    // Physical switch order on the V2 board is KEY4, KEY3, KEY2, KEY1 from left to right.
    // Keep the existing enum values as button positions so terminal logs read 1,2,3,4 left-to-right.
    {DURA_BUTTON_UP, DURA_GPIO_KEY_4, "button1_left", NULL},
    {DURA_BUTTON_DOWN, DURA_GPIO_KEY_3, "button2", NULL},
    {DURA_BUTTON_SELECT, DURA_GPIO_KEY_2, "button3", NULL},
    {DURA_BUTTON_BACK, DURA_GPIO_KEY_1, "button4_right", NULL},
};

typedef struct {
    gpio_num_t gpio;
    const char *name;
} dura_flow_def_t;

static const dura_flow_def_t s_flow_inputs[] = {
    {DURA_GPIO_FLOW_A, "flow_a"},
    {DURA_GPIO_FLOW_B, "flow_b"},
};

typedef struct {
    uint8_t source; /* 1 = reed A, 2 = reed B */
    uint8_t levels; /* bit 0 = reed A, bit 1 = reed B */
} dura_flow_event_t;

static uint8_t s_last_valid_reed;

bool dura_board_ext_power_present(void)
{
#if CONFIG_DURA_BOARD_ENABLE_EXT_POWER_PRESENT_INPUT
    return gpio_get_level(DURA_GPIO_EXT_POWER_PRESENT) == DURA_EXT_POWER_PRESENT_ACTIVE_LEVEL;
#else
    return true;
#endif
}

const char *dura_board_power_source_name(void)
{
#if CONFIG_DURA_BOARD_ENABLE_EXT_POWER_PRESENT_INPUT
    return dura_board_ext_power_present() ? "external" : "backup_battery";
#else
    return "unmonitored";
#endif
}

esp_err_t dura_board_set_boost_enabled(bool enabled)
{
    /* Round 11 hard-wires TPS61021A EN to 3v3_digital.  No ESP GPIO owns it. */
    s_boost_enabled = true;
    return enabled ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

bool dura_board_boost_enabled(void)
{
    return true;
}

void dura_board_dump_button_io_config(const char *reason)
{
    ESP_LOGI(TAG,
             "button gpio diag%s%s: active_low=%d active_level=%d cfg_pullup=%d cfg_pulldown=%d",
             reason != NULL ? " " : "", reason != NULL ? reason : "",
             CONFIG_DURA_BOARD_BUTTON_ACTIVE_LOW,
             DURA_BUTTON_ACTIVE_LEVEL,
             CONFIG_DURA_BOARD_BUTTON_PULLUP,
             CONFIG_DURA_BOARD_BUTTON_PULLDOWN);

    for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); ++i) {
        const int raw_level = gpio_get_level(s_buttons[i].gpio);
        const int pressed = s_buttons[i].handle != NULL ? iot_button_get_key_level(s_buttons[i].handle) : -1;
        ESP_LOGI(TAG, "button diag: %s gpio=%d raw_level=%d pressed=%d handle=%p",
                 s_buttons[i].name, (int)s_buttons[i].gpio, raw_level, pressed, s_buttons[i].handle);
        gpio_dump_io_configuration(stdout, (1ULL << s_buttons[i].gpio));
    }
}

uint64_t dura_board_button_wake_mask(void)
{
    uint64_t mask = 0;
    for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); ++i) {
        mask |= (1ULL << s_buttons[i].gpio);
    }
    return mask;
}

uint64_t dura_board_flow_wake_mask(void)
{
    uint64_t mask = 0;
    for (size_t i = 0; i < sizeof(s_flow_inputs) / sizeof(s_flow_inputs[0]); ++i) {
        mask |= (1ULL << s_flow_inputs[i].gpio);
    }
    return mask;
}

uint64_t dura_board_combined_wake_mask(void)
{
    return dura_board_button_wake_mask() | dura_board_flow_wake_mask();
}

dura_wake_group_t dura_board_wake_group_from_ext1_status(uint64_t ext1_status)
{
    dura_wake_group_t group = DURA_WAKE_GROUP_NONE;
    if ((ext1_status & dura_board_button_wake_mask()) != 0) {
        group |= DURA_WAKE_GROUP_BUTTON;
    }
    if ((ext1_status & dura_board_flow_wake_mask()) != 0) {
        group |= DURA_WAKE_GROUP_FLOW;
    }
    return group;
}

const char *dura_board_wake_group_name(dura_wake_group_t group)
{
    switch ((uint32_t)group) {
    case DURA_WAKE_GROUP_NONE:
        return "none";
    case DURA_WAKE_GROUP_BUTTON:
        return "button";
    case DURA_WAKE_GROUP_FLOW:
        return "flow";
    case DURA_WAKE_GROUP_BUTTON | DURA_WAKE_GROUP_FLOW:
        return "button+flow";
    default:
        return "unknown";
    }
}

static esp_err_t dura_board_init_buttons(void)
{
    const button_config_t button_cfg = {
        .long_press_time = 0,
        .short_press_time = 0,
    };

    for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); ++i) {
        (void)rtc_gpio_hold_dis(s_buttons[i].gpio);
        ESP_LOGI(TAG, "button init request: %s gpio=%d active_level=%d pullup_cfg=%d pulldown_cfg=%d",
                 s_buttons[i].name, (int)s_buttons[i].gpio, DURA_BUTTON_ACTIVE_LEVEL,
                 CONFIG_DURA_BOARD_BUTTON_PULLUP, CONFIG_DURA_BOARD_BUTTON_PULLDOWN);
        const button_gpio_config_t gpio_cfg = {
            .gpio_num = s_buttons[i].gpio,
            .active_level = DURA_BUTTON_ACTIVE_LEVEL,
            .enable_power_save = false,
            .disable_pull = !(CONFIG_DURA_BOARD_BUTTON_PULLUP || CONFIG_DURA_BOARD_BUTTON_PULLDOWN),
        };
        ESP_RETURN_ON_ERROR(iot_button_new_gpio_device(&button_cfg, &gpio_cfg, &s_buttons[i].handle),
                            TAG, "button %s gpio %d", s_buttons[i].name, (int)s_buttons[i].gpio);
        dura_board_dump_button_io_config("after iot_button_new_gpio_device");
    }
    return ESP_OK;
}

static void IRAM_ATTR flow_isr(void *arg)
{
    if (s_flow_event_queue == NULL) return;
    portENTER_CRITICAL_ISR(&s_flow_boundary_mux);
    const dura_flow_event_t event = {
        .source = (uint8_t)(uintptr_t)arg,
        .levels = (uint8_t)((gpio_get_level(DURA_GPIO_FLOW_A) ? 1u : 0u) |
                           (gpio_get_level(DURA_GPIO_FLOW_B) ? 2u : 0u)),
    };
    BaseType_t wake = pdFALSE;
    /* Qualification is deliberately deferred; the ISR only snapshots the two
     * reeds and queues the edge.  A full queue rejects rather than fabricates. */
    (void)xQueueSendFromISR(s_flow_event_queue, &event, &wake);
    portEXIT_CRITICAL_ISR(&s_flow_boundary_mux);
    if (wake == pdTRUE) portYIELD_FROM_ISR();
}

static bool flow_event_counts(const dura_flow_event_t *event)
{
    const uint8_t source_mask = event->source == 1u ? 1u : event->source == 2u ? 2u : 0u;
    if (source_mask == 0u) return false;
    const bool source_active = ((event->levels & source_mask) != 0u) ==
                               (DURA_FLOW_ACTIVE_LEVEL != 0);
    if (!source_active || event->source == s_last_valid_reed) return false;
    s_last_valid_reed = event->source;
    return true;
}

uint32_t dura_board_take_flow_pulses(void)
{
    dura_flow_event_t event;
    while (s_flow_event_queue != NULL && xQueueReceive(s_flow_event_queue, &event, 0) == pdTRUE) {
        if (flow_event_counts(&event)) {
            portENTER_CRITICAL(&s_flow_mux);
            if (s_flow_pulses != UINT32_MAX) s_flow_pulses++;
            portEXIT_CRITICAL(&s_flow_mux);
        }
    }

    uint32_t pulses;
    portENTER_CRITICAL_SAFE(&s_flow_mux);
    pulses = s_flow_pulses;
    s_flow_pulses = 0;
    portEXIT_CRITICAL_SAFE(&s_flow_mux);
    return pulses;
}

/* Only the meter may consume ingress, with its state lock held. The bounded
 * queue is drained without resetting reed qualification history. */
static uint32_t flow_boundary(bool enter)
{
    if (enter) {
        portENTER_CRITICAL(&s_flow_boundary_mux);
        return dura_board_take_flow_pulses();
    }
    portEXIT_CRITICAL(&s_flow_boundary_mux);
    return 0;
}

dura_button_t dura_board_poll_button(void)
{
    for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); ++i) {
        if (s_buttons[i].handle != NULL && iot_button_get_key_level(s_buttons[i].handle) != 0) {
            return s_buttons[i].id;
        }
    }
    return DURA_BUTTON_NONE;
}

static esp_err_t config_rtc_wake_input(gpio_num_t gpio, const char *name, bool pullup, bool pulldown)
{
    /* Track before the first fallible call, including partially changed pads. */
    s_sleep_touched_mask |= 1ULL << gpio;
    ESP_LOGI(TAG, "rtc wake input request: %s gpio=%d pullup=%d pulldown=%d",
             name, (int)gpio, pullup, pulldown);
    ESP_RETURN_ON_ERROR(rtc_gpio_init(gpio), TAG, "rtc init %s", name);
    ESP_RETURN_ON_ERROR(rtc_gpio_set_direction(gpio, RTC_GPIO_MODE_INPUT_ONLY), TAG, "rtc input %s", name);
    if (pullup) {
        ESP_RETURN_ON_ERROR(rtc_gpio_pullup_en(gpio), TAG, "rtc pullup on %s", name);
    } else {
        ESP_RETURN_ON_ERROR(rtc_gpio_pullup_dis(gpio), TAG, "rtc pullup off %s", name);
    }
    if (pulldown) {
        ESP_RETURN_ON_ERROR(rtc_gpio_pulldown_en(gpio), TAG, "rtc pulldown on %s", name);
    } else {
        ESP_RETURN_ON_ERROR(rtc_gpio_pulldown_dis(gpio), TAG, "rtc pulldown off %s", name);
    }
    ESP_RETURN_ON_ERROR(rtc_gpio_hold_en(gpio), TAG, "rtc hold %s", name);
    gpio_dump_io_configuration(stdout, (1ULL << gpio));
    return ESP_OK;
}

static esp_err_t prepare_deep_sleep_inputs(void)
{
    const uint64_t button_mask = dura_board_button_wake_mask();
    const uint64_t flow_mask = dura_board_flow_wake_mask();
    const uint64_t wake_mask = button_mask | flow_mask;
    ESP_LOGI(TAG, "entering deep sleep; button wake mask=0x%llx flow wake mask=0x%llx active_level=%d",
             (unsigned long long)button_mask, (unsigned long long)flow_mask, DURA_BUTTON_ACTIVE_LEVEL);

    ESP_RETURN_ON_FALSE(DURA_BUTTON_ACTIVE_LEVEL == DURA_FLOW_ACTIVE_LEVEL,
                        ESP_ERR_NOT_SUPPORTED, TAG,
                        "button and flow active levels must match for one EXT1 wake mode");

    (void)dura_board_sync_outputs();

    for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); ++i) {
        ESP_RETURN_ON_ERROR(config_rtc_wake_input(s_buttons[i].gpio, s_buttons[i].name,
                                                  CONFIG_DURA_BOARD_BUTTON_PULLUP,
                                                  CONFIG_DURA_BOARD_BUTTON_PULLDOWN),
                            TAG, "rtc config %s", s_buttons[i].name);
    }

    for (size_t i = 0; i < sizeof(s_flow_inputs) / sizeof(s_flow_inputs[0]); ++i) {
        s_sleep_touched_mask |= 1ULL << s_flow_inputs[i].gpio;
        ESP_RETURN_ON_ERROR(gpio_intr_disable(s_flow_inputs[i].gpio), TAG, "pause flow irq");
        ESP_RETURN_ON_ERROR(config_rtc_wake_input(s_flow_inputs[i].gpio, s_flow_inputs[i].name,
                                                  CONFIG_DURA_BOARD_FLOW_PULLUP,
                                                  CONFIG_DURA_BOARD_FLOW_PULLDOWN),
                            TAG, "rtc config %s", s_flow_inputs[i].name);
    }

    s_sleep_ext1_attempted = true;
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext1_wakeup(wake_mask,
                                                     CONFIG_DURA_BOARD_BUTTON_ACTIVE_LOW ?
                                                     ESP_EXT1_WAKEUP_ANY_LOW : ESP_EXT1_WAKEUP_ANY_HIGH),
                        TAG, "enable ext1 button/flow wake");
    return ESP_OK;
}

static esp_err_t restore_deep_sleep_inputs(void)
{
    esp_err_t result = ESP_OK;
    if (s_sleep_ext1_attempted) {
        const esp_err_t e = esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT1);
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) result = e;
    }
    for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]) +
                           sizeof(s_flow_inputs) / sizeof(s_flow_inputs[0]); ++i) {
        const bool button = i < sizeof(s_buttons) / sizeof(s_buttons[0]);
        const gpio_num_t pin = button ? s_buttons[i].gpio :
            s_flow_inputs[i - sizeof(s_buttons) / sizeof(s_buttons[0])].gpio;
        if (!(s_sleep_touched_mask & (1ULL << pin))) continue;
        esp_err_t e = rtc_gpio_hold_dis(pin);
        if (e != ESP_OK) { result = e; continue; }
        e = rtc_gpio_deinit(pin);
        if (e != ESP_OK) { result = e; continue; }
        const gpio_config_t input = {
            .pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_INPUT,
            .pull_up_en = (button ? CONFIG_DURA_BOARD_BUTTON_PULLUP : CONFIG_DURA_BOARD_FLOW_PULLUP) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
            .pull_down_en = (button ? CONFIG_DURA_BOARD_BUTTON_PULLDOWN : CONFIG_DURA_BOARD_FLOW_PULLDOWN) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        e = gpio_config(&input);
        if (e == ESP_OK && !button) e = gpio_set_intr_type(pin, GPIO_INTR_ANYEDGE);
        if (e == ESP_OK && !button) e = gpio_intr_enable(pin);
        if (e != ESP_OK) result = e;
    }
    if (result == ESP_OK) {
        s_sleep_touched_mask = 0;
        s_sleep_ext1_attempted = false;
    }
    return result;
}

static bool sleep_attempt_current(uint64_t generation)
{
    portENTER_CRITICAL(&s_ui_snapshot_mux);
    const bool current = !s_ble_connected && s_activity_generation == generation;
    portEXIT_CRITICAL(&s_ui_snapshot_mux);
    if (!current) return false;
    if (s_button_queue && uxQueueMessagesWaiting(s_button_queue)) return false;
    if (s_flow_event_queue && uxQueueMessagesWaiting(s_flow_event_queue)) return false;
    for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); ++i) {
        if (s_sleep_touched_mask & (1ULL << s_buttons[i].gpio)) {
            if (rtc_gpio_get_level(s_buttons[i].gpio) == DURA_BUTTON_ACTIVE_LEVEL) return false;
        } else if (dura_board_poll_button() != DURA_BUTTON_NONE) return false;
    }
    return true;
}

esp_err_t dura_board_enter_deep_sleep(void)
{
    /* Diagnostic callers request owner execution; never race UI/hold locals. */
    if (s_task == NULL) return ESP_ERR_INVALID_STATE;
    if (xTaskGetCurrentTaskHandle() != s_task) {
        portENTER_CRITICAL(&s_ui_snapshot_mux);
        s_sleep_requested = true;
        portEXIT_CRITICAL(&s_ui_snapshot_mux);
        return ESP_OK;
    }
    if (s_sleep_recovery_pending) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_ui_snapshot_mux);
    const uint64_t generation = s_activity_generation;
    portEXIT_CRITICAL(&s_ui_snapshot_mux);
    if (!sleep_attempt_current(generation)) return ESP_ERR_INVALID_STATE;
    s_sleep_recovery_pending = true;
    /* Meter applies OFF before save/log/RTC calls and fences late activation. */
    esp_err_t err = dura_meter_prepare_for_sleep();
    if (err != ESP_OK) goto recover;
    if (!sleep_attempt_current(generation)) { err = ESP_ERR_INVALID_STATE; goto recover; }
    err = prepare_deep_sleep_inputs();
    if (err != ESP_OK) goto recover;
    if (!sleep_attempt_current(generation)) { err = ESP_ERR_INVALID_STATE; goto recover; }
    /* Host-serialized disconnected cutoff; a new racing link may be lost. */
    err = gama_blefob_sleep_shutdown();
    if (err != ESP_OK) goto recover;
    if (!sleep_attempt_current(generation)) { err = ESP_ERR_INVALID_STATE; goto recover; }
    (void)dura_lcd_set_backlight(false);
    /* Reassert inactive levels on enabled outputs immediately before sleep,
     * independently of meter state. */
#if CONFIG_DURA_BOARD_ENABLE_PUMP_OUTPUT
    gpio_set_level(DURA_GPIO_PUMP_OUTPUT, DURA_PUMP_OUTPUT_INACTIVE_LEVEL);
#endif
#if CONFIG_DURA_BOARD_ENABLE_EV_OUTPUTS
    gpio_set_level(DURA_GPIO_RECIRC_EV_OUTPUT, DURA_RECIRC_EV_INACTIVE_LEVEL);
    gpio_set_level(DURA_GPIO_INJECT_EV_OUTPUT, DURA_INJECT_EV_INACTIVE_LEVEL);
#endif
    err = esp_deep_sleep_try_to_start();
    /* Success never returns. A wake-source rejection must roll back. */
    if (err == ESP_OK) err = ESP_ERR_INVALID_STATE;
recover:
    {
        const esp_err_t restored = restore_deep_sleep_inputs();
        const esp_err_t radio_restored = gama_blefob_sleep_resume();
        s_ignore_buttons_until_release = true;
        dura_board_note_activity();
        /* Recovery is not user activity: preserve OFF and an expired timer,
         * without even a transient ON write during rejected-sleep rollback. */
        update_backlight();
        if (restored != ESP_OK) {
            ESP_LOGE(TAG, "sleep input recovery failed; activation remains fenced: %s", esp_err_to_name(restored));
            return restored;
        }
        if (radio_restored != ESP_OK) {
            ESP_LOGE(TAG, "sleep radio recovery failed; activation remains fenced: %s", esp_err_to_name(radio_restored));
            return radio_restored;
        }
    }
    /* Reconcile from safe current meter state, without changing UI selection. */
    const esp_err_t rendered = dura_board_render_debug_screen();
    if (rendered != ESP_OK) return rendered; /* remain fenced */
    dura_meter_snapshot_t safe_meter;
    if (dura_meter_get_snapshot(&safe_meter) == ESP_OK) publish_ui_snapshot(&safe_meter);
    (void)dura_meter_cancel_sleep_prepare();
    s_sleep_recovery_pending = false;
    return err;
}

/* Runs only through the meter's serialized task-context intent handoff.
 * No snapshot round-trip, blocking driver calls, logs or meter re-entry here.
 * OFF drops the pump first; ON establishes the valve intent before the pump. */
static void apply_meter_outputs(dura_output_intent_t intent)
{
#if CONFIG_DURA_BOARD_ENABLE_PUMP_OUTPUT
    if (!intent.pump) gpio_set_level(DURA_GPIO_PUMP_OUTPUT, DURA_PUMP_OUTPUT_INACTIVE_LEVEL);
#endif
#if CONFIG_DURA_BOARD_ENABLE_EV_OUTPUTS
    gpio_set_level(DURA_GPIO_RECIRC_EV_OUTPUT,
                   intent.recirc_ev ? DURA_RECIRC_EV_ACTIVE_LEVEL : DURA_RECIRC_EV_INACTIVE_LEVEL);
    gpio_set_level(DURA_GPIO_INJECT_EV_OUTPUT,
                   intent.inject_ev ? DURA_INJECT_EV_ACTIVE_LEVEL : DURA_INJECT_EV_INACTIVE_LEVEL);
#endif
#if CONFIG_DURA_BOARD_ENABLE_PUMP_OUTPUT
    if (intent.pump) gpio_set_level(DURA_GPIO_PUMP_OUTPUT, DURA_PUMP_OUTPUT_ACTIVE_LEVEL);
#endif
    (void)intent;
}

esp_err_t dura_board_sync_outputs(void)
{
    return dura_meter_sync_outputs();
}

#if !CONFIG_DURA_BOARD_LCD_HOLD_SPLASH_ON_BOOT
static int16_t num40_desc_index(char ch)
{
    if (ch < '.' || ch > ':') {
        return -1;
    }
    return (int16_t)((ch - '.') * 2);
}

static uint8_t num40_char_width(char ch)
{
    const int16_t idx = num40_desc_index(ch);
    if (idx < 0) {
        return 0;
    }
    return (uint8_t)dura_asset_arialNarrow40ptCharDescriptors[idx];
}

static uint8_t num40_text_width(const char *text)
{
    uint8_t width = 0;
    while (text != NULL && *text != '\0') {
        const uint8_t ch_w = num40_char_width(*text++);
        if (ch_w != 0) {
            width = (uint8_t)(width + ch_w + 1u);
        }
    }
    return width;
}

static uint8_t draw_num40_char(uint8_t x, uint8_t y, char ch)
{
    const int16_t idx = num40_desc_index(ch);
    if (idx < 0) {
        return 0;
    }

    const uint8_t char_w = (uint8_t)dura_asset_arialNarrow40ptCharDescriptors[idx];
    uint16_t bitmap = dura_asset_arialNarrow40ptCharDescriptors[idx + 1];
    const uint8_t bytes_wide = (uint8_t)((char_w + 7u) / 8u);

    for (uint8_t row = 0; row < 40u; row++) {
        for (uint8_t col_byte = 0; col_byte < bytes_wide; col_byte++) {
            const uint8_t bits = dura_asset_arialNarrow40ptCharBitmaps[bitmap++];
            for (uint8_t bit = 0; bit < 8u; bit++) {
                const uint8_t px = (uint8_t)((col_byte * 8u) + bit);
                if (px < char_w) {
                    dura_lcd_set_pixel((int)x + px, (int)y + row, (bits & (uint8_t)(0x80u >> bit)) != 0u);
                }
            }
        }
    }
    return char_w;
}

static void __attribute__((unused)) draw_num40_right(uint8_t right_x, uint8_t y, const char *text)
{
    const uint8_t width = num40_text_width(text);
    uint8_t x = (width < right_x) ? (uint8_t)(right_x - width) : 0;
    while (text != NULL && *text != '\0') {
        const uint8_t w = draw_num40_char(x, y, *text++);
        if (w != 0) {
            x = (uint8_t)(x + w + 1u);
        }
    }
}

static int16_t num24_desc_index(char ch)
{
    if (ch < '.' || ch > '<') {
        return -1;
    }
    return (int16_t)((ch - '.') * 2);
}

static uint8_t draw_num24_char(uint8_t x, uint8_t y, char ch)
{
    const int16_t idx = num24_desc_index(ch);
    if (idx < 0) return dura_font_num_24.space_width;
    const uint8_t char_w = (uint8_t)dura_asset_arialNarrow24ptCharDescriptors[idx];
    uint16_t bitmap = dura_asset_arialNarrow24ptCharDescriptors[idx + 1];
    const uint8_t bytes_wide = (uint8_t)((char_w + 7u) / 8u);
    for (uint8_t row = 0; row < 23u; row++) {
        for (uint8_t col_byte = 0; col_byte < bytes_wide; col_byte++) {
            const uint8_t bits = dura_asset_arialNarrow24ptCharBitmaps[bitmap++];
            for (uint8_t bit = 0; bit < 8u; bit++) {
                const uint8_t px = (uint8_t)((col_byte * 8u) + bit);
                if (px < char_w) {
                    dura_lcd_set_pixel((int)x + px, (int)y + row, (bits & (uint8_t)(0x80u >> bit)) != 0u);
                }
            }
        }
    }
    return char_w;
}

static void draw_num24_text(uint8_t x, uint8_t y, const char *text)
{
    while (text != NULL && *text != '\0') {
        const uint8_t w = draw_num24_char(x, y, *text++);
        x = (uint8_t)(x + w + 1u);
    }
}

static uint8_t num24_text_width(const char *text)
{
    uint8_t width = 0;
    while (text != NULL && *text != '\0') {
        const int16_t idx = num24_desc_index(*text++);
        if (idx >= 0) {
            width = (uint8_t)(width +
                              (uint8_t)dura_asset_arialNarrow24ptCharDescriptors[idx] + 1u);
        }
    }
    return width;
}

static void draw_num24_right(uint8_t right_x, uint8_t y, const char *text)
{
    const uint8_t width = num24_text_width(text);
    const uint8_t x = (width < right_x) ? (uint8_t)(right_x - width) : 0u;
    draw_num24_text(x, y, text);
}


static const char *unit_label(dura_units_t units)
{
    switch (units) {
    case DURA_UNITS_LITER:
        return "Liters";
    case DURA_UNITS_OUNCE:
        return "Ounces";
    case DURA_UNITS_GALLON:
    default:
        return "Gallons";
    }
}

typedef enum {
    DURA_UI_MAIN_MENU = 0,
    DURA_UI_HOME = DURA_UI_MAIN_MENU,
    DURA_UI_MANUAL,
    DURA_UI_RECIRC_EDIT,
    DURA_UI_RECIRC_RUN,
    DURA_UI_SETUP,
    DURA_UI_RESET_TOTALS,
    DURA_UI_RESET_HELP,
    DURA_UI_INFO_START,
    DURA_UI_INFO_HELP,
    DURA_UI_COMPANY,
    DURA_UI_SWV,
    DURA_UI_BATTERY,
    DURA_UI_CAL_START,
    DURA_UI_PRECAL_1,
    DURA_UI_PRECAL_2,
    DURA_UI_QUICK_CAL,
    DURA_UI_BUCKET_GAL,
    DURA_UI_BUCKET_LITER,
    DURA_UI_CAL_HELP_GAL,
    DURA_UI_CAL_HELP_LITER,
    DURA_UI_CAL_ADJUST,
    DURA_UI_CAL_SAVE_1,
    DURA_UI_CAL_SAVE_2,
    DURA_UI_BATCH_SET,
    DURA_UI_BATCH_RUN,
    DURA_UI_AUTO_BATCH_EDIT,
    DURA_UI_AUTO_BATCH_RUN,
    DURA_UI_BATCH_COMPLETE,
    DURA_UI_FLOW_ERROR,
    DURA_UI_SCREEN_COUNT,
} dura_ui_screen_t;

RTC_DATA_ATTR static dura_ui_screen_t s_ui_screen = DURA_UI_MAIN_MENU;
RTC_DATA_ATTR static uint8_t s_ui_setup_item;
RTC_DATA_ATTR static uint8_t s_ui_reset_item;
RTC_DATA_ATTR static uint8_t s_ui_help_page;
RTC_DATA_ATTR static uint8_t s_ui_info_page;
RTC_DATA_ATTR static float s_ui_batch_amount = DURA_AUTO_BATCH_DEFAULT_GAL;
RTC_DATA_ATTR static float s_ui_cal_measured_amount;
RTC_DATA_ATTR static bool s_ui_cal_measured_amount_valid;
RTC_DATA_ATTR static uint8_t s_ui_quick_ref = 17u;
static bool s_low_battery_blink_active;
static int64_t s_low_battery_blink_epoch_ms;

static bool battery_percentage_is_visible(const dura_battery_measurement_t *battery)
{
    dura_ui_runtime_config_t config;
    if (battery == NULL || dura_ui_config_get(&config) != ESP_OK) {
        return true;
    }

    const bool low = battery->valid &&
                     config.low_battery_threshold_percent > 0u &&
                     battery->percent < config.low_battery_threshold_percent;
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (!low) {
        s_low_battery_blink_active = false;
        return true;
    }
    if (!s_low_battery_blink_active) {
        s_low_battery_blink_active = true;
        s_low_battery_blink_epoch_ms = now_ms;
    }

    const uint64_t elapsed_ms = now_ms >= s_low_battery_blink_epoch_ms
                                    ? (uint64_t)(now_ms - s_low_battery_blink_epoch_ms)
                                    : 0u;
    return dura_ui_battery_percent_visible(
        battery->valid, battery->percent, elapsed_ms,
        config.low_battery_threshold_percent,
        config.low_battery_blink_on_ms,
        config.low_battery_blink_off_ms);
}

static void ui_legacy_ids(uint16_t *screen_id, uint16_t *menu_id)
{
    *screen_id = DURA_UI_LEGACY_SCREEN_EDIT;
    *menu_id = DURA_UI_LEGACY_MENU_NONE;
    switch (s_ui_screen) {
    case DURA_UI_HOME: *screen_id = DURA_UI_LEGACY_SCREEN_HOME; *menu_id = DURA_UI_LEGACY_MENU_HOME; break;
    case DURA_UI_MANUAL: *screen_id = DURA_UI_LEGACY_SCREEN_RUN; break;
    case DURA_UI_RECIRC_EDIT: *screen_id = DURA_UI_LEGACY_SCREEN_BATCH; *menu_id = DURA_UI_LEGACY_MENU_BATCH_SET; break;
    case DURA_UI_RECIRC_RUN: *screen_id = DURA_UI_LEGACY_SCREEN_BATCH; *menu_id = DURA_UI_LEGACY_MENU_BATCH_RUN; break;
    case DURA_UI_SETUP: *menu_id = s_ui_setup_item; break;
    case DURA_UI_RESET_TOTALS:
    case DURA_UI_RESET_HELP: *menu_id = DURA_UI_LEGACY_MENU_METER_RESET + s_ui_reset_item; break;
    case DURA_UI_INFO_START: *screen_id = DURA_UI_LEGACY_SCREEN_INFO; *menu_id = DURA_UI_LEGACY_MENU_INFO_START; break;
    case DURA_UI_INFO_HELP: *screen_id = DURA_UI_LEGACY_SCREEN_INFO; *menu_id = DURA_UI_LEGACY_MENU_INFO_HELP_FIRST + (s_ui_info_page % 8u); break;
    case DURA_UI_COMPANY: *screen_id = DURA_UI_LEGACY_SCREEN_COMPANY; *menu_id = DURA_UI_LEGACY_MENU_COMPANY; break;
    case DURA_UI_SWV: *screen_id = DURA_UI_LEGACY_SCREEN_COMPANY; *menu_id = DURA_UI_LEGACY_MENU_SW_VERSION; break;
    case DURA_UI_BATTERY: *screen_id = DURA_UI_LEGACY_SCREEN_INFO; *menu_id = DURA_UI_LEGACY_MENU_HEALTH; break;
    case DURA_UI_CAL_START: *screen_id = DURA_UI_LEGACY_SCREEN_CALIBRATION; *menu_id = DURA_UI_LEGACY_MENU_CAL_START; break;
    case DURA_UI_PRECAL_1: *screen_id = DURA_UI_LEGACY_SCREEN_CALIBRATION; *menu_id = DURA_UI_LEGACY_MENU_PRECAL_FIRST; break;
    case DURA_UI_PRECAL_2: *screen_id = DURA_UI_LEGACY_SCREEN_CALIBRATION; *menu_id = DURA_UI_LEGACY_MENU_PRECAL_SECOND; break;
    case DURA_UI_QUICK_CAL: *screen_id = DURA_UI_LEGACY_SCREEN_CALIBRATION; *menu_id = DURA_UI_LEGACY_MENU_QUICK_CAL; break;
    case DURA_UI_BUCKET_GAL:
    case DURA_UI_CAL_HELP_GAL: *screen_id = DURA_UI_LEGACY_SCREEN_CALIBRATION; *menu_id = DURA_UI_LEGACY_MENU_BUCKET_GALLON; break;
    case DURA_UI_BUCKET_LITER:
    case DURA_UI_CAL_HELP_LITER: *screen_id = DURA_UI_LEGACY_SCREEN_CALIBRATION; *menu_id = DURA_UI_LEGACY_MENU_BUCKET_LITER; break;
    /* The recovered app renders measured-amount editing as EDIT/0x2c. */
    case DURA_UI_CAL_ADJUST: *screen_id = DURA_UI_LEGACY_SCREEN_EDIT; *menu_id = DURA_UI_LEGACY_MENU_CAL_ADJUST; break;
    case DURA_UI_CAL_SAVE_1: *screen_id = DURA_UI_LEGACY_SCREEN_CALIBRATION; *menu_id = DURA_UI_LEGACY_MENU_CAL_SAVE_FIRST; break;
    case DURA_UI_CAL_SAVE_2: *screen_id = DURA_UI_LEGACY_SCREEN_CALIBRATION; *menu_id = DURA_UI_LEGACY_MENU_CAL_SAVE_SECOND; break;
    case DURA_UI_BATCH_SET: *screen_id = DURA_UI_LEGACY_SCREEN_BATCH; *menu_id = DURA_UI_LEGACY_MENU_BATCH_SET; break;
    case DURA_UI_BATCH_RUN: *screen_id = DURA_UI_LEGACY_SCREEN_BATCH; *menu_id = DURA_UI_LEGACY_MENU_BATCH_RUN; break;
    case DURA_UI_AUTO_BATCH_EDIT: *screen_id = DURA_UI_LEGACY_SCREEN_BATCH; *menu_id = DURA_UI_LEGACY_MENU_AUTO_BATCH_EDIT; break;
    case DURA_UI_AUTO_BATCH_RUN: *screen_id = DURA_UI_LEGACY_SCREEN_BATCH; *menu_id = DURA_UI_LEGACY_MENU_AUTO_BATCH_RUN; break;
    case DURA_UI_BATCH_COMPLETE: *screen_id = DURA_UI_LEGACY_SCREEN_BATCH; *menu_id = DURA_UI_LEGACY_MENU_BATCH_COMPLETE; break;
    case DURA_UI_FLOW_ERROR: *screen_id = DURA_UI_LEGACY_SCREEN_BATCH; *menu_id = DURA_UI_LEGACY_MENU_FLOW_ERROR; break;
    default: break;
    }
}

static void publish_ui_snapshot(const dura_meter_snapshot_t *meter)
{
    dura_board_ui_snapshot_t next = {0};
    dura_board_ui_change_cb_t callback = NULL;
    void *context = NULL;
    ui_legacy_ids(&next.legacy_screen_id, &next.legacy_menu_id);
    next.selected_units = (uint8_t)meter->selected_units;
    next.backlight_timeout_sec = meter->backlight_timeout_sec;
    next.batch_mode = (uint8_t)meter->batch_mode;
    next.calibration_mode = (uint8_t)meter->calibration_mode;
    next.pump_enabled = meter->pump_enabled;
    next.fault_latched = meter->fault_latched;
    next.show_flow_rate = meter->show_flow_rate;
    next.show_auto_batch = meter->show_auto_batch;
    next.show_batch_total = meter->show_batch_total;
    next.fault_code = meter->fault_code;
    next.calibration_counts = meter->calibration_counts;
    next.meter_total = meter->meter_total;
    next.batch_total = meter->batch_total;
    next.total_total = meter->total_total;
    next.remaining_batch = meter->remaining_batch;
    next.flow_rate = meter->flow_rate;
    next.batch_amount = s_ui_batch_amount;
    next.calibration_measured_amount = s_ui_cal_measured_amount_valid ? s_ui_cal_measured_amount : 0.0f;

    portENTER_CRITICAL(&s_ui_snapshot_mux);
    next.generation = s_ui_snapshot.generation;
    if (!s_ui_snapshot_valid || memcmp(&next, &s_ui_snapshot, sizeof(next)) != 0) {
        next.generation = s_ui_snapshot.generation + 1u;
        s_ui_snapshot = next;
        s_ui_snapshot_valid = true;
        callback = s_ui_change_cb;
        context = s_ui_change_context;
    }
    portEXIT_CRITICAL(&s_ui_snapshot_mux);
    if (callback != NULL) callback(&next, context);
}

static uint8_t legacy_font_char_width(const dura_lcd_font_asset_t *font, char ch)
{
    const uint16_t glyph = (uint8_t)ch;
    const uint16_t first = font ? font->first_char : 0u;
    const uint16_t last_exclusive = font ? (uint16_t)(font->first_char + font->char_count) : 0u;
    if (font == NULL || font->descriptors == NULL ||
        glyph < first || glyph >= last_exclusive) {
        return font ? font->space_width : 0u;
    }
    const uint16_t idx = (uint16_t)((glyph - first) * 2u);
    if ((idx + 1u) >= font->descriptors_len) return font->space_width;
    return (uint8_t)font->descriptors[idx];
}

static uint8_t legacy_text_width(const dura_lcd_font_asset_t *font, const char *text)
{
    uint16_t width = 0;
    while (text != NULL && *text != '\0') {
        uint8_t ch_w = legacy_font_char_width(font, *text++);
        if (ch_w == 0u) ch_w = font ? font->space_width : 0u;
        width = (uint16_t)(width + ch_w + 1u);
        if (width > 255u) return 255u;
    }
    return (uint8_t)width;
}

static uint8_t draw_legacy_font_char(int x, int y, char ch, bool inverted)
{
    const dura_lcd_font_asset_t *font = &dura_font_ui_small;
    const uint16_t first = font->first_char;
    const uint16_t last_exclusive = (uint16_t)(font->first_char + font->char_count);
    uint16_t glyph = (uint8_t)ch;
    if ((uint8_t)ch <= ' ') {
        return font->space_width;
    }
    if (glyph < first || glyph >= last_exclusive) {
        glyph = '?';
    }

    const uint16_t desc_idx = (uint16_t)((glyph - first) * 2u);
    if ((desc_idx + 1u) >= font->descriptors_len) return font->space_width;
    const uint8_t char_w = (uint8_t)font->descriptors[desc_idx];
    uint16_t bitmap = font->descriptors[desc_idx + 1u];
    const uint8_t bytes_wide = (uint8_t)((char_w + 7u) / 8u);

    for (uint8_t row = 0; row < font->height; row++) {
        uint8_t remaining = (uint8_t)(char_w + 1u);
        for (uint8_t col_byte = 0; col_byte < bytes_wide; col_byte++) {
            if (bitmap >= font->bitmaps_len) return char_w;
            const uint8_t bits = font->bitmaps[bitmap++];
            const uint8_t bits_this_byte = remaining > 8u ? 8u : remaining;
            for (uint8_t bit = 0; bit < bits_this_byte; bit++) {
                const bool fg = (bits & (uint8_t)(0x80u >> bit)) != 0u;
                dura_lcd_set_pixel(x + (col_byte * 8) + bit, y + row, inverted ? !fg : fg);
            }
            if (remaining > 8u) remaining = (uint8_t)(remaining - 8u);
        }
        // Legacy renderer clears one cleanup column past each glyph edge.
        dura_lcd_set_pixel(x + char_w + 1, y + row, inverted);
    }
    return char_w;
}

static int draw_legacy_text_aligned(char orient, int x, int y, const char *text, bool inverted)
{
    int xa = x;
    if (orient == 'C' || orient == 'R') {
        const int w = legacy_text_width(&dura_font_ui_small, text);
        if (orient == 'C') xa = x - (w / 2) + 1;
        else xa = x - w;
    }

    while (text != NULL && *text != '\0') {
        if ((uint8_t)*text <= ' ') {
            xa += dura_font_ui_small.space_width;
        } else {
            xa += draw_legacy_font_char(xa, y, *text, inverted) + 1;
        }
        text++;
    }
    return xa;
}

static uint8_t draw_font_asset_char(const dura_lcd_font_asset_t *font,
                                    int x, int y, char ch, bool inverted)
{
    if (font == NULL) return 0u;

    const uint16_t first = font->first_char;
    const uint16_t last_exclusive = (uint16_t)(font->first_char + font->char_count);
    uint16_t glyph = (uint8_t)ch;
    if ((uint8_t)ch <= ' ') return font->space_width;
    if (glyph < first || glyph >= last_exclusive) glyph = '?';

    const uint16_t desc_idx = (uint16_t)((glyph - first) * 2u);
    if ((desc_idx + 1u) >= font->descriptors_len) return font->space_width;
    const uint8_t char_w = (uint8_t)font->descriptors[desc_idx];
    uint16_t bitmap = font->descriptors[desc_idx + 1u];
    const uint8_t bytes_wide = (uint8_t)((char_w + 7u) / 8u);

    for (uint8_t row = 0; row < font->height; ++row) {
        for (uint8_t col_byte = 0; col_byte < bytes_wide; ++col_byte) {
            if (bitmap >= font->bitmaps_len) return char_w;
            const uint8_t bits = font->bitmaps[bitmap++];
            const uint8_t first_bit = (uint8_t)(col_byte * 8u);
            const uint8_t bits_this_byte = (uint8_t)((char_w - first_bit) > 8u ?
                                                     8u : (char_w - first_bit));
            for (uint8_t bit = 0; bit < bits_this_byte; ++bit) {
                const bool fg = (bits & (uint8_t)(0x80u >> bit)) != 0u;
                dura_lcd_set_pixel(x + first_bit + bit, y + row, inverted ? !fg : fg);
            }
        }
        dura_lcd_set_pixel(x + char_w + 1, y + row, inverted);
    }
    return char_w;
}

static int draw_font_asset_text_aligned(const dura_lcd_font_asset_t *font,
                                        char orient, int x, int y,
                                        const char *text, bool inverted)
{
    if (font == NULL) return x;

    int xa = x;
    if (orient == 'C' || orient == 'R') {
        const int w = legacy_text_width(font, text);
        if (orient == 'C') xa = x - (w / 2) + 1;
        else xa = x - w;
    }

    while (text != NULL && *text != '\0') {
        if ((uint8_t)*text <= ' ') xa += font->space_width;
        else xa += draw_font_asset_char(font, xa, y, *text, inverted) + 1;
        ++text;
    }
    return xa;
}

static void draw_legacy_rounded_box(int x, int y, int w, int h)
{
    dura_lcd_draw_rect(x, y, w, h, true);
    dura_lcd_set_pixel(x, y, false);
    dura_lcd_set_pixel(x + 1, y + 1, true);
    dura_lcd_set_pixel(x, y + h - 1, false);
    dura_lcd_set_pixel(x + 1, y + h - 2, true);
    dura_lcd_set_pixel(x + w - 1, y, false);
    dura_lcd_set_pixel(x + w - 2, y + 1, true);
    dura_lcd_set_pixel(x + w - 1, y + h - 1, false);
    dura_lcd_set_pixel(x + w - 2, y + h - 2, true);
}

static esp_err_t draw_boot_powering_up_popup(void)
{
    const char *message = "Powering up";

    /* Display.mbas greyout(1, 1, 125, 62, WHITE_COLOR): preserve the
     * logo in alternating cells while clearing the checkerboard complement. */
    for (int i = 1; i < (1 + 125); i += 2) {
        for (int j = 1; j < (1 + 62 - 1); j += 2) {
            dura_lcd_set_pixel(i, j, false);
        }
    }
    for (int i = 2; i < (2 + 125); i += 2) {
        for (int j = 2; j < (2 + 62); j += 2) {
            dura_lcd_set_pixel(i, j, false);
        }
    }

    const int h = 11 + 5;
    const int w = legacy_text_width(&dura_font_ui_small, message) + 5;
    const int y = (DURA_LCD_HEIGHT - h) / 2;
    const int x = (DURA_LCD_WIDTH - w) / 2;
    dura_lcd_fill_rect(x, y, w, h, false);
    draw_legacy_text_aligned('C', 64, y + 3, message, false);
    draw_legacy_rounded_box(x, y, w, h);
    draw_legacy_rounded_box(x - 1, y - 1, w + 2, h + 2);
    return dura_lcd_flush();
}

static void draw_legacy_button(int slot, const char *label)
{
    if (label == NULL || label[0] == '\0') return;
    static const int x0[] = {1, 32, 63, 94};
    static const int xc[] = {16, 47, 78, 109};
    draw_legacy_text_aligned('C', xc[slot], 53, label, false);
    dura_lcd_draw_rect(x0[slot], 52, 31, 12, true);
}

static void draw_legacy_buttons(const char *b0, const char *b1, const char *b2, const char *b3)
{
    draw_legacy_button(0, b0);
    draw_legacy_button(1, b1);
    draw_legacy_button(2, b2);
    draw_legacy_button(3, b3);
}

static void draw_legacy_titles(const char *l1, const char *l2, const char *l3, const char *l4, const char *l5)
{
    if (l1) draw_legacy_text_aligned('C', 64, 2, l1, false);
    if (l2) draw_legacy_text_aligned('C', 64, 12, l2, false);
    if (l3) draw_legacy_text_aligned('C', 64, 22, l3, false);
    if (l4) draw_legacy_text_aligned('C', 64, 32, l4, false);
    if (l5) draw_legacy_text_aligned('C', 64, 42, l5, false);
}

static void draw_legacy_value_box(const char *text)
{
    dura_lcd_draw_rect(14, 30, 100, 14, true);
    draw_legacy_text_aligned('C', 64, 32, text, false);
}

static void draw_legacy_menu_screen(const char *l1, const char *l2, const char *l3, const char *l4, const char *l5,
                                    const char *b0, const char *b1, const char *b2, const char *b3)
{
    draw_legacy_titles(l1, l2, l3, l4, l5);
    draw_legacy_buttons(b0, b1, b2, b3);
}

static void draw_legacy_home(const dura_meter_snapshot_t *meter)
{
    const dura_battery_measurement_t battery = dura_battery_get_measurement();
    char power[20];
    (void)meter;

    if (battery.valid) {
        snprintf(power, sizeof(power), "Power: %u%%", (unsigned)battery.percent);
    } else {
        snprintf(power, sizeof(power), "Power: --%%");
    }

    /* Dan's 2026-09-10 font-only change uses the existing Comic Sans MS
     * 7 pt asset for the four instruction lines. Title, coordinates, power
     * font and button rendering remain unchanged. */
    dura_lcd_draw_bitmap_1bpp(0, 0, dura_asset_durametertitle_bmp_glcd_bmp, 128, 8, true);
    draw_font_asset_text_aligned(&dura_font_ui_small, 'C', 64, 9,
                                 "Hold outside buttons", false);
    draw_font_asset_text_aligned(&dura_font_ui_small, 'C', 64, 18,
                                 "for Program Settings.", false);
    draw_font_asset_text_aligned(&dura_font_ui_small, 'C', 64, 27,
                                 "Tote Auto Recirc:", false);
    draw_font_asset_text_aligned(&dura_font_ui_small, 'C', 64, 36,
                                 "auto recirculation.", false);
    draw_font_asset_text_aligned(&dura_font_ui_main_menu_10, 'L', 0, 44, power, false);
    draw_legacy_buttons("Man", "Calibr", "Recirc", "Auto");
}

static const char *language_label(dura_language_t language)
{
    switch (language) {
    case DURA_LANGUAGE_PORTUGUESE: return "Portuguese";
    case DURA_LANGUAGE_SPANISH: return "Spanish";
    case DURA_LANGUAGE_ENGLISH:
    default: return "English";
    }
}

static void render_setup(const dura_meter_snapshot_t *meter)
{
    char value[32];
    switch (s_ui_setup_item) {
    case 1:
        draw_legacy_menu_screen("Backlight", "Timeout", NULL, NULL, NULL, "Home", "Dn", "Up", "Next");
        if (meter->backlight_timeout_sec == DURA_BACKLIGHT_TIMEOUT_OFF) {
            draw_legacy_value_box("Off");
        } else {
            snprintf(value, sizeof(value), "%u  Secs", (unsigned)meter->backlight_timeout_sec);
            draw_legacy_value_box(value);
        }
        break;
    case 2:
        draw_legacy_menu_screen("Show Batch", "Total on Home", NULL, NULL, NULL, "Home", "Yes", "No", "Next");
        draw_legacy_value_box(meter->show_batch_total ? "Yes" : "No");
        break;
    case 3:
        draw_legacy_menu_screen("Select Units", NULL, NULL, NULL, NULL, "Gal", "Liter", "Oz", "Next");
        snprintf(value, sizeof(value), "Current %s", unit_label(meter->selected_units));
        draw_legacy_value_box(value);
        break;
    case 4:
        draw_legacy_menu_screen("Show Flow", "Rate on Home", NULL, NULL, NULL, "Home", "Yes", "No", "Next");
        draw_legacy_value_box(meter->show_flow_rate ? "Yes" : "No");
        break;
    case 5:
        draw_legacy_menu_screen("Show Auto", "Batch Mode", NULL, NULL, NULL, "Home", "Yes", "No", "Next");
        draw_legacy_value_box(meter->show_auto_batch ? "Yes" : "No");
        break;
    case 7:
        draw_legacy_menu_screen("Language", "Select", NULL, NULL, NULL, "EN", "PT", "SP", "Next");
        draw_legacy_value_box(language_label(meter->selected_language));
        break;
    case 8:
        draw_legacy_menu_screen("Reset Meter?", "Factory", "defaults", NULL, NULL, "Home", "Yes", NULL, "Next");
        break;
    case 9:
        draw_legacy_menu_screen("Confirm Reset", "Are you sure?", NULL, NULL, NULL, "Home", "Yes", NULL, "Home");
        break;
    case 48:
        draw_legacy_menu_screen("Dura Products", "Dura Meter", "Made in U.S.A.", NULL, NULL, "Home", NULL, "Help", "Next");
        break;
    case 50:
        draw_legacy_menu_screen("Dura Meter", "Information", "Scan for docs", NULL, NULL, "Home", NULL, "Help", "SWV");
        break;
    case 53:
        draw_legacy_menu_screen("Version", DURA_METER_FIRMWARE_VERSION, CONFIG_DURA_BOARD_NAME, NULL, NULL, "Home", NULL, "Help", "Home");
        break;
    case 0:
    default:
        draw_legacy_menu_screen("Meter", "Timeout", NULL, NULL, NULL, "Home", "Dn", "Up", "Next");
        snprintf(value, sizeof(value), "%u  Secs", (unsigned)meter->meter_timeout_sec);
        draw_legacy_value_box(value);
        break;
    }
}

static void render_reset_totals(const dura_meter_snapshot_t *meter)
{
    char value[24];
    const char *title = "Meter Amount";
    float amount = meter->meter_total;
    if (s_ui_reset_item == 1u) {
        title = "Batch Total";
        amount = meter->batch_total;
    } else if (s_ui_reset_item == 2u) {
        title = "Total Total";
        amount = meter->total_total;
    }
    snprintf(value, sizeof(value), "%.1f", (double)amount);
    draw_legacy_menu_screen(title, "Reset?", NULL, NULL, NULL, "Home", "Reset", "Help", "Next");
    draw_legacy_value_box(value);
}

static void render_reset_help(void)
{
    if (s_ui_reset_item == 0u) {
        draw_legacy_menu_screen("Meter reset", "clears current", "amount only", NULL, NULL, "Back", NULL, NULL, NULL);
    } else if (s_ui_reset_item == 1u) {
        draw_legacy_menu_screen("Batch reset", "clears batch", "total only", NULL, NULL, "Back", NULL, NULL, NULL);
    } else {
        draw_legacy_menu_screen("Total reset", "clears total", "counter only", NULL, NULL, "Back", NULL, NULL, NULL);
    }
}

static void render_info_help(void)
{
    static const char *const pages[][5] = {
        {"Meter Info", "Amount metered", "batch total", "and total", NULL},
        {"Flow Info", "Flow rate shown", "when enabled", NULL, NULL},
        {"Units", "Gallons liters", "or ounces", NULL, NULL},
        {"Battery", "Power level", "shown at bottom", NULL, NULL},
        {"Calibration", "Use Cal menu", "for bucket cal", NULL, NULL},
        {"Batch", "Preset batch", "starts pump", NULL, NULL},
        {"Support", "Contact Dura", "for service", NULL, NULL},
        {"More Info", "Use More to", "cycle pages", NULL, NULL},
    };
    const uint8_t page = s_ui_info_page % 8u;
    draw_legacy_menu_screen(pages[page][0], pages[page][1], pages[page][2], pages[page][3], pages[page][4],
                            "Home", NULL, "More", "Info");
}

static void render_info_start(const dura_meter_snapshot_t *meter)
{
    char line1[32];
    char line2[32];
    snprintf(line1, sizeof(line1), "Amount: %.2f", (double)meter->meter_total);
    snprintf(line2, sizeof(line2), "Flow: %.2f", (double)meter->flow_rate);
    draw_legacy_menu_screen("Meter Info", line1, line2, unit_label(meter->selected_units), NULL,
                            "Home", "Scan", "Help", "Next");
}

static void render_company(void)
{
    draw_legacy_menu_screen("Dura Products", "Dura Meter", "Made in U.S.A.", "USA", NULL,
                            "Home", NULL, "Help", "SWV");
}

static void render_swv(const dura_meter_snapshot_t *meter)
{
    char line[32];
    snprintf(line, sizeof(line), "%.0f %s", (double)meter->total_total, unit_label(meter->selected_units));
    draw_legacy_menu_screen("Version", DURA_METER_FIRMWARE_VERSION, line, CONFIG_DURA_BOARD_NAME, NULL,
                            "Home", NULL, "Help", "Battery");
}

static void render_battery(void)
{
    const dura_battery_measurement_t battery = dura_battery_get_measurement();
    const bool show_battery_percentage = battery_percentage_is_visible(&battery);
    char power[20];
    char voltage[24];
    if (battery.valid) {
        snprintf(power, sizeof(power), "Power: %u%%", (unsigned)battery.percent);
    } else {
        snprintf(power, sizeof(power), "Power: -- %%");
    }
    if (!dura_battery_format_voltage(voltage, sizeof(voltage), &battery)) {
        snprintf(voltage, sizeof(voltage), "Voltage: --.-- V");
    }
    draw_legacy_menu_screen("Battery", show_battery_percentage ? power : NULL, voltage, NULL, NULL,
                            "Home", NULL, "Help", "More");
}

static void render_cal_start(void)
{
    draw_legacy_menu_screen("Select 'Calibr' to calibrate",
                            "and save as a 'PreCal' ref #",
                            "or select a cal ref # from", "'Quick'", NULL,
                            "Home", "Calibr", "PreCal", "Quick");
}

static void render_precal(const dura_meter_snapshot_t *meter, uint8_t page)
{
    static const char *const names[DURA_PRECAL_PROFILE_COUNT] = {
        "Water", "Fluid1", "Fluid2", "Fluid3", "Fluid4", "Fluid5"
    };
    char current[24];
    snprintf(current, sizeof(current), "Current %s", names[meter->selected_precal_profile]);
    if (page == 0u) {
        draw_legacy_menu_screen("PreCal", "Select fluid", current, NULL, NULL,
                                "Water", "Fluid1", "Fluid2", "Next");
    } else {
        draw_legacy_menu_screen("PreCal", "Select fluid", current, NULL, NULL,
                                "Fluid3", "Fluid4", "Fluid5", "Cancel");
    }
}

static uint8_t calibration_progress_percent(uint32_t calibration_counts)
{
    if (calibration_counts >= DURA_MIN_FLOW_COUNTS) return 100u;
    return (uint8_t)((calibration_counts * 100u) / DURA_MIN_FLOW_COUNTS);
}

static uint8_t calibration_wheel_phase(uint32_t calibration_counts)
{
    return (uint8_t)(calibration_counts & 0x07u);
}

static void draw_calibration_wheel(int center_x, int center_y, uint32_t calibration_counts)
{
    const uint8_t phase = calibration_wheel_phase(calibration_counts);
    dura_lcd_draw_bitmap_1bpp(center_x - 11, center_y - 11,
                              dura_asset_calibration_wheel_frames[phase], 23, 23, true);
}

static void render_cal_bucket(const dura_meter_snapshot_t *meter, bool liters)
{
    char percent[4];
    (void)liters;
    snprintf(percent, sizeof(percent), "%u",
             (unsigned)calibration_progress_percent(meter->calibration_counts));
    draw_legacy_text_aligned('C', 64, 1, "CAL: Fill A Container", false);
    draw_legacy_text_aligned('C', 64, 12, "Minimum Calibration Amount", false);
    draw_calibration_wheel(27, 36, meter->calibration_counts);
    draw_num24_right(104, 25, percent);
    draw_legacy_text_aligned('L', 108, 34, "%", false);
    draw_legacy_buttons("Cancel", "Help", "Reset", "Cont");
}

static void render_cal_help(bool liters)
{
    static const char *const g_pages[][4] = {
        {"Gal bucket cal", "Dispense into", "5 Gal bucket", NULL},
        {"Start flow", "then press Cal", "to finish", NULL},
        {"Adjust amount", "save to fluid", "slot", NULL},
        {"Return", "goes back to", "bucket setup", NULL},
    };
    static const char *const l_pages[][4] = {
        {"Liter bucket", "Dispense into", "19 L bucket", NULL},
        {"Start flow", "then press Cal", "to finish", NULL},
        {"Adjust amount", "save to fluid", "slot", NULL},
        {"Return", "goes back to", "bucket setup", NULL},
    };
    const uint8_t page = s_ui_help_page % 4u;
    const char *const *p = liters ? l_pages[page] : g_pages[page];
    draw_legacy_menu_screen(p[0], p[1], p[2], p[3], NULL, "Return", "More", NULL, "Cal");
}

static void render_quick_cal(void)
{
    char ref[8];
    snprintf(ref, sizeof(ref), "%u", (unsigned)s_ui_quick_ref);
    draw_legacy_menu_screen(NULL, NULL, NULL, NULL, NULL,
                            "Cancel", "Dn", "Up", "Select");
    draw_legacy_text_aligned('C', 88, 2, "Quick Calibration", false);
    draw_legacy_text_aligned('C', 88, 12, "Fluid Reference", false);
    dura_lcd_draw_bitmap_1bpp(20, 2, dura_asset_thin_glcd_bmp, 13, 5, true);
    dura_lcd_draw_bitmap_1bpp(20, 45, dura_asset_thick_glcd_bmp, 17, 5, true);
    dura_lcd_draw_rect(46, 5, 1, 45, true);
    for (uint8_t i = 1; i <= 23; i++) {
        dura_lcd_fill_rect(8, i * 2, i / 3, 3, true);
    }
    dura_lcd_fill_rect(0, 1, 7, 50, false);
    dura_lcd_draw_bitmap_1bpp(0, (int)s_ui_quick_ref <= 23 ? (int)s_ui_quick_ref * 2 : 42,
                              dura_asset_thick_pointer, 7, 5, true);
    draw_legacy_text_aligned('C', 88, 25, "Reference #", false);
    draw_legacy_text_aligned('C', 88, 36, ref, false);
    /* Keep the large reference value right-aligned beside the x=46 divider. */
    draw_num24_right(46, 14, ref);
}

static void render_cal_adjust(const dura_meter_snapshot_t *meter)
{
    char line[24];
    (void)meter;
    snprintf(line, sizeof(line), "%.2f", (double)s_ui_cal_measured_amount);
    draw_legacy_menu_screen("Adjust", "Measured Amount", NULL, NULL, NULL,
                            "Cancel", "Dn", "Up", "Save");
    draw_legacy_value_box(line);
}

static const char *legacy_batch_unit_label(dura_units_t units)
{
    switch (units) {
    case DURA_UNITS_GALLON: return "Gallon";
    case DURA_UNITS_LITER: return "Litre";
    case DURA_UNITS_OUNCE: return "Ounce";
    default: return "";
    }
}

static float adjust_batch_amount_tenths(float amount, int delta_tenths)
{
    int amount_tenths = (int)lroundf(amount * 10.0f);
    amount_tenths += delta_tenths;
    if (amount_tenths < 10) amount_tenths = 10;
    return (float)amount_tenths / 10.0f;
}

static int volume_display_decimals(float amount, dura_units_t units)
{
    int decimals;
    switch (units) {
    case DURA_UNITS_LITER: decimals = CONFIG_DURA_BOARD_VOLUME_LITER_DECIMALS; break;
    case DURA_UNITS_OUNCE: decimals = CONFIG_DURA_BOARD_VOLUME_OUNCE_DECIMALS; break;
    case DURA_UNITS_GALLON:
    default: decimals = CONFIG_DURA_BOARD_VOLUME_GALLON_DECIMALS; break;
    }
    /* Display-unit cutoff is inclusive. Never increase a unit configured for
     * fewer decimals; this policy does not alter stored/metered quantities. */
    const float cutoff = (float)CONFIG_DURA_BOARD_VOLUME_2DP_LIMIT_TENTHS / 10.0f;
    return amount > cutoff && decimals > 1 ? 1 : decimals;
}

static void format_editable_batch_amount(char *text, size_t text_len,
                                          float amount, dura_units_t units)
{
    const int decimals = volume_display_decimals(amount, units);
    (void)snprintf(text, text_len, "%.*f", decimals, (double)amount);
}

static void format_legacy_batch_amount(char *text, size_t text_len,
                                       float amount, dura_units_t units)
{
    const int decimals = volume_display_decimals(amount, units);

    const double scale = decimals == 2 ? 100.0 : (decimals == 1 ? 10.0 : 1.0);
    const double truncated = trunc((double)amount * scale) / scale;
    (void)snprintf(text, text_len, "%.*f", decimals, truncated);

}

static void render_legacy_batch_amount(float amount, dura_units_t units)
{
    char text[24];
    format_legacy_batch_amount(text, sizeof(text), amount, units);
    draw_num24_text(2, 26, text);
    draw_legacy_text_aligned('R', 128, 40, legacy_batch_unit_label(units), false);
}

static void render_editable_batch_amount(float amount, dura_units_t units)
{
    char text[24];
    format_editable_batch_amount(text, sizeof(text), amount, units);
    draw_num24_text(2, 26, text);
    draw_legacy_text_aligned('R', 128, 40, legacy_batch_unit_label(units), false);
}

static void render_batch_edit(const dura_meter_snapshot_t *meter, bool auto_mode)
{
    draw_legacy_menu_screen("Enter the preset amount", "to be pumped.",
                            NULL, NULL, NULL,
                            auto_mode ? "Home" : "Reset", "Dn\x7f", "Up~", "Start");
    render_editable_batch_amount(s_ui_batch_amount, meter->selected_units);
}

static void render_batch_run(const dura_meter_snapshot_t *meter, bool auto_mode)
{
    draw_legacy_menu_screen("Press Start to begin.", "Press Stop to end.",
                            NULL, NULL, NULL,
                            auto_mode ? "Home" : "Stop", NULL,
                            auto_mode ? "Stop" : NULL, "Start");
    render_legacy_batch_amount(meter->remaining_batch, meter->selected_units);
}

static void render_manual_run(const dura_meter_snapshot_t *meter)
{
    char amount[24];
    char power[20];
    const dura_battery_measurement_t battery = dura_battery_get_measurement();

    format_legacy_batch_amount(amount, sizeof(amount), meter->meter_total, meter->selected_units);
    if (battery.valid) {
        snprintf(power, sizeof(power), "Power: %u%%", (unsigned)battery.percent);
    } else {
        snprintf(power, sizeof(power), "Power: -- %%");
    }

    dura_lcd_draw_bitmap_1bpp(0, 0, dura_asset_durametertitle_bmp_glcd_bmp, 128, 8, true);
    draw_num40_right(128, 12, amount);
    draw_legacy_text_aligned('L', 0, 42, power, false);
    draw_legacy_text_aligned('R', 128, 42, unit_label(meter->selected_units), false);
    draw_legacy_buttons("Home", "Reset", "Stop", "Start");
}

static void render_recirc_edit(const dura_meter_snapshot_t *meter)
{
    draw_legacy_menu_screen("Enter the preset amount", "to be recirculated in tote.", NULL, NULL, NULL,
                            "Home", "Dn\x7f", "Up~", "Start");
    render_editable_batch_amount(s_ui_batch_amount, meter->selected_units);
}

static void render_recirc_run(const dura_meter_snapshot_t *meter)
{
    draw_legacy_menu_screen("Press Start to begin.", "Press Stop to end.", NULL, NULL, NULL,
                            "Home", NULL, "Stop", "Start");
    render_legacy_batch_amount(meter->remaining_batch, meter->selected_units);
}

static void render_popup_confirm(const char *line1, const char *line2)
{
    draw_legacy_menu_screen(line1, line2, NULL, NULL, NULL, "Home", NULL, NULL, NULL);
}

typedef struct {
    int x;
    int y;
    int scale;
    int quiet;
} dura_qr_draw_ctx_t;

static const dura_qr_draw_ctx_t s_provisioning_qr_ctx = {
    .x = 0,
    .y = 3,
    .scale = 2,
    .quiet = 2,
};

static void draw_dynamic_qr_to_lcd(esp_qrcode_handle_t qrcode)
{
    const dura_qr_draw_ctx_t *ctx = &s_provisioning_qr_ctx;
    if (qrcode == NULL) return;

    const int modules = esp_qrcode_get_size(qrcode);
    const int size_px = (modules + (ctx->quiet * 2)) * ctx->scale;
    dura_lcd_fill_rect(ctx->x, ctx->y, size_px, size_px, false);

    for (int my = 0; my < modules; ++my) {
        for (int mx = 0; mx < modules; ++mx) {
            if (esp_qrcode_get_module(qrcode, mx, my)) {
                dura_lcd_fill_rect(ctx->x + ((mx + ctx->quiet) * ctx->scale),
                                   ctx->y + ((my + ctx->quiet) * ctx->scale),
                                   ctx->scale, ctx->scale, true);
            }
        }
    }
}

static esp_err_t format_provisioning_qr_payload(char *payload, size_t payload_len)
{
    dura_peers_snapshot_t peers = {0};
    ESP_RETURN_ON_FALSE(payload != NULL && payload_len > 0, ESP_ERR_INVALID_ARG, TAG, "bad QR payload buffer");
    ESP_RETURN_ON_ERROR(dura_peers_get_snapshot(&peers), TAG, "get peer snapshot for QR");

    const int written = snprintf(payload, payload_len,
                                 "dura://claim?model=dura_m3000&mac=%02X%02X%02X%02X%02X%02X&fw=%s",
                                 peers.local_mac[0], peers.local_mac[1], peers.local_mac[2],
                                 peers.local_mac[3], peers.local_mac[4], peers.local_mac[5],
                                 DURA_METER_FIRMWARE_VERSION);
    return (written > 0 && (size_t)written < payload_len) ? ESP_OK : ESP_ERR_NO_MEM;
}

static void render_provisioning_qr(void)
{
    // Boot-time provisioning is an optional app shortcut, not a required setup gate.
    // The QR remains visible until any local button is pressed; once dismissed, the
    // unit falls through to the normal meter UI and can run standalone.
    //
    // Physical QR contract for Scout:
    //   dura://claim?model=dura_m3000&mac=<WIFI_STA_MAC_NO_COLONS>&fw=<firmware>
    // The selected System remains app-owned; firmware only exposes unit identity so
    // Scout can add the scanned unit directly to the currently selected System.
    char payload[80];
    esp_err_t err = format_provisioning_qr_payload(payload, sizeof(payload));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "provisioning QR payload unavailable 0x%x", err);
        draw_legacy_menu_screen("Provisioning", "QR unavailable", "Use BLE Scan", NULL, NULL,
                                "Home", NULL, NULL, NULL);
        return;
    }

    if (s_provisioning_qr_rendered && strcmp(s_provisioning_qr_payload, payload) == 0) {
        return;
    }

    dura_lcd_clear();
    esp_log_level_set("QRCODE", ESP_LOG_WARN);

    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func = draw_dynamic_qr_to_lcd;
    cfg.max_qrcode_version = 2;
    cfg.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;

    err = esp_qrcode_generate(&cfg, payload);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "provisioning QR generation failed 0x%x", err);
        draw_legacy_menu_screen("Provisioning", "QR encode fail", "Use BLE Scan", NULL, NULL,
                                "Home", NULL, NULL, NULL);
        return;
    }

    strlcpy(s_provisioning_qr_payload, payload, sizeof(s_provisioning_qr_payload));
    s_provisioning_qr_rendered = true;

    draw_legacy_text_aligned('C', 96, 8, "SCAN QR", false);
    draw_legacy_text_aligned('C', 96, 20, "TO ADD", false);
    draw_legacy_text_aligned('C', 96, 32, "METER", false);
    draw_legacy_text_aligned('C', 96, 44, DURA_METER_FIRMWARE_VERSION, false);
}

static bool should_show_provisioning_qr(const dura_meter_snapshot_t *meter)
{
#if CONFIG_DURA_APP_M3000_LEGACY_ONLY
    (void)meter;
    return false;
#else
    return meter != NULL &&
           !s_provisioning_qr_dismissed &&
           meter->batch_mode != DURA_BATCH_RUNNING &&
           meter->batch_mode != DURA_BATCH_PAUSED &&
           meter->calibration_mode == DURA_CAL_IDLE;
#endif
}

esp_err_t dura_board_render_debug_screen(void)
{
    dura_meter_snapshot_t meter;
    esp_err_t err = dura_meter_get_snapshot(&meter);
    if (err != ESP_OK) return err;

    if (meter.operation_mode == DURA_OPERATION_MANUAL && meter.pump_enabled) {
        s_ui_screen = DURA_UI_MANUAL;
    } else if (meter.batch_mode == DURA_BATCH_DONE && s_ui_screen != DURA_UI_RECIRC_RUN) {
        s_ui_screen = DURA_UI_BATCH_COMPLETE;
    } else if (meter.batch_mode == DURA_BATCH_RUNNING || meter.batch_mode == DURA_BATCH_PAUSED) {
        if (meter.operation_mode == DURA_OPERATION_RECIRC) {
            s_ui_screen = DURA_UI_RECIRC_RUN;
        } else if (meter.operation_mode == DURA_OPERATION_AUTO) {
            s_ui_screen = DURA_UI_AUTO_BATCH_RUN;
        } else {
            s_ui_screen = meter.show_auto_batch ? DURA_UI_AUTO_BATCH_RUN : DURA_UI_BATCH_RUN;
        }
    } else if (meter.calibration_mode == DURA_CAL_RUNNING &&
               s_ui_screen != DURA_UI_CAL_HELP_GAL &&
               s_ui_screen != DURA_UI_CAL_HELP_LITER) {
        s_ui_screen = (meter.selected_units == DURA_UNITS_LITER) ? DURA_UI_BUCKET_LITER : DURA_UI_BUCKET_GAL;
    } else if (meter.calibration_mode == DURA_CAL_WAIT_MEASURED_AMOUNT &&
               s_ui_screen != DURA_UI_CAL_SAVE_1 && s_ui_screen != DURA_UI_CAL_SAVE_2) {
        if (!s_ui_cal_measured_amount_valid) {
            switch (meter.selected_units) {
            case DURA_UNITS_LITER: s_ui_cal_measured_amount = 19.0f; break;
            case DURA_UNITS_OUNCE: s_ui_cal_measured_amount = 640.0f; break;
            case DURA_UNITS_GALLON:
            default: s_ui_cal_measured_amount = 5.0f; break;
            }
            s_ui_cal_measured_amount_valid = true;
        }
        s_ui_screen = DURA_UI_CAL_ADJUST;
    }

    if (should_show_provisioning_qr(&meter)) {
        render_provisioning_qr();
        return dura_lcd_flush();
    }
    s_provisioning_qr_rendered = false;
    s_provisioning_qr_payload[0] = '\0';

    dura_lcd_clear();
    switch (s_ui_screen) {
    case DURA_UI_MANUAL: render_manual_run(&meter); break;
    case DURA_UI_RECIRC_EDIT: render_recirc_edit(&meter); break;
    case DURA_UI_RECIRC_RUN: render_recirc_run(&meter); break;
    case DURA_UI_SETUP: render_setup(&meter); break;
    case DURA_UI_RESET_TOTALS: render_reset_totals(&meter); break;
    case DURA_UI_RESET_HELP: render_reset_help(); break;
    case DURA_UI_INFO_START: render_info_start(&meter); break;
    case DURA_UI_INFO_HELP: render_info_help(); break;
    case DURA_UI_COMPANY: render_company(); break;
    case DURA_UI_SWV: render_swv(&meter); break;
    case DURA_UI_BATTERY: render_battery(); break;
    case DURA_UI_CAL_START: render_cal_start(); break;
    case DURA_UI_PRECAL_1: render_precal(&meter, 0); break;
    case DURA_UI_PRECAL_2: render_precal(&meter, 1); break;
    case DURA_UI_QUICK_CAL: render_quick_cal(); break;
    case DURA_UI_BUCKET_GAL: render_cal_bucket(&meter, false); break;
    case DURA_UI_BUCKET_LITER: render_cal_bucket(&meter, true); break;
    case DURA_UI_CAL_HELP_GAL: render_cal_help(false); break;
    case DURA_UI_CAL_HELP_LITER: render_cal_help(true); break;
    case DURA_UI_CAL_ADJUST: render_cal_adjust(&meter); break;
    case DURA_UI_CAL_SAVE_1: render_precal(&meter, 0); break;
    case DURA_UI_CAL_SAVE_2: render_precal(&meter, 1); break;
    case DURA_UI_BATCH_SET: render_batch_edit(&meter, false); break;
    case DURA_UI_BATCH_RUN: render_batch_run(&meter, false); break;
    case DURA_UI_AUTO_BATCH_EDIT: render_batch_edit(&meter, true); break;
    case DURA_UI_AUTO_BATCH_RUN: render_batch_run(&meter, true); break;
    case DURA_UI_BATCH_COMPLETE: render_popup_confirm("BATCH PRESET", "FINISHED"); break;
    case DURA_UI_FLOW_ERROR: render_popup_confirm("FLOW DETECTION", "ERROR"); break;
    case DURA_UI_HOME:
    default: draw_legacy_home(&meter); break;
    }
    return dura_lcd_flush();
}

static void ui_go_main(void)
{
    (void)dura_meter_set_screen_home();
    s_ui_screen = DURA_UI_MAIN_MENU;
}

static void ui_go_home(void)
{
    ui_go_main();
}

#if CONFIG_DURA_BOARD_ENABLE_RECIRC_INPUT && !CONFIG_DURA_BOARD_LCD_HOLD_SPLASH_ON_BOOT
static void handle_external_recirc_press(void)
{
    if (s_ui_screen != DURA_UI_MAIN_MENU) return;
    dura_board_note_user_activity();
    (void)dura_meter_set_screen_home();
    s_ui_batch_amount = DURA_RECIRC_BATCH_DEFAULT_GAL;
    s_ui_screen = DURA_UI_RECIRC_EDIT;
}
#endif

static void ui_cancel_calibration_to_start(void)
{
    if (dura_meter_cancel_calibration() == ESP_OK) {
        s_ui_cal_measured_amount_valid = false;
        s_ui_screen = DURA_UI_CAL_START;
    }
}

static void ui_cancel_batch_to_home(void)
{
    esp_err_t err = dura_meter_cancel_batch();
    if (err == ESP_OK) {
        ui_go_main();
        return;
    }

    /* Meter cancellation is intentionally pump-safe before NVS is written.
     * If persistence fails, follow the already-applied in-memory state instead
     * of leaving the controls stranded on Run/Complete with no valid action.
     * An already-idle Home (e.g. a dismissed error) is safe too. A rejected
     * cancellation must not navigate away from an operation still active. */
    dura_meter_snapshot_t updated;
    if (dura_meter_get_snapshot(&updated) == ESP_OK &&
        (updated.batch_mode == DURA_BATCH_EDIT || updated.batch_mode == DURA_BATCH_IDLE) &&
        updated.operation_mode == DURA_OPERATION_IDLE && updated.calibration_mode == DURA_CAL_IDLE &&
        !updated.pump_enabled && updated.recipe_owner_id == 0) {
        ESP_LOGW(TAG, "batch Home follows safe RAM state after cancel error: %s", esp_err_to_name(err));
        ui_go_main();
    } else {
        ESP_LOGE(TAG, "batch cancel failed: %s", esp_err_to_name(err));
    }
}

static uint8_t next_setup_item(uint8_t item)
{
    switch (item) {
    case 0: return 1;
    case 1: return 2;
    case 2: return 3;
    case 3: return 4;
    case 4: return 5;
    case 5: return 7;   // AVR item 6 Reset2Zero is present but skipped in normal navigation.
    case 7: return 8;
    case 8: return 48;  // Factory reset first prompt Next skips confirm and enters add-on/company info.
    case 48: return 50;
    case 50: return 53;
    default: return 0;
    }
}

static void reset_current_total(void)
{
    if (s_ui_reset_item == 0u) {
        (void)dura_meter_reset_meter_total();
    } else if (s_ui_reset_item == 1u) {
        (void)dura_meter_reset_batch_total();
    } else {
        (void)dura_meter_reset_total_total();
    }
}

static void handle_setup_button(dura_button_t b, const dura_meter_snapshot_t *meter)
{
    if (b == DURA_BUTTON_UP && s_ui_setup_item != 3u && s_ui_setup_item != 7u) {
        ui_go_home();
        return;
    }
    if (s_ui_setup_item == 0u) {
        if (b == DURA_BUTTON_DOWN && meter->meter_timeout_sec > DURA_METER_TIMEOUT_MIN_SEC) {
            (void)dura_meter_set_meter_timeout((uint8_t)(meter->meter_timeout_sec - 1u));
        } else if (b == DURA_BUTTON_SELECT && meter->meter_timeout_sec < DURA_METER_TIMEOUT_MAX_SEC) {
            (void)dura_meter_set_meter_timeout((uint8_t)(meter->meter_timeout_sec + 1u));
        }
    } else if (s_ui_setup_item == 1u) {
        if (b == DURA_BUTTON_DOWN && meter->backlight_timeout_sec > DURA_BACKLIGHT_TIMEOUT_OFF) {
            (void)dura_meter_set_backlight_timeout((uint8_t)(meter->backlight_timeout_sec - 1u));
        } else if (b == DURA_BUTTON_SELECT && meter->backlight_timeout_sec < DURA_BACKLIGHT_TIMEOUT_MAX_SEC) {
            (void)dura_meter_set_backlight_timeout((uint8_t)(meter->backlight_timeout_sec + 1u));
        }
    } else if (s_ui_setup_item == 2u) {
        if (b == DURA_BUTTON_DOWN) (void)dura_meter_set_show_batch_total(true);
        else if (b == DURA_BUTTON_SELECT) (void)dura_meter_set_show_batch_total(false);
    } else if (s_ui_setup_item == 3u) {
        if (b == DURA_BUTTON_UP) (void)dura_meter_set_units(DURA_UNITS_GALLON);
        else if (b == DURA_BUTTON_DOWN) (void)dura_meter_set_units(DURA_UNITS_LITER);
        else if (b == DURA_BUTTON_SELECT) (void)dura_meter_set_units(DURA_UNITS_OUNCE);
    } else if (s_ui_setup_item == 4u) {
        if (b == DURA_BUTTON_DOWN) (void)dura_meter_set_show_flow_rate(true);
        else if (b == DURA_BUTTON_SELECT) (void)dura_meter_set_show_flow_rate(false);
    } else if (s_ui_setup_item == 5u) {
        if (b == DURA_BUTTON_DOWN) (void)dura_meter_set_auto_batch(true);
        else if (b == DURA_BUTTON_SELECT) (void)dura_meter_set_auto_batch(false);
    } else if (s_ui_setup_item == 7u) {
        if (b == DURA_BUTTON_UP) (void)dura_meter_set_language(DURA_LANGUAGE_ENGLISH);
        else if (b == DURA_BUTTON_DOWN) (void)dura_meter_set_language(DURA_LANGUAGE_PORTUGUESE);
        else if (b == DURA_BUTTON_SELECT) (void)dura_meter_set_language(DURA_LANGUAGE_SPANISH);
    } else if (s_ui_setup_item == 8u && b == DURA_BUTTON_DOWN) {
        s_ui_setup_item = 9u;
        return;
    } else if (s_ui_setup_item == 9u && b == DURA_BUTTON_DOWN) {
        (void)dura_meter_factory_defaults();
        (void)dura_meter_save();
        ui_go_home();
        return;
    }

    if (b == DURA_BUTTON_BACK) {
        s_ui_setup_item = next_setup_item(s_ui_setup_item);
        if (s_ui_setup_item == 0u) ui_go_home();
    }
    (void)meter;
}

static bool choose_precal_fluid(uint8_t profile_index)
{
    if (dura_meter_select_precal_profile(profile_index) == ESP_OK) {
        ui_go_home();
        return true;
    }
    /* Legacy source leaves an empty Fluid slot selected on the menu and shows
     * "no saved calibration". Keep the menu open without changing coefficients. */
    return false;
}

static bool store_precal_fluid(uint8_t profile_index)
{
    if (dura_meter_finish_calibration(profile_index) == ESP_OK) {
        ui_go_home();
        return true;
    }
    return false;
}

static void dura_board_handle_button(dura_button_t b)
{
    dura_meter_snapshot_t meter;
    (void)dura_meter_get_snapshot(&meter);

    if (should_show_provisioning_qr(&meter)) {
        s_provisioning_qr_dismissed = true;
        ui_go_home();
        ESP_LOGI(TAG, "provisioning QR dismissed by button=%d; entering standalone meter UI", (int)b);
        return;
    }

    // DURA_BUTTON_UP/DOWN/SELECT/BACK are physical B0/B1/B2/B3 left-to-right on Dura M3000.
    switch (s_ui_screen) {
    case DURA_UI_HOME:
        if (b == DURA_BUTTON_UP) {
            s_ui_screen = DURA_UI_MANUAL;
        } else if (b == DURA_BUTTON_DOWN) {
            s_ui_screen = DURA_UI_CAL_START;
        } else if (b == DURA_BUTTON_SELECT) {
            s_ui_batch_amount = DURA_RECIRC_BATCH_DEFAULT_GAL;
            s_ui_screen = DURA_UI_RECIRC_EDIT;
        } else if (b == DURA_BUTTON_BACK) {
            if (dura_meter_set_auto_batch(true) == ESP_OK) {
                s_ui_batch_amount = DURA_AUTO_BATCH_DEFAULT_GAL;
                s_ui_screen = DURA_UI_AUTO_BATCH_EDIT;
            }
        }
        break;

    case DURA_UI_MANUAL:
        if (b == DURA_BUTTON_UP) {
            if (meter.operation_mode == DURA_OPERATION_MANUAL) (void)dura_meter_stop_manual();
            ui_go_main();
        } else if (b == DURA_BUTTON_DOWN) {
            (void)dura_meter_reset_meter_total();
        } else if (b == DURA_BUTTON_SELECT) {
            if (meter.operation_mode == DURA_OPERATION_MANUAL) (void)dura_meter_stop_manual();
        } else if (b == DURA_BUTTON_BACK && meter.operation_mode == DURA_OPERATION_IDLE) {
            (void)dura_meter_start_manual();
        }
        break;

    case DURA_UI_RECIRC_EDIT:
        if (b == DURA_BUTTON_UP) ui_go_main();
        else if (b == DURA_BUTTON_DOWN) s_ui_batch_amount = adjust_batch_amount_tenths(s_ui_batch_amount, -1);
        else if (b == DURA_BUTTON_SELECT) s_ui_batch_amount = adjust_batch_amount_tenths(s_ui_batch_amount, 1);
        else if (b == DURA_BUTTON_BACK &&
                 dura_meter_start_batch_mode(s_ui_batch_amount, DURA_OPERATION_RECIRC) == ESP_OK) {
            s_ui_screen = DURA_UI_RECIRC_RUN;
        }
        break;

    case DURA_UI_RECIRC_RUN:
        if (b == DURA_BUTTON_UP) {
            if (meter.batch_mode == DURA_BATCH_RUNNING || meter.batch_mode == DURA_BATCH_PAUSED ||
                meter.batch_mode == DURA_BATCH_DONE) {
                (void)dura_meter_cancel_batch();
            }
            ui_go_main();
        } else if (b == DURA_BUTTON_SELECT && meter.batch_mode == DURA_BATCH_RUNNING) {
            (void)dura_meter_pause_batch();
        } else if (b == DURA_BUTTON_BACK && meter.batch_mode == DURA_BATCH_PAUSED) {
            (void)dura_meter_resume_batch();
        }
        break;

    case DURA_UI_SETUP:
        handle_setup_button(b, &meter);
        break;

    case DURA_UI_RESET_TOTALS:
        if (b == DURA_BUTTON_UP) ui_go_home();
        else if (b == DURA_BUTTON_DOWN) reset_current_total();
        else if (b == DURA_BUTTON_SELECT) s_ui_screen = DURA_UI_RESET_HELP;
        else if (b == DURA_BUTTON_BACK) s_ui_reset_item = (uint8_t)((s_ui_reset_item + 1u) % 3u);
        break;

    case DURA_UI_RESET_HELP:
        if (b == DURA_BUTTON_UP) s_ui_screen = DURA_UI_RESET_TOTALS;
        break;

    case DURA_UI_INFO_START:
        if (b == DURA_BUTTON_UP) ui_go_home();
        else if (b == DURA_BUTTON_DOWN) s_ui_screen = DURA_UI_HOME; // Scan screen not ported; return safely.
        else if (b == DURA_BUTTON_SELECT) { s_ui_info_page = 0u; s_ui_screen = DURA_UI_INFO_HELP; }
        else if (b == DURA_BUTTON_BACK) s_ui_screen = DURA_UI_COMPANY;
        break;

    case DURA_UI_INFO_HELP:
        if (b == DURA_BUTTON_UP) ui_go_home();
        else if (b == DURA_BUTTON_SELECT) s_ui_info_page = (uint8_t)((s_ui_info_page + 1u) % 8u);
        else if (b == DURA_BUTTON_BACK) s_ui_screen = DURA_UI_INFO_START;
        break;

    case DURA_UI_COMPANY:
        if (b == DURA_BUTTON_UP) ui_go_home();
        else if (b == DURA_BUTTON_SELECT) { s_ui_info_page = 0u; s_ui_screen = DURA_UI_INFO_HELP; }
        else if (b == DURA_BUTTON_BACK) s_ui_screen = DURA_UI_SWV;
        break;

    case DURA_UI_SWV:
        if (b == DURA_BUTTON_UP) ui_go_home();
        else if (b == DURA_BUTTON_SELECT) { s_ui_info_page = 0u; s_ui_screen = DURA_UI_INFO_HELP; }
        else if (b == DURA_BUTTON_BACK) s_ui_screen = DURA_UI_BATTERY;
        break;

    case DURA_UI_BATTERY:
        if (b == DURA_BUTTON_UP) ui_go_home();
        else if (b == DURA_BUTTON_SELECT) { s_ui_info_page = 0u; s_ui_screen = DURA_UI_INFO_HELP; }
        else if (b == DURA_BUTTON_BACK) s_ui_screen = DURA_UI_COMPANY;
        break;

    case DURA_UI_CAL_START:
        if (b == DURA_BUTTON_UP) ui_go_home();
        else if (b == DURA_BUTTON_DOWN && dura_meter_start_calibration() == ESP_OK) {
            s_ui_cal_measured_amount_valid = false;
            s_ui_screen = (meter.selected_units == DURA_UNITS_LITER) ? DURA_UI_BUCKET_LITER : DURA_UI_BUCKET_GAL;
        }
        else if (b == DURA_BUTTON_SELECT) s_ui_screen = DURA_UI_PRECAL_1;
        else if (b == DURA_BUTTON_BACK) {
            s_ui_quick_ref = meter.quick_cal_reference;
            s_ui_screen = DURA_UI_QUICK_CAL;
        }
        break;

    case DURA_UI_PRECAL_1:
        if (b == DURA_BUTTON_UP) (void)choose_precal_fluid(0u);
        else if (b == DURA_BUTTON_DOWN) (void)choose_precal_fluid(1u);
        else if (b == DURA_BUTTON_SELECT) (void)choose_precal_fluid(2u);
        else if (b == DURA_BUTTON_BACK) s_ui_screen = DURA_UI_PRECAL_2;
        break;

    case DURA_UI_PRECAL_2:
        if (b == DURA_BUTTON_UP) (void)choose_precal_fluid(3u);
        else if (b == DURA_BUTTON_DOWN) (void)choose_precal_fluid(4u);
        else if (b == DURA_BUTTON_SELECT) (void)choose_precal_fluid(5u);
        else if (b == DURA_BUTTON_BACK) s_ui_screen = DURA_UI_CAL_START;
        break;

    case DURA_UI_QUICK_CAL:
        if (b == DURA_BUTTON_UP) {
            (void)dura_meter_cancel_calibration();
            s_ui_screen = DURA_UI_CAL_START;
        }
        else if (b == DURA_BUTTON_DOWN && s_ui_quick_ref > DURA_QUICK_CAL_REFERENCE_MIN) s_ui_quick_ref--;
        else if (b == DURA_BUTTON_SELECT && s_ui_quick_ref < DURA_QUICK_CAL_REFERENCE_MAX) s_ui_quick_ref++;
        else if (b == DURA_BUTTON_BACK && dura_meter_apply_quick_cal(s_ui_quick_ref) == ESP_OK) ui_go_home();
        break;

    case DURA_UI_BUCKET_GAL:
    case DURA_UI_BUCKET_LITER:
        if (b == DURA_BUTTON_UP) { ui_cancel_calibration_to_start(); }
        else if (b == DURA_BUTTON_DOWN) { s_ui_help_page = 0u; s_ui_screen = (s_ui_screen == DURA_UI_BUCKET_LITER) ? DURA_UI_CAL_HELP_LITER : DURA_UI_CAL_HELP_GAL; }
        else if (b == DURA_BUTTON_SELECT) { (void)dura_meter_reset_calibration(); }
        else if (b == DURA_BUTTON_BACK) { (void)dura_meter_continue_calibration(); }
        break;

    case DURA_UI_CAL_HELP_GAL:
    case DURA_UI_CAL_HELP_LITER:
        if (b == DURA_BUTTON_UP) s_ui_screen = (s_ui_screen == DURA_UI_CAL_HELP_LITER) ? DURA_UI_BUCKET_LITER : DURA_UI_BUCKET_GAL;
        else if (b == DURA_BUTTON_DOWN) s_ui_help_page = (uint8_t)((s_ui_help_page + 1u) % 4u);
        else if (b == DURA_BUTTON_BACK) s_ui_screen = (s_ui_screen == DURA_UI_CAL_HELP_LITER) ? DURA_UI_BUCKET_LITER : DURA_UI_BUCKET_GAL;
        break;

    case DURA_UI_CAL_ADJUST:
        if (b == DURA_BUTTON_UP) { ui_cancel_calibration_to_start(); }
        else if (b == DURA_BUTTON_DOWN && s_ui_cal_measured_amount > 0.01f) {
            s_ui_cal_measured_amount -= (meter.selected_units == DURA_UNITS_OUNCE) ? 0.1f : 0.01f;
        } else if (b == DURA_BUTTON_SELECT) {
            s_ui_cal_measured_amount += (meter.selected_units == DURA_UNITS_OUNCE) ? 0.1f : 0.01f;
        } else if (b == DURA_BUTTON_BACK &&
                   dura_meter_accept_calibration_amount(s_ui_cal_measured_amount) == ESP_OK) {
            s_ui_cal_measured_amount_valid = false;
            s_ui_screen = DURA_UI_CAL_SAVE_1;
        }
        break;

    case DURA_UI_CAL_SAVE_1:
        if (b == DURA_BUTTON_BACK) s_ui_screen = DURA_UI_CAL_SAVE_2;
        else (void)store_precal_fluid((uint8_t)(b - 1u));
        break;

    case DURA_UI_CAL_SAVE_2:
        if (b == DURA_BUTTON_BACK) { ui_cancel_calibration_to_start(); }
        else (void)store_precal_fluid((uint8_t)(b + 2u));
        break;

    case DURA_UI_BATCH_SET:
        if (b == DURA_BUTTON_UP) s_ui_batch_amount = 10.0f;
        else if (b == DURA_BUTTON_DOWN) s_ui_batch_amount = adjust_batch_amount_tenths(s_ui_batch_amount, -1);
        else if (b == DURA_BUTTON_SELECT) s_ui_batch_amount = adjust_batch_amount_tenths(s_ui_batch_amount, 1);
        else if (b == DURA_BUTTON_BACK && dura_meter_start_batch(s_ui_batch_amount) == ESP_OK) s_ui_screen = DURA_UI_BATCH_RUN;
        break;

    case DURA_UI_BATCH_RUN:
        if (b == DURA_BUTTON_UP && meter.batch_mode == DURA_BATCH_RUNNING) {
            (void)dura_meter_pause_batch();
        } else if (b == DURA_BUTTON_BACK && meter.batch_mode == DURA_BATCH_PAUSED) {
            (void)dura_meter_resume_batch();
        }
        break;

    case DURA_UI_AUTO_BATCH_EDIT:
        if (b == DURA_BUTTON_UP) ui_go_home();
        else if (b == DURA_BUTTON_DOWN) s_ui_batch_amount = adjust_batch_amount_tenths(s_ui_batch_amount, -1);
        else if (b == DURA_BUTTON_SELECT) s_ui_batch_amount = adjust_batch_amount_tenths(s_ui_batch_amount, 1);
        else if (b == DURA_BUTTON_BACK && dura_meter_start_batch(s_ui_batch_amount) == ESP_OK) s_ui_screen = DURA_UI_AUTO_BATCH_RUN;
        break;

    case DURA_UI_AUTO_BATCH_RUN:
        if (b == DURA_BUTTON_UP) {
            ui_cancel_batch_to_home();
        } else if (b == DURA_BUTTON_SELECT && meter.batch_mode == DURA_BATCH_RUNNING) {
            (void)dura_meter_pause_batch();
        } else if (b == DURA_BUTTON_BACK && meter.batch_mode == DURA_BATCH_PAUSED) {
            (void)dura_meter_resume_batch();
        }
        break;

    case DURA_UI_BATCH_COMPLETE:
        if (b == DURA_BUTTON_UP) {
            ui_cancel_batch_to_home();
        }
        break;

    case DURA_UI_FLOW_ERROR:
        if (b == DURA_BUTTON_UP) {
            (void)dura_meter_clear_fault();
            ui_cancel_batch_to_home();
        }
        break;

    default:
        ui_go_home();
        break;
    }
}

static bool ui_button_is_repeatable(dura_button_t button)
{
    if (button != DURA_BUTTON_DOWN && button != DURA_BUTTON_SELECT) {
        return false;
    }

    switch (s_ui_screen) {
    case DURA_UI_SETUP:
        return s_ui_setup_item == 0u || s_ui_setup_item == 1u;
    case DURA_UI_QUICK_CAL:
    case DURA_UI_CAL_ADJUST:
    case DURA_UI_BATCH_SET:
    case DURA_UI_RECIRC_EDIT:
    case DURA_UI_AUTO_BATCH_EDIT:
        return true;
    default:
        return false;
    }
}
#endif

esp_err_t dura_board_enqueue_button(dura_button_t button)
{
    if (button <= DURA_BUTTON_NONE || button > DURA_BUTTON_BACK) return ESP_ERR_INVALID_ARG;
    if (s_button_queue == NULL) return ESP_ERR_INVALID_STATE;
    if (xQueueSend(s_button_queue, &button, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    dura_board_note_user_activity();
    return ESP_OK;
}

static void dispatch_ui_button(dura_button_t button)
{
#if !CONFIG_DURA_BOARD_LCD_HOLD_SPLASH_ON_BOOT
    dura_board_handle_button(button);
    dura_meter_snapshot_t meter;
    if (dura_meter_get_snapshot(&meter) == ESP_OK) publish_ui_snapshot(&meter);
#else
    if (button == DURA_BUTTON_BACK) (void)dura_meter_stop_batch();
#endif
}

static void board_task(void *arg)
{
    (void)arg;
    uint32_t save_ticks = 0;
    dura_button_t last_button = DURA_BUTTON_NONE;
    int64_t button_pressed_us = 0;
    int64_t last_button_repeat_us = 0;
    bool hold_repeat_enabled = false;
    bool main_outside_hold_pending = false;
    bool settings_hold_fired = false;
#if CONFIG_DURA_BOARD_ENABLE_RECIRC_INPUT && !CONFIG_DURA_BOARD_LCD_HOLD_SPLASH_ON_BOOT
    bool recirc_was_pressed = false;
#endif

    portENTER_CRITICAL(&s_ui_snapshot_mux);
    s_last_activity_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_ui_snapshot_mux);

    while (true) {
        dura_battery_poll();
        uint32_t pulses = dura_meter_drain_flow();
        if (pulses > 0) {
            dura_board_note_user_activity();
        }

        dura_button_t queued_button;
        while (xQueueReceive(s_button_queue, &queued_button, 0) == pdTRUE) {
            ESP_LOGI(TAG, "remote button=%d", (int)queued_button);
            dura_board_note_user_activity();
            dispatch_ui_button(queued_button);
        }

#if CONFIG_DURA_BOARD_ENABLE_RECIRC_INPUT && !CONFIG_DURA_BOARD_LCD_HOLD_SPLASH_ON_BOOT
        const bool recirc_pressed =
            gpio_get_level(DURA_GPIO_RECIRC_INPUT) == DURA_RECIRC_INPUT_ACTIVE_LEVEL;
        if (recirc_pressed && !recirc_was_pressed) {
            portENTER_CRITICAL(&s_ui_snapshot_mux);
            s_last_activity_us = esp_timer_get_time();
            portEXIT_CRITICAL(&s_ui_snapshot_mux);
            handle_external_recirc_press();
        }
        recirc_was_pressed = recirc_pressed;
#endif

        dura_button_t b = dura_board_poll_button();
        /* A held key is activity, including the wake key whose action remains
         * suppressed until release. Never consume an ordinary dark-screen key. */
        if (b != DURA_BUTTON_NONE) dura_board_note_user_activity();
        if (s_ignore_buttons_until_release) {
            if (b == DURA_BUTTON_NONE) {
                s_ignore_buttons_until_release = false;
            }
            b = DURA_BUTTON_NONE;
        }
        const int64_t button_sample_us = esp_timer_get_time();
        if (b != DURA_BUTTON_NONE && b != last_button) {
            button_pressed_us = button_sample_us;
            last_button_repeat_us = button_sample_us;
            portENTER_CRITICAL(&s_ui_snapshot_mux);
            s_last_activity_us = button_sample_us;
            portEXIT_CRITICAL(&s_ui_snapshot_mux);
#if !CONFIG_DURA_BOARD_LCD_HOLD_SPLASH_ON_BOOT
            main_outside_hold_pending = s_ui_screen == DURA_UI_MAIN_MENU &&
                                        (b == DURA_BUTTON_UP || b == DURA_BUTTON_BACK);
            settings_hold_fired = false;
            hold_repeat_enabled = !main_outside_hold_pending && ui_button_is_repeatable(b);
#else
            main_outside_hold_pending = false;
            settings_hold_fired = false;
            hold_repeat_enabled = false;
#endif
            ESP_LOGI(TAG, "button=%d", (int)b);
            if (!main_outside_hold_pending) dispatch_ui_button(b);
        } else if (b != DURA_BUTTON_NONE && b == last_button) {
#if !CONFIG_DURA_BOARD_LCD_HOLD_SPLASH_ON_BOOT
            const int64_t held_us = button_sample_us >= button_pressed_us
                                        ? button_sample_us - button_pressed_us
                                        : 0;
            if (main_outside_hold_pending && !settings_hold_fired &&
                held_us >= ((int64_t)CONFIG_DURA_BOARD_SETTINGS_HOLD_MS * 1000LL)) {
                s_ui_setup_item = 0u;
                s_ui_screen = DURA_UI_SETUP;
                settings_hold_fired = true;
                main_outside_hold_pending = false;
                portENTER_CRITICAL(&s_ui_snapshot_mux);
                s_last_activity_us = button_sample_us;
                portEXIT_CRITICAL(&s_ui_snapshot_mux);
            } else if (hold_repeat_enabled && ui_button_is_repeatable(b)) {
                dura_ui_runtime_config_t config;
                if (dura_ui_config_get(&config) == ESP_OK) {
                    const uint64_t held_ms_u64 = (uint64_t)held_us / 1000ULL;
                    const uint32_t held_ms = held_ms_u64 > UINT32_MAX
                                                 ? UINT32_MAX
                                                 : (uint32_t)held_ms_u64;
                    const uint32_t interval_ms = dura_ui_button_repeat_interval_ms(
                        held_ms, config.button_hold_max_rate_x);
                    const int64_t since_repeat_us = button_sample_us >= last_button_repeat_us
                                                        ? button_sample_us - last_button_repeat_us
                                                        : 0;
                    if (interval_ms != UINT32_MAX) {
                        const int64_t interval_us = (int64_t)interval_ms * 1000LL;
                        if (since_repeat_us >= interval_us) {
                            dispatch_ui_button(b);
                            last_button_repeat_us = button_sample_us -
                                (since_repeat_us % interval_us);
                        }
                    }
                }
            }
#endif
        } else {
#if !CONFIG_DURA_BOARD_LCD_HOLD_SPLASH_ON_BOOT
            if (main_outside_hold_pending && !settings_hold_fired &&
                (last_button == DURA_BUTTON_UP || last_button == DURA_BUTTON_BACK)) {
                dispatch_ui_button(last_button);
            }
#endif
            button_pressed_us = 0;
            last_button_repeat_us = 0;
            hold_repeat_enabled = false;
            main_outside_hold_pending = false;
            settings_hold_fired = false;
        }
        last_button = b;

        (void)dura_board_sync_outputs();
        update_backlight();
#if !CONFIG_DURA_BOARD_LCD_HOLD_SPLASH_ON_BOOT
        (void)dura_board_render_debug_screen();
        dura_meter_snapshot_t meter;
        if (dura_meter_get_snapshot(&meter) == ESP_OK) publish_ui_snapshot(&meter);
#endif

        portENTER_CRITICAL(&s_ui_snapshot_mux);
        const bool requested_sleep = s_sleep_requested;
        s_sleep_requested = false;
        portEXIT_CRITICAL(&s_ui_snapshot_mux);
        if (requested_sleep) {
            const esp_err_t e = dura_board_enter_deep_sleep();
            ESP_LOGE(TAG, "requested sleep returned: %s", esp_err_to_name(e));
            last_button = DURA_BUTTON_NONE;
            button_pressed_us = last_button_repeat_us = 0;
            hold_repeat_enabled = main_outside_hold_pending = settings_hold_fired = false;
        }
#if CONFIG_DURA_BOARD_ENABLE_DEEP_SLEEP
        dura_meter_snapshot_t sleep_meter;
        if (dura_meter_get_snapshot(&sleep_meter) == ESP_OK) {
            bool ble_connected;
            int64_t last_activity_us;
            uint64_t activity_generation;
            portENTER_CRITICAL(&s_ui_snapshot_mux);
            ble_connected = s_ble_connected;
            last_activity_us = s_last_activity_us;
            activity_generation = s_activity_generation;
            portEXIT_CRITICAL(&s_ui_snapshot_mux);
            const int64_t now_us = esp_timer_get_time();
            const uint64_t inactive_ms = now_us >= last_activity_us
                                             ? (uint64_t)(now_us - last_activity_us) / 1000ULL
                                             : 0u;
            const dura_sleep_policy_input_t sleep_input = {
                .operation_mode = sleep_meter.operation_mode,
                .pump_enabled = sleep_meter.pump_enabled,
                .batch_active = sleep_meter.batch_mode == DURA_BATCH_RUNNING ||
                                sleep_meter.batch_mode == DURA_BATCH_PAUSED,
                .calibration_active = sleep_meter.calibration_mode != DURA_CAL_IDLE,
                .fault_latched = sleep_meter.fault_latched,
                .ble_connected = ble_connected,
                .inactive_ms = inactive_ms,
                .timeout_sec = CONFIG_DURA_BOARD_DEEP_SLEEP_DELAY_SEC,
            };
            if (dura_control_sleep_allowed(&sleep_input) &&
                sleep_attempt_current(activity_generation)) {
                ESP_LOGI(TAG, "deep-sleep delay %u s reached; entering gated deep sleep",
                         (unsigned)CONFIG_DURA_BOARD_DEEP_SLEEP_DELAY_SEC);
                const esp_err_t sleep_err = dura_board_enter_deep_sleep();
                ESP_LOGE(TAG, "deep sleep entry returned: %s", esp_err_to_name(sleep_err));
                last_button = DURA_BUTTON_NONE;
                button_pressed_us = last_button_repeat_us = 0;
                hold_repeat_enabled = main_outside_hold_pending = settings_hold_fired = false;
            }
        }
#endif

        save_ticks += DURA_BOARD_UI_PERIOD_MS;
        if (save_ticks >= DURA_BOARD_SAVE_PERIOD_MS) {
            save_ticks = 0;
            (void)dura_meter_save();
        }
        vTaskDelay(pdMS_TO_TICKS(DURA_BOARD_UI_PERIOD_MS));
    }
}

esp_err_t dura_board_init_power_sense(void)
{
    s_boost_enabled = true;
    ESP_LOGI(TAG, "Round 11 TPS61021A EN is hard-wired to 3v3_digital; no boost GPIO configured");
#if CONFIG_DURA_BOARD_ENABLE_EXT_POWER_PRESENT_INPUT
    gpio_config_t ext_power = {
        .pin_bit_mask = (1ULL << DURA_GPIO_EXT_POWER_PRESENT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = CONFIG_DURA_BOARD_EXT_POWER_PRESENT_PULLUP ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = CONFIG_DURA_BOARD_EXT_POWER_PRESENT_PULLDOWN ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&ext_power), TAG, "external power present input");
    ESP_LOGI(TAG, "external power present input: gpio=%d active_level=%d raw=%d source=%s",
             (int)DURA_GPIO_EXT_POWER_PRESENT,
             DURA_EXT_POWER_PRESENT_ACTIVE_LEVEL,
             gpio_get_level(DURA_GPIO_EXT_POWER_PRESENT),
             dura_board_power_source_name());
#endif
    return ESP_OK;
}

esp_err_t dura_board_init(void)
{
    const bool deep_sleep_wake =
        esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_UNDEFINED;
    s_ignore_buttons_until_release = deep_sleep_wake;

    if (s_button_queue == NULL) {
        s_button_queue = xQueueCreate(16, sizeof(dura_button_t));
        ESP_RETURN_ON_FALSE(s_button_queue != NULL, ESP_ERR_NO_MEM, TAG, "button queue");
    }
    ESP_RETURN_ON_ERROR(dura_board_init_power_sense(), TAG, "power sense input");
    esp_err_t battery_err = dura_battery_init();
    if (battery_err != ESP_OK) {
        ESP_LOGW(TAG, "battery measurement unavailable; continuing without ADC: %s",
                 esp_err_to_name(battery_err));
    }

    uint64_t output_mask = 0;
#if CONFIG_DURA_BOARD_ENABLE_PUMP_OUTPUT
    output_mask |= (1ULL << DURA_GPIO_PUMP_OUTPUT);
#endif
#if CONFIG_DURA_BOARD_ENABLE_EV_OUTPUTS
    output_mask |= (1ULL << DURA_GPIO_RECIRC_EV_OUTPUT) |
                   (1ULL << DURA_GPIO_INJECT_EV_OUTPUT);
#endif
#if CONFIG_DURA_BOARD_ENABLE_LEGACY_POWER_OUTPUTS
    output_mask |= (1ULL << DURA_GPIO_BOOST_ENABLE) |
                   (1ULL << DURA_GPIO_BOOST_DISABLE) |
                   (1ULL << DURA_GPIO_BATTERY_DISCONNECT) |
                   (1ULL << DURA_GPIO_SW_LO_ENABLE1) |
                   (1ULL << DURA_GPIO_SW_LO_ENABLE2);
#endif
    if (output_mask != 0u) {
        gpio_config_t outputs = {
            .pin_bit_mask = output_mask,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&outputs), TAG, "gpio outputs");
    }
#if CONFIG_DURA_BOARD_ENABLE_PUMP_OUTPUT
    gpio_set_level(DURA_GPIO_PUMP_OUTPUT, DURA_PUMP_OUTPUT_INACTIVE_LEVEL);
#endif
#if CONFIG_DURA_BOARD_ENABLE_EV_OUTPUTS
    gpio_set_level(DURA_GPIO_RECIRC_EV_OUTPUT, DURA_RECIRC_EV_INACTIVE_LEVEL);
    gpio_set_level(DURA_GPIO_INJECT_EV_OUTPUT, DURA_INJECT_EV_INACTIVE_LEVEL);
#endif
#if CONFIG_DURA_BOARD_ENABLE_LEGACY_POWER_OUTPUTS
    gpio_set_level(DURA_GPIO_BOOST_ENABLE, DURA_BOOST_ENABLE_INACTIVE_LEVEL);
    gpio_set_level(DURA_GPIO_BOOST_DISABLE, DURA_BOOST_DISABLE_INACTIVE_LEVEL);
    gpio_set_level(DURA_GPIO_BATTERY_DISCONNECT, DURA_BATTERY_DISCONNECT_INACTIVE_LEVEL);
    gpio_set_level(DURA_GPIO_SW_LO_ENABLE1, DURA_SW_LO_ENABLE1_INACTIVE_LEVEL);
    gpio_set_level(DURA_GPIO_SW_LO_ENABLE2, DURA_SW_LO_ENABLE2_INACTIVE_LEVEL);
#endif

    /* Install only after the output GPIOs have been configured inactive. */
    ESP_RETURN_ON_ERROR(dura_meter_set_output_handler(apply_meter_outputs), TAG, "meter output handoff");

    for (size_t i = 0; i < sizeof(s_flow_inputs) / sizeof(s_flow_inputs[0]); ++i) {
        (void)rtc_gpio_hold_dis(s_flow_inputs[i].gpio);
    }

    gpio_config_t flow_inputs = {
        .pin_bit_mask = (1ULL << DURA_GPIO_FLOW_A) |
                        (1ULL << DURA_GPIO_FLOW_B),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = CONFIG_DURA_BOARD_FLOW_PULLUP ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = CONFIG_DURA_BOARD_FLOW_PULLDOWN ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&flow_inputs), TAG, "flow inputs");

    s_flow_event_queue = xQueueCreate(32, sizeof(dura_flow_event_t));
    ESP_RETURN_ON_FALSE(s_flow_event_queue != NULL, ESP_ERR_NO_MEM, TAG, "flow event queue");
    ESP_RETURN_ON_ERROR(dura_meter_set_flow_handler(flow_boundary), TAG, "meter flow handoff");
    s_last_valid_reed = 0;

    ESP_RETURN_ON_ERROR(dura_board_init_buttons(), TAG, "button inputs");
#if CONFIG_DURA_BOARD_ENABLE_RECIRC_INPUT
    gpio_config_t recirc_input = {
        .pin_bit_mask = (1ULL << DURA_GPIO_RECIRC_INPUT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = CONFIG_DURA_BOARD_RECIRC_INPUT_PULLUP ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = CONFIG_DURA_BOARD_RECIRC_INPUT_PULLDOWN ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&recirc_input), TAG, "separate recirc input");
#endif

    gpio_set_intr_type(DURA_GPIO_FLOW_A, GPIO_INTR_ANYEDGE);
    gpio_set_intr_type(DURA_GPIO_FLOW_B, GPIO_INTR_ANYEDGE);
    ESP_RETURN_ON_ERROR(gpio_install_isr_service(ESP_INTR_FLAG_IRAM), TAG, "isr service");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(DURA_GPIO_FLOW_A, flow_isr, (void *)(uintptr_t)1u), TAG, "flow A isr");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(DURA_GPIO_FLOW_B, flow_isr, (void *)(uintptr_t)2u), TAG, "flow B isr");

    ESP_RETURN_ON_ERROR(dura_lcd_init(), TAG, "lcd init");
    dura_board_note_user_activity();
    update_backlight();
    if (!deep_sleep_wake) {
        ESP_RETURN_ON_ERROR(dura_lcd_draw_splash(), TAG, "lcd splash");
#if !CONFIG_DURA_BOARD_LCD_HOLD_SPLASH_ON_BOOT
        vTaskDelay(pdMS_TO_TICKS(DURA_BOOT_LOGO_HOLD_MS));
        ESP_RETURN_ON_ERROR(draw_boot_powering_up_popup(), TAG, "lcd powering-up popup");
        vTaskDelay(pdMS_TO_TICKS(DURA_BOOT_POWERING_UP_HOLD_MS));
        ESP_RETURN_ON_ERROR(dura_lcd_draw_splash(), TAG, "lcd restore splash after popup");
#endif
    }
#if !CONFIG_DURA_BOARD_LCD_HOLD_SPLASH_ON_BOOT
    ESP_RETURN_ON_ERROR(dura_board_render_debug_screen(), TAG, "lcd debug initial draw");
    dura_meter_snapshot_t meter;
    ESP_RETURN_ON_ERROR(dura_meter_get_snapshot(&meter), TAG, "initial UI snapshot");
    publish_ui_snapshot(&meter);
#else
    if (deep_sleep_wake) {
        ESP_RETURN_ON_ERROR(dura_board_render_debug_screen(), TAG, "lcd resume initial draw");
        dura_meter_snapshot_t meter;
        ESP_RETURN_ON_ERROR(dura_meter_get_snapshot(&meter), TAG, "resume UI snapshot");
        publish_ui_snapshot(&meter);
    }
#endif
    ESP_LOGI(TAG, "board initialized: %s", CONFIG_DURA_BOARD_NAME);
    return ESP_OK;
}

esp_err_t dura_board_start(void)
{
    if (s_task != NULL) return ESP_OK;
    BaseType_t ok = xTaskCreate(board_task, "dura_board", DURA_BOARD_TASK_STACK, NULL,
                                DURA_BOARD_TASK_PRIORITY, &s_task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}


