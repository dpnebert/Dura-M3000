#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "dura_ota.h"
#include "dura_wifi_provisioning.h"
#include "dura_board.h"

static const char *TAG = "dura_ota";
static const char *NVS_NAMESPACE = "dura_ota";
static bool s_polling;
static TaskHandle_t s_poll_task;
static bool s_boot_update_check_pending;

static esp_err_t load_server_etag(char *etag, size_t len)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    size_t actual = len;
    err = nvs_get_str(handle, "etag", etag, &actual);
    nvs_close(handle);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

static esp_err_t save_server_etag(const char *etag)
{
    nvs_handle_t handle = 0;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle),
                        TAG, "open OTA NVS failed");
    esp_err_t err = nvs_set_str(handle, "etag", etag);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t poll_server(char *etag, size_t len)
{
    char *response_etag = NULL;
    char *response_modified = NULL;
    esp_http_client_config_t config = {
        .url = CONFIG_DURA_OTA_URL,
        .method = HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    ESP_RETURN_ON_FALSE(client != NULL, ESP_FAIL, TAG, "HTTP client init failed");

    esp_err_t err = esp_http_client_open(client, 0);
    int64_t content_length = -1;
    if (err == ESP_OK) {
        content_length = esp_http_client_fetch_headers(client);
        if (content_length < 0) {
            err = ESP_FAIL;
        }
    }
    int status = esp_http_client_get_status_code(client);
    ESP_LOGV(TAG, "OTA poll transport complete: open=%s headers=%" PRId64
             " status=%d",
             esp_err_to_name(err), content_length, status);
    if (err == ESP_OK && status == 200) {
        esp_err_t etag_err = esp_http_client_get_response_header(client, "ETag", &response_etag);
        esp_err_t modified_err =
            esp_http_client_get_response_header(client, "Last-Modified", &response_modified);
        ESP_LOGI(TAG, "OTA response headers: ETag=%s Last-Modified=%s",
                 response_etag ? response_etag : "<none>",
                 response_modified ? response_modified : "<none>");
        ESP_LOGV(TAG, "OTA response header lookup: ETag=%s Last-Modified=%s",
                 esp_err_to_name(etag_err), esp_err_to_name(modified_err));
    }
    if (err != ESP_OK || status != 200) {
        esp_http_client_cleanup(client);
        ESP_LOGE(TAG, "OTA endpoint returned %d, fetch error %s",
                 status, esp_err_to_name(err));
        return ESP_FAIL;
    }

    const char *token = response_etag && response_etag[0] ? response_etag : response_modified;
    if (token == NULL || token[0] == '\0') {
        esp_http_client_cleanup(client);
        ESP_LOGE(TAG, "OTA endpoint has no change token");
        return ESP_FAIL;
    }
    strlcpy(etag, token, len);
    esp_http_client_cleanup(client);
    return ESP_OK;
}

static esp_err_t perform_ota(const char *etag)
{
    ESP_LOGI(TAG, "Downloading OTA image from %s", CONFIG_DURA_OTA_URL);
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_WARN);
    esp_log_level_set("esp-tls", ESP_LOG_WARN);
    esp_log_level_set("esp_https_ota", ESP_LOG_INFO);
    esp_http_client_config_t http_config = {
        .url = CONFIG_DURA_OTA_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };
    esp_https_ota_handle_t ota_handle = NULL;
    ESP_RETURN_ON_ERROR(esp_https_ota_begin(&ota_config, &ota_handle),
                        TAG, "OTA download begin failed");
    ESP_LOGI(TAG, "OTA HTTP status: %d, image size: %" PRId32,
             esp_https_ota_get_status_code(ota_handle),
             esp_https_ota_get_image_size(ota_handle));

    esp_err_t err;
    int last_progress_log = 0;
    do {
        err = esp_https_ota_perform(ota_handle);
        if (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            int bytes_read = esp_https_ota_get_image_len_read(ota_handle);
            if (bytes_read >= 0 && bytes_read - last_progress_log >= 65536) {
                ESP_LOGI(TAG, "OTA progress: %d bytes read", bytes_read);
                last_progress_log = bytes_read;
            }
        }
    } while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);
    if (err != ESP_OK) {
        esp_https_ota_abort(ota_handle);
        return err;
    }
    ESP_RETURN_ON_FALSE(esp_https_ota_is_complete_data_received(ota_handle),
                        ESP_FAIL, TAG, "OTA image incomplete");

    ESP_RETURN_ON_ERROR(esp_https_ota_finish(ota_handle), TAG, "OTA image finish failed");
    ESP_RETURN_ON_ERROR(save_server_etag(etag), TAG, "save OTA state failed");
    ESP_RETURN_ON_ERROR(dura_wifi_reset_provisioning(), TAG, "Wi-Fi credential clear failed");
    const esp_app_desc_t *running = esp_app_get_description();
    ESP_LOGI(TAG, "OTA complete; restarting image %s", running->version);
    esp_restart();
    return ESP_OK;
}

static void ota_poll_task(void *arg)
{
    (void)arg;
    char seen[128] = {0};
    char remote[128] = {0};
    esp_err_t err = load_server_etag(seen, sizeof(seen));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Unable to load OTA state: %s", esp_err_to_name(err));
    }

    while (s_polling) {
        remote[0] = '\0';
        err = poll_server(remote, sizeof(remote));
        if (err == ESP_OK && strcmp(remote, seen) != 0) {
            ESP_LOGI(TAG, "OTA update available: %s", remote);
            vTaskDelay(pdMS_TO_TICKS(1000));
            err = perform_ota(remote);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "OTA update failed: %s", esp_err_to_name(err));
            } else {
                break;
            }
        } else if (err != ESP_OK) {
            ESP_LOGW(TAG, "OTA poll failed: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_DURA_OTA_POLL_SECONDS * 1000));
    }
    s_poll_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t dura_ota_start_polling(void)
{
    nvs_handle_t handle = 0;
    char applied[128] = {0};
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        size_t len = sizeof(applied);
        if (nvs_get_str(handle, "etag", applied, &len) == ESP_OK && applied[0] != '\0') {
            s_boot_update_check_pending = false;
        }
        nvs_close(handle);
    }
    if (s_polling) return ESP_OK;
    dura_board_set_ota_active(true);
    s_polling = true;
    if (xTaskCreate(ota_poll_task, "dura_ota", 16384, NULL, 5, &s_poll_task) != pdPASS) {
        s_polling = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void dura_ota_stop_polling(void)
{
    s_polling = false;
    dura_board_set_ota_active(false);
}
