#include "gama_blefob_internal.h"

const char *TAG = "gama_blefob";

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED

QueueHandle_t s_ble_shell_queue;
uint8_t s_ble_addr_type;
uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
uint16_t s_response_chr_val_handle;
ble_shell_config_t s_cfg;
ble_shell_app_config_t s_app_cfg;
bool s_initialized;
bool s_fault_latched;
uint32_t s_fault_code;
bool s_maintenance_unlocked;
bool s_ble_synced;
bool s_response_notify_active;
bool s_advertising_window_open;
int64_t s_adv_deadline_us;
char s_last_response[BLE_SHELL_TEXT_RESPONSE_CAPACITY] = "ready";
char s_last_status[BLE_SHELL_MAX_PAYLOAD];
char s_last_log[BLE_SHELL_MAX_PAYLOAD] = "boot";
char s_last_diagnostic[BLE_SHELL_MAX_PAYLOAD] = "diagnostic idle";

static portMUX_TYPE s_stack_mux = portMUX_INITIALIZER_UNLOCKED;
static unsigned s_stack_users;
static bool s_sleep_cutoff;
static bool s_radio_off;
static bool s_radio_failed;
static bool s_resume_advertising;
static SemaphoreHandle_t s_admission_done;
static SemaphoreHandle_t s_host_exited;
static SemaphoreHandle_t s_host_synced;
static struct ble_npl_event s_admission_event;
static esp_err_t s_admission_result;

bool ble_shell_stack_enter(void)
{
    portENTER_CRITICAL(&s_stack_mux);
    const bool allowed = !s_sleep_cutoff;
    if (allowed) ++s_stack_users;
    portEXIT_CRITICAL(&s_stack_mux);
    return allowed;
}

void ble_shell_stack_exit(void)
{
    portENTER_CRITICAL(&s_stack_mux);
    --s_stack_users;
    portEXIT_CRITICAL(&s_stack_mux);
}

static void ble_sleep_set_cutoff(bool closed)
{
    portENTER_CRITICAL(&s_stack_mux);
    s_sleep_cutoff = closed;
    portEXIT_CRITICAL(&s_stack_mux);
}

void ble_shell_sleep_synced(void)
{
    xSemaphoreGive(s_host_synced);
}

/* v6.0.2 exports this table iterator in ble_gap.c but omits its prototype
 * from ble_gap.h. It visits the actual connection table under the host lock,
 * unlike ble_gap_conn_active(), which only reports connection initiation. */
extern void ble_gap_conn_foreach_handle(ble_gap_conn_foreach_handle_fn *cb, void *arg);
extern void ble_hs_lock(void);
extern void ble_hs_unlock(void);
static int ble_sleep_connection_found(uint16_t handle, void *arg)
{
    (void)handle;
    *(bool *)arg = true;
    return 1;
}

static void ble_sleep_admit(struct ble_npl_event *event)
{
    (void)event;
    s_admission_result = ESP_ERR_INVALID_STATE;
    /* Host event serialization protects established callbacks. A lease that
     * was already in flight vetoes, rather than blocking the host task. */
    portENTER_CRITICAL(&s_stack_mux);
    const bool idle = !s_sleep_cutoff && s_stack_users == 0;
    if (idle) s_sleep_cutoff = true;
    portEXIT_CRITICAL(&s_stack_mux);
    if (idle) {
        bool connected = s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
        /* The exported iterator requires its caller to hold the host lock. */
        ble_hs_lock();
        ble_gap_conn_foreach_handle(ble_sleep_connection_found, &connected);
        ble_hs_unlock();
        if (connected || !s_ble_synced) {
            ble_sleep_set_cutoff(false);
        } else {
            /* This disconnected observation is the admission cutoff. Later
             * controller connections are deliberately dropped by normal stop. */
            s_resume_advertising = s_advertising_window_open;
            s_admission_result = ESP_OK;
        }
    }
    xSemaphoreGive(s_admission_done);
}

