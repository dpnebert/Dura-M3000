#include "dura_meter.h"
/* Header-only serializer; no runtime/component dependency on recipe state. */
#include "../dura_recipe/include/dura_json_builder.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "dura_meter";

#define DURA_EE_CHECK_VALUE        20240921U
#define DURA_DEFAULT_BATCH_PRESET  10.0f
#define DURA_DEFAULT_WATER_GAL     0.00892857f
#define DURA_DEFAULT_WATER_LITER   0.03379831f
#define DURA_DEFAULT_WATER_OUNCE   1.14285696f
#define DURA_SPIN_DOWN_OFFSET      1U

typedef struct {
    dura_screen_t screen;
    dura_units_t selected_units;
    dura_batch_mode_t batch_mode;
    dura_calibration_mode_t calibration_mode;
    dura_operation_mode_t operation_mode;
    uint32_t recipe_owner_id;
    uint32_t activation_id;
    bool activation_pending;
    bool pump_enabled;
    bool fault_latched;
    uint32_t fault_code;
    bool show_flow_rate;
    bool show_auto_batch;
    bool show_batch_total;
    uint32_t meter_total_counts;
    uint32_t batch_total_counts;
    uint32_t total_total_counts;
    uint32_t odometer_counts;
    uint32_t preset_batch_counts;
    uint32_t batch_initial_counts;
    uint32_t calibration_counts;
    dura_cal_coefficients_t pending_calibration;
    uint8_t pending_quick_reference; /* Volatile retry ownership; never persisted. */
    float batch_preset_gal;
    float batch_requested_amount;
    float working_coef_gal;
    float working_coef_liter;
    float working_coef_ounce;
    dura_cal_coefficients_t precal_profiles[DURA_PRECAL_PROFILE_COUNT];
    dura_language_t selected_language;
    uint8_t meter_timeout_sec;
    uint8_t backlight_timeout_sec;
    bool reset_to_zero;
    uint8_t selected_precal_profile;
    uint8_t quick_cal_reference;
    int64_t last_flow_tick;
    int64_t next_flow_update_us;
    uint16_t flow_fir[31];
    uint16_t flow_delta[40];
    uint8_t flow_index;
    uint8_t flow_samples;
    float flow_rate;
} dura_meter_state_t;

static dura_meter_state_t s_meter;
static portMUX_TYPE s_meter_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_save_mutex;
static const char *s_log_context;
static dura_meter_output_handler_t s_output_handler;
static uint32_t s_next_identity; /* Never reset by defaults/load (prevents ABA). */
static bool s_load_in_progress;
static bool s_sleep_prepared;
static uint64_t s_control_epoch;
/* Not part of s_meter: cancellation/defaults cannot free an in-flight writer. */
static bool s_calibration_save_in_progress;
static void sync_outputs_locked(void);
static bool record_pulse_locked(uint32_t count);
static void advance_flow_locked(int64_t now_us);
static dura_meter_flow_handler_t s_flow_handler;
static uint32_t s_flow_activity;
static bool s_flow_save_pending;

/* Order: meter -> board ingress -> queue. The ISR takes ingress only. Drain
 * into OLD state, then hold ingress through mutation and output publication.
 * No transition can overtake an already queued edge, nor split a drained batch. */
static void begin_flow_locked(void)
{
    if (s_flow_handler != NULL) {
        const uint32_t count = s_flow_handler(true);
        s_flow_activity = count > UINT32_MAX - s_flow_activity ? UINT32_MAX : s_flow_activity + count;
        if (record_pulse_locked(count)) s_flow_save_pending = true;
    }
    /* Also serviced by the board's regular drain with no events or BLE reads. */
    advance_flow_locked(esp_timer_get_time());
}

#define METER_LOCK() do { taskENTER_CRITICAL(&s_meter_lock); begin_flow_locked(); } while (0)
/* OFF still precedes every save-mutex/NVS wait. */
#define METER_UNLOCK() do { sync_outputs_locked(); if (s_flow_handler != NULL) (void)s_flow_handler(false); taskEXIT_CRITICAL(&s_meter_lock); } while (0)

esp_err_t dura_meter_set_flow_handler(dura_meter_flow_handler_t handler)
{
    if (handler == NULL) return ESP_ERR_INVALID_ARG;
    /* Registration has no matching ingress-enter yet. It is one-time wiring,
     * not a runtime source replacement; defaults/load never remove it. */
    taskENTER_CRITICAL(&s_meter_lock);
    const bool allowed = s_flow_handler == NULL || s_flow_handler == handler;
    if (allowed) s_flow_handler = handler;
    taskEXIT_CRITICAL(&s_meter_lock);
    return allowed ? ESP_OK : ESP_ERR_INVALID_STATE;
}

uint32_t dura_meter_drain_flow(void)
{
    METER_LOCK();
    const uint32_t count = s_flow_activity;
    const bool save = s_flow_save_pending;
    s_flow_activity = 0;
    s_flow_save_pending = false;
    METER_UNLOCK();
    /* Completion detected by any boundary is persisted by the board poll,
     * never from the critical section or ISR. */
    if (save) (void)dura_meter_save();
    return count;
}

static void sync_outputs_locked(void)
{
    if (s_output_handler != NULL) {
        s_output_handler(dura_control_output_intent(s_meter.operation_mode,
                         s_meter.pump_enabled && !s_meter.fault_latched && !s_sleep_prepared));
    }
}

static uint32_t next_identity_locked(void)
{
    /* Exhaustion refuses admission rather than reusing a stale lease. */
    return s_next_identity == UINT32_MAX ? 0 : ++s_next_identity;
}

static bool operation_busy_locked(void)
{
    return s_meter.pending_quick_reference != 0 || s_calibration_save_in_progress || s_load_in_progress || s_sleep_prepared || s_meter.fault_latched || s_meter.recipe_owner_id != 0 ||
           s_meter.activation_pending || s_meter.pump_enabled ||
           s_meter.operation_mode != DURA_OPERATION_IDLE ||
           s_meter.calibration_mode != DURA_CAL_IDLE ||
           s_meter.batch_mode == DURA_BATCH_RUNNING || s_meter.batch_mode == DURA_BATCH_PAUSED;
}

