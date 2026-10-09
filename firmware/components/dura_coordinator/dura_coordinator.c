#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "dura_coordinator.h"
#include "dura_json_builder.h"
#include "dura_meter.h"
#include "dura_peers.h"
#include "dura_recipe.h"

static const char *TAG = "dura_coord";
static const int64_t DURA_COORDINATOR_PEER_LIVE_TIMEOUT_MS = 300000;
static const int64_t DURA_COORDINATOR_PEER_ACK_TIMEOUT_MS = 5000;
static const TickType_t DURA_COORDINATOR_POLL_TICKS = pdMS_TO_TICKS(100);
static const size_t DURA_COORDINATOR_NO_LOCAL_INDEX = DURA_RECIPE_MAX_INGREDIENTS;

static dura_coordinator_snapshot_t s_snapshot;
static bool s_initialized;
static const char *s_log_context;

static dura_recipe_t s_active_recipe;
static size_t s_plan[DURA_RECIPE_MAX_INGREDIENTS];
static bool s_step_complete[DURA_RECIPE_MAX_INGREDIENTS];
static size_t s_plan_count;
static size_t s_plan_pos;
static size_t s_step_start;
static size_t s_step_end;
static size_t s_leader_local_index = DURA_RECIPE_MAX_INGREDIENTS;

typedef struct {
    bool active;
    bool acked;
    bool complete;
    bool timed_out;
    uint32_t transaction_id;
    uint8_t sequence;
    int64_t sent_ms;
    char liquid[DURA_RECIPE_MAX_LIQUID_NAME_LEN];
} dura_coord_peer_txn_t;

static char s_follower_recipe[DURA_RECIPE_MAX_NAME_LEN];
static char s_follower_liquid[DURA_RECIPE_MAX_LIQUID_NAME_LEN];
static uint8_t s_follower_sequence;
static uint32_t s_follower_transaction_id;
static bool s_follower_completion_sent;
static bool s_test_autorun_enabled;
static char s_test_autorun_recipe[DURA_RECIPE_MAX_NAME_LEN];
static uint32_t s_test_autorun_pulses;
static bool s_test_autorun_last_pump;
static bool s_test_autorun_used;
static dura_coord_peer_txn_t s_peer_txns[DURA_RECIPE_MAX_INGREDIENTS];
static TaskHandle_t s_task_handle;
/* Initialized by the app before runtime clients. All coordinator state is
 * serialized by a task mutex, including NVS/peer calls (never a spinlock).
 * Lock order: coordinator -> meter; the meter never calls back here. */
static SemaphoreHandle_t s_mutex;
static uint32_t s_recipe_owner_id;
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "cancel publication must be lock-free");
/* This short lock binds cancel-generation and owner publication/capture.
 * No meter/GPIO, NVS, logging, or task-mutex operation occurs while held. */
static portMUX_TYPE s_publication_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_published_owner;
static _Atomic uint32_t s_cancel_generation;
static uint32_t s_owner_cancel_generation;

/* A cancellation request must not wait behind a start's NVS/peer call before
 * revoking local authority. The meter performs OFF before persistence waits. */
static void request_recipe_cancel(void)
{
    taskENTER_CRITICAL(&s_publication_lock);
    const uint32_t generation = atomic_load(&s_cancel_generation);
    /* Saturate rather than reusing an epoch after wrap. New claims then fail
     * closed until reboot, just as exhausted meter operation identities do. */
    if (generation != UINT32_MAX) atomic_store(&s_cancel_generation, generation + 1);
    const uint32_t owner = s_published_owner;
    taskEXIT_CRITICAL(&s_publication_lock);
    if (owner != 0) (void)dura_meter_release_recipe(owner);
}

static esp_err_t claim_recipe(uint32_t request_generation)
{
    s_owner_cancel_generation = request_generation;
    esp_err_t err = dura_meter_claim_recipe(&s_recipe_owner_id);
    if (err != ESP_OK) return err;
    taskENTER_CRITICAL(&s_publication_lock);
    if (request_generation == UINT32_MAX ||
        request_generation != atomic_load(&s_cancel_generation)) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        s_published_owner = s_recipe_owner_id;
    }
    taskEXIT_CRITICAL(&s_publication_lock);
    if (err != ESP_OK) {
        /* A cancellation that observed no published lease still invalidates
         * this claim before any local ingredient may activate. */
        (void)dura_meter_release_recipe(s_recipe_owner_id);
        s_recipe_owner_id = 0;
    }
    return err;
}
static dura_peer_event_t s_last_follower_start;
static bool s_have_last_follower_start;

static bool coordinator_lock(void)
{
    return s_mutex != NULL && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE;
}

static void coordinator_unlock(void)
{
    xSemaphoreGive(s_mutex);
}

static bool recipe_ownership_valid(void)
{
    dura_meter_snapshot_t meter = {0};
    return s_recipe_owner_id != 0 &&
           s_owner_cancel_generation == atomic_load(&s_cancel_generation) &&
           dura_meter_get_snapshot(&meter) == ESP_OK &&
           meter.recipe_owner_id == s_recipe_owner_id && !meter.fault_latched;
}

static const char *log_context_prefix(void)
{
    return (s_log_context != NULL && s_log_context[0] != '\0') ? s_log_context : "runtime";
}