static esp_err_t ble_radio_init(void)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) return err;
    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = s_app_cfg.security_mode == GAMA_BLEFOB_SECURITY_PAIR_REQUIRED;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    if (ble_hs_cfg.sm_bonding) {
        ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
        ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    }
    ble_svc_gap_init();
    ble_svc_gatt_init();
    err = ble_svc_gap_device_name_set(s_cfg.device_name);
    if (err != ESP_OK) return err;
    s_response_chr_val_handle = 0;
    if (ble_gatts_count_cfg(gatt_svcs) != 0 || ble_gatts_add_svcs(gatt_svcs) != 0) return ESP_FAIL;
    ble_store_config_init();
    return ESP_OK;
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    /* nimble_port_stop() can return before run() unwinds. The owner waits
     * here, then deletes this task through the normal FreeRTOS adapter before
     * deinit/reinit; an old host must never delete a newly created host. */
    xSemaphoreGive(s_host_exited);
    vTaskSuspend(NULL);
}
#endif

esp_err_t gama_blefob_init(const gama_blefob_config_t *config)
{
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
	esp_err_t err;

	if (s_initialized) {
		return ESP_ERR_INVALID_STATE;
	}
	if (!s_admission_done) s_admission_done = xSemaphoreCreateBinary();
	if (!s_host_exited) s_host_exited = xSemaphoreCreateBinary();
	if (!s_host_synced) s_host_synced = xSemaphoreCreateBinary();
	if (!s_admission_done || !s_host_exited || !s_host_synced) return ESP_ERR_NO_MEM;
	ble_shell_apply_app_config(config);
	ESP_LOGI(TAG, "Starting reusable ESP BLE shell");

	s_ble_shell_queue = xQueueCreate(BLE_SHELL_EVENT_QUEUE_LEN, sizeof(ble_shell_msg_t *));
	if (s_ble_shell_queue == NULL) {
		ESP_LOGE(TAG, "Failed to create BLE shell queue");
		return ESP_FAIL;
	}

	if (s_app_cfg.init_nvs_if_needed) {
		err = nvs_flash_init();
		if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
			return err;
		}
	} else {
		/* Production path: the application owns NVS init and any recovery/erase policy. */
		ESP_LOGI(TAG, "Expecting application-owned NVS initialization");
	}
	ble_shell_config_load();

	err = ble_radio_init();
	if (err != ESP_OK) return err;

	if (xTaskCreate(ble_shell_task, "ble_shell", s_app_cfg.shell_task_stack_size, NULL, CONFIG_BLE_SHELL_TASK_PRIORITY, NULL) != pdPASS) {
		ESP_LOGE(TAG, "Failed to create BLE shell task");
		return ESP_ERR_NO_MEM;
	}
#if CONFIG_BLE_SHELL_PAIR_BUTTON_GPIO >= 0
	if (xTaskCreate(pair_button_task, "pair_button", CONFIG_BLE_SHELL_PAIR_BUTTON_TASK_STACK, NULL, CONFIG_BLE_SHELL_PAIR_BUTTON_TASK_PRIORITY, NULL) != pdPASS) {
		ESP_LOGE(TAG, "Failed to create BLE pair button task");
		return ESP_ERR_NO_MEM;
	}
#else
	ESP_LOGI(TAG, "Pair button task not created; CONFIG_BLE_SHELL_PAIR_BUTTON_GPIO=-1");
#endif
	nimble_port_freertos_init(ble_host_task);
	s_initialized = true;
	return ESP_OK;