esp_err_t dura_meter_set_output_handler(dura_meter_output_handler_t handler)
{
    if (handler == NULL) return ESP_ERR_INVALID_ARG;
    METER_LOCK();
    if (s_meter.pump_enabled || s_meter.activation_pending ||
        (s_output_handler != NULL && s_output_handler != handler)) {
        METER_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    s_output_handler = handler;
    METER_UNLOCK();
    return ESP_OK;
}

esp_err_t dura_meter_sync_outputs(void)
{
    METER_LOCK();
    METER_UNLOCK();
    return ESP_OK;
}

static bool units_valid(dura_units_t units)
{
    return units >= DURA_UNITS_GALLON && units <= DURA_UNITS_OUNCE;
}

static bool coefficient_valid(float coefficient)
{
    return isfinite(coefficient) && coefficient > 0.0f;
}

static bool coefficients_valid(const dura_cal_coefficients_t *coef)
{
    return coef != NULL && coefficient_valid(coef->gallon_per_count) &&
           coefficient_valid(coef->liter_per_count) && coefficient_valid(coef->ounce_per_count);
}

static bool coefficients_empty(const dura_cal_coefficients_t *coef)
{
    return coef != NULL && coef->gallon_per_count == 0.0f &&
           coef->liter_per_count == 0.0f && coef->ounce_per_count == 0.0f;
}

static dura_cal_coefficients_t working_coefficients_locked(void)
{
    return (dura_cal_coefficients_t){s_meter.working_coef_gal,
                                     s_meter.working_coef_liter,
                                     s_meter.working_coef_ounce};
}

static void set_working_coefficients_locked(const dura_cal_coefficients_t *coef)
{
    s_meter.working_coef_gal = coef->gallon_per_count;
    s_meter.working_coef_liter = coef->liter_per_count;
    s_meter.working_coef_ounce = coef->ounce_per_count;
}

static dura_meter_state_t default_state(void)
{
    dura_meter_state_t state = {0};
    state.selected_units = DURA_UNITS_GALLON;
    state.batch_preset_gal = DURA_DEFAULT_BATCH_PRESET;
    state.working_coef_gal = DURA_DEFAULT_WATER_GAL;
    state.working_coef_liter = DURA_DEFAULT_WATER_LITER;
    state.working_coef_ounce = DURA_DEFAULT_WATER_OUNCE;
    state.precal_profiles[0] = (dura_cal_coefficients_t){DURA_DEFAULT_WATER_GAL,
                                                        DURA_DEFAULT_WATER_LITER,
                                                        DURA_DEFAULT_WATER_OUNCE};
    state.selected_language = DURA_LANGUAGE_ENGLISH;
    state.meter_timeout_sec = DURA_METER_TIMEOUT_MIN_SEC;
    state.backlight_timeout_sec = DURA_BACKLIGHT_TIMEOUT_MIN_SEC;
    state.selected_precal_profile = 0;
    state.quick_cal_reference = DURA_QUICK_CAL_DEFAULT;
    state.screen = DURA_SCREEN_HOME;
    state.batch_mode = DURA_BATCH_IDLE;
    state.calibration_mode = DURA_CAL_IDLE;
    state.operation_mode = DURA_OPERATION_IDLE;
    state.pump_enabled = false;
    const int64_t now_us = esp_timer_get_time();
    state.last_flow_tick = now_us / 1000;
    state.next_flow_update_us = now_us + 100000;
    return state;
}

static const char *log_context_prefix(void)
{
    return (s_log_context != NULL && s_log_context[0] != '\0') ? s_log_context : "runtime";
}

static float selected_coef(void)
{
    switch (s_meter.selected_units) {
    case DURA_UNITS_LITER:
        return s_meter.working_coef_liter;
    case DURA_UNITS_OUNCE:
        return s_meter.working_coef_ounce;
    case DURA_UNITS_GALLON:
    default:
        return s_meter.working_coef_gal;
    }
}

static float selected_amount_to_gal(float amount)
{
    switch (s_meter.selected_units) {
    case DURA_UNITS_LITER:
        return amount / 3.8f;
    case DURA_UNITS_OUNCE:
        return amount / 128.0f;
    case DURA_UNITS_GALLON:
    default:
        return amount;
    }
}

static uint32_t amount_to_counts(float amount)
{
    const float coef = selected_coef();
    if (!isfinite(amount) || amount <= 0.0f || !coefficient_valid(coef)) {
        return 0;
    }
    /* Retained ARM code rounds the quotient to binary32, THEN truncates it.
     * Do not divide in double or round fractional counts up to a pulse. */
    volatile float counts = amount / coef;
    /* The port's counters/NVS are uint32_t, wider than legacy signed longint.
     * Keep that safe extension without reproducing signed overflow. 2^32 is
     * exactly representable; (float)UINT32_MAX rounds UP to this same bound. */
    if (!isfinite(counts) || counts < 1.0f || counts >= 0x1p32f) {
        return 0;
    }
    return (uint32_t)counts;
}

static float batch_remaining_display_value(float requested_amount,
                                           uint32_t initial_counts,
                                           uint32_t remaining_counts,
                                           float coefficient)
{
    if (initial_counts > 0 && remaining_counts == initial_counts) {
        return requested_amount;
    }
    return coefficient * (float)remaining_counts;
}

/* Caller holds s_meter_lock. METER_UNLOCK applies the resulting intent before release. */
static esp_err_t set_pump_locked(bool enabled)
{
    if (enabled && (s_meter.fault_latched || s_load_in_progress || s_sleep_prepared)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (enabled) {
        const bool valid_manual = s_meter.operation_mode == DURA_OPERATION_MANUAL &&
                                  s_meter.screen == DURA_SCREEN_RUN;
        const bool valid_calibration = s_meter.operation_mode == DURA_OPERATION_CALIBRATION &&
                                       s_meter.screen == DURA_SCREEN_CALIBRATION;
        const bool valid_batch = (s_meter.operation_mode == DURA_OPERATION_RECIRC ||
                                  s_meter.operation_mode == DURA_OPERATION_AUTO) &&
                                 s_meter.screen == DURA_SCREEN_BATCH;
        if (!valid_manual && !valid_calibration && !valid_batch) return ESP_ERR_INVALID_STATE;
    }
    s_meter.pump_enabled = enabled;
    if (!enabled) s_meter.activation_pending = false;
    return ESP_OK;
}

esp_err_t dura_meter_factory_defaults(void)
{
    dura_meter_state_t defaults = default_state();
    METER_LOCK();
    /* Lifetime history survives an operator reset, including drained flow. */
    defaults.odometer_counts = s_meter.odometer_counts;
    s_meter = defaults;
    ++s_control_epoch;
    METER_UNLOCK();
    return ESP_OK;
}

static esp_err_t load_meter_state(uint64_t epoch)
{
    nvs_handle_t nvs;
    uint32_t check = 0;
    esp_err_t err;
    bool invalid = false;
    dura_meter_state_t loaded = default_state();

    /* Wrapper already revoked ownership and applied OFF before any NVS access. */
    err = nvs_open(DURA_METER_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open nvs");

    err = nvs_get_u32(nvs, "ee_check", &check);
    if (err != ESP_OK || check != DURA_EE_CHECK_VALUE) {
        nvs_close(nvs);
        return ESP_OK;
    }

    (void)nvs_get_u32(nvs, "meter_counts", &loaded.meter_total_counts);
    (void)nvs_get_u32(nvs, "batch_counts", &loaded.batch_total_counts);
    (void)nvs_get_u32(nvs, "total_counts", &loaded.total_total_counts);
    (void)nvs_get_u32(nvs, "odom_counts", &loaded.odometer_counts);
    (void)nvs_get_u32(nvs, "preset_counts", &loaded.preset_batch_counts);

    uint32_t units = loaded.selected_units;
    err = nvs_get_u32(nvs, "units", &units);
    if (err == ESP_OK) {
        invalid |= !units_valid((dura_units_t)units);
        if (!invalid) loaded.selected_units = (dura_units_t)units;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        invalid = true;
    }
    err = nvs_get_u32(nvs, "fault_code", &loaded.fault_code);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        invalid = true;
    }
    /* fault_code is the persisted latch authority.  Never restore a nonzero
     * code into a runnable state merely because the transient bool was absent. */
    loaded.fault_latched = loaded.fault_code != 0;

    uint8_t bool_value;
#define LOAD_BOOL(key, member) do { \
        err = nvs_get_u8(nvs, (key), &bool_value); \
        if (err == ESP_OK) { \
            if (bool_value > 1U) invalid = true; else loaded.member = bool_value != 0U; \
        } else if (err != ESP_ERR_NVS_NOT_FOUND) invalid = true; \
    } while (0)
    LOAD_BOOL("show_flow", show_flow_rate);
    LOAD_BOOL("auto_batch", show_auto_batch);
    LOAD_BOOL("show_batch", show_batch_total);
    LOAD_BOOL("reset_zero", reset_to_zero);
#undef LOAD_BOOL

    uint8_t u8_value = loaded.meter_timeout_sec;
    err = nvs_get_u8(nvs, "meter_timeout", &u8_value);
    if (err == ESP_OK) loaded.meter_timeout_sec = u8_value;
    else if (err != ESP_ERR_NVS_NOT_FOUND) invalid = true;
    u8_value = loaded.backlight_timeout_sec;
    err = nvs_get_u8(nvs, "backlight", &u8_value);
    if (err == ESP_OK) loaded.backlight_timeout_sec = u8_value;
    else if (err != ESP_ERR_NVS_NOT_FOUND) invalid = true;
    u8_value = (uint8_t)loaded.selected_language;
    err = nvs_get_u8(nvs, "language", &u8_value);
    if (err == ESP_OK) loaded.selected_language = (dura_language_t)u8_value;
    else if (err != ESP_ERR_NVS_NOT_FOUND) invalid = true;
    u8_value = loaded.selected_precal_profile;
    err = nvs_get_u8(nvs, "precal_sel", &u8_value);
    if (err == ESP_OK) loaded.selected_precal_profile = u8_value;
    else if (err != ESP_ERR_NVS_NOT_FOUND) invalid = true;
    u8_value = loaded.quick_cal_reference;
    err = nvs_get_u8(nvs, DURA_QUICK_CAL_NVS_KEY, &u8_value);
    if (err == ESP_OK) loaded.quick_cal_reference = u8_value;
    else if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Legacy refs 1..20 mean .84..1.03, now rows 20..1. Old21's
         * 1.04 has no row: retain a neutral editor identity. Never touch
         * saved or working coefficient bytes during this migration. */
        err = nvs_get_u8(nvs, "quick_ref", &u8_value);
        if (err == ESP_OK) {
            if (u8_value < 1 || u8_value > 21) invalid = true;
            else loaded.quick_cal_reference = (uint8_t)(21u - u8_value);
        } else if (err != ESP_ERR_NVS_NOT_FOUND) invalid = true;
    }
    else if (err != ESP_ERR_NVS_NOT_FOUND) invalid = true;

    size_t profiles_len = sizeof(loaded.precal_profiles);
    err = nvs_get_blob(nvs, "precal", loaded.precal_profiles, &profiles_len);
    if (err == ESP_OK) invalid |= profiles_len != sizeof(loaded.precal_profiles);
    else if (err != ESP_ERR_NVS_NOT_FOUND) invalid = true;

    size_t len = sizeof(float);
    err = nvs_get_blob(nvs, "batch_gal", &loaded.batch_preset_gal, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) invalid = true;
    len = sizeof(float);
    err = nvs_get_blob(nvs, "coef_gal", &loaded.working_coef_gal, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) invalid = true;
    len = sizeof(float);
    err = nvs_get_blob(nvs, "coef_liter", &loaded.working_coef_liter, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) invalid = true;
    len = sizeof(float);
    err = nvs_get_blob(nvs, "coef_ounce", &loaded.working_coef_ounce, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) invalid = true;

    nvs_close(nvs);
    invalid |= !coefficient_valid(loaded.working_coef_gal) ||
               !coefficient_valid(loaded.working_coef_liter) ||
               !coefficient_valid(loaded.working_coef_ounce) ||
               !isfinite(loaded.batch_preset_gal) || loaded.batch_preset_gal <= 0.0f ||
               loaded.meter_timeout_sec < DURA_METER_TIMEOUT_MIN_SEC ||
               loaded.meter_timeout_sec > DURA_METER_TIMEOUT_MAX_SEC ||
               !((loaded.backlight_timeout_sec == DURA_BACKLIGHT_TIMEOUT_OFF) ||
                 (loaded.backlight_timeout_sec >= DURA_BACKLIGHT_TIMEOUT_MIN_SEC &&
                  loaded.backlight_timeout_sec <= DURA_BACKLIGHT_TIMEOUT_MAX_SEC)) ||
               loaded.selected_language > DURA_LANGUAGE_SPANISH ||
               loaded.selected_precal_profile >= DURA_PRECAL_PROFILE_COUNT ||
               loaded.quick_cal_reference > DURA_QUICK_CAL_REFERENCE_MAX;
    for (uint8_t i = 0; i < DURA_PRECAL_PROFILE_COUNT; ++i) {
        invalid |= !coefficients_empty(&loaded.precal_profiles[i]) &&
                   !coefficients_valid(&loaded.precal_profiles[i]);
    }
    if (loaded.selected_precal_profile < DURA_PRECAL_PROFILE_COUNT) {
        invalid |= !coefficients_valid(&loaded.precal_profiles[loaded.selected_precal_profile]);
    }
    if (invalid) {
        ESP_LOGE(TAG, "rejected invalid persisted meter configuration");
        return ESP_ERR_INVALID_ARG;
    }
    loaded.screen = loaded.show_auto_batch ? DURA_SCREEN_BATCH : DURA_SCREEN_HOME;
    loaded.operation_mode = DURA_OPERATION_IDLE;
    loaded.pump_enabled = false;
    METER_LOCK();
    if (epoch != s_control_epoch) {
        METER_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    s_meter = loaded;
    METER_UNLOCK();
    return ESP_OK;
}

esp_err_t dura_meter_load(void)
{
    METER_LOCK();
    if (s_load_in_progress || s_calibration_save_in_progress) {
        METER_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    s_load_in_progress = true;
    /* Keep illumination policy stable until validated NVS state is published. */
    const uint8_t backlight_timeout_sec = s_meter.backlight_timeout_sec;
    s_meter = default_state();
    /* Zero is the uninitialized boot state, not a configured timeout. */
    if (backlight_timeout_sec != 0) {
        s_meter.backlight_timeout_sec = backlight_timeout_sec;
    }
    const uint64_t epoch = ++s_control_epoch;
    METER_UNLOCK();
    const esp_err_t err = load_meter_state(epoch);
    METER_LOCK();
    s_load_in_progress = false;
    METER_UNLOCK();
    return err;
}

/* Caller owns s_save_mutex; never hold the meter critical section over NVS. */
static esp_err_t save_meter_state(const dura_meter_state_t *saved)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(DURA_METER_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    if (err == ESP_OK) err = nvs_set_u32(nvs, "ee_check", DURA_EE_CHECK_VALUE);
    if (err == ESP_OK) err = nvs_set_u32(nvs, "meter_counts", saved->meter_total_counts);
    if (err == ESP_OK) err = nvs_set_u32(nvs, "batch_counts", saved->batch_total_counts);
    if (err == ESP_OK) err = nvs_set_u32(nvs, "total_counts", saved->total_total_counts);
    if (err == ESP_OK) err = nvs_set_u32(nvs, "odom_counts", saved->odometer_counts);
    if (err == ESP_OK) err = nvs_set_u32(nvs, "preset_counts", saved->preset_batch_counts);
    if (err == ESP_OK) err = nvs_set_u32(nvs, "units", (uint32_t)saved->selected_units);
    if (err == ESP_OK) err = nvs_set_u32(nvs, "fault_code", saved->fault_code);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "show_flow", saved->show_flow_rate ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "auto_batch", saved->show_auto_batch ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "show_batch", saved->show_batch_total ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "reset_zero", saved->reset_to_zero ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "meter_timeout", saved->meter_timeout_sec);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "backlight", saved->backlight_timeout_sec);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "language", (uint8_t)saved->selected_language);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "precal_sel", saved->selected_precal_profile);
    if (err == ESP_OK) err = nvs_set_u8(nvs, DURA_QUICK_CAL_NVS_KEY, saved->quick_cal_reference);
    if (err == ESP_OK) err = nvs_set_blob(nvs, "precal", saved->precal_profiles, sizeof(saved->precal_profiles));
    if (err == ESP_OK) err = nvs_set_blob(nvs, "batch_gal", &saved->batch_preset_gal, sizeof(float));
    if (err == ESP_OK) err = nvs_set_blob(nvs, "coef_gal", &saved->working_coef_gal, sizeof(float));
    if (err == ESP_OK) err = nvs_set_blob(nvs, "coef_liter", &saved->working_coef_liter, sizeof(float));
    if (err == ESP_OK) err = nvs_set_blob(nvs, "coef_ounce", &saved->working_coef_ounce, sizeof(float));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t dura_meter_save(void)
{
    ESP_RETURN_ON_FALSE(s_save_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "save mutex not initialized");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_save_mutex, portMAX_DELAY) == pdTRUE,
                        ESP_ERR_INVALID_STATE, TAG, "take save mutex");
    dura_meter_state_t saved;
    METER_LOCK();
    saved = s_meter;
    METER_UNLOCK();
    const esp_err_t err = save_meter_state(&saved);
    xSemaphoreGive(s_save_mutex);
    return err;
}