static void copy_text(char *dst, size_t dst_len, const char *src)
{
    if (dst == NULL || dst_len == 0) {
        return;
    }
    snprintf(dst, dst_len, "%s", src != NULL ? src : "");
}

static bool local_liquid_matches(const char *liquid_name)
{
    if (liquid_name == NULL || liquid_name[0] == '\0') {
        return false;
    }
    dura_liquid_config_t local_liquid = {0};
    if (dura_recipe_get_local_liquid(&local_liquid) != ESP_OK) {
        return false;
    }
    return strcasecmp(liquid_name, local_liquid.liquid_name) == 0;
}

static bool local_liquid_is_configured(void)
{
    dura_liquid_config_t local_liquid = {0};
    return dura_recipe_get_local_liquid(&local_liquid) == ESP_OK &&
           local_liquid.liquid_name[0] != '\0' &&
           strcasecmp(local_liquid.liquid_name, "unassigned") != 0 &&
           local_liquid.viscosity_cP > 0.0f &&
           local_liquid.calibration_id != 0;
}

static bool live_peer_liquid_matches(const char *liquid_name)
{
    if (liquid_name == NULL || liquid_name[0] == '\0') {
        return false;
    }

    dura_peers_snapshot_t peers = {0};
    if (dura_peers_get_snapshot(&peers) != ESP_OK) {
        return false;
    }

    const int64_t now_ms = esp_timer_get_time() / 1000;
    for (size_t i = 0; i < peers.peer_count; ++i) {
        const int64_t age_ms = now_ms - peers.peers[i].last_seen_ms;
        if (peers.peers[i].last_seen_ms <= 0 || age_ms > DURA_COORDINATOR_PEER_LIVE_TIMEOUT_MS) {
            continue;
        }
        if (strcasecmp(liquid_name, peers.peers[i].liquid_name) == 0) {
            return true;
        }
    }

    return false;
}