#else
	ESP_LOGW(TAG, "BLE not enabled in sdkconfig.");
	ESP_LOGW(TAG, "Enable BT and NimBLE in menuconfig to run the BLE server.");
	return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t gama_blefob_sleep_shutdown(void)
{
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
    if (!s_initialized || s_radio_failed || s_radio_off) return ESP_ERR_INVALID_STATE;
    ble_npl_event_init(&s_admission_event, ble_sleep_admit, NULL);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_admission_event);
    /* No timed-out event may outlive its caller and later shut a live system
     * down. Like the SDK stop, wait for completion with outputs fenced. */
    xSemaphoreTake(s_admission_done, portMAX_DELAY);
    if (s_admission_result != ESP_OK) return s_admission_result;
    s_radio_failed = true; /* Unknown/partial failure is never success. */
    if (nimble_port_stop() != 0) return ESP_FAIL;
    xSemaphoreTake(s_host_exited, portMAX_DELAY);
    nimble_port_freertos_deinit();
    /* v6.0.2 NPL events own a block in the SDK pool. Release and clear the
     * handle after host exit, while that pool still exists, before teardown. */
    ble_npl_event_deinit(&s_admission_event);
    /* Do not replay writes queued before the cutoff after a sleep rejection. */
    ble_shell_msg_t *stale = NULL;
    while (xQueueReceive(s_ble_shell_queue, &stale, 0) == pdTRUE) free(stale);
    const esp_err_t err = nimble_port_deinit();
    if (err != ESP_OK) return err;
    s_ble_synced = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_response_notify_active = false;
    s_advertising_window_open = false;
    s_radio_off = true;
    s_radio_failed = false;
#endif
    return ESP_OK;
}

esp_err_t gama_blefob_sleep_resume(void)
{
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
    if (s_radio_failed) return ESP_ERR_INVALID_STATE;
    if (!s_radio_off) return ESP_OK; /* Preparation failed or admission vetoed. */
    s_radio_failed = true;
    (void)xSemaphoreTake(s_host_synced, 0); /* discard cold-boot sync */
    esp_err_t err = ble_radio_init();
    if (err != ESP_OK) return err;
    nimble_port_freertos_init(ble_host_task);
    if (xSemaphoreTake(s_host_synced, pdMS_TO_TICKS(2000)) != pdTRUE || !s_ble_synced) {
        return ESP_ERR_TIMEOUT;
    }
    s_radio_off = false;
    ble_sleep_set_cutoff(false);
    if (s_resume_advertising && s_cfg.ble_advertising_enabled) {
        ble_shell_open_pairing_window(s_cfg.pair_window_seconds, "sleep rollback advertising");
        if (!ble_gap_adv_active()) {
            ble_sleep_set_cutoff(true);
            return ESP_FAIL;
        }
    }
    s_radio_failed = false;
#endif
    return ESP_OK;
}

esp_err_t gama_blefob_init_default(void)
{
	return gama_blefob_init(NULL);
}

