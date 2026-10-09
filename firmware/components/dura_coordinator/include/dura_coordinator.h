#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DURA_COORDINATOR_IDLE = 0,
    DURA_COORDINATOR_LEADER_RUNNING,
    DURA_COORDINATOR_FOLLOWER_RUNNING,
    DURA_COORDINATOR_FAULT,
} dura_coordinator_state_t;

typedef struct {
    dura_coordinator_state_t state;
    bool leader_active;
    char active_recipe[32];
    uint32_t active_transaction_id;
    uint32_t pending_ack_count;
    uint32_t acked_count;
    uint32_t completed_count;
    uint32_t timeout_count;
    uint32_t last_failed_transaction_id;
    char last_failed_liquid[32];
} dura_coordinator_snapshot_t;

esp_err_t dura_coordinator_init(void);
esp_err_t dura_coordinator_start_recipe_as_leader(const char *recipe_name);
esp_err_t dura_coordinator_start_batch_as_leader(const char *recipe_name, float scale);
esp_err_t dura_coordinator_abort(void);
esp_err_t dura_coordinator_enable_test_autorun(const char *recipe_name, uint32_t pulse_count);
void dura_coordinator_set_log_context(const char *context);
esp_err_t dura_coordinator_get_snapshot(dura_coordinator_snapshot_t *snapshot);
esp_err_t dura_coordinator_format_status_json(char *json, size_t json_len);
const char *dura_coordinator_state_name(dura_coordinator_state_t state);

#ifdef __cplusplus
}
#endif