static esp_err_t validate_recipe_ingredients_available(const dura_recipe_t *recipe)
{
    ESP_RETURN_ON_FALSE(recipe != NULL, ESP_ERR_INVALID_ARG, TAG, "recipe is NULL");

    for (size_t i = 0; i < recipe->ingredient_count; ++i) {
        const char *liquid = recipe->ingredients[i].liquid_name;
        if (local_liquid_matches(liquid) || live_peer_liquid_matches(liquid)) {
            continue;
        }
        ESP_LOGE(TAG, "[%s] recipe=%s missing live meter for ingredient=%s", log_context_prefix(), recipe->name, liquid);
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

static int compare_recipe_plan_items(const dura_recipe_t *recipe, size_t lhs, size_t rhs)
{
    const dura_recipe_ingredient_t *a = &recipe->ingredients[lhs];
    const dura_recipe_ingredient_t *b = &recipe->ingredients[rhs];

    switch (recipe->order) {
    case DURA_RECIPE_ORDER_LARGEST_FIRST:
        if (a->amount > b->amount) {
            return -1;
        }
        if (a->amount < b->amount) {
            return 1;
        }
        break;
    case DURA_RECIPE_ORDER_SMALLEST_FIRST:
        if (a->amount < b->amount) {
            return -1;
        }
        if (a->amount > b->amount) {
            return 1;
        }
        break;
    case DURA_RECIPE_ORDER_CONFIGURED:
    default:
        if (a->sequence < b->sequence) {
            return -1;
        }
        if (a->sequence > b->sequence) {
            return 1;
        }
        break;
    }

    return lhs < rhs ? -1 : (lhs > rhs ? 1 : 0);
}

static void build_recipe_plan(const dura_recipe_t *recipe)
{
    s_plan_count = recipe->ingredient_count;
    for (size_t i = 0; i < s_plan_count; ++i) {
        s_plan[i] = i;
        s_step_complete[i] = false;
    }

    for (size_t i = 1; i < s_plan_count; ++i) {
        const size_t item = s_plan[i];
        size_t j = i;
        while (j > 0 && compare_recipe_plan_items(recipe, item, s_plan[j - 1]) < 0) {
            s_plan[j] = s_plan[j - 1];
            --j;
        }
        s_plan[j] = item;
    }
}

static bool ingredients_share_step(const dura_recipe_t *recipe, size_t lhs, size_t rhs)
{
    const dura_recipe_ingredient_t *a = &recipe->ingredients[lhs];
    const dura_recipe_ingredient_t *b = &recipe->ingredients[rhs];

    switch (recipe->order) {
    case DURA_RECIPE_ORDER_LARGEST_FIRST:
    case DURA_RECIPE_ORDER_SMALLEST_FIRST:
        return fabsf(a->amount - b->amount) < 0.0001f;
    case DURA_RECIPE_ORDER_CONFIGURED:
    default:
        return a->sequence == b->sequence;
    }
}

static bool meter_batch_is_done(void)
{
    dura_meter_snapshot_t meter = {0};
    return s_recipe_owner_id != 0 && dura_meter_get_snapshot(&meter) == ESP_OK &&
           meter.recipe_owner_id == s_recipe_owner_id && !meter.fault_latched &&
           meter.batch_mode == DURA_BATCH_DONE && !meter.pump_enabled;
}

static void clear_leader_recipe_state(void)
{
    memset(&s_active_recipe, 0, sizeof(s_active_recipe));
    memset(s_plan, 0, sizeof(s_plan));
    memset(s_step_complete, 0, sizeof(s_step_complete));
    memset(s_peer_txns, 0, sizeof(s_peer_txns));
    s_plan_count = 0;
    s_plan_pos = 0;
    s_step_start = 0;
    s_step_end = 0;
    s_leader_local_index = DURA_COORDINATOR_NO_LOCAL_INDEX;
    s_snapshot.active_transaction_id = 0;
    s_snapshot.pending_ack_count = 0;
    s_snapshot.acked_count = 0;
    s_snapshot.completed_count = 0;
    s_snapshot.timeout_count = 0;
}

static void clear_follower_recipe_state(void)
{
    s_follower_recipe[0] = '\0';
    s_follower_liquid[0] = '\0';
    s_follower_sequence = 0;
    s_follower_transaction_id = 0;
    s_follower_completion_sent = false;
}

/* Caller holds s_mutex. Detach before potentially blocking persistence. A
 * stale release is harmless: only the meter can atomically revoke its owner. */
static void finish_recipe(dura_coordinator_state_t state, bool abort_peers)
{
    const uint32_t owner = s_recipe_owner_id;
    const uint32_t timeouts = s_snapshot.timeout_count;
    s_recipe_owner_id = 0;
    taskENTER_CRITICAL(&s_publication_lock);
    if (s_published_owner == owner) s_published_owner = 0;
    taskEXIT_CRITICAL(&s_publication_lock);
    if (owner != 0) {
        (void)dura_meter_release_recipe(owner);
    }
    clear_leader_recipe_state();
    clear_follower_recipe_state();
    s_snapshot.state = state;
    s_snapshot.leader_active = false;
    s_snapshot.active_recipe[0] = '\0';
    if (state == DURA_COORDINATOR_FAULT) {
        s_snapshot.timeout_count = timeouts;
    }
    s_test_autorun_last_pump = false;
    if (abort_peers) {
        (void)dura_peers_send_abort();
    }
}

static bool discard_lost_recipe(void)
{
    if (s_recipe_owner_id != 0 && !recipe_ownership_valid()) {
        finish_recipe(DURA_COORDINATOR_IDLE, true);
        return true;
    }
    return false;
}

/* Validate the same sorted/grouped steps that will actually be dispatched.
 * Largest/smallest-first grouping compares with the first item of each step. */
static esp_err_t validate_local_steps(void)
{
    for (size_t start = 0; start < s_plan_count;) {
        size_t end = start + 1;
        while (end < s_plan_count && ingredients_share_step(&s_active_recipe, s_plan[start], s_plan[end])) {
            ++end;
        }
        bool have_local = false;
        for (size_t i = start; i < end; ++i) {
            if (local_liquid_matches(s_active_recipe.ingredients[s_plan[i]].liquid_name)) {
                if (have_local) {
                    return ESP_ERR_INVALID_STATE;
                }
                have_local = true;
            }
        }
        start = end;
    }
    return ESP_OK;
}

static uint32_t next_transaction_id(size_t ingredient_index)
{
    uint32_t id = (uint32_t)(esp_timer_get_time() / 1000ULL);
    id ^= ((uint32_t)(ingredient_index + 1U) << 16);
    id ^= ((uint32_t)s_plan_pos + 1U);
    return id == 0 ? (uint32_t)(ingredient_index + 1U) : id;
}

static void recompute_ack_counters(void)
{
    uint32_t pending = 0;
    uint32_t acked = 0;
    uint32_t completed = 0;
    uint32_t timed_out = 0;
    for (size_t i = 0; i < DURA_RECIPE_MAX_INGREDIENTS; ++i) {
        if (!s_peer_txns[i].active) {
            continue;
        }
        if (!s_peer_txns[i].acked && !s_peer_txns[i].timed_out) {
            ++pending;
        }
        if (s_peer_txns[i].acked) {
            ++acked;
        }
        if (s_peer_txns[i].complete) {
            ++completed;
        }
        if (s_peer_txns[i].timed_out) {
            ++timed_out;
        }
    }
    s_snapshot.pending_ack_count = pending;
    s_snapshot.acked_count = acked;
    s_snapshot.completed_count = completed;
    s_snapshot.timeout_count = timed_out;
}

static void remember_peer_transaction(size_t ingredient_index, const dura_recipe_ingredient_t *ingredient, uint32_t transaction_id)
{
    if (ingredient_index >= DURA_RECIPE_MAX_INGREDIENTS || ingredient == NULL || transaction_id == 0) {
        return;
    }
    dura_coord_peer_txn_t *txn = &s_peer_txns[ingredient_index];
    memset(txn, 0, sizeof(*txn));
    txn->active = true;
    txn->transaction_id = transaction_id;
    txn->sequence = ingredient->sequence;
    txn->sent_ms = esp_timer_get_time() / 1000;
    copy_text(txn->liquid, sizeof(txn->liquid), ingredient->liquid_name);
    s_snapshot.active_transaction_id = transaction_id;
    recompute_ack_counters();
}

static esp_err_t start_local_ingredient(const char *recipe_name, const char *liquid_name, float amount, bool leader)
{
    ESP_RETURN_ON_FALSE(amount > 0.0f && isfinite(amount), ESP_ERR_INVALID_ARG, TAG, "amount must be positive");
    ESP_RETURN_ON_FALSE(local_liquid_matches(liquid_name), ESP_ERR_NOT_FOUND, TAG, "local liquid does not match ingredient");

    esp_err_t err = dura_meter_start_owned_batch(amount, s_recipe_owner_id);
    if (err == ESP_OK) {
        s_snapshot.state = leader ? DURA_COORDINATOR_LEADER_RUNNING : DURA_COORDINATOR_FOLLOWER_RUNNING;
        s_snapshot.leader_active = leader;
        copy_text(s_snapshot.active_recipe, sizeof(s_snapshot.active_recipe), recipe_name);
    }
    return err;
}

static esp_err_t start_current_leader_step(void)
{
    ESP_RETURN_ON_FALSE(recipe_ownership_valid(), ESP_ERR_INVALID_STATE, TAG, "recipe ownership lost");
    if (s_plan_pos >= s_plan_count) {
        ESP_LOGI(TAG, "leader recipe=%s complete", s_active_recipe.name);
        finish_recipe(DURA_COORDINATOR_IDLE, false);
        return ESP_OK;
    }

    s_step_start = s_plan_pos;
    s_step_end = s_step_start + 1;
    while (s_step_end < s_plan_count && ingredients_share_step(&s_active_recipe, s_plan[s_step_start], s_plan[s_step_end])) {
        ++s_step_end;
    }

    s_leader_local_index = DURA_COORDINATOR_NO_LOCAL_INDEX;
    for (size_t i = s_step_start; i < s_step_end; ++i) {
        s_step_complete[s_plan[i]] = false;
    }

    const dura_recipe_ingredient_t *step_first = &s_active_recipe.ingredients[s_plan[s_step_start]];
    ESP_LOGI(TAG, "leader recipe=%s starting %s step seq=%u amount=%.2f ingredients=%u",
             s_active_recipe.name, dura_recipe_order_name(s_active_recipe.order),
             (unsigned)step_first->sequence, (double)step_first->amount,
             (unsigned)(s_step_end - s_step_start));

    for (size_t i = s_step_start; i < s_step_end; ++i) {
        const size_t idx = s_plan[i];
        const dura_recipe_ingredient_t *ingredient = &s_active_recipe.ingredients[idx];
        ESP_RETURN_ON_FALSE(recipe_ownership_valid(), ESP_ERR_INVALID_STATE, TAG, "recipe ownership lost");
        esp_err_t err = ESP_OK;
        if (local_liquid_matches(ingredient->liquid_name)) {
            err = start_local_ingredient(s_active_recipe.name, ingredient->liquid_name, ingredient->amount, true);
            if (err == ESP_OK) {
                s_leader_local_index = idx;
            }
        } else {
            const uint32_t transaction_id = next_transaction_id(idx);
            err = dura_peers_send_recipe_ingredient_start(s_active_recipe.name, ingredient, transaction_id);
            if (err == ESP_OK) {
                remember_peer_transaction(idx, ingredient, transaction_id);
                ESP_LOGI(TAG, "leader recipe=%s ingredient=%s seq=%u tx=%lu awaiting ACK",
                         s_active_recipe.name, ingredient->liquid_name, (unsigned)ingredient->sequence,
                         (unsigned long)transaction_id);
            }
        }
        if (err != ESP_OK) {
            return err;
        }
    }

    return recipe_ownership_valid() ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static bool current_leader_step_complete(void)
{
    if (s_snapshot.state != DURA_COORDINATOR_LEADER_RUNNING) {
        return false;
    }
    for (size_t i = s_step_start; i < s_step_end; ++i) {
        if (!s_step_complete[s_plan[i]]) {
            return false;
        }
    }
    return true;
}

static bool event_matches_ingredient(const dura_peer_event_t *event, const dura_recipe_ingredient_t *ingredient)
{
    return event != NULL && ingredient != NULL &&
           strcasecmp(event->recipe_name, s_active_recipe.name) == 0 &&
           strcasecmp(event->target_liquid, ingredient->liquid_name) == 0 &&
           event->sequence == ingredient->sequence;
}

static bool event_matches_transaction(const dura_peer_event_t *event, size_t ingredient_index)
{
    return event != NULL && ingredient_index < DURA_RECIPE_MAX_INGREDIENTS &&
           s_peer_txns[ingredient_index].active &&
           s_peer_txns[ingredient_index].transaction_id == event->claim_id;
}

static void mark_peer_ingredient_acked(const dura_peer_event_t *event)
{
    if (s_snapshot.state != DURA_COORDINATOR_LEADER_RUNNING) {
        return;
    }

    for (size_t i = s_step_start; i < s_step_end; ++i) {
        const size_t idx = s_plan[i];
        if (event_matches_ingredient(event, &s_active_recipe.ingredients[idx]) &&
            event_matches_transaction(event, idx) && !s_peer_txns[idx].acked) {
            s_peer_txns[idx].acked = true;
            ESP_LOGI(TAG, "leader recipe=%s ingredient=%s seq=%u tx=%lu ACK from peer",
                     event->recipe_name, event->target_liquid, (unsigned)event->sequence,
                     (unsigned long)event->claim_id);
            recompute_ack_counters();
            return;
        }
    }
}

static void mark_peer_ingredient_complete(const dura_peer_event_t *event)
{
    if (s_snapshot.state != DURA_COORDINATOR_LEADER_RUNNING) {
        return;
    }

    for (size_t i = s_step_start; i < s_step_end; ++i) {
        const size_t idx = s_plan[i];
        if (!s_step_complete[idx] && event_matches_ingredient(event, &s_active_recipe.ingredients[idx]) &&
            event_matches_transaction(event, idx)) {
            s_step_complete[idx] = true;
            s_peer_txns[idx].acked = true;
            s_peer_txns[idx].complete = true;
            ESP_LOGI(TAG, "leader recipe=%s ingredient=%s seq=%u tx=%lu complete from peer",
                     event->recipe_name, event->target_liquid, (unsigned)event->sequence,
                     (unsigned long)event->claim_id);
            recompute_ack_counters();
            return;
        }
    }
}

static esp_err_t enable_test_autorun_locked(const char *recipe_name, uint32_t pulse_count)
{
    ESP_RETURN_ON_FALSE(recipe_name != NULL && recipe_name[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "recipe name required");
    ESP_RETURN_ON_FALSE(pulse_count > 0, ESP_ERR_INVALID_ARG, TAG, "pulse count required");
    copy_text(s_test_autorun_recipe, sizeof(s_test_autorun_recipe), recipe_name);
    s_test_autorun_pulses = pulse_count;
    s_test_autorun_enabled = true;
    s_test_autorun_last_pump = false;
    s_test_autorun_used = false;
    ESP_LOGI(TAG, "test autorun armed recipe=%s pulses=%lu", s_test_autorun_recipe, (unsigned long)s_test_autorun_pulses);
    return ESP_OK;
}

static void apply_test_autorun_if_needed(void)
{
    if (!s_test_autorun_enabled) {
        return;
    }

    dura_meter_snapshot_t meter = {0};
    if (dura_meter_get_snapshot(&meter) != ESP_OK) {
        return;
    }

    const bool active_recipe_matches = s_snapshot.active_recipe[0] != '\0' &&
                                       strcasecmp(s_snapshot.active_recipe, s_test_autorun_recipe) == 0;
    const bool running = s_recipe_owner_id != 0 && meter.recipe_owner_id == s_recipe_owner_id &&
                         active_recipe_matches && meter.pump_enabled && meter.batch_mode == DURA_BATCH_RUNNING;
    if (running && !s_test_autorun_last_pump) {
        esp_err_t err = dura_meter_record_pulse(s_test_autorun_pulses);
        s_test_autorun_used = true;
        ESP_LOGI(TAG, "test autorun recipe=%s applied pulses=%lu result=0x%x",
                 s_test_autorun_recipe, (unsigned long)s_test_autorun_pulses, err);
    }
    s_test_autorun_last_pump = running;

    if (s_test_autorun_used &&
        (s_snapshot.state == DURA_COORDINATOR_IDLE || s_snapshot.state == DURA_COORDINATOR_FAULT) && !meter.pump_enabled) {
        s_test_autorun_enabled = false;
        s_test_autorun_recipe[0] = '\0';
        s_test_autorun_pulses = 0;
        s_test_autorun_last_pump = false;
        s_test_autorun_used = false;
    }
}

static const char *peer_msg_type_name(dura_peer_msg_type_t type)
{
    switch (type) {
    case DURA_PEER_MSG_HELLO:
        return "HELLO";
    case DURA_PEER_MSG_RECIPE_START:
        return "RECIPE_START";
    case DURA_PEER_MSG_RECIPE_ABORT:
        return "RECIPE_ABORT";
    case DURA_PEER_MSG_STATUS:
        return "STATUS";
    case DURA_PEER_MSG_PING:
        return "PING";
    case DURA_PEER_MSG_PONG:
        return "PONG";
    case DURA_PEER_MSG_IDENTITY_UPDATE:
        return "IDENTITY_UPDATE";
    case DURA_PEER_MSG_RECIPE_COMPLETE:
        return "RECIPE_COMPLETE";
    case DURA_PEER_MSG_RECIPE_ACK:
        return "RECIPE_ACK";
    case DURA_PEER_MSG_BLE_GATEWAY_CLAIM:
        return "BLE_GATEWAY_CLAIM";
    case DURA_PEER_MSG_BLE_GATEWAY_RELEASE:
        return "BLE_GATEWAY_RELEASE";
    case DURA_PEER_MSG_TEST_AUTORUN:
        return "TEST_AUTORUN";
    default:
        return "UNKNOWN";
    }
}

static void print_peer_event_to_monitor(const dura_peer_event_t *event)
{
    if (event == NULL) {
        return;
    }

    const bool is_local_target = local_liquid_matches(event->target_liquid);
    const char *prefix = is_local_target ? "peer->local" : "peer rx";

    printf("\r\n[%s] type=%s from=%s target=%s recipe=%s amount=%.2f seq=%u tx=%lu\r\n> ",
           prefix,
           peer_msg_type_name(event->type),
           event->source_liquid[0] != '\0' ? event->source_liquid : "(unknown)",
           event->target_liquid[0] != '\0' ? event->target_liquid : "(broadcast)",
           event->recipe_name,
           (double)event->amount,
           event->sequence,
           (unsigned long)event->claim_id);
    fflush(stdout);
}

static bool repeated_follower_start(const dura_peer_event_t *event)
{
    return s_have_last_follower_start && event->claim_id == s_last_follower_start.claim_id &&
           event->sequence == s_last_follower_start.sequence &&
           memcmp(event->src_mac, s_last_follower_start.src_mac, sizeof(event->src_mac)) == 0 &&
           strcasecmp(event->recipe_name, s_last_follower_start.recipe_name) == 0 &&
           strcasecmp(event->target_liquid, s_last_follower_start.target_liquid) == 0;
}

static void on_peer_event_locked(const dura_peer_event_t *event)
{
    const uint32_t request_generation = atomic_load(&s_cancel_generation);
    if (!s_initialized || (event->type != DURA_PEER_MSG_RECIPE_ABORT && discard_lost_recipe())) {
        return;
    }

    print_peer_event_to_monitor(event);

    switch (event->type) {
    case DURA_PEER_MSG_RECIPE_START:
        if (local_liquid_matches(event->target_liquid)) {
            if (s_snapshot.state != DURA_COORDINATOR_IDLE || repeated_follower_start(event) ||
                event->claim_id == 0 || !isfinite(event->amount) || event->amount <= 0.0f) {
                break;
            }
            /* Retain this validated attempt even if cancellation interrupts
             * claim publication; its delayed duplicate must not restart it. */
            s_last_follower_start = *event;
            s_have_last_follower_start = true;
            esp_err_t err = claim_recipe(request_generation);
            if (err != ESP_OK) {
                break;
            }
            err = start_local_ingredient(event->recipe_name, event->target_liquid, event->amount, false);
            if (err == ESP_OK) {
                copy_text(s_follower_recipe, sizeof(s_follower_recipe), event->recipe_name);
                copy_text(s_follower_liquid, sizeof(s_follower_liquid), event->target_liquid);
                s_follower_sequence = event->sequence;
                s_follower_transaction_id = event->claim_id;
                s_follower_completion_sent = false;
                err = recipe_ownership_valid() ?
                    dura_peers_send_recipe_ack(event->recipe_name, event->target_liquid,
                                              event->sequence, event->claim_id) : ESP_ERR_INVALID_STATE;
            }
            if (err != ESP_OK || !recipe_ownership_valid()) {
                finish_recipe(DURA_COORDINATOR_FAULT, true);
            }
            ESP_LOGI(TAG, "follower recipe=%s ingredient=%s amount=%.2f seq=%u result=0x%x",
                     event->recipe_name, event->target_liquid, (double)event->amount,
                     (unsigned)event->sequence, err);
        }
        break;
    case DURA_PEER_MSG_RECIPE_ACK:
        mark_peer_ingredient_acked(event);
        break;
    case DURA_PEER_MSG_RECIPE_COMPLETE:
        mark_peer_ingredient_complete(event);
        break;
    case DURA_PEER_MSG_TEST_AUTORUN:
        (void)enable_test_autorun_locked(event->recipe_name, event->claim_id);
        break;
    case DURA_PEER_MSG_RECIPE_ABORT:
        /* A new valid recipe may have acquired the mutex after the preemptive
         * cancellation; delayed cleanup must not release that newer owner. */
        if (s_recipe_owner_id == 0 || !recipe_ownership_valid()) {
            finish_recipe(DURA_COORDINATOR_IDLE, false);
        }
        ESP_LOGI(TAG, "[%s] peer abort received from %s", log_context_prefix(), event->source_liquid);
        break;
    case DURA_PEER_MSG_PING:
    case DURA_PEER_MSG_PONG:
    case DURA_PEER_MSG_HELLO:
    case DURA_PEER_MSG_IDENTITY_UPDATE:
    case DURA_PEER_MSG_STATUS:
    default:
        break;
    }
}

static void on_peer_event(const dura_peer_event_t *event, void *user_ctx)
{
    (void)user_ctx;
    if (event != NULL && event->type == DURA_PEER_MSG_RECIPE_ABORT) request_recipe_cancel();
    if (event == NULL || !coordinator_lock()) {
        return;
    }
    on_peer_event_locked(event);
    coordinator_unlock();
}

static void coordinator_tick_locked(void)
{
    if (!s_initialized || discard_lost_recipe()) {
        return;
    }
    apply_test_autorun_if_needed();

    if (s_snapshot.state == DURA_COORDINATOR_FOLLOWER_RUNNING &&
        !s_follower_completion_sent && meter_batch_is_done()) {
        esp_err_t err = dura_peers_send_recipe_complete(s_follower_recipe, s_follower_liquid,
                                                       s_follower_sequence, s_follower_transaction_id);
        ESP_LOGI(TAG, "follower recipe=%s ingredient=%s seq=%u complete report result=0x%x",
                 s_follower_recipe, s_follower_liquid, (unsigned)s_follower_sequence, err);
        s_follower_completion_sent = true;
        finish_recipe(err == ESP_OK ? DURA_COORDINATOR_IDLE : DURA_COORDINATOR_FAULT, err != ESP_OK);
        return;
    }

    if (s_snapshot.state != DURA_COORDINATOR_LEADER_RUNNING) {
        return;
    }
    const int64_t now_ms = esp_timer_get_time() / 1000;
    for (size_t i = s_step_start; i < s_step_end; ++i) {
        const size_t idx = s_plan[i];
        if (s_peer_txns[idx].active && !s_peer_txns[idx].acked && !s_peer_txns[idx].timed_out &&
            now_ms - s_peer_txns[idx].sent_ms > DURA_COORDINATOR_PEER_ACK_TIMEOUT_MS) {
            s_peer_txns[idx].timed_out = true;
            s_snapshot.last_failed_transaction_id = s_peer_txns[idx].transaction_id;
            copy_text(s_snapshot.last_failed_liquid, sizeof(s_snapshot.last_failed_liquid), s_peer_txns[idx].liquid);
            recompute_ack_counters();
            ESP_LOGE(TAG, "leader recipe=%s ingredient=%s seq=%u tx=%lu ACK timeout",
                     s_active_recipe.name, s_peer_txns[idx].liquid, (unsigned)s_peer_txns[idx].sequence,
                     (unsigned long)s_peer_txns[idx].transaction_id);
            finish_recipe(DURA_COORDINATOR_FAULT, true);
            return;
        }
    }

    if (s_leader_local_index != DURA_COORDINATOR_NO_LOCAL_INDEX &&
        !s_step_complete[s_leader_local_index] && meter_batch_is_done()) {
        s_step_complete[s_leader_local_index] = true;
        ESP_LOGI(TAG, "leader recipe=%s local ingredient=%s seq=%u complete",
                 s_active_recipe.name,
                 s_active_recipe.ingredients[s_leader_local_index].liquid_name,
                 (unsigned)s_active_recipe.ingredients[s_leader_local_index].sequence);
    }
    if (current_leader_step_complete()) {
        s_plan_pos = s_step_end;
        esp_err_t err = start_current_leader_step();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "leader recipe=%s failed to start next step: 0x%x", s_active_recipe.name, err);
            finish_recipe(DURA_COORDINATOR_FAULT, true);
        }
    }
}

static void coordinator_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(DURA_COORDINATOR_POLL_TICKS);
        if (coordinator_lock()) {
            coordinator_tick_locked();
            coordinator_unlock();
        }
    }
}