esp_err_t gama_blefob_open_pairing_window(uint32_t seconds)
{
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
	if (!s_initialized) {
		return ESP_ERR_INVALID_STATE;
	}
	ble_shell_open_pairing_window(seconds, "app opened pair window");
	return ESP_OK;
#else
	return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t gama_blefob_close_pairing_window(void)
{
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
	if (!s_initialized) {
		return ESP_ERR_INVALID_STATE;
	}
	ble_shell_close_pairing_window();
	return ESP_OK;
#else
	return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t gama_blefob_disconnect(const char *reason)
{
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
	if (!s_initialized) {
		return ESP_ERR_INVALID_STATE;
	}
	ble_shell_close_pairing_window();
	if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
		return ESP_OK;
	}
	if (!ble_shell_stack_enter()) return ESP_ERR_INVALID_STATE;
	ble_shell_notify_log(reason != NULL ? reason : "ble disconnect requested");
	int rc = ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
	ble_shell_stack_exit();
	return rc == 0 ? ESP_OK : ESP_FAIL;
#else
	return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t gama_blefob_set_fault_latched(bool fault_latched)
{
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
	if (!s_initialized) {
		return ESP_ERR_INVALID_STATE;
	}
	s_fault_latched = fault_latched;
	if (!fault_latched) {
		s_fault_code = 0;
	}
	ble_shell_notify_status();
	return ESP_OK;
#else
	return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t gama_blefob_set_fault_code(uint32_t fault_code)
{
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
	if (!s_initialized) {
		return ESP_ERR_INVALID_STATE;
	}
	s_fault_code = fault_code;
	s_fault_latched = fault_code != 0;
	ble_shell_notify_status();
	return ESP_OK;
#else
	return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t gama_blefob_send_status(void)
{
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
	if (!s_initialized) {
		return ESP_ERR_INVALID_STATE;
	}
	ble_shell_notify_status();
	return ESP_OK;
#else
	return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t gama_blefob_log(const char *text)
{
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
	if (!s_initialized || text == NULL) {
		return ESP_ERR_INVALID_ARG;
	}
	ble_shell_notify_log(text);
	return ESP_OK;
#else
	return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t gama_blefob_respond(const char *text)
{
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
	if (!s_initialized || text == NULL) {
		return ESP_ERR_INVALID_ARG;
	}
	ble_shell_notify_response(text);
	return ESP_OK;
#else
	return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t gama_blefob_notify_binary(const uint8_t *data, size_t size)
{
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
	if (data == NULL || size == 0 || size > GAMA_BLEFOB_MAX_PAYLOAD) {
		return ESP_ERR_INVALID_ARG;
	}
	if (!s_initialized) {
		return ESP_ERR_INVALID_STATE;
	}
	return ble_shell_notify_binary(data, size);
#else
	(void)data;
	(void)size;
	return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t gama_blefob_is_connected(bool *connected)
{
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
	if (connected == NULL) {
		return ESP_ERR_INVALID_ARG;
	}
	if (!s_initialized) {
		return ESP_ERR_INVALID_STATE;
	}
	*connected = s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
	return ESP_OK;
#else
	return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t gama_blefob_is_advertising(bool *advertising)
{
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
	if (advertising == NULL) {
		return ESP_ERR_INVALID_ARG;
	}
	if (!s_initialized) {
		return ESP_ERR_INVALID_STATE;
	}
	if (!ble_shell_stack_enter()) { *advertising = false; return ESP_ERR_INVALID_STATE; }
	*advertising = s_advertising_window_open && ble_gap_adv_active();
	ble_shell_stack_exit();
	return ESP_OK;
#else
	return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t gama_blefob_register_binary_write_handler(gama_blefob_binary_write_handler_t handler,
                                                    void *user_ctx)
{
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
	if (handler == NULL) {
		return ESP_ERR_INVALID_ARG;
	}
	if (!s_initialized) {
		return ESP_ERR_INVALID_STATE;
	}
	s_app_cfg.binary_write_user_ctx = user_ctx;
	s_app_cfg.binary_write_handler = handler;
	return ESP_OK;
#else
	(void)handler;
	(void)user_ctx;
	return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t gama_blefob_unregister_binary_write_handler(void)
{
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
	if (!s_initialized) {
		return ESP_ERR_INVALID_STATE;
	}
	s_app_cfg.binary_write_handler = NULL;
	s_app_cfg.binary_write_user_ctx = NULL;
	return ESP_OK;
#else
	return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t gama_blefob_register_rf_button_handler(gama_blefob_rf_button_handler_t handler,
                                                 void *user_ctx)
{
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
	if (handler == NULL) {
		return ESP_ERR_INVALID_ARG;
	}
	if (!s_initialized) {
		return ESP_ERR_INVALID_STATE;
	}
	s_app_cfg.rf_button_handler = handler;
	s_app_cfg.rf_button_user_ctx = user_ctx;
	return ESP_OK;
#else
	return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t gama_blefob_unregister_rf_button_handler(void)
{
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
	if (!s_initialized) {
		return ESP_ERR_INVALID_STATE;
	}
	s_app_cfg.rf_button_handler = NULL;
	s_app_cfg.rf_button_user_ctx = NULL;
	return ESP_OK;
#else
	return ESP_ERR_NOT_SUPPORTED;
#endif
}

