#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "dura_peers.h"
#include "dura_json_builder.h"

static const char *TAG = "dura_peers";
static const uint8_t DURA_PEER_BROADCAST_MAC[ESP_NOW_ETH_ALEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static const uint32_t DURA_PEER_PROTOCOL_MAGIC = 0x44555241U; /* DURA */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    uint8_t sequence;
    uint8_t reserved;
    char source_liquid[DURA_RECIPE_MAX_LIQUID_NAME_LEN];
    char recipe_name[DURA_RECIPE_MAX_NAME_LEN];
    char target_liquid[DURA_RECIPE_MAX_LIQUID_NAME_LEN];
    float amount;
    uint32_t claim_id;
} dura_peer_wire_msg_t;

static dura_peers_snapshot_t s_snapshot;
#define DURA_PEER_EVENT_CALLBACK_MAX 4
static dura_peer_event_cb_t s_event_cbs[DURA_PEER_EVENT_CALLBACK_MAX];
static void *s_event_user_ctxs[DURA_PEER_EVENT_CALLBACK_MAX];
static bool s_initialized;

static void copy_text(char *dst, size_t dst_len, const char *src)
{
    if (dst == NULL || dst_len == 0) {
        return;
    }
    snprintf(dst, dst_len, "%s", src != NULL ? src : "");
}

static bool is_own_mac(const uint8_t *mac)
{
    return mac != NULL && memcmp(mac, s_snapshot.local_mac, ESP_NOW_ETH_ALEN) == 0;
}

static esp_err_t ensure_peer(const uint8_t *mac)
{
    ESP_RETURN_ON_FALSE(mac != NULL, ESP_ERR_INVALID_ARG, TAG, "mac is NULL");
#if CONFIG_DURA_APP_QEMU_NO_RADIO
    return ESP_OK;
#else
    if (esp_now_is_peer_exist(mac)) {
        return ESP_OK;
    }

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, mac, ESP_NOW_ETH_ALEN);
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    return esp_now_add_peer(&peer);
#endif
}

static void remember_peer(const uint8_t *mac, const char *liquid_name)
{
    if (mac == NULL || is_own_mac(mac)) {
        return;
    }

    for (size_t i = 0; i < s_snapshot.peer_count; ++i) {
        if (memcmp(s_snapshot.peers[i].mac, mac, ESP_NOW_ETH_ALEN) == 0) {
            copy_text(s_snapshot.peers[i].liquid_name, sizeof(s_snapshot.peers[i].liquid_name), liquid_name);
            s_snapshot.peers[i].last_seen_ms = esp_timer_get_time() / 1000;
            (void)ensure_peer(mac);
            return;
        }
    }

    if (s_snapshot.peer_count >= DURA_PEER_MAX_PEERS) {
        ESP_LOGW(TAG, "peer table full; ignoring " MACSTR, MAC2STR(mac));
        return;
    }

    dura_peer_info_t *peer = &s_snapshot.peers[s_snapshot.peer_count++];
    memcpy(peer->mac, mac, ESP_NOW_ETH_ALEN);
    copy_text(peer->liquid_name, sizeof(peer->liquid_name), liquid_name);
    peer->last_seen_ms = esp_timer_get_time() / 1000;
    (void)ensure_peer(mac);
    ESP_LOGI(TAG, "peer seen " MACSTR " liquid=%s", MAC2STR(mac), peer->liquid_name);
}

static bool gateway_claim_is_newer(uint32_t incoming, uint32_t current)
{
    return current == 0 || (int32_t)(incoming - current) > 0;
}

static void set_ble_gateway(const uint8_t *mac, uint32_t claim_id)
{
    if (mac == NULL || claim_id == 0) {
        return;
    }
    if (!s_snapshot.ble_gateway_active ||
        memcmp(s_snapshot.ble_gateway_mac, mac, ESP_NOW_ETH_ALEN) != 0 ||
        gateway_claim_is_newer(claim_id, s_snapshot.ble_gateway_claim_id)) {
        s_snapshot.ble_gateway_active = true;
        memcpy(s_snapshot.ble_gateway_mac, mac, ESP_NOW_ETH_ALEN);
        s_snapshot.ble_gateway_claim_id = claim_id;
        ESP_LOGI(TAG, "BLE gateway claim " MACSTR " claim=%lu",
                 MAC2STR(mac), (unsigned long)claim_id);
    }
    s_snapshot.ble_gateway_last_seen_ms = esp_timer_get_time() / 1000;
}

