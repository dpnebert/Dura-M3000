#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Runtime-only command routing.  This is intentionally not persisted: every
 * boot, fault, Stop, and sleep transition begins in the all-off state. */
typedef enum {
    DURA_OPERATION_IDLE = 0,
    DURA_OPERATION_MANUAL,
    DURA_OPERATION_CALIBRATION,
    DURA_OPERATION_RECIRC,
    DURA_OPERATION_AUTO,
} dura_operation_mode_t;

typedef struct {
    bool pump;
    bool recirc_ev;
    bool inject_ev;
} dura_output_intent_t;

typedef struct {
    dura_operation_mode_t operation_mode;
    bool pump_enabled;
    bool batch_active;
    bool calibration_active;
    bool fault_latched;
    bool ble_connected;
    uint64_t inactive_ms;
    uint32_t timeout_sec;
} dura_sleep_policy_input_t;

dura_output_intent_t dura_control_output_intent(dura_operation_mode_t mode,
                                                 bool pump_enabled);
bool dura_control_sleep_allowed(const dura_sleep_policy_input_t *input);

#ifdef __cplusplus
}
#endif