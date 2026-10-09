#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dura_control_policy.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DURA_METER_FIRMWARE_VERSION "0.1.3"
#define DURA_METER_NVS_NAMESPACE    "dura_meter"

typedef enum {
    DURA_SCREEN_EDIT = 0,
    DURA_SCREEN_HOME = 30,
    DURA_SCREEN_RUN = 40,
    DURA_SCREEN_INFO = 50,
    DURA_SCREEN_SCAN = 60,
    DURA_SCREEN_COMPANY_INFO = 70,
    DURA_SCREEN_CALIBRATION = 80,
    DURA_SCREEN_BATCH = 90,
} dura_screen_t;

typedef enum {
    DURA_UNITS_GALLON = 1,
    DURA_UNITS_LITER = 2,
    DURA_UNITS_OUNCE = 3,
} dura_units_t;

typedef enum {
    DURA_LANGUAGE_ENGLISH = 0,
    DURA_LANGUAGE_PORTUGUESE = 1,
    DURA_LANGUAGE_SPANISH = 2,
} dura_language_t;

#define DURA_METER_TIMEOUT_MIN_SEC       20u
#define DURA_METER_TIMEOUT_MAX_SEC       120u
#define DURA_BACKLIGHT_TIMEOUT_OFF       9u
#define DURA_BACKLIGHT_TIMEOUT_MIN_SEC   10u
#define DURA_BACKLIGHT_TIMEOUT_MAX_SEC   30u
#define DURA_PRECAL_PROFILE_COUNT        6u
#define DURA_QUICK_CAL_REFERENCE_MIN     1u
#define DURA_QUICK_CAL_REFERENCE_MAX     21u
#define DURA_MIN_FLOW_COUNTS       250U

typedef struct {
    float gallon_per_count;
    float liter_per_count;
    float ounce_per_count;
} dura_cal_coefficients_t;

typedef enum {
    DURA_BATCH_IDLE = 0,
    DURA_BATCH_EDIT,
    DURA_BATCH_RUNNING,
    DURA_BATCH_PAUSED,
    DURA_BATCH_DONE,
} dura_batch_mode_t;

typedef enum {
    DURA_CAL_IDLE = 0,
    DURA_CAL_RUNNING,
    DURA_CAL_WAIT_MEASURED_AMOUNT,
    DURA_CAL_WAIT_SAVE,
} dura_calibration_mode_t;

typedef struct {
    dura_screen_t screen;
    dura_units_t selected_units;
    dura_language_t selected_language;
    uint8_t meter_timeout_sec;
    uint8_t backlight_timeout_sec; /* 9 is the legacy "off" sentinel. */
    bool reset_to_zero;
    uint8_t selected_precal_profile;
    uint8_t quick_cal_reference;
    dura_cal_coefficients_t working_coefficients;
    dura_cal_coefficients_t precal_profiles[DURA_PRECAL_PROFILE_COUNT];
    dura_batch_mode_t batch_mode;
    dura_calibration_mode_t calibration_mode;
    dura_operation_mode_t operation_mode;
    uint32_t recipe_owner_id; /* Boot-scoped lease; zero means no recipe owner. */
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
    uint32_t calibration_counts;
    float batch_preset_gal;
    float meter_total;
    float batch_total;
    float total_total;
    float remaining_batch;
    float flow_rate;
} dura_meter_snapshot_t;

/* Task-context output handoff. Handler runs inside the meter critical section:
 * bounded GPIO writes only, no blocking/logging/allocation or meter re-entry.
 * The board owns pins/polarities; the meter owns the atomic output intent. */
#define DURA_METER_HAS_SYNC_OUTPUT_HANDLER 1
typedef void (*dura_meter_output_handler_t)(dura_output_intent_t intent);
esp_err_t dura_meter_set_output_handler(dura_meter_output_handler_t handler);
esp_err_t dura_meter_sync_outputs(void);
/* Recipe lease spans every step, including pauses and remote-only intervals.
 * Generic cancel/Home/fault/sleep/load/defaults revoke it. A stale release never
 * cancels unrelated work. IDs are not reused by runtime load/defaults. */