static void clear_ble_gateway_if_current(const uint8_t *mac, uint32_t claim_id)
{
    if (mac == NULL || !s_snapshot.ble_gateway_active) {
        return;
    }
    if (memcmp(s_snapshot.ble_gateway_mac, mac, ESP_NOW_ETH_ALEN) == 0 &&
        (claim_id == 0 || claim_id == s_snapshot.ble_gateway_claim_id)) {
        ESP_LOGI(TAG, "BLE gateway release " MACSTR " claim=%lu",
                 MAC2STR(mac), (unsigned long)claim_id);
        s_snapshot.ble_gateway_active = false;
        memset(s_snapshot.ble_gateway_mac, 0, sizeof(s_snapshot.ble_gateway_mac));
        s_snapshot.ble_gateway_claim_id = 0;
        s_snapshot.ble_gateway_last_seen_ms = 0;
    }
}

static esp_err_t send_wire_to(const uint8_t *mac, dura_peer_msg_type_t type,
                              const char *recipe_name, const char *target_liquid,
                              float amount, uint8_t sequence, uint32_t claim_id)
{
#if CONFIG_DURA_APP_M3000_LEGACY_ONLY
    (void)mac;
    (void)type;
    (void)recipe_name;
    (void)target_liquid;
    (void)amount;
    (void)sequence;
    (void)claim_id;
    return ESP_ERR_NOT_SUPPORTED;
#else
    ESP_RETURN_ON_FALSE(s_initialized && s_snapshot.esp_now_ready, ESP_ERR_INVALID_STATE, TAG, "ESP-NOW not ready");
    ESP_RETURN_ON_FALSE(mac != NULL, ESP_ERR_INVALID_ARG, TAG, "mac is NULL");

    dura_liquid_config_t liquid = {0};
    (void)dura_recipe_get_local_liquid(&liquid);

    dura_peer_wire_msg_t msg = {0};
    msg.magic = DURA_PEER_PROTOCOL_MAGIC;
    msg.version = 1;
    msg.type = (uint8_t)type;
    msg.sequence = sequence;
    msg.amount = amount;
    msg.claim_id = claim_id;
    copy_text(msg.source_liquid, sizeof(msg.source_liquid), liquid.liquid_name);
    copy_text(msg.recipe_name, sizeof(msg.recipe_name), recipe_name);
    copy_text(msg.target_liquid, sizeof(msg.target_liquid), target_liquid);

#if CONFIG_DURA_APP_QEMU_NO_RADIO
    ESP_LOGI(TAG, "qemu no-radio tx type=%u recipe=%s target=%s amount=%.2f seq=%u claim=%lu",
             (unsigned)type, msg.recipe_name, msg.target_liquid, (double)amount,
             (unsigned)sequence, (unsigned long)claim_id);
    return ESP_OK;
#else
    ESP_RETURN_ON_ERROR(ensure_peer(mac), TAG, "add peer failed");
    return esp_now_send(mac, (const uint8_t *)&msg, sizeof(msg));
#endif
#endif
}

static esp_err_t broadcast_wire(dura_peer_msg_type_t type, const char *recipe_name,
                                const char *target_liquid, float amount, uint8_t sequence, uint32_t claim_id)
{
    return send_wire_to(DURA_PEER_BROADCAST_MAC, type, recipe_name, target_liquid, amount, sequence, claim_id);
}

static void dispatch_event(const uint8_t *src_mac, const dura_peer_wire_msg_t *msg)
{
    if (src_mac == NULL || msg == NULL) {
        return;
    }

    dura_peer_event_t event = {0};
    event.type = (dura_peer_msg_type_t)msg->type;
    memcpy(event.src_mac, src_mac, ESP_NOW_ETH_ALEN);
    copy_text(event.source_liquid, sizeof(event.source_liquid), msg->source_liquid);
    copy_text(event.recipe_name, sizeof(event.recipe_name), msg->recipe_name);
    copy_text(event.target_liquid, sizeof(event.target_liquid), msg->target_liquid);
    event.amount = msg->amount;
    event.sequence = msg->sequence;
    event.claim_id = msg->claim_id;
    for (size_t i = 0; i < DURA_PEER_EVENT_CALLBACK_MAX; ++i) {
        if (s_event_cbs[i] != NULL) {
            s_event_cbs[i](&event, s_event_user_ctxs[i]);
        }
    }
}