const char *dura_coordinator_state_name(dura_coordinator_state_t state)
{
    switch (state) {
    case DURA_COORDINATOR_IDLE:
        return "idle";
    case DURA_COORDINATOR_LEADER_RUNNING:
        return "leader_running";
    case DURA_COORDINATOR_FOLLOWER_RUNNING:
        return "follower_running";
    case DURA_COORDINATOR_FAULT:
        return "fault";
    default:
        return "unknown";
    }
}

esp_err_t dura_coordinator_init(void)
{
    /* Bootstrap is called by app_main before publishing runtime clients. */
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_mutex != NULL, ESP_ERR_NO_MEM, TAG, "create coordinator mutex failed");
    }
    ESP_RETURN_ON_FALSE(coordinator_lock(), ESP_ERR_INVALID_STATE, TAG, "coordinator lock failed");
    if (s_initialized) {
        coordinator_unlock();
        return ESP_OK;
    }
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    clear_leader_recipe_state();
    clear_follower_recipe_state();
    s_snapshot.state = DURA_COORDINATOR_IDLE;
    esp_err_t err = dura_peers_register_event_callback(on_peer_event, NULL);
    if (err == ESP_OK && s_task_handle == NULL) {
        BaseType_t task_ok = xTaskCreate(coordinator_task, "dura_coord", 4096, NULL, 5, &s_task_handle);
        if (task_ok != pdPASS) {
            err = ESP_ERR_NO_MEM;
        }
    }
    s_initialized = err == ESP_OK;
    coordinator_unlock();
    return err;
}