/* quick_reference == 0 selects a profile save/recall; finish_collection is
 * true only for the measured/bucket path, not an idle PreCal operation.
 * The reservation is acquired by the caller under s_meter_lock. Keep it through
 * the save-mutex wait, NVS, and publication, even if a safety action cancels us. */
static esp_err_t save_calibration(const dura_cal_coefficients_t *calibrated,
                                  uint8_t profile_index, uint8_t quick_reference,
                                  bool finish_collection, uint64_t epoch)
{
    if (s_save_mutex == NULL || xSemaphoreTake(s_save_mutex, portMAX_DELAY) != pdTRUE) {
        METER_LOCK();
        s_calibration_save_in_progress = false;
        METER_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    dura_meter_state_t saved;
    METER_LOCK();
    if (epoch != s_control_epoch) {
        s_calibration_save_in_progress = false;
        METER_UNLOCK();
        xSemaphoreGive(s_save_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    saved = s_meter;
    METER_UNLOCK();
    saved.working_coef_gal = calibrated->gallon_per_count;
    saved.working_coef_liter = calibrated->liter_per_count;
    saved.working_coef_ounce = calibrated->ounce_per_count;
    if (quick_reference != 0) {
        saved.quick_cal_reference = quick_reference;
    } else {
        saved.precal_profiles[profile_index] = *calibrated;
        saved.selected_precal_profile = profile_index;
    }
    esp_err_t err = save_meter_state(&saved);
    METER_LOCK();
    if (epoch != s_control_epoch) {
        err = ESP_ERR_INVALID_STATE; /* Never publish a revoked completion. */
    } else if (err == ESP_OK) {
        set_working_coefficients_locked(calibrated);
        if (quick_reference != 0) {
            s_meter.quick_cal_reference = quick_reference;
            s_meter.pending_quick_reference = 0;
            memset(&s_meter.pending_calibration, 0, sizeof(s_meter.pending_calibration));
        } else {
            s_meter.precal_profiles[profile_index] = *calibrated;
            s_meter.selected_precal_profile = profile_index;
            if (finish_collection) {
                memset(&s_meter.pending_calibration, 0, sizeof(s_meter.pending_calibration));
                s_meter.calibration_mode = DURA_CAL_IDLE;
                s_meter.screen = DURA_SCREEN_HOME;
                s_meter.operation_mode = DURA_OPERATION_IDLE;
            }
        }
    }
    /* Failure retains the measured WAIT_SAVE or Quick pending session. Idle
     * PreCal leaves its old coefficients/selection unchanged and may retry.
     * Only the in-flight writer reservation ends here. */
    s_calibration_save_in_progress = false;
    METER_UNLOCK();
    xSemaphoreGive(s_save_mutex);
    return err;
}

esp_err_t dura_meter_init(void)
{
    if (s_save_mutex == NULL) {
        s_save_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_save_mutex != NULL, ESP_ERR_NO_MEM, TAG, "create save mutex");
    }
    ESP_RETURN_ON_ERROR(dura_meter_load(), TAG, "load meter state");
    dura_meter_snapshot_t snapshot;
    ESP_RETURN_ON_ERROR(dura_meter_get_snapshot(&snapshot), TAG, "initial snapshot");
    ESP_LOGI(TAG, "initialized: screen=%s units=%s", dura_meter_screen_name(snapshot.screen), dura_meter_units_name(snapshot.selected_units));
    return ESP_OK;
}

esp_err_t dura_meter_get_snapshot(dura_meter_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    METER_LOCK();
    const float coef = selected_coef();
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->screen = s_meter.screen;
    snapshot->selected_units = s_meter.selected_units;
    snapshot->selected_language = s_meter.selected_language;
    snapshot->meter_timeout_sec = s_meter.meter_timeout_sec;
    snapshot->backlight_timeout_sec = s_meter.backlight_timeout_sec;
    snapshot->reset_to_zero = s_meter.reset_to_zero;
    snapshot->selected_precal_profile = s_meter.selected_precal_profile;
    snapshot->quick_cal_reference = s_meter.quick_cal_reference;
    snapshot->working_coefficients = working_coefficients_locked();
    memcpy(snapshot->precal_profiles, s_meter.precal_profiles, sizeof(snapshot->precal_profiles));
    snapshot->batch_mode = s_meter.batch_mode;
    snapshot->calibration_mode = s_meter.calibration_mode;
    snapshot->operation_mode = s_meter.operation_mode;
    snapshot->recipe_owner_id = s_meter.recipe_owner_id;
    snapshot->pump_enabled = s_meter.pump_enabled;
    snapshot->fault_latched = s_meter.fault_latched;
    snapshot->fault_code = s_meter.fault_code;
    snapshot->show_flow_rate = s_meter.show_flow_rate;
    snapshot->show_auto_batch = s_meter.show_auto_batch;
    snapshot->show_batch_total = s_meter.show_batch_total;
    snapshot->meter_total_counts = s_meter.meter_total_counts;
    snapshot->batch_total_counts = s_meter.batch_total_counts;
    snapshot->total_total_counts = s_meter.total_total_counts;
    snapshot->odometer_counts = s_meter.odometer_counts;
    snapshot->preset_batch_counts = s_meter.preset_batch_counts;
    snapshot->calibration_counts = s_meter.calibration_counts;
    snapshot->batch_preset_gal = s_meter.batch_preset_gal;
    snapshot->meter_total = coef * (float)s_meter.meter_total_counts;
    snapshot->batch_total = coef * (float)s_meter.batch_total_counts;
    snapshot->total_total = coef * (float)s_meter.total_total_counts;
    snapshot->remaining_batch = batch_remaining_display_value(s_meter.batch_requested_amount,
                                                               s_meter.batch_initial_counts,
                                                               s_meter.preset_batch_counts,
                                                               coef);
    snapshot->flow_rate = s_meter.flow_rate;
    METER_UNLOCK();
    return ESP_OK;
}

static const char *batch_mode_name(dura_batch_mode_t mode)
{
    switch (mode) {
    case DURA_BATCH_IDLE: return "idle";
    case DURA_BATCH_EDIT: return "edit";
    case DURA_BATCH_RUNNING: return "running";
    case DURA_BATCH_PAUSED: return "paused";
    case DURA_BATCH_DONE: return "done";
    default: return "unknown";
    }
}

esp_err_t dura_meter_format_status_json(char *json, size_t json_len)
{
    dura_meter_snapshot_t s;
    ESP_RETURN_ON_ERROR(dura_meter_get_snapshot(&s), TAG, "snapshot");
    int written = dura_json_snprintf(json, json_len,
                           "{\"screen\":\"%s\",\"units\":\"%s\",\"pump\":%s,\"fault\":%s,\"fault_code\":%lu,"
                           "\"batch_mode\":%u,\"batch_mode_name\":\"%s\",\"cal_mode\":%u,\"meter\":%.3f,\"batch\":%.3f,\"total\":%.3f,"
                           "\"remaining\":%.3f,\"flow_rate\":%.3f,\"counts\":{\"meter\":%lu,\"batch\":%lu,\"total\":%lu,\"odom\":%lu}}",
                           dura_meter_screen_name(s.screen), dura_meter_units_name(s.selected_units),
                           s.pump_enabled ? "true" : "false", s.fault_latched ? "true" : "false", (unsigned long)s.fault_code,
                           (unsigned)s.batch_mode, batch_mode_name(s.batch_mode), (unsigned)s.calibration_mode, s.meter_total, s.batch_total, s.total_total,
                           s.remaining_batch, s.flow_rate, (unsigned long)s.meter_total_counts, (unsigned long)s.batch_total_counts,
                           (unsigned long)s.total_total_counts, (unsigned long)s.odometer_counts);
    return dura_json_result(json, json_len, written);
}

esp_err_t dura_meter_format_config_json(char *json, size_t json_len)
{
    dura_meter_state_t state;
    METER_LOCK();
    state = s_meter;
    METER_UNLOCK();
    int written = dura_json_snprintf(json, json_len,
                           "{\"firmware\":\"%s\",\"show_auto_batch\":%s,\"show_flow_rate\":%s,"
                           "\"show_batch_total\":%s,\"batch_preset_gal\":%.3f,\"coef\":{\"gal\":%.8f,\"liter\":%.8f,\"ounce\":%.8f}}",
                           DURA_METER_FIRMWARE_VERSION,
                           state.show_auto_batch ? "true" : "false",
                           state.show_flow_rate ? "true" : "false",
                           state.show_batch_total ? "true" : "false",
                           state.batch_preset_gal,
                           state.working_coef_gal,
                           state.working_coef_liter,
                           state.working_coef_ounce);
    return dura_json_result(json, json_len, written);
}

esp_err_t dura_meter_start_manual(void)
{
    METER_LOCK();
    if (operation_busy_locked()) {
        METER_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    s_meter.batch_mode = DURA_BATCH_IDLE; /* A previous DONE cannot own this run. */
    s_meter.screen = DURA_SCREEN_RUN;
    s_meter.operation_mode = DURA_OPERATION_MANUAL;
    esp_err_t err = set_pump_locked(true);
    if (err != ESP_OK) s_meter.operation_mode = DURA_OPERATION_IDLE;
    METER_UNLOCK();
    return err;
}

esp_err_t dura_meter_stop_manual(void)
{
    METER_LOCK();
    if (s_load_in_progress || s_meter.recipe_owner_id != 0 || s_meter.calibration_mode != DURA_CAL_IDLE ||
        (s_meter.operation_mode != DURA_OPERATION_IDLE &&
         s_meter.operation_mode != DURA_OPERATION_MANUAL)) {
        METER_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    (void)set_pump_locked(false);
    s_meter.operation_mode = DURA_OPERATION_IDLE;
    s_meter.screen = DURA_SCREEN_HOME;
    METER_UNLOCK();
    return ESP_OK;
}

esp_err_t dura_meter_start_batch(float amount_in_selected_units)
{
    return dura_meter_start_batch_mode(amount_in_selected_units, DURA_OPERATION_AUTO);
}

/* Persist an OFF reservation. A cancellation can always acquire the brief meter
 * lock while this task waits on NVS. Only this still-current activation may ON. */
static esp_err_t finish_batch_activation(uint32_t activation_id, bool resume)
{
    const esp_err_t saved = dura_meter_save();
    METER_LOCK();
    if (!s_meter.activation_pending || s_meter.activation_id != activation_id ||
        s_meter.fault_latched) {
        METER_UNLOCK();
        return saved != ESP_OK ? saved : ESP_ERR_INVALID_STATE;
    }
    s_meter.activation_pending = false;
    if (saved != ESP_OK) {
        (void)set_pump_locked(false);
        if (!resume) {
            s_meter.operation_mode = DURA_OPERATION_IDLE;
            s_meter.batch_mode = DURA_BATCH_EDIT;
        }
        METER_UNLOCK();
        return saved;
    }
    s_meter.batch_mode = DURA_BATCH_RUNNING;
    const esp_err_t err = set_pump_locked(true);
    METER_UNLOCK();
    return err;
}

static esp_err_t start_batch(float amount, dura_operation_mode_t mode, uint32_t owner)
{
    if (!isfinite(amount) || amount <= 0.0f ||
        (mode != DURA_OPERATION_RECIRC && mode != DURA_OPERATION_AUTO)) {
        return ESP_ERR_INVALID_ARG;
    }
    METER_LOCK();
    if (s_calibration_save_in_progress || s_meter.pending_quick_reference != 0 ||
        s_load_in_progress || s_sleep_prepared || s_meter.fault_latched || s_meter.recipe_owner_id != owner ||
        s_meter.calibration_mode != DURA_CAL_IDLE || s_meter.activation_pending ||
        s_meter.batch_mode == DURA_BATCH_RUNNING || s_meter.batch_mode == DURA_BATCH_PAUSED ||
        s_meter.operation_mode != DURA_OPERATION_IDLE) {
        METER_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t counts = amount_to_counts(amount);
    if (counts == 0) {
        METER_UNLOCK();
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t activation = next_identity_locked();
    if (activation == 0) {
        METER_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    (void)set_pump_locked(false);
    s_meter.screen = DURA_SCREEN_BATCH;
    s_meter.batch_mode = DURA_BATCH_PAUSED;
    s_meter.operation_mode = mode;
    s_meter.preset_batch_counts = counts;
    s_meter.batch_requested_amount = amount;
    s_meter.batch_initial_counts = counts;
    s_meter.batch_preset_gal = selected_amount_to_gal(amount);
    s_meter.activation_id = activation;
    s_meter.activation_pending = true;
    METER_UNLOCK();
    return finish_batch_activation(activation, false);
}

esp_err_t dura_meter_start_batch_mode(float amount_in_selected_units,
                                      dura_operation_mode_t operation_mode)
{
    return start_batch(amount_in_selected_units, operation_mode, 0);
}

esp_err_t dura_meter_claim_recipe(uint32_t *owner_id)
{
    if (owner_id == NULL) return ESP_ERR_INVALID_ARG;
    *owner_id = 0;
    METER_LOCK();
    if (operation_busy_locked()) {
        METER_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t owner = next_identity_locked();
    if (owner == 0) {
        METER_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    s_meter.recipe_owner_id = owner;
    s_meter.batch_mode = DURA_BATCH_IDLE;
    *owner_id = owner;
    METER_UNLOCK();
    return ESP_OK;
}

esp_err_t dura_meter_start_owned_batch(float amount, uint32_t owner_id)
{
    if (owner_id == 0) return ESP_ERR_INVALID_ARG;
    return start_batch(amount, DURA_OPERATION_AUTO, owner_id);
}

esp_err_t dura_meter_release_recipe(uint32_t owner_id)
{
    if (owner_id == 0) return ESP_ERR_INVALID_ARG;
    METER_LOCK();
    if (s_meter.recipe_owner_id != owner_id) {
        METER_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    (void)set_pump_locked(false);
    s_meter.recipe_owner_id = 0;
    ++s_control_epoch;
    s_meter.operation_mode = DURA_OPERATION_IDLE;
    s_meter.batch_mode = DURA_BATCH_EDIT;
    s_meter.preset_batch_counts = 0;
    s_meter.batch_initial_counts = 0;
    s_meter.batch_requested_amount = 0.0f;
    METER_UNLOCK();
    return dura_meter_save();
}

esp_err_t dura_meter_stop_batch(void)
{
    return dura_meter_pause_batch();
}

esp_err_t dura_meter_pause_batch(void)
{
    METER_LOCK();
    if (s_meter.batch_mode != DURA_BATCH_RUNNING && !s_meter.activation_pending) {
        METER_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    (void)set_pump_locked(false);
    s_meter.batch_mode = DURA_BATCH_PAUSED;
    METER_UNLOCK();
    return dura_meter_save();
}

esp_err_t dura_meter_resume_batch(void)
{
    METER_LOCK();
    if (s_load_in_progress || s_sleep_prepared || s_meter.fault_latched || s_meter.activation_pending ||
        s_meter.calibration_mode != DURA_CAL_IDLE || s_meter.batch_mode != DURA_BATCH_PAUSED ||
        (s_meter.operation_mode != DURA_OPERATION_RECIRC && s_meter.operation_mode != DURA_OPERATION_AUTO) ||
        s_meter.preset_batch_counts <= DURA_SPIN_DOWN_OFFSET) {
        METER_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t activation = next_identity_locked();
    if (activation == 0) {
        METER_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    s_meter.screen = DURA_SCREEN_BATCH;
    s_meter.activation_id = activation;
    s_meter.activation_pending = true;
    METER_UNLOCK();
    return finish_batch_activation(activation, true);
}

esp_err_t dura_meter_cancel_batch(void)
{
    METER_LOCK();
    if (s_meter.recipe_owner_id == 0 && s_meter.batch_mode != DURA_BATCH_RUNNING &&
        s_meter.batch_mode != DURA_BATCH_PAUSED &&
        s_meter.batch_mode != DURA_BATCH_DONE) {
        METER_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    (void)set_pump_locked(false);
    s_meter.recipe_owner_id = 0;
    ++s_control_epoch;
    s_meter.screen = DURA_SCREEN_BATCH;
    s_meter.batch_mode = DURA_BATCH_EDIT;
    s_meter.operation_mode = DURA_OPERATION_IDLE;
    s_meter.preset_batch_counts = 0;
    s_meter.batch_initial_counts = 0;
    s_meter.batch_requested_amount = 0.0f;
    METER_UNLOCK();
    return dura_meter_save();
}

esp_err_t dura_meter_start_calibration(void)
{
    METER_LOCK();
    if (operation_busy_locked()) {
        METER_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    s_meter.batch_mode = DURA_BATCH_IDLE;
    s_meter.screen = DURA_SCREEN_CALIBRATION;
    s_meter.calibration_mode = DURA_CAL_RUNNING;
    s_meter.calibration_counts = 0;
    s_meter.operation_mode = DURA_OPERATION_CALIBRATION;
    const esp_err_t err = set_pump_locked(true);
    if (err != ESP_OK) s_meter.operation_mode = DURA_OPERATION_IDLE;
    METER_UNLOCK();
    return err;
}

esp_err_t dura_meter_stop_calibration(void)
{
    METER_LOCK();
    if (s_meter.calibration_mode != DURA_CAL_RUNNING) {
        METER_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    (void)set_pump_locked(false);
    s_meter.calibration_mode = (s_meter.calibration_counts >= DURA_MIN_FLOW_COUNTS) ? DURA_CAL_WAIT_MEASURED_AMOUNT : DURA_CAL_IDLE;
    s_meter.operation_mode = s_meter.calibration_mode == DURA_CAL_IDLE ?
                             DURA_OPERATION_IDLE : DURA_OPERATION_CALIBRATION;
    const uint32_t counts = s_meter.calibration_counts;
    const bool rejected = s_meter.calibration_mode == DURA_CAL_IDLE;
    METER_UNLOCK();
    if (rejected) {
        ESP_LOGW(TAG, "[%s] calibration rejected: only %lu counts", log_context_prefix(), (unsigned long)counts);
    }
    return ESP_OK;
}

esp_err_t dura_meter_continue_calibration(void)
{
    METER_LOCK();
    if (s_meter.calibration_mode != DURA_CAL_RUNNING) {
        METER_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_meter.calibration_counts >= DURA_MIN_FLOW_COUNTS) {
        (void)set_pump_locked(false);
        s_meter.operation_mode = DURA_OPERATION_CALIBRATION;
        s_meter.calibration_mode = DURA_CAL_WAIT_MEASURED_AMOUNT;
        METER_UNLOCK();
        return ESP_OK;
    }
    s_meter.screen = DURA_SCREEN_CALIBRATION;
    s_meter.calibration_counts = 0;
    s_meter.operation_mode = DURA_OPERATION_CALIBRATION;
    esp_err_t err = set_pump_locked(true);
    if (err != ESP_OK) s_meter.operation_mode = DURA_OPERATION_IDLE;
    METER_UNLOCK();
    return err;
}

esp_err_t dura_meter_reset_calibration(void)
{
    METER_LOCK();
    if (s_meter.calibration_mode != DURA_CAL_RUNNING) {
        METER_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    s_meter.screen = DURA_SCREEN_CALIBRATION;
    s_meter.calibration_counts = 0;
    s_meter.operation_mode = DURA_OPERATION_CALIBRATION;
    esp_err_t err = set_pump_locked(true);
    if (err != ESP_OK) s_meter.operation_mode = DURA_OPERATION_IDLE;
    METER_UNLOCK();
    return err;
}

esp_err_t dura_meter_cancel_calibration(void)
{
    METER_LOCK();
    if (s_meter.calibration_mode != DURA_CAL_RUNNING &&
        s_meter.calibration_mode != DURA_CAL_WAIT_MEASURED_AMOUNT &&
        s_meter.calibration_mode != DURA_CAL_WAIT_SAVE && s_meter.pending_quick_reference == 0) {
        METER_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    (void)set_pump_locked(false);
    s_meter.operation_mode = DURA_OPERATION_IDLE;
    s_meter.screen = DURA_SCREEN_CALIBRATION;
    s_meter.calibration_mode = DURA_CAL_IDLE;
    s_meter.calibration_counts = 0;
    memset(&s_meter.pending_calibration, 0, sizeof(s_meter.pending_calibration));
    s_meter.pending_quick_reference = 0;
    ++s_control_epoch;
    METER_UNLOCK();
    return ESP_OK;
}

esp_err_t dura_meter_accept_calibration_amount(float measured_amount_in_selected_units)
{
    if (!isfinite(measured_amount_in_selected_units) || measured_amount_in_selected_units <= 0.0f) {
        return ESP_ERR_INVALID_ARG;
    }

    METER_LOCK();
    if (s_meter.calibration_mode != DURA_CAL_WAIT_MEASURED_AMOUNT ||
        s_meter.calibration_counts < DURA_MIN_FLOW_COUNTS ||
        (s_meter.batch_mode != DURA_BATCH_IDLE && s_meter.batch_mode != DURA_BATCH_DONE) ||
        !units_valid(s_meter.selected_units)) {
        METER_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }

    const float selected = measured_amount_in_selected_units / (float)s_meter.calibration_counts;
    float gallon;
    float liter;
    float ounce;
    switch (s_meter.selected_units) {
    case DURA_UNITS_GALLON:
        gallon = selected;
        liter = gallon * 3.8f;
        ounce = gallon * 128.0f;
        break;
    case DURA_UNITS_LITER:
        liter = selected;
        gallon = liter / 3.8f;
        ounce = gallon * 128.0f;
        break;
    case DURA_UNITS_OUNCE:
        ounce = selected;
        gallon = ounce / 128.0f;
        liter = gallon * 3.8f;
        break;
    default:
        METER_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }

    if (!coefficient_valid(gallon) || !coefficient_valid(liter) || !coefficient_valid(ounce)) {
        METER_UNLOCK();
        return ESP_ERR_INVALID_ARG;
    }

    /* Acceptance only stages a candidate.  The active coefficients remain
     * untouched until an explicit choice on the following Save screen. */
    const dura_cal_coefficients_t calibrated = {gallon, liter, ounce};
    s_meter.pending_calibration = calibrated;
    s_meter.calibration_mode = DURA_CAL_WAIT_SAVE;
    (void)set_pump_locked(false);
    s_meter.operation_mode = DURA_OPERATION_CALIBRATION;
    METER_UNLOCK();
    return ESP_OK;
}

esp_err_t dura_meter_finish_calibration(uint8_t profile_index)
{
    if (profile_index >= DURA_PRECAL_PROFILE_COUNT) return ESP_ERR_INVALID_ARG;
    METER_LOCK();
    if (s_calibration_save_in_progress || s_load_in_progress || s_sleep_prepared ||
        s_meter.fault_latched || s_meter.calibration_mode != DURA_CAL_WAIT_SAVE ||
        !coefficients_valid(&s_meter.pending_calibration)) {
        METER_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    const dura_cal_coefficients_t calibrated = s_meter.pending_calibration;
    s_calibration_save_in_progress = true;
    const uint64_t epoch = s_control_epoch;
    METER_UNLOCK();
    return save_calibration(&calibrated, profile_index, 0, true, epoch);
}

esp_err_t dura_meter_prepare_for_sleep(void)
{
    METER_LOCK();
    if (s_sleep_prepared) {
        METER_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    s_sleep_prepared = true;
    (void)set_pump_locked(false);
    s_meter.recipe_owner_id = 0;
    ++s_control_epoch;
    if (s_meter.batch_mode == DURA_BATCH_RUNNING) s_meter.batch_mode = DURA_BATCH_PAUSED;
    /* A paused recirculation keeps its existing UI/explicit-resume context,
     * just as pause_batch does. OFF above invalidates activation; retaining
     * this discriminator does not restore RUNNING or any output intent. */
    if (s_meter.batch_mode != DURA_BATCH_PAUSED ||
        s_meter.operation_mode != DURA_OPERATION_RECIRC) {
        s_meter.operation_mode = DURA_OPERATION_IDLE;
    }
    if (s_meter.calibration_mode == DURA_CAL_RUNNING) {
        s_meter.calibration_mode = (s_meter.calibration_counts >= DURA_MIN_FLOW_COUNTS) ?
                                   DURA_CAL_WAIT_MEASURED_AMOUNT : DURA_CAL_IDLE;
    }
    METER_UNLOCK();
    const esp_err_t err = dura_meter_save();
    /* Board owns unwind, including persistence failure; keep activation fenced. */
    return err;
}

esp_err_t dura_meter_cancel_sleep_prepare(void)
{
    METER_LOCK();
    s_sleep_prepared = false;
    METER_UNLOCK();
    return ESP_OK;
}

/* Release_1_1_3: enabled 31-tap integer FIR /524288; 40-slot trimmed average,
 * min 5 samples; 80/20 blend; 1500-tick timeout. The 1ms tick corresponds to
 * TIM2 at 8MHz, PSC=0, ARR=7999 and the source's 0.001 conversion, not its
 * conflicting 100us comment or compiler Clock metadata.
 * Port adaptation: fixed 100ms logical blend cadence in every mode, independent
 * of snapshot frequency. Replay this timeline around queued pulse timestamps.
 * Preserve legacy selected-unit scalar across unit/coefficient changes; only
 * the next blend applies the newly selected coefficient to its new term. */
static void clear_flow_delta_locked(void)
{
    memset(s_meter.flow_delta, 0, sizeof(s_meter.flow_delta));
    s_meter.flow_samples = 0;
    s_meter.flow_rate = 0.0f;
    /* Legacy clearDeltaFlow deliberately retains FIR history AND ring index. */
}

static void advance_flow_locked(int64_t now_us)
{
    if (now_us < 0) return;
    if (now_us / 1000 - s_meter.last_flow_tick >= 1500) {
        clear_flow_delta_locked();
        /* Skip idle time in O(1); no unbounded catch-up after sleep/long gaps. */
        if (s_meter.next_flow_update_us <= now_us) {
            s_meter.next_flow_update_us +=
                ((now_us - s_meter.next_flow_update_us) / 100000 + 1) * 100000;
        }
        return;
    }
    while (s_meter.next_flow_update_us <= now_us) {
        float hertz = 0.0f;
        if (s_meter.flow_samples >= 5) {
            uint32_t sum = 0;
            uint16_t low = UINT16_MAX, high = 0;
            /* Preserve legacy prefix scan, including its post-clear behavior. */
            for (unsigned i = 0; i < s_meter.flow_samples; ++i) {
                const uint16_t value = s_meter.flow_delta[i];
                sum += value;
                if (value < low) low = value;
                if (value > high) high = value;
            }
            const float average = (float)(sum - low - high) /
                                  (float)(s_meter.flow_samples - 2);
            if (average > 0.0f) hertz = 1.0f / (0.001f * average);
        }
        /* Separate binary32 steps match the verified listing's operation order.
         * Volatile temporaries prevent fusion/reassociation across these steps.
         * Compiled (1.00f - .80f) is 0x3e4ccccc, NOT the literal .20f. */
        volatile float old_rate = s_meter.flow_rate * (1.00f - 0.80f);
        volatile float new_rate = hertz * 60.0f;
        new_rate = new_rate * selected_coef();
        new_rate = 0.80f * new_rate;
        s_meter.flow_rate = old_rate + new_rate;
        s_meter.next_flow_update_us += 100000;
    }
}

void dura_meter_ingest_flow_timestamp_locked(int64_t timestamp_us)
{
    static const uint16_t coefficients[31] = {
        14750,15149,15531,15893,16235,16555,16851,17121,
        17364,17578,17762,17915,18035,18123,18177,18198,
        18177,18123,18035,17915,17762,17578,17364,17121,
        16851,16555,16235,15893,15531,15149,14750
    };
    if (timestamp_us < 0) return;
    advance_flow_locked(timestamp_us - 1);
    if (s_meter.screen == DURA_SCREEN_CALIBRATION &&
        s_meter.calibration_mode == DURA_CAL_RUNNING) return;
    const int64_t tick = timestamp_us / 1000;
    if (tick < s_meter.last_flow_tick) return;
    int64_t delta = tick - s_meter.last_flow_tick;
    if (delta >= 1500) {
        clear_flow_delta_locked();
        delta = 1500;
    }
    for (unsigned i = 30; i > 0; --i) s_meter.flow_fir[i] = s_meter.flow_fir[i - 1];
    s_meter.flow_fir[0] = (uint16_t)delta;
    uint32_t sum = 0; /* 1500 * sum(coefficients) fits signed 32 legacy accumulator. */
    for (unsigned i = 0; i < 31; ++i) sum += (uint32_t)coefficients[i] * s_meter.flow_fir[i];
    s_meter.flow_delta[s_meter.flow_index] = (uint16_t)(sum / 524288U);
    s_meter.flow_index = (s_meter.flow_index + 1) % 40;
    if (s_meter.flow_samples < 40) ++s_meter.flow_samples;
    s_meter.last_flow_tick = tick;
}

static bool record_pulse_locked(uint32_t count)
{
    if (count == 0) {
        return false;
    }
    bool batch_done = false;

    if (s_meter.screen == DURA_SCREEN_CALIBRATION && s_meter.calibration_mode == DURA_CAL_RUNNING) {
        s_meter.calibration_counts = (count > UINT32_MAX - s_meter.calibration_counts) ?
                                     UINT32_MAX : s_meter.calibration_counts + count;
        return false;
    }

    s_meter.meter_total_counts = (count > UINT32_MAX - s_meter.meter_total_counts) ? UINT32_MAX : s_meter.meter_total_counts + count;
    s_meter.batch_total_counts = (count > UINT32_MAX - s_meter.batch_total_counts) ? UINT32_MAX : s_meter.batch_total_counts + count;
    s_meter.total_total_counts = (count > UINT32_MAX - s_meter.total_total_counts) ? UINT32_MAX : s_meter.total_total_counts + count;
    s_meter.odometer_counts = (count > UINT32_MAX - s_meter.odometer_counts) ? UINT32_MAX : s_meter.odometer_counts + count;

    if (s_meter.pump_enabled && s_meter.screen == DURA_SCREEN_BATCH && s_meter.batch_mode == DURA_BATCH_RUNNING) {
        if (s_meter.preset_batch_counts > count) {
            s_meter.preset_batch_counts -= count;
        } else {
            s_meter.preset_batch_counts = 0;
        }
        if (s_meter.preset_batch_counts <= DURA_SPIN_DOWN_OFFSET) {
            (void)set_pump_locked(false);
            s_meter.batch_mode = DURA_BATCH_DONE;
            s_meter.operation_mode = DURA_OPERATION_IDLE;
            batch_done = true;
        }
    }
    return batch_done;
}

esp_err_t dura_meter_record_pulse(uint32_t count)
{
    METER_LOCK();
    const bool batch_done = record_pulse_locked(count);
    METER_UNLOCK();
    if (batch_done) (void)dura_meter_save();
    return ESP_OK;
}

esp_err_t dura_meter_set_units(dura_units_t units)
{
    if (!units_valid(units)) {
        return ESP_ERR_INVALID_ARG;
    }
    METER_LOCK();
    if (operation_busy_locked()) {
        METER_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    s_meter.selected_units = units;
    METER_UNLOCK();
    return dura_meter_save();
}

esp_err_t dura_meter_set_language(dura_language_t language)
{
    if (language < DURA_LANGUAGE_ENGLISH || language > DURA_LANGUAGE_SPANISH) return ESP_ERR_INVALID_ARG;
    METER_LOCK();
    s_meter.selected_language = language;
    METER_UNLOCK();
    return dura_meter_save();
}

esp_err_t dura_meter_set_meter_timeout(uint8_t seconds)
{
    if (seconds < DURA_METER_TIMEOUT_MIN_SEC || seconds > DURA_METER_TIMEOUT_MAX_SEC) return ESP_ERR_INVALID_ARG;
    METER_LOCK();
    s_meter.meter_timeout_sec = seconds;
    METER_UNLOCK();
    return dura_meter_save();
}

esp_err_t dura_meter_set_backlight_timeout(uint8_t seconds)
{
    if (seconds != DURA_BACKLIGHT_TIMEOUT_OFF &&
        (seconds < DURA_BACKLIGHT_TIMEOUT_MIN_SEC || seconds > DURA_BACKLIGHT_TIMEOUT_MAX_SEC)) {
        return ESP_ERR_INVALID_ARG;
    }
    METER_LOCK();
    s_meter.backlight_timeout_sec = seconds;
    METER_UNLOCK();
    return dura_meter_save();
}

esp_err_t dura_meter_set_reset_to_zero(bool enabled)
{
    METER_LOCK();
    s_meter.reset_to_zero = enabled;
    METER_UNLOCK();
    return dura_meter_save();
}

esp_err_t dura_meter_select_precal_profile(uint8_t profile_index)
{
    if (profile_index >= DURA_PRECAL_PROFILE_COUNT) return ESP_ERR_INVALID_ARG;
    METER_LOCK();
    if (operation_busy_locked()) {
        METER_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    if (!coefficients_valid(&s_meter.precal_profiles[profile_index])) {
        METER_UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }
    const dura_cal_coefficients_t selected = s_meter.precal_profiles[profile_index];
    s_calibration_save_in_progress = true;
    const uint64_t epoch = s_control_epoch;
    METER_UNLOCK();
    return save_calibration(&selected, profile_index, 0, false, epoch);
}

esp_err_t dura_meter_store_working_precal_profile(uint8_t profile_index)
{
    if (profile_index >= DURA_PRECAL_PROFILE_COUNT) return ESP_ERR_INVALID_ARG;
    METER_LOCK();
    if (operation_busy_locked()) {
        METER_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    const dura_cal_coefficients_t working = working_coefficients_locked();
    if (!coefficients_valid(&working)) {
        METER_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    s_calibration_save_in_progress = true;
    const uint64_t epoch = s_control_epoch;
    METER_UNLOCK();
    return save_calibration(&working, profile_index, 0, false, epoch);
}

esp_err_t dura_meter_apply_quick_cal(uint8_t reference)
{
    if (reference < DURA_QUICK_CAL_REFERENCE_MIN || reference > DURA_QUICK_CAL_REFERENCE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    METER_LOCK();
    /* Retry may edit its reference, but must not bypass another owner or writer. */
    if (s_calibration_save_in_progress || s_load_in_progress || s_sleep_prepared ||
        s_meter.fault_latched || s_meter.recipe_owner_id != 0 ||
        s_meter.activation_pending || s_meter.pump_enabled ||
        s_meter.operation_mode != DURA_OPERATION_IDLE ||
        s_meter.calibration_mode != DURA_CAL_IDLE ||
        s_meter.batch_mode == DURA_BATCH_RUNNING || s_meter.batch_mode == DURA_BATCH_PAUSED) {
        METER_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    const float factor = dura_quick_cal_entry(reference)->water_factor;
    const dura_cal_coefficients_t *water = &s_meter.precal_profiles[0];
    if (!coefficients_valid(water)) {
        METER_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    const dura_cal_coefficients_t quick = {
        water->gallon_per_count * factor,
        water->liter_per_count * factor,
        water->ounce_per_count * factor,
    };
    s_meter.pending_quick_reference = reference;
    s_meter.pending_calibration = quick;
    s_calibration_save_in_progress = true;
    const uint64_t epoch = s_control_epoch;
    METER_UNLOCK();
    return save_calibration(&quick, 0, reference, false, epoch);
}

esp_err_t dura_meter_set_show_flow_rate(bool enabled)
{
    METER_LOCK();
    s_meter.show_flow_rate = enabled;
    METER_UNLOCK();
    return dura_meter_save();
}

esp_err_t dura_meter_set_show_batch_total(bool enabled)
{
    METER_LOCK();
    s_meter.show_batch_total = enabled;
    METER_UNLOCK();
    return dura_meter_save();
}

esp_err_t dura_meter_set_auto_batch(bool enabled)
{
    METER_LOCK();
    if (operation_busy_locked()) {
        METER_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    s_meter.show_auto_batch = enabled;
    s_meter.screen = enabled ? DURA_SCREEN_BATCH : DURA_SCREEN_HOME;
    (void)set_pump_locked(false);
    s_meter.operation_mode = DURA_OPERATION_IDLE;
    METER_UNLOCK();
    return dura_meter_save();
}

esp_err_t dura_meter_reset_meter_total(void)
{
    METER_LOCK();
    s_meter.meter_total_counts = 0;
    METER_UNLOCK();
    return dura_meter_save();
}

esp_err_t dura_meter_reset_batch_total(void)
{
    METER_LOCK();
    s_meter.batch_total_counts = 0;
    s_meter.meter_total_counts = 0;
    METER_UNLOCK();
    return dura_meter_save();
}

esp_err_t dura_meter_reset_total_total(void)
{
    METER_LOCK();
    s_meter.total_total_counts = 0;
    s_meter.meter_total_counts = 0;
    METER_UNLOCK();
    return dura_meter_save();
}

esp_err_t dura_meter_set_screen_home(void)
{
    METER_LOCK();
    (void)set_pump_locked(false);
    s_meter.recipe_owner_id = 0;
    ++s_control_epoch;
    s_meter.operation_mode = DURA_OPERATION_IDLE;
    s_meter.screen = DURA_SCREEN_HOME;
    s_meter.batch_mode = DURA_BATCH_IDLE;
    s_meter.calibration_mode = DURA_CAL_IDLE;
    s_meter.calibration_counts = 0;
    memset(&s_meter.pending_calibration, 0, sizeof(s_meter.pending_calibration));
    s_meter.pending_quick_reference = 0;
    METER_UNLOCK();
    return dura_meter_save();
}

esp_err_t dura_meter_set_fault(uint32_t fault_code)
{
    METER_LOCK();
    s_meter.fault_latched = true;
    s_meter.fault_code = fault_code;
    (void)set_pump_locked(false);
    s_meter.recipe_owner_id = 0;
    ++s_control_epoch;
    s_meter.operation_mode = DURA_OPERATION_IDLE;
    if (s_meter.batch_mode == DURA_BATCH_RUNNING) s_meter.batch_mode = DURA_BATCH_PAUSED;
    if (s_meter.calibration_mode == DURA_CAL_RUNNING) {
        s_meter.calibration_mode = (s_meter.calibration_counts >= DURA_MIN_FLOW_COUNTS) ?
                                   DURA_CAL_WAIT_MEASURED_AMOUNT : DURA_CAL_IDLE;
    }
    METER_UNLOCK();
    return dura_meter_save();
}

esp_err_t dura_meter_clear_fault(void)
{
    METER_LOCK();
    s_meter.fault_latched = false;
    s_meter.fault_code = 0;
    METER_UNLOCK();
    return dura_meter_save();
}

void dura_meter_set_log_context(const char *context)
{
    METER_LOCK();
    s_log_context = context;
    METER_UNLOCK();
}

const char *dura_meter_screen_name(dura_screen_t screen)
{
    switch (screen) {
    case DURA_SCREEN_EDIT: return "edit";
    case DURA_SCREEN_HOME: return "home";
    case DURA_SCREEN_RUN: return "run";
    case DURA_SCREEN_INFO: return "info";
    case DURA_SCREEN_SCAN: return "scan";
    case DURA_SCREEN_COMPANY_INFO: return "company_info";
    case DURA_SCREEN_CALIBRATION: return "calibration";
    case DURA_SCREEN_BATCH: return "batch";
    default: return "unknown";
    }
}

const char *dura_meter_units_name(dura_units_t units)
{
    switch (units) {
    case DURA_UNITS_GALLON: return "gal";
    case DURA_UNITS_LITER: return "liter";
    case DURA_UNITS_OUNCE: return "ounce";
    default: return "unknown";
    }
}