static void on_esp_now_recv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int data_len)
{
    if (recv_info == NULL || data == NULL || data_len < (int)sizeof(dura_peer_wire_msg_t)) {
        return;
    }
    if (is_own_mac(recv_info->src_addr)) {
        return;
    }

    const dura_peer_wire_msg_t *msg = (const dura_peer_wire_msg_t *)data;
    if (msg->magic != DURA_PEER_PROTOCOL_MAGIC || msg->version != 1) {
        return;
    }

    remember_peer(recv_info->src_addr, msg->source_liquid);
    ESP_LOGI(TAG, "rx %u from=" MACSTR " src=%s recipe=%s target=%s amount=%.2f",
             msg->type, MAC2STR(recv_info->src_addr), msg->source_liquid,
             msg->recipe_name, msg->target_liquid, (double)msg->amount);

    if (msg->type == DURA_PEER_MSG_HELLO) {
        (void)send_wire_to(recv_info->src_addr, DURA_PEER_MSG_IDENTITY_UPDATE, NULL, NULL, 0.0f, 0, 0);
    } else if (msg->type == DURA_PEER_MSG_PING) {
        (void)send_wire_to(recv_info->src_addr, DURA_PEER_MSG_PONG, NULL, NULL, 0.0f, 0, 0);
    } else if (msg->type == DURA_PEER_MSG_BLE_GATEWAY_CLAIM) {
        set_ble_gateway(recv_info->src_addr, msg->claim_id);
    } else if (msg->type == DURA_PEER_MSG_BLE_GATEWAY_RELEASE) {
        clear_ble_gateway_if_current(recv_info->src_addr, msg->claim_id);
    }

    dispatch_event(recv_info->src_addr, msg);
}

static void on_esp_now_send(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    if (tx_info != NULL) {
        ESP_LOGI(TAG, "tx to " MACSTR " status=%s", MAC2STR(tx_info->des_addr),
                 status == ESP_NOW_SEND_SUCCESS ? "success" : "fail");
    } else {
        ESP_LOGI(TAG, "tx status=%s", status == ESP_NOW_SEND_SUCCESS ? "success" : "fail");
    }
}

esp_err_t dura_peers_init(void)
{
    memset(&s_snapshot, 0, sizeof(s_snapshot));

#if CONFIG_DURA_APP_QEMU_NO_RADIO
    const uint8_t qemu_mac[ESP_NOW_ETH_ALEN] = {0x02, 0x00, 0x00, 0x00, 0xD2, 0xA2};
    memcpy(s_snapshot.local_mac, qemu_mac, sizeof(s_snapshot.local_mac));
    s_snapshot.esp_now_ready = true;
    s_initialized = true;
    ESP_LOGW(TAG, "QEMU no-radio peer service ready; WiFi/ESP-NOW init skipped local MAC=" MACSTR,
             MAC2STR(s_snapshot.local_mac));
    return ESP_OK;
#endif

#if CONFIG_DURA_APP_M3000_LEGACY_ONLY
    ESP_RETURN_ON_ERROR(esp_read_mac(s_snapshot.local_mac, ESP_MAC_WIFI_STA), TAG, "read local MAC failed");
    s_snapshot.esp_now_ready = false;
    s_initialized = true;
    ESP_LOGI(TAG, "M3000 legacy local identity ready; WiFi/ESP-NOW init skipped local MAC=" MACSTR,
             MAC2STR(s_snapshot.local_mac));
    return ESP_OK;
#endif

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_cfg);
    if (err != ESP_OK && err != ESP_ERR_WIFI_INIT_STATE) {
        return err;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set WiFi STA failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start WiFi failed");
    ESP_RETURN_ON_ERROR(esp_read_mac(s_snapshot.local_mac, ESP_MAC_WIFI_STA), TAG, "read MAC failed");

    err = esp_now_init();
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_INTERNAL) {
        return err;
    }

    ESP_RETURN_ON_ERROR(esp_now_register_recv_cb(on_esp_now_recv), TAG, "register recv cb failed");
    ESP_RETURN_ON_ERROR(esp_now_register_send_cb(on_esp_now_send), TAG, "register send cb failed");
    ESP_RETURN_ON_ERROR(ensure_peer(DURA_PEER_BROADCAST_MAC), TAG, "add broadcast peer failed");

    s_snapshot.esp_now_ready = true;
    s_initialized = true;
    ESP_LOGI(TAG, "ESP-NOW peer service ready; local MAC=" MACSTR " max_peers=%u",
             MAC2STR(s_snapshot.local_mac), (unsigned)DURA_PEER_MAX_PEERS);
    return dura_peers_broadcast_hello();
}

