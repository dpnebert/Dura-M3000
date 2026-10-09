#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "gama_blefob.h"

#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
#include "driver/gpio.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "os/os_mbuf.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"
#endif

#ifndef CONFIG_BLE_SHELL_SECURITY_PAIR_WINDOW_REQUIRED
#define CONFIG_BLE_SHELL_SECURITY_PAIR_WINDOW_REQUIRED 0
#endif
#ifndef CONFIG_BLE_SHELL_SECURITY_UNLOCK_COMMAND_REQUIRED
#define CONFIG_BLE_SHELL_SECURITY_UNLOCK_COMMAND_REQUIRED 0
#endif
#ifndef CONFIG_BLE_SHELL_UNLOCK_TOKEN
#define CONFIG_BLE_SHELL_UNLOCK_TOKEN ""
#endif

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
#define BLE_SHELL_NVS_NAMESPACE       "ble_shell"
#define BLE_SHELL_MAX_PAYLOAD         GAMA_BLEFOB_MAX_PAYLOAD
#define BLE_SHELL_TEXT_RESPONSE_CAPACITY GAMA_BLEFOB_TEXT_RESPONSE_CAPACITY
#define BLE_SHELL_REPLY_HEADER_SIZE 12U
#define BLE_SHELL_REPLY_META 0x01U
#define BLE_SHELL_REPLY_PAGE 0x02U
#define BLE_SHELL_REPLY_STALE 0x81U
#define BLE_SHELL_REPLY_OFFSET 0x82U
#define BLE_SHELL_REPLY_BAD_REQUEST 0x83U
#define BLE_SHELL_REPLY_OVERFLOW_JSON "{\"ok\":false,\"error\":\"response_overflow\",\"capacity\":8191}"
#define BLE_SHELL_EVENT_QUEUE_LEN     CONFIG_BLE_SHELL_EVENT_QUEUE_LEN
#define BLE_SHELL_DEFAULT_SERIAL      GAMA_BLEFOB_DEFAULT_SERIAL_ID
#define BLE_SHELL_STRING_FIELD_LEN    CONFIG_BLE_SHELL_STRING_FIELD_LEN
#define BLE_SHELL_JSON_ESCAPE_BUF_LEN CONFIG_BLE_SHELL_JSON_ESCAPE_BUF_LEN
#define BLE_SHELL_MAC_STRING_LEN      CONFIG_BLE_SHELL_MAC_STRING_LEN
#define BLE_SHELL_MAC_ADDR_LEN        6U
#define BLE_SHELL_ADV_STATUS_OK       0x00
#define BLE_SHELL_ADV_STATUS_FAULT    0x01
#define BLE_SHELL_ADV_STATUS_DIAG     0x02
#define BLE_SHELL_ADV_STATUS_UNLOCKED 0x04
#define BLE_SHELL_USECONDS_PER_SECOND 1000000LL
#define BLE_SHELL_USECONDS_PER_MS     1000LL
#define BLE_SHELL_PAIR_ADV_SECONDS_MIN 1U
#define BLE_SHELL_PAIR_ADV_SECONDS_MAX 600U
#define BLE_SHELL_CMD_PREFIX_LEN(prefix) (sizeof(prefix) - 1U)
#define BLE_SHELL_CMD_HAS_PREFIX(cmd, prefix) \
    (strncasecmp((cmd), (prefix), BLE_SHELL_CMD_PREFIX_LEN(prefix)) == 0)
#define BLE_SHELL_CMD_ARG(cmd, prefix) ((cmd) + BLE_SHELL_CMD_PREFIX_LEN(prefix))

/* Nordic UART UUIDs are stored least-significant byte first for NimBLE. */
#define BLE_SHELL_NUS_SERVICE_UUID BLE_UUID128_DECLARE( \
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, \
    0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E)
#define BLE_SHELL_NUS_COMMAND_UUID BLE_UUID128_DECLARE( \
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, \
    0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E)
#define BLE_SHELL_NUS_RESPONSE_UUID BLE_UUID128_DECLARE( \
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, \
    0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E)

#define BLE_SHELL_EVT_COMMAND         1
#define BLE_SHELL_EVT_BINARY          2

typedef struct {
    uint8_t type;
    uint16_t len;
    uint8_t data[BLE_SHELL_MAX_PAYLOAD];
} ble_shell_msg_t;

typedef struct {
    char device_name[BLE_SHELL_STRING_FIELD_LEN];
    char serial_id[BLE_SHELL_STRING_FIELD_LEN];
    bool ble_advertising_enabled;
    bool diagnostic_mode_enabled;
    bool ping_debug_enabled;
    uint32_t pair_window_seconds;
} ble_shell_config_t;

