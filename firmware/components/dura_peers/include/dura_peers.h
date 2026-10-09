#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "dura_recipe.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DURA_PEER_MAX_PEERS DURA_RECIPE_MAX_INGREDIENTS

typedef enum {
    DURA_PEER_MSG_HELLO = 1,
    DURA_PEER_MSG_RECIPE_START,
    DURA_PEER_MSG_RECIPE_ABORT,
    DURA_PEER_MSG_STATUS,
    DURA_PEER_MSG_PING,
    DURA_PEER_MSG_PONG,
    DURA_PEER_MSG_IDENTITY_UPDATE,
    DURA_PEER_MSG_RECIPE_COMPLETE,
    DURA_PEER_MSG_RECIPE_ACK,
    DURA_PEER_MSG_BLE_GATEWAY_CLAIM,
    DURA_PEER_MSG_BLE_GATEWAY_RELEASE,
    DURA_PEER_MSG_TEST_AUTORUN,
} dura_peer_msg_type_t;

typedef struct {
    uint8_t mac[6];
    char liquid_name[DURA_RECIPE_MAX_LIQUID_NAME_LEN];
    int64_t last_seen_ms;
} dura_peer_info_t;

typedef struct {
    size_t peer_count;
    bool esp_now_ready;
    uint8_t local_mac[6];
    bool ble_gateway_active;
    uint8_t ble_gateway_mac[6];
    uint32_t ble_gateway_claim_id;
    int64_t ble_gateway_last_seen_ms;
    dura_peer_info_t peers[DURA_PEER_MAX_PEERS];
} dura_peers_snapshot_t;

typedef struct {
    dura_peer_msg_type_t type;
    uint8_t src_mac[6];
    char source_liquid[DURA_RECIPE_MAX_LIQUID_NAME_LEN];
    char recipe_name[DURA_RECIPE_MAX_NAME_LEN];
    char target_liquid[DURA_RECIPE_MAX_LIQUID_NAME_LEN];
    float amount;
    uint8_t sequence;
    uint32_t claim_id;
} dura_peer_event_t;

typedef void (*dura_peer_event_cb_t)(const dura_peer_event_t *event, void *user_ctx);

esp_err_t dura_peers_init(void);
esp_err_t dura_peers_register_event_callback(dura_peer_event_cb_t cb, void *user_ctx);
esp_err_t dura_peers_broadcast_hello(void);
esp_err_t dura_peers_broadcast_identity_update(void);
esp_err_t dura_peers_send_ping(void);
esp_err_t dura_peers_claim_ble_gateway(uint32_t claim_id);
esp_err_t dura_peers_release_ble_gateway(uint32_t claim_id);
esp_err_t dura_peers_send_abort(void);
esp_err_t dura_peers_send_recipe_ingredient_start(const char *recipe_name, const dura_recipe_ingredient_t *ingredient, uint32_t transaction_id);
esp_err_t dura_peers_send_recipe_ack(const char *recipe_name, const char *target_liquid, uint8_t sequence, uint32_t transaction_id);
esp_err_t dura_peers_send_recipe_complete(const char *recipe_name, const char *target_liquid, uint8_t sequence, uint32_t transaction_id);
esp_err_t dura_peers_send_recipe_start(const dura_recipe_t *recipe);
esp_err_t dura_peers_send_test_autorun(const char *recipe_name, uint32_t pulse_count);
esp_err_t dura_peers_format_status_json(char *json, size_t json_len);
esp_err_t dura_peers_get_snapshot(dura_peers_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
