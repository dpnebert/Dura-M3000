#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dura_board.h"
#include "dura_ota.h"
#include "dura_wifi_provisioning.h"

static const char *TAG = "dura_ota";
static atomic_bool s_polling;
static bool s_pending_verify;
static esp_timer_handle_t s_verify_deadline;

static void verification_expired(void *arg)
{
    (void)arg;
    /* Still unconfirmed: reset lets the rollback-enabled bootloader recover. */
    esp_restart();
}

esp_err_t dura_ota_boot_begin(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    ESP_LOGI(TAG, "OTA slots: running=%s boot=%s next=%s",
             running ? running->label : "none",
             boot ? boot->label : "none",
             next ? next->label : "none");
    esp_ota_img_states_t state;
    esp_err_t err = esp_ota_get_state_partition(running, &state);
    if (err == ESP_ERR_NOT_FOUND) return ESP_OK; /* Initial serial-loaded ota_0. */
    ESP_RETURN_ON_ERROR(err, TAG, "read running OTA state");
    s_pending_verify = state == ESP_OTA_IMG_PENDING_VERIFY;
    if (!s_pending_verify) return ESP_OK;
    dura_board_set_ota_active(true);
    const esp_timer_create_args_t timer = {
        .callback = verification_expired, .name = "ota_verify",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer, &s_verify_deadline), TAG, "verify timer");
    return esp_timer_start_once(s_verify_deadline, 30000000ULL);
}

bool dura_ota_boot_pending(void)
{
    return s_pending_verify;
}

esp_err_t dura_ota_boot_finish(bool controller_healthy)
{
    if (s_pending_verify) {
        if (!controller_healthy) {
            return esp_ota_mark_app_invalid_rollback_and_reboot();
        }
        ESP_RETURN_ON_ERROR(esp_ota_mark_app_valid_cancel_rollback(), TAG, "confirm image");
        ESP_ERROR_CHECK(esp_timer_stop(s_verify_deadline));
        ESP_ERROR_CHECK(esp_timer_delete(s_verify_deadline));
        s_verify_deadline = NULL;
        s_pending_verify = false;
        dura_board_set_ota_active(false);
        ESP_LOGI(TAG, "Controller health check passed; OTA image confirmed");
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running && (running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ||
                    running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1)) {
        esp_ota_img_states_t state;
        ESP_RETURN_ON_ERROR(esp_ota_get_state_partition(running, &state), TAG, "cleanup OTA state");
        ESP_RETURN_ON_FALSE(state == ESP_OTA_IMG_VALID, ESP_ERR_INVALID_STATE,
                            TAG, "unconfirmed image cannot clear credentials");
        /* Retry cleanup on later boots if a reset interrupted it. No success
         * token is written by the downloader. otadata is the authority. */
        return dura_wifi_reset_provisioning();
    }
    return ESP_OK;
}

bool dura_ota_update_allowed(void)
{
#if !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    return false;
#else
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    /* Alternate OTA slots on every confirmed update. The running slot and
     * selected boot slot must match so the gesture cannot race a reboot. */
    return !s_pending_verify && running && boot && next &&
           (running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ||
            running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) &&
           (next->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ||
            next->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) &&
           next->address != running->address &&
           running->address == boot->address;
#endif
}

static esp_err_t perform_ota(void)
{
    ESP_RETURN_ON_FALSE(dura_ota_update_allowed(), ESP_ERR_INVALID_STATE,
                        TAG, "one-off update unavailable");
    const esp_partition_t *running_slot = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    ESP_LOGI(TAG, "Starting OTA download from running %s to inactive %s",
             running_slot ? running_slot->label : "none",
             next ? next->label : "none");
    esp_http_client_config_t http_config = {
        .url = CONFIG_DURA_OTA_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    const esp_https_ota_config_t config = { .http_config = &http_config };
    esp_https_ota_handle_t handle = NULL;
    ESP_RETURN_ON_ERROR(esp_https_ota_begin(&config, &handle), TAG, "OTA begin");
    esp_app_desc_t candidate;
    esp_app_desc_t failed;
    esp_err_t err = esp_https_ota_get_img_desc(handle, &candidate);
    if (err != ESP_OK) goto abort;
    const esp_app_desc_t *running = esp_app_get_description();
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    /* Read the failed descriptor BEFORE perform() can erase that slot. The
     * ELF hash identifies a build, independent of version text or HTTP ETags;
     * it is not a signature or an adversarial authenticity check. */
    if (strncmp(candidate.project_name, running->project_name,
                sizeof(candidate.project_name)) != 0 ||
        memcmp(candidate.app_elf_sha256, running->app_elf_sha256, 32) == 0 ||
        (invalid && esp_ota_get_partition_description(invalid, &failed) == ESP_OK &&
         memcmp(candidate.app_elf_sha256, failed.app_elf_sha256, 32) == 0)) {
        err = ESP_ERR_INVALID_VERSION;
        goto abort;
    }
    do {
        err = esp_https_ota_perform(handle);
    } while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS && atomic_load(&s_polling));
    if (err != ESP_OK) goto abort;
    if (!esp_https_ota_is_complete_data_received(handle)) {
        err = ESP_FAIL;
        goto abort;
    }
    /* finish verifies the image and selects the boot slot; it frees handle
     * even on failure. Never abort it again after this call. */
    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "Image staged; rebooting for controller health validation");
    esp_restart();
    return ESP_OK;
abort:
    (void)esp_https_ota_abort(handle);
    return err;
}

static void ota_poll_task(void *arg)
{
    (void)arg;
    while (atomic_load(&s_polling)) {
        esp_err_t err = perform_ota();
        if (err == ESP_ERR_INVALID_STATE || err == ESP_ERR_INVALID_VERSION) {
            ESP_LOGW(TAG, "Image rejected; press SELECT to return to normal operation");
            break;
        }
        ESP_LOGW(TAG, "OTA attempt: %s; retrying while update screen is open",
                 esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(CONFIG_DURA_OTA_POLL_SECONDS * 1000));
    }
    atomic_store(&s_polling, false);
    /* The board remains awake and outputs fenced until SELECT/reboot. */
    vTaskDelete(NULL);
}

esp_err_t dura_ota_start_polling(void)
{
    ESP_RETURN_ON_FALSE(dura_ota_update_allowed(), ESP_ERR_INVALID_STATE,
                        TAG, "one-off update unavailable");
    if (atomic_exchange(&s_polling, true)) return ESP_OK;
    if (xTaskCreate(ota_poll_task, "dura_ota", 16384, NULL, 5, NULL) != pdPASS) {
        atomic_store(&s_polling, false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