esp_err_t dura_peers_register_event_callback(dura_peer_event_cb_t cb, void *user_ctx)
{
    ESP_RETURN_ON_FALSE(cb != NULL, ESP_ERR_INVALID_ARG, TAG, "event cb is NULL");
    for (size_t i = 0; i < DURA_PEER_EVENT_CALLBACK_MAX; ++i) {
        if (s_event_cbs[i] == cb) {
            s_event_user_ctxs[i] = user_ctx;
            return ESP_OK;
        }
    }
    for (size_t i = 0; i < DURA_PEER_EVENT_CALLBACK_MAX; ++i) {
        if (s_event_cbs[i] == NULL) {
            s_event_cbs[i] = cb;
            s_event_user_ctxs[i] = user_ctx;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t dura_peers_broadcast_hello(void)
{
    return broadcast_wire(DURA_PEER_MSG_HELLO, NULL, NULL, 0.0f, 0, 0);
}

esp_err_t dura_peers_broadcast_identity_update(void)
{
    return broadcast_wire(DURA_PEER_MSG_IDENTITY_UPDATE, NULL, NULL, 0.0f, 0, 0);
}

esp_err_t dura_peers_send_ping(void)
{
    ESP_LOGI(TAG, "broadcast ping");
    return broadcast_wire(DURA_PEER_MSG_PING, NULL, NULL, 0.0f, 0, 0);
}

esp_err_t dura_peers_claim_ble_gateway(uint32_t claim_id)
{
    ESP_RETURN_ON_FALSE(claim_id != 0, ESP_ERR_INVALID_ARG, TAG, "claim_id is zero");
    set_ble_gateway(s_snapshot.local_mac, claim_id);
    return broadcast_wire(DURA_PEER_MSG_BLE_GATEWAY_CLAIM, NULL, NULL, 0.0f, 0, claim_id);
}

esp_err_t dura_peers_release_ble_gateway(uint32_t claim_id)
{
    clear_ble_gateway_if_current(s_snapshot.local_mac, claim_id);
    return broadcast_wire(DURA_PEER_MSG_BLE_GATEWAY_RELEASE, NULL, NULL, 0.0f, 0, claim_id);
}

esp_err_t dura_peers_send_abort(void)
{
    return broadcast_wire(DURA_PEER_MSG_RECIPE_ABORT, NULL, NULL, 0.0f, 0, 0);
}

esp_err_t dura_peers_send_recipe_ingredient_start(const char *recipe_name, const dura_recipe_ingredient_t *ingredient, uint32_t transaction_id)
{
    ESP_RETURN_ON_FALSE(ingredient != NULL, ESP_ERR_INVALID_ARG, TAG, "ingredient is NULL");
    ESP_RETURN_ON_FALSE(transaction_id != 0, ESP_ERR_INVALID_ARG, TAG, "transaction_id is zero");
    return broadcast_wire(DURA_PEER_MSG_RECIPE_START, recipe_name,
                          ingredient->liquid_name, ingredient->amount, ingredient->sequence, transaction_id);
}

esp_err_t dura_peers_send_recipe_ack(const char *recipe_name, const char *target_liquid, uint8_t sequence, uint32_t transaction_id)
{
    ESP_RETURN_ON_FALSE(transaction_id != 0, ESP_ERR_INVALID_ARG, TAG, "transaction_id is zero");
    return broadcast_wire(DURA_PEER_MSG_RECIPE_ACK, recipe_name, target_liquid, 0.0f, sequence, transaction_id);
}

esp_err_t dura_peers_send_recipe_complete(const char *recipe_name, const char *target_liquid, uint8_t sequence, uint32_t transaction_id)
{
    ESP_RETURN_ON_FALSE(transaction_id != 0, ESP_ERR_INVALID_ARG, TAG, "transaction_id is zero");
    return broadcast_wire(DURA_PEER_MSG_RECIPE_COMPLETE, recipe_name, target_liquid, 0.0f, sequence, transaction_id);
}

esp_err_t dura_peers_send_recipe_start(const dura_recipe_t *recipe)
{
    ESP_RETURN_ON_FALSE(recipe != NULL, ESP_ERR_INVALID_ARG, TAG, "recipe is NULL");
    ESP_RETURN_ON_FALSE(recipe->ingredient_count <= DURA_RECIPE_MAX_INGREDIENTS,
                        ESP_ERR_INVALID_ARG, TAG, "ingredient count invalid");

    esp_err_t first_err = ESP_OK;
    for (size_t i = 0; i < recipe->ingredient_count; ++i) {
        uint32_t transaction_id = (uint32_t)((esp_timer_get_time() / 1000ULL) + (uint64_t)i + 1ULL);
        if (transaction_id == 0) {
            transaction_id = (uint32_t)i + 1U;
        }
        esp_err_t err = dura_peers_send_recipe_ingredient_start(recipe->name, &recipe->ingredients[i], transaction_id);
        if (first_err == ESP_OK && err != ESP_OK) {
            first_err = err;
        }
    }
    return first_err;
}

esp_err_t dura_peers_send_test_autorun(const char *recipe_name, uint32_t pulse_count)
{
    ESP_RETURN_ON_FALSE(recipe_name != NULL && recipe_name[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "recipe name required");
    ESP_RETURN_ON_FALSE(pulse_count > 0, ESP_ERR_INVALID_ARG, TAG, "pulse count required");
    return broadcast_wire(DURA_PEER_MSG_TEST_AUTORUN, recipe_name, NULL, 0.0f, 0, pulse_count);
}

esp_err_t dura_peers_get_snapshot(dura_peers_snapshot_t *snapshot)
{
    ESP_RETURN_ON_FALSE(snapshot != NULL, ESP_ERR_INVALID_ARG, TAG, "snapshot is NULL");
    *snapshot = s_snapshot;
    return ESP_OK;
}

esp_err_t dura_peers_format_status_json(char *json, size_t json_len)
{
    ESP_RETURN_ON_FALSE(json != NULL && json_len > 0, ESP_ERR_INVALID_ARG, TAG, "bad json buffer");

    int64_t now_ms = esp_timer_get_time() / 1000;
    int64_t gateway_age_ms = s_snapshot.ble_gateway_active && s_snapshot.ble_gateway_last_seen_ms > 0 && now_ms >= s_snapshot.ble_gateway_last_seen_ms ?
                             now_ms - s_snapshot.ble_gateway_last_seen_ms : -1;
    const bool gateway_is_local = s_snapshot.ble_gateway_active &&
                                  memcmp(s_snapshot.ble_gateway_mac, s_snapshot.local_mac,
                                         sizeof(s_snapshot.local_mac)) == 0;
    int written = dura_json_snprintf(json, json_len,
                           "{\"esp_now_ready\":%s,\"local_mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"ble_gateway\":{\"active\":%s,\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"claim_id\":%lu,\"age_ms\":%lld,\"local\":%s},\"peer_count\":%u,\"max_peers\":%u,\"peers\":[",
                           s_snapshot.esp_now_ready ? "true" : "false",
                           s_snapshot.local_mac[0], s_snapshot.local_mac[1], s_snapshot.local_mac[2],
                           s_snapshot.local_mac[3], s_snapshot.local_mac[4], s_snapshot.local_mac[5],
                           s_snapshot.ble_gateway_active ? "true" : "false",
                           s_snapshot.ble_gateway_mac[0], s_snapshot.ble_gateway_mac[1], s_snapshot.ble_gateway_mac[2],
                           s_snapshot.ble_gateway_mac[3], s_snapshot.ble_gateway_mac[4], s_snapshot.ble_gateway_mac[5],
                           (unsigned long)s_snapshot.ble_gateway_claim_id,
                           (long long)gateway_age_ms,
                           gateway_is_local ? "true" : "false",
                           (unsigned)s_snapshot.peer_count,
                           (unsigned)DURA_PEER_MAX_PEERS);
    if (written <= 0 || (size_t)written >= json_len) {
        return dura_json_fail(json, json_len, written);
    }

    size_t used = (size_t)written;
    for (size_t i = 0; i < s_snapshot.peer_count; ++i) {
        const dura_peer_info_t *peer = &s_snapshot.peers[i];
        int64_t age_ms = peer->last_seen_ms > 0 && now_ms >= peer->last_seen_ms ? now_ms - peer->last_seen_ms : -1;
        written = dura_json_snprintf(json + used, json_len - used,
                           "%s{\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"liquid\":\"%s\",\"last_seen_ms\":%lld,\"age_ms\":%lld}",
                           i == 0 ? "" : ",",
                           peer->mac[0], peer->mac[1], peer->mac[2], peer->mac[3], peer->mac[4], peer->mac[5],
                           peer->liquid_name,
                           (long long)peer->last_seen_ms,
                           (long long)age_ms);
        if (written <= 0 || (size_t)written >= json_len - used) {
            return dura_json_fail(json, json_len, written);
        }
        used += (size_t)written;
    }

    written = dura_json_snprintf(json + used, json_len - used, "]}");
    return dura_json_result(json, json_len, written);
}