esp_err_t dura_meter_claim_recipe(uint32_t *owner_id);
esp_err_t dura_meter_start_owned_batch(float amount, uint32_t owner_id);
esp_err_t dura_meter_release_recipe(uint32_t owner_id);

esp_err_t dura_meter_init(void);
esp_err_t dura_meter_load(void);
esp_err_t dura_meter_save(void);
esp_err_t dura_meter_factory_defaults(void);
esp_err_t dura_meter_get_snapshot(dura_meter_snapshot_t *snapshot);
esp_err_t dura_meter_format_status_json(char *json, size_t json_len);
esp_err_t dura_meter_format_config_json(char *json, size_t json_len);
esp_err_t dura_meter_start_manual(void);
esp_err_t dura_meter_stop_manual(void);
esp_err_t dura_meter_start_batch(float amount_in_selected_units);
esp_err_t dura_meter_start_batch_mode(float amount_in_selected_units,
                                      dura_operation_mode_t operation_mode);
esp_err_t dura_meter_stop_batch(void);
esp_err_t dura_meter_pause_batch(void);
esp_err_t dura_meter_resume_batch(void);
esp_err_t dura_meter_cancel_batch(void);
esp_err_t dura_meter_start_calibration(void);
esp_err_t dura_meter_stop_calibration(void);
esp_err_t dura_meter_continue_calibration(void);
esp_err_t dura_meter_reset_calibration(void);
esp_err_t dura_meter_cancel_calibration(void);
esp_err_t dura_meter_accept_calibration_amount(float measured_amount_in_selected_units);
esp_err_t dura_meter_finish_calibration(uint8_t profile_index);
#define DURA_METER_HAS_SLEEP_FENCE 1
/* Prepare (including persistence failure) fences all activation until sleep, or explicit cleanup
 * after a board preparation error. Cleanup never restarts an operation. */
esp_err_t dura_meter_prepare_for_sleep(void);
esp_err_t dura_meter_cancel_sleep_prepare(void);
esp_err_t dura_meter_record_pulse(uint32_t count);
/* Task-only board ingress boundary. enter=true locks ingress and returns all
 * qualified pre-boundary pulses; enter=false releases ingress. No blocking,
 * allocation, NVS, logging or meter re-entry is permitted in this callback. */
typedef uint32_t (*dura_meter_flow_handler_t)(bool enter);
esp_err_t dura_meter_set_flow_handler(dura_meter_flow_handler_t handler);
/* Drain atomically with meter transitions; returns activity since last poll. */
uint32_t dura_meter_drain_flow(void);
esp_err_t dura_meter_set_units(dura_units_t units);
esp_err_t dura_meter_set_language(dura_language_t language);
esp_err_t dura_meter_set_meter_timeout(uint8_t seconds);
esp_err_t dura_meter_set_backlight_timeout(uint8_t seconds);
esp_err_t dura_meter_set_reset_to_zero(bool enabled);
esp_err_t dura_meter_select_precal_profile(uint8_t profile_index);
esp_err_t dura_meter_store_working_precal_profile(uint8_t profile_index);
esp_err_t dura_meter_apply_quick_cal(uint8_t reference);
esp_err_t dura_meter_set_show_flow_rate(bool enabled);
esp_err_t dura_meter_set_show_batch_total(bool enabled);
esp_err_t dura_meter_set_auto_batch(bool enabled);
esp_err_t dura_meter_reset_meter_total(void);
esp_err_t dura_meter_reset_batch_total(void);
esp_err_t dura_meter_reset_total_total(void);
esp_err_t dura_meter_set_screen_home(void);
esp_err_t dura_meter_set_fault(uint32_t fault_code);
esp_err_t dura_meter_clear_fault(void);
void dura_meter_set_log_context(const char *context);
const char *dura_meter_screen_name(dura_screen_t screen);
const char *dura_meter_units_name(dura_units_t units);

#ifdef __cplusplus
}
#endif