static esp_err_t start_batch_as_leader_locked(const char *recipe_name, float scale)
{
    const uint32_t request_generation = atomic_load(&s_cancel_generation);
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(!discard_lost_recipe(), ESP_ERR_INVALID_STATE, TAG, "recipe ownership lost");
    ESP_RETURN_ON_FALSE(s_snapshot.state == DURA_COORDINATOR_IDLE, ESP_ERR_INVALID_STATE, TAG, "coordinator already active");
    ESP_RETURN_ON_FALSE(scale > 0.0f && isfinite(scale), ESP_ERR_INVALID_ARG, TAG, "batch scale must be positive");

    dura_recipe_t recipe = {0};
    esp_err_t err = dura_recipe_find(recipe_name, &recipe);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[%s] recipe not found: %s", log_context_prefix(), recipe_name != NULL ? recipe_name : "(null)");
        return err;
    }
    ESP_RETURN_ON_FALSE(local_liquid_is_configured(), ESP_ERR_INVALID_STATE, TAG,
                        "[%s] local liquid unassigned; select liquid before batch start", log_context_prefix());
    ESP_RETURN_ON_FALSE(recipe.ingredient_count > 0 && recipe.ingredient_count <= DURA_RECIPE_MAX_INGREDIENTS,
                        ESP_ERR_INVALID_STATE, TAG, "invalid ingredient count");
    for (size_t i = 0; i < recipe.ingredient_count; ++i) {
        recipe.ingredients[i].amount *= scale;
        ESP_RETURN_ON_FALSE(recipe.ingredients[i].amount > 0.0f && isfinite(recipe.ingredients[i].amount),
                            ESP_ERR_INVALID_ARG, TAG, "invalid scaled amount");
    }
    err = validate_recipe_ingredients_available(&recipe);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[%s] recipe ingredient preflight failed", log_context_prefix());
        return err;
    }

    clear_leader_recipe_state();
    s_active_recipe = recipe;
    build_recipe_plan(&s_active_recipe);
    err = validate_local_steps();
    if (err == ESP_OK) {
        err = claim_recipe(request_generation);
    }
    if (err != ESP_OK) {
        clear_leader_recipe_state();
        return err;
    }
    s_plan_pos = 0;
    s_snapshot.state = DURA_COORDINATOR_LEADER_RUNNING;
    s_snapshot.leader_active = true;
    copy_text(s_snapshot.active_recipe, sizeof(s_snapshot.active_recipe), recipe.name);

    err = start_current_leader_step();
    if (err != ESP_OK) {
        finish_recipe(DURA_COORDINATOR_IDLE, true);
        return err;
    }

    ESP_LOGI(TAG, "leader started batch recipe=%s scale=%.3f order=%s ingredients=%u",
             recipe.name, (double)scale, dura_recipe_order_name(recipe.order), (unsigned)recipe.ingredient_count);
    return ESP_OK;
}

