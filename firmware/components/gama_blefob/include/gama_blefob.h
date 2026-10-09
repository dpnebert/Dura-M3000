#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CONFIG_BLE_SHELL_MAX_PAYLOAD
#define CONFIG_BLE_SHELL_MAX_PAYLOAD 256
#endif
#ifndef CONFIG_BLE_SHELL_MAX_NAME_LEN
#define CONFIG_BLE_SHELL_MAX_NAME_LEN 32
#endif
#ifndef CONFIG_BLE_SHELL_DEFAULT_SERIAL_ID
#define CONFIG_BLE_SHELL_DEFAULT_SERIAL_ID "UNSET"
#endif
#ifndef CONFIG_BLE_SHELL_STATUS_FAULT_MASK
#define CONFIG_BLE_SHELL_STATUS_FAULT_MASK 0x01
#endif
#ifndef CONFIG_BLE_SHELL_STATUS_DIAGNOSTIC_MASK
#define CONFIG_BLE_SHELL_STATUS_DIAGNOSTIC_MASK 0x02
#endif
#ifndef CONFIG_BLE_SHELL_STATUS_UNLOCKED_MASK
#define CONFIG_BLE_SHELL_STATUS_UNLOCKED_MASK 0x04
#endif

#define GAMA_BLEFOB_MAX_PAYLOAD       CONFIG_BLE_SHELL_MAX_PAYLOAD
/* Text-only capacity includes NUL. Raw writes, binary snapshots and the event
 * queue retain GAMA_BLEFOB_MAX_PAYLOAD; this is not an ATT value length. */
#define GAMA_BLEFOB_TEXT_RESPONSE_CAPACITY 8192U
#define GAMA_BLEFOB_MAX_NAME_LEN      CONFIG_BLE_SHELL_MAX_NAME_LEN
#define GAMA_BLEFOB_DEFAULT_SERIAL_ID CONFIG_BLE_SHELL_DEFAULT_SERIAL_ID

#define GAMA_BLEFOB_STATUS_FAULT      CONFIG_BLE_SHELL_STATUS_FAULT_MASK
#define GAMA_BLEFOB_STATUS_DIAGNOSTIC CONFIG_BLE_SHELL_STATUS_DIAGNOSTIC_MASK
#define GAMA_BLEFOB_STATUS_UNLOCKED   CONFIG_BLE_SHELL_STATUS_UNLOCKED_MASK

typedef esp_err_t (*gama_blefob_command_handler_t)(const char *command,
                                                   char *response,
                                                   size_t response_len,
                                                   void *user_ctx);

/**
 * Handles one complete binary value written to the command characteristic.
 * The data pointer is valid only for the duration of the callback.
 */
typedef esp_err_t (*gama_blefob_binary_write_handler_t)(const uint8_t *data,
                                                       size_t size,
                                                       void *user_ctx);

typedef esp_err_t (*gama_blefob_status_handler_t)(char *json,
                                                  size_t json_len,
                                                  void *user_ctx);

typedef esp_err_t (*gama_blefob_rf_button_handler_t)(uint32_t button,
                                                     bool active,
                                                     void *user_ctx);

typedef void (*gama_blefob_connection_handler_t)(void *user_ctx);

typedef enum {
    GAMA_BLEFOB_SECURITY_OPEN = 0,
    GAMA_BLEFOB_SECURITY_PAIR_REQUIRED,
    GAMA_BLEFOB_SECURITY_UNLOCK_COMMAND_REQUIRED,
} gama_blefob_security_mode_t;

typedef struct {
    const char *device_name;
    const char *project_id;
    const char *module_name;
    const char *module_version;
    const char *firmware_version;
    const char *default_serial_id;
    uint32_t default_pair_window_seconds;
    bool advertise_on_boot;
    uint32_t shell_task_stack_size;
    bool init_nvs_if_needed;
    gama_blefob_security_mode_t security_mode;
    gama_blefob_command_handler_t command_handler;
    gama_blefob_binary_write_handler_t binary_write_handler;
    void *binary_write_user_ctx;
    gama_blefob_rf_button_handler_t rf_button_handler;
    void *rf_button_user_ctx;
    gama_blefob_status_handler_t status_handler;
    gama_blefob_connection_handler_t connected_handler;
    gama_blefob_connection_handler_t disconnected_handler;
    /* Accepted generic state-changing command only; never connect/poll/ping.
     * App commands and RF presses own their acceptance/activity reporting. */
    gama_blefob_connection_handler_t user_activity_handler;
    void *user_ctx;
} gama_blefob_config_t;

void gama_blefob_config_init_default(gama_blefob_config_t *config);

esp_err_t gama_blefob_init(const gama_blefob_config_t *config);
esp_err_t gama_blefob_init_default(void);

/** Board-owner only, serialized pair. Established links or in-flight stack
 * users veto admission. A new link racing the disconnected cutoff may be lost.
 * Shutdown returns OK only after normal NimBLE stop/host exit/deinit.
 * Always call resume on an aborted/rejected sleep, even if shutdown failed.
 * Unknown/partial SDK failures stay fenced and resume returns an error;
 * callers must keep outputs OFF and activation fenced in that case.
 * Neither function may be called from the NimBLE host or a BLE command callback.
 * Compiled-out BLE is a successful no-op. */
esp_err_t gama_blefob_sleep_shutdown(void);
esp_err_t gama_blefob_sleep_resume(void);

esp_err_t gama_blefob_open_pairing_window(uint32_t seconds);
esp_err_t gama_blefob_close_pairing_window(void);
esp_err_t gama_blefob_disconnect(const char *reason);
esp_err_t gama_blefob_set_fault_latched(bool fault_latched);
esp_err_t gama_blefob_set_fault_code(uint32_t fault_code);
esp_err_t gama_blefob_send_status(void);
esp_err_t gama_blefob_log(const char *text);
esp_err_t gama_blefob_respond(const char *text);
/** Dispatch NUL-terminated text synchronously from the BLE shell task's binary
 * write callback. Generic commands run first, then the registered app fallback.
 * Responses/status notifications are emitted by the shell; ESP_OK means the
 * command was dispatched, not that its reported application result succeeded.
 * Not a cross-task submission API; do not call recursively from app fallback. */
esp_err_t gama_blefob_dispatch_text(const char *command);
esp_err_t gama_blefob_notify_binary(const uint8_t *data, size_t size);
esp_err_t gama_blefob_is_connected(bool *connected);
esp_err_t gama_blefob_is_advertising(bool *advertising);
esp_err_t gama_blefob_register_binary_write_handler(gama_blefob_binary_write_handler_t handler,
                                                    void *user_ctx);
esp_err_t gama_blefob_unregister_binary_write_handler(void);
esp_err_t gama_blefob_register_rf_button_handler(gama_blefob_rf_button_handler_t handler,
                                                 void *user_ctx);
esp_err_t gama_blefob_unregister_rf_button_handler(void);

#ifdef __cplusplus
}
#endif
