#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"

#include "dura_peers.h"
#include "dura_json_builder.h"

static const char *TAG = "dura_identity";
static dura_peers_snapshot_t s_snapshot;
static bool s_initialized;

static esp_err_t unsupported(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t dura_peers_init(void)
{
    memset(&s_snapshot, 0, sizeof(s_snapshot));
#if CONFIG_DURA_APP_QEMU_NO_RADIO
    const uint8_t qemu_mac[6] = {0x02, 0x00, 0x00, 0x00, 0xD2, 0xA2};
    memcpy(s_snapshot.local_mac, qemu_mac, sizeof(s_snapshot.local_mac));
#else
    ESP_RETURN_ON_ERROR(esp_read_mac(s_snapshot.local_mac, ESP_MAC_WIFI_STA),
                        TAG, "read local identity MAC failed");
#endif
    s_snapshot.esp_now_ready = false;
    s_initialized = true;
    ESP_LOGI(TAG, "M3000 local identity ready; peer radio implementation excluded");
    return ESP_OK;
}

esp_err_t dura_peers_register_event_callback(dura_peer_event_cb_t cb, void *user_ctx)
{
    (void)user_ctx;
    ESP_RETURN_ON_FALSE(cb != NULL, ESP_ERR_INVALID_ARG, TAG, "event callback is NULL");
    /* Preserve the API expected by the coordinator without installing a radio callback. */
    return ESP_OK;
}

esp_err_t dura_peers_broadcast_hello(void) { return unsupported(); }
esp_err_t dura_peers_broadcast_identity_update(void) { return unsupported(); }
esp_err_t dura_peers_send_ping(void) { return unsupported(); }
esp_err_t dura_peers_claim_ble_gateway(uint32_t claim_id)
{
    (void)claim_id;
    return unsupported();
}
esp_err_t dura_peers_release_ble_gateway(uint32_t claim_id)
{
    (void)claim_id;
    return unsupported();
}
esp_err_t dura_peers_send_abort(void) { return unsupported(); }
esp_err_t dura_peers_send_recipe_ingredient_start(const char *recipe_name,
                                                   const dura_recipe_ingredient_t *ingredient,
                                                   uint32_t transaction_id)
{
    (void)recipe_name;
    (void)ingredient;
    (void)transaction_id;
    return unsupported();
}
esp_err_t dura_peers_send_recipe_ack(const char *recipe_name, const char *target_liquid,
                                     uint8_t sequence, uint32_t transaction_id)
{
    (void)recipe_name;
    (void)target_liquid;
    (void)sequence;
    (void)transaction_id;
    return unsupported();
}
esp_err_t dura_peers_send_recipe_complete(const char *recipe_name, const char *target_liquid,
                                          uint8_t sequence, uint32_t transaction_id)
{
    (void)recipe_name;
    (void)target_liquid;
    (void)sequence;
    (void)transaction_id;
    return unsupported();
}
esp_err_t dura_peers_send_recipe_start(const dura_recipe_t *recipe)
{
    (void)recipe;
    return unsupported();
}
esp_err_t dura_peers_send_test_autorun(const char *recipe_name, uint32_t pulse_count)
{
    (void)recipe_name;
    (void)pulse_count;
    return unsupported();
}

esp_err_t dura_peers_get_snapshot(dura_peers_snapshot_t *snapshot)
{
    ESP_RETURN_ON_FALSE(snapshot != NULL, ESP_ERR_INVALID_ARG, TAG, "snapshot is NULL");
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "identity is not initialized");
    *snapshot = s_snapshot;
    return ESP_OK;
}

esp_err_t dura_peers_format_status_json(char *json, size_t json_len)
{
    ESP_RETURN_ON_FALSE(json != NULL && json_len > 0, ESP_ERR_INVALID_ARG, TAG, "bad JSON buffer");
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "identity is not initialized");

    int written = dura_json_snprintf(json, json_len,
                           "{\"peer_radio_included\":false,\"peer_radio_ready\":false,"
                           "\"local_mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
                           "\"ble_gateway\":{\"active\":false},\"peer_count\":0,\"peers\":[]}",
                           s_snapshot.local_mac[0], s_snapshot.local_mac[1],
                           s_snapshot.local_mac[2], s_snapshot.local_mac[3],
                           s_snapshot.local_mac[4], s_snapshot.local_mac[5]);
    return dura_json_result(json, json_len, written);
}