typedef struct {
    char device_name[GAMA_BLEFOB_MAX_NAME_LEN];
    char project_id[BLE_SHELL_STRING_FIELD_LEN];
    char module_name[BLE_SHELL_STRING_FIELD_LEN];
    char module_version[BLE_SHELL_STRING_FIELD_LEN];
    char firmware_version[BLE_SHELL_STRING_FIELD_LEN];
    char default_serial_id[BLE_SHELL_STRING_FIELD_LEN];
    uint32_t default_pair_window_seconds;
    bool advertise_on_boot;
    uint32_t shell_task_stack_size;
    bool init_nvs_if_needed;
    gama_blefob_security_mode_t security_mode;
    gama_blefob_command_handler_t command_handler;
    gama_blefob_status_handler_t status_handler;
    gama_blefob_connection_handler_t connected_handler;
    gama_blefob_connection_handler_t disconnected_handler;
    gama_blefob_connection_handler_t user_activity_handler;
    gama_blefob_binary_write_handler_t binary_write_handler;
    void *binary_write_user_ctx;
    gama_blefob_rf_button_handler_t rf_button_handler;
    void *rf_button_user_ctx;
    void *user_ctx;
} ble_shell_app_config_t;

extern const char *TAG;
extern QueueHandle_t s_ble_shell_queue;
extern uint8_t s_ble_addr_type;
extern uint16_t s_conn_handle;
extern uint16_t s_response_chr_val_handle;
extern ble_shell_config_t s_cfg;
extern ble_shell_app_config_t s_app_cfg;
extern bool s_initialized;
extern bool s_fault_latched;
extern uint32_t s_fault_code;
extern bool s_maintenance_unlocked;
extern bool s_ble_synced;
extern bool s_response_notify_active;
extern bool s_advertising_window_open;
extern int64_t s_adv_deadline_us;
extern char s_last_response[BLE_SHELL_TEXT_RESPONSE_CAPACITY];
extern char s_last_status[BLE_SHELL_MAX_PAYLOAD];
extern char s_last_log[BLE_SHELL_MAX_PAYLOAD];
extern char s_last_diagnostic[BLE_SHELL_MAX_PAYLOAD];
extern const struct ble_gatt_svc_def gatt_svcs[];

/* Short leases fence stack calls from tasks/callbacks against sleep teardown.
 * Never hold a spinlock across a NimBLE/RTOS operation. */
bool ble_shell_stack_enter(void);
void ble_shell_stack_exit(void);
void ble_shell_sleep_synced(void);

void ble_store_config_init(void);

void str_trim(char *s);
uint32_t ble_shell_uptime_s(void);
void ble_shell_get_mac_string(char *out, size_t out_len);
uint8_t ble_shell_adv_status_byte(void);
const char *ble_shell_nonempty(const char *text, const char *fallback);
void ble_shell_json_escape(const char *src, char *dst, size_t dst_len);
void ble_shell_parse_semver(const char *text, uint8_t *major, uint8_t *minor, uint8_t *patch);
void ble_shell_set_bool_from_arg(const char *arg, bool *dst);
esp_err_t ble_shell_parse_positive_u32(const char *text, uint32_t *value);

void ble_shell_apply_app_config(const gama_blefob_config_t *config);
void ble_shell_config_defaults(void);
void ble_shell_config_load(void);
esp_err_t ble_shell_config_save(void);
esp_err_t ble_shell_config_factory_reset(void);

void ble_shell_make_info_json(char *out, size_t out_len);
void ble_shell_make_status_json(char *out, size_t out_len);
void ble_shell_make_config_json(char *out, size_t out_len);
void ble_shell_make_diagnostic_json(char *out, size_t out_len);

void ble_shell_notify_response(const char *text);
/* Reserved generic fetch; true means handled (including malformed arguments).
 * Never publishes a new response, calls the app, or marks user activity. */
bool ble_shell_reply_command(const char *cmd);
int ble_shell_reply_read(uint16_t conn_handle, struct os_mbuf *om);
esp_err_t ble_shell_notify_binary(const uint8_t *data, size_t size);
void ble_shell_replay_binary(void);
void ble_shell_notify_status(void);
void ble_shell_notify_log(const char *text);
void ble_shell_notify_diagnostic(const char *text);

void ble_advertise_start_remaining(void);
void ble_shell_open_pairing_window(uint32_t seconds, const char *reason);
void ble_shell_close_pairing_window(void);
void ble_on_sync(void);
void ble_on_reset(int reason);

void ble_shell_task(void *arg);
void pair_button_task(void *arg);
#endif
