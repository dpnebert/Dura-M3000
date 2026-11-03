#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"

#include "dura_wifi_provisioning.h"
#include "dura_ota.h"

static const char *TAG = "dura_wifi_provisioning";
static bool s_provisioning_manager_initialized;

static void provisioning_cleanup_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_err_t err = network_prov_mgr_deinit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Provisioning BLE cleanup failed: %s",
                 esp_err_to_name(err));
    } else {
        s_provisioning_manager_initialized = false;
        ESP_LOGI(TAG, "Provisioning BLE resources released");
    }
    ESP_ERROR_CHECK(dura_ota_start_polling());
    vTaskDelete(NULL);
}

static void provisioning_event_handler(void *arg, esp_event_base_t base,
                                       int32_t id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (base != NETWORK_PROV_EVENT) return;
    if (id == NETWORK_PROV_WIFI_CRED_SUCCESS) {
        ESP_LOGI(TAG, "Wi-Fi provisioning complete");
    } else if (id == NETWORK_PROV_END) {
        ESP_LOGI(TAG, "Provisioning confirmation received; stopping provisioning");
        if (xTaskCreate(provisioning_cleanup_task, "prov_cleanup", 4096,
                        NULL, tskIDLE_PRIORITY + 3, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Unable to create provisioning cleanup task");
        }
    } else if (id == NETWORK_PROV_WIFI_CRED_FAIL) {
        ESP_LOGW(TAG, "Wi-Fi provisioning credentials failed");
    }
}

esp_err_t dura_wifi_reset_provisioning(void)
{
    ESP_RETURN_ON_ERROR(esp_wifi_stop(), TAG, "wifi stop failed");
    ESP_RETURN_ON_ERROR(esp_wifi_restore(), TAG, "wifi provisioning reset failed");
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Wi-Fi connected");
    }
}

esp_err_t dura_wifi_init_and_provision(void)
{
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
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_cfg), TAG, "wifi init failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   wifi_event_handler, NULL),
                        TAG, "wifi event handler registration failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   wifi_event_handler, NULL),
                        TAG, "IP event handler registration failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi STA mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");

    wifi_config_t sta_config = {0};
    ESP_RETURN_ON_ERROR(esp_wifi_get_config(WIFI_IF_STA, &sta_config),
                        TAG, "wifi STA config read failed");
    if (sta_config.sta.ssid[0] != '\0') {
        ESP_LOGI(TAG, "Saved Wi-Fi credentials found");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "No saved Wi-Fi credentials; waiting for provisioning gesture");
    return ESP_OK;
}

esp_err_t dura_wifi_start_provisioning(void)
{
    if (!s_provisioning_manager_initialized) {
        ESP_RETURN_ON_ERROR(esp_event_handler_register(NETWORK_PROV_EVENT,
                                                       ESP_EVENT_ANY_ID,
                                                       provisioning_event_handler,
                                                       NULL),
                            TAG, "provisioning event handler failed");
        network_prov_mgr_config_t config = {
            .scheme = network_prov_scheme_ble,
            .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
            .app_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
            .network_prov_wifi_conn_cfg = { .wifi_conn_attempts = 0 },
        };
        ESP_RETURN_ON_ERROR(network_prov_mgr_init(config),
                            TAG, "provisioning manager init failed");
        s_provisioning_manager_initialized = true;
    }

    const char *pop = "dura-m3000";
    return network_prov_mgr_start_provisioning(NETWORK_PROV_SECURITY_1,
                                               pop, "PROV_M3000", NULL);
}
