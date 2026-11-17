#include <stdbool.h>
#include <stdatomic.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"

#include "dura_wifi_provisioning.h"
#include "dura_ota.h"

static const char *TAG = "dura_wifi_provisioning";
static bool s_wifi_initialized;
static atomic_bool s_provisioning_manager_initialized;
static atomic_bool s_credentials_accepted;
static atomic_bool s_cleanup_started;
static atomic_bool s_stopping;

static void provisioning_cleanup_task(void *arg)
{
    (void)arg;
    /* NETWORK_PROV_END is service shutdown, not proof of credential success. */
    esp_err_t err = network_prov_mgr_deinit();
    if (err != ESP_OK || !s_credentials_accepted) {
        ESP_LOGE(TAG, "Provisioning ended without a usable handoff: %s", esp_err_to_name(err));
        esp_restart();
    }
    s_provisioning_manager_initialized = false;
    /* Cover a disconnect queued while deinit removed the manager handlers. */
    (void)esp_wifi_connect();
    err = dura_ota_start_polling();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA task start failed: %s", esp_err_to_name(err));
        esp_restart();
    }
    vTaskDelete(NULL);
}

static void provisioning_event_handler(void *arg, esp_event_base_t base,
                                       int32_t id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (base != NETWORK_PROV_EVENT) return;
    if (id == NETWORK_PROV_WIFI_CRED_SUCCESS) {
        s_credentials_accepted = true;
        ESP_LOGI(TAG, "Wi-Fi provisioning complete");
    } else if (id == NETWORK_PROV_END && !s_cleanup_started) {
        s_cleanup_started = true;
        if (xTaskCreate(provisioning_cleanup_task, "prov_cleanup", 4096,
                        NULL, tskIDLE_PRIORITY + 3, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Unable to create provisioning cleanup task");
            esp_restart();
        }
    } else if (id == NETWORK_PROV_WIFI_CRED_FAIL) {
        s_credentials_accepted = false;
        ESP_LOGW(TAG, "Wi-Fi credentials failed; accepting corrected credentials");
        esp_err_t err = network_prov_mgr_reset_wifi_sm_state_on_failure();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Provisioning recovery failed: %s", esp_err_to_name(err));
            esp_restart();
        }
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED &&
        !s_stopping && !s_provisioning_manager_initialized) {
        /* While provisioning, the manager owns retries. Afterwards this
         * session must recover an AP interruption during download. */
        (void)esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Wi-Fi connected");
    }
}

esp_err_t dura_wifi_init_and_provision(void)
{
    if (s_wifi_initialized) return ESP_OK;
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    ESP_RETURN_ON_FALSE(esp_netif_create_default_wifi_sta() != NULL,
                        ESP_ERR_NO_MEM, TAG, "wifi netif");
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&config), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   wifi_event_handler, NULL), TAG, "wifi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   wifi_event_handler, NULL), TAG, "IP events");
    s_wifi_initialized = true;
    /* Normal controller boot never starts Wi-Fi or the provisioning radio. */
    return ESP_OK;
}

esp_err_t dura_wifi_reset_provisioning(void)
{
    ESP_RETURN_ON_ERROR(dura_wifi_init_and_provision(), TAG, "wifi cleanup init");
    s_stopping = true;
    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) return err;
    return esp_wifi_restore();
}

esp_err_t dura_wifi_start_provisioning(void)
{
    ESP_RETURN_ON_ERROR(dura_wifi_init_and_provision(), TAG, "wifi provisioning init");
    ESP_RETURN_ON_FALSE(!s_provisioning_manager_initialized,
                        ESP_ERR_INVALID_STATE, TAG, "provisioning already initialized");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID,
                                                   provisioning_event_handler, NULL), TAG, "provisioning events");
    network_prov_mgr_config_t config = {
        .scheme = network_prov_scheme_ble,
        .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
        .app_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
        .network_prov_wifi_conn_cfg = { .wifi_conn_attempts = 3 },
    };
    ESP_RETURN_ON_ERROR(network_prov_mgr_init(config), TAG, "provisioning manager init");
    s_provisioning_manager_initialized = true;
    s_credentials_accepted = false;
    s_cleanup_started = false;
    s_stopping = false;
    return network_prov_mgr_start_provisioning(NETWORK_PROV_SECURITY_1,
                                               "dura-m3000", "PROV_M3000", NULL);
}