esp_err_t dura_coordinator_start_batch_as_leader(const char *recipe_name, float scale)
{
    ESP_RETURN_ON_FALSE(coordinator_lock(), ESP_ERR_INVALID_STATE, TAG, "not initialized");
    esp_err_t err = start_batch_as_leader_locked(recipe_name, scale);
    coordinator_unlock();
    return err;
}

esp_err_t dura_coordinator_enable_test_autorun(const char *recipe_name, uint32_t pulse_count)
{
    ESP_RETURN_ON_FALSE(coordinator_lock(), ESP_ERR_INVALID_STATE, TAG, "not initialized");
    esp_err_t err = enable_test_autorun_locked(recipe_name, pulse_count);
    coordinator_unlock();
    return err;
}

esp_err_t dura_coordinator_start_recipe_as_leader(const char *recipe_name)
{
    return dura_coordinator_start_batch_as_leader(recipe_name, 1.0f);
}

esp_err_t dura_coordinator_abort(void)
{
    request_recipe_cancel();
    ESP_RETURN_ON_FALSE(coordinator_lock(), ESP_ERR_INVALID_STATE, TAG, "not initialized");
    esp_err_t err = s_initialized ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK && (s_recipe_owner_id == 0 || !recipe_ownership_valid())) {
        finish_recipe(DURA_COORDINATOR_IDLE, true);
    }
    coordinator_unlock();
    return err;
}

