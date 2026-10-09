#include "dura_control_policy.h"

#include <stddef.h>

dura_output_intent_t dura_control_output_intent(dura_operation_mode_t mode,
                                                 bool pump_enabled)
{
    dura_output_intent_t intent = {0};
    if (!pump_enabled) return intent;

    switch (mode) {
    case DURA_OPERATION_MANUAL:
    case DURA_OPERATION_AUTO:
        intent.pump = true;
        intent.inject_ev = true;
        break;
    case DURA_OPERATION_CALIBRATION:
    case DURA_OPERATION_RECIRC:
        intent.pump = true;
        intent.recirc_ev = true;
        break;
    case DURA_OPERATION_IDLE:
    default:
        break;
    }
    return intent;
}

bool dura_control_sleep_allowed(const dura_sleep_policy_input_t *input)
{
    if (input == NULL || input->timeout_sec == 0u) return false;

    const uint64_t timeout_ms = (uint64_t)input->timeout_sec * 1000u;
    return input->operation_mode == DURA_OPERATION_IDLE &&
           !input->pump_enabled &&
           !input->batch_active &&
           !input->calibration_active &&
           !input->fault_latched &&
           !input->ble_connected &&
           input->inactive_ms >= timeout_ms;
}