void dura_coordinator_set_log_context(const char *context)
{
    if (coordinator_lock()) {
        s_log_context = context;
        coordinator_unlock();
    }
}

esp_err_t dura_coordinator_get_snapshot(dura_coordinator_snapshot_t *snapshot)
{
    ESP_RETURN_ON_FALSE(snapshot != NULL, ESP_ERR_INVALID_ARG, TAG, "snapshot is NULL");
    ESP_RETURN_ON_FALSE(coordinator_lock(), ESP_ERR_INVALID_STATE, TAG, "not initialized");
    *snapshot = s_snapshot;
    coordinator_unlock();
    return ESP_OK;
}

esp_err_t dura_coordinator_format_status_json(char *json, size_t json_len)
{
    ESP_RETURN_ON_FALSE(json != NULL && json_len > 0, ESP_ERR_INVALID_ARG, TAG, "bad json buffer");
    ESP_RETURN_ON_FALSE(coordinator_lock(), ESP_ERR_INVALID_STATE, TAG, "not initialized");
    int written = dura_json_snprintf(json, json_len,
                           "{\"state\":\"%s\",\"leader_active\":%s,\"active_recipe\":\"%s\","
                           "\"ack\":{\"active_tx\":%lu,\"pending\":%lu,\"acked\":%lu,\"completed\":%lu,"
                           "\"timeouts\":%lu,\"last_failed_tx\":%lu,\"last_failed_liquid\":\"%s\"}}",
                           dura_coordinator_state_name(s_snapshot.state),
                           s_snapshot.leader_active ? "true" : "false",
                           s_snapshot.active_recipe,
                           (unsigned long)s_snapshot.active_transaction_id,
                           (unsigned long)s_snapshot.pending_ack_count,
                           (unsigned long)s_snapshot.acked_count,
                           (unsigned long)s_snapshot.completed_count,
                           (unsigned long)s_snapshot.timeout_count,
                           (unsigned long)s_snapshot.last_failed_transaction_id,
                           s_snapshot.last_failed_liquid);
    coordinator_unlock();
    return dura_json_result(json, json_len, written);
}
