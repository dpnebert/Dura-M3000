#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "dura_board.h"
#include "dura_ui_config.h"
#include "dura_coordinator.h"
#include "dura_legacy_protocol.h"
#include "dura_meter.h"
#include "dura_peers.h"
#include "dura_recipe.h"
#include "gama_blefob.h"

static const char *TAG = "dura_m3000";

#ifndef CONFIG_DURA_SERIAL_CONSOLE_STACK_SIZE
#define CONFIG_DURA_SERIAL_CONSOLE_STACK_SIZE 8192
#endif

#ifndef CONFIG_DURA_APP_COMMAND_STACK_SIZE
#define CONFIG_DURA_APP_COMMAND_STACK_SIZE 16384
#endif

#if CONFIG_DURA_APP_QEMU_NO_RADIO
#define DURA_SERIAL_CONSOLE_STACK_SIZE 12288
#define DURA_APP_COMMAND_STACK_SIZE 16384
#else
#define DURA_SERIAL_CONSOLE_STACK_SIZE CONFIG_DURA_SERIAL_CONSOLE_STACK_SIZE
#define DURA_APP_COMMAND_STACK_SIZE CONFIG_DURA_APP_COMMAND_STACK_SIZE
#endif

#define DURA_APP_COMMAND_QUEUE_LEN 8
#define DURA_APP_COMMAND_MAX_LEN 128
#define DURA_APP_RESPONSE_MAX_LEN 8192
#include "dura_json.h"
#define DURA_APP_COMMAND_TIMEOUT_MS 5000
#define DURA_PEER_DISCOVERY_SETTLE_MS 350

typedef enum {
    DURA_COMMAND_SOURCE_UART = 0,
    DURA_COMMAND_SOURCE_BLE,
} dura_command_source_t;

typedef struct {
    uint32_t request_id;
    esp_err_t err;
    char response[DURA_APP_RESPONSE_MAX_LEN];
} dura_command_reply_t;

typedef struct {
    dura_command_source_t source;
    uint32_t request_id;
    char command[DURA_APP_COMMAND_MAX_LEN];
} dura_command_request_t;

static QueueHandle_t s_app_command_queue;
static QueueHandle_t s_app_command_reply_queue;
static SemaphoreHandle_t s_app_command_submit_mutex;
static uint32_t s_app_command_next_request_id;
static bool s_local_ble_gateway_active;
static uint32_t s_local_ble_gateway_claim_id;
static char s_assigned_system_id[32] = "unassigned";
static char s_assigned_system_name[32] = "unassigned";
static void refresh_peer_table_before_command(const char *reason);
static esp_err_t dura_ble_command_handler(const char *command, char *response,
                                          size_t response_len, void *user_ctx);

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static void load_system_assignment(void)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("dura_app", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return;
    }
    size_t id_len = sizeof(s_assigned_system_id);
    size_t name_len = sizeof(s_assigned_system_name);
    if (nvs_get_str(nvs, "system_id", s_assigned_system_id, &id_len) != ESP_OK || s_assigned_system_id[0] == '\0') {
        strlcpy(s_assigned_system_id, "unassigned", sizeof(s_assigned_system_id));
    }
    if (nvs_get_str(nvs, "system_name", s_assigned_system_name, &name_len) != ESP_OK || s_assigned_system_name[0] == '\0') {
        strlcpy(s_assigned_system_name, "unassigned", sizeof(s_assigned_system_name));
    }
    nvs_close(nvs);
}

static esp_err_t save_system_assignment(const char *system_id, const char *system_name)
{
    ESP_RETURN_ON_FALSE(system_id != NULL && system_id[0] != '\0' && strlen(system_id) < sizeof(s_assigned_system_id),
                        ESP_ERR_INVALID_ARG, TAG, "bad system id");
    ESP_RETURN_ON_FALSE(system_name != NULL && system_name[0] != '\0' && strlen(system_name) < sizeof(s_assigned_system_name),
                        ESP_ERR_INVALID_ARG, TAG, "bad system name");
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("dura_app", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(nvs, "system_id", system_id);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "system_name", system_name);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        strlcpy(s_assigned_system_id, system_id, sizeof(s_assigned_system_id));
        strlcpy(s_assigned_system_name, system_name, sizeof(s_assigned_system_name));
    }
    return err;
}

static esp_err_t clear_system_assignment(void)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("dura_app", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        (void)nvs_erase_key(nvs, "system_id");
        (void)nvs_erase_key(nvs, "system_name");
        err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    strlcpy(s_assigned_system_id, "unassigned", sizeof(s_assigned_system_id));
    strlcpy(s_assigned_system_name, "unassigned", sizeof(s_assigned_system_name));
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

static bool system_assignment_configured(void)
{
    return s_assigned_system_id[0] != '\0' && strcmp(s_assigned_system_id, "unassigned") != 0;
}

static void log_heap_state(const char *label)
{
    ESP_LOGI(TAG, "heap %s: free=%lu min=%lu internal_free=%lu internal_largest=%lu",
             label != NULL ? label : "-",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)esp_get_minimum_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}


static void log_module_info(void)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    char elf_sha256[65] = {0};
    esp_app_get_elf_sha256(elf_sha256, sizeof(elf_sha256));
    esp_chip_info_t chip_info;
    uint32_t flash_size = 0;
    uint8_t mac[6] = {0};

    esp_chip_info(&chip_info);
    (void)esp_flash_get_size(NULL, &flash_size);
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);

    ESP_LOGI(TAG, "Dura M3000 firmware %s", DURA_METER_FIRMWARE_VERSION);
    ESP_LOGI(TAG, "HANDOFF_WATERMARK=%s ELF_SHA256=%s", app_desc->version, elf_sha256);
    ESP_LOGI(TAG, "ESP-IDF: %s", esp_get_idf_version());
    ESP_LOGI(TAG, "SoC: %s rev %d.%d, cores=%d", CONFIG_IDF_TARGET,
             chip_info.revision / 100, chip_info.revision % 100, chip_info.cores);
    ESP_LOGI(TAG, "Flash: %lu MB", (unsigned long)(flash_size / (1024U * 1024U)));
    ESP_LOGI(TAG, "Free heap: %lu, min free heap: %lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)esp_get_minimum_free_heap_size());
    ESP_LOGI(TAG, "STA MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "Reset reason: %d", esp_reset_reason());
    uint32_t wakes = esp_sleep_get_wakeup_causes();
    ESP_LOGI(TAG, "Wake causes: 0x%lx", (unsigned long)wakes);
    if (wakes & BIT(ESP_SLEEP_WAKEUP_EXT1)) {
        const uint64_t ext1_status = esp_sleep_get_ext1_wakeup_status();
        const dura_wake_group_t wake_group = dura_board_wake_group_from_ext1_status(ext1_status);
        ESP_LOGI(TAG, "EXT1 wake status: 0x%llx group=%s",
                 (unsigned long long)ext1_status,
                 dura_board_wake_group_name(wake_group));
    }
}

static void skip_spaces(const char **cursor)
{
    while (cursor != NULL && *cursor != NULL && isspace((unsigned char)**cursor)) {
        (*cursor)++;
    }
}

static const char *command_args(const char *command, const char *name)
{
    size_t name_len = strlen(name);
    if (strncmp(command, name, name_len) != 0) {
        return NULL;
    }
    if (command[name_len] == '\0') {
        return command + name_len;
    }
    if (!isspace((unsigned char)command[name_len])) {
        return NULL;
    }
    return command + name_len + 1;
}

static bool no_extra_args(const char *cursor)
{
    skip_spaces(&cursor);
    return cursor != NULL && *cursor == '\0';
}

static bool parse_string_arg(const char **cursor, char *value, size_t value_len)
{
    if (cursor == NULL || *cursor == NULL || value == NULL || value_len == 0) {
        return false;
    }
    skip_spaces(cursor);
    if (**cursor == '\0') {
        return false;
    }

    const char *start = *cursor;
    size_t len = 0;
    while ((*cursor)[len] != '\0' && !isspace((unsigned char)(*cursor)[len])) {
        len++;
    }
    if (len == 0 || len >= value_len) {
        return false;
    }
    memcpy(value, start, len);
    value[len] = '\0';
    *cursor = start + len;
    return true;
}

static bool parse_float_arg(const char **cursor, float *value)
{
    char token[32] = {0};
    if (!parse_string_arg(cursor, token, sizeof(token)) || value == NULL) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    float parsed = strtof(token, &end);
    if (errno != 0 || end == token || *end != '\0' || !isfinite(parsed)) {
        return false;
    }
    *value = parsed;
    return true;
}

static bool parse_uint_arg(const char **cursor, uint32_t *value, int base)
{
    char token[32] = {0};
    if (!parse_string_arg(cursor, token, sizeof(token)) || value == NULL || token[0] == '-') {
        return false;
    }
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(token, &end, base);
    if (errno != 0 || end == token || *end != '\0' || parsed > UINT32_MAX) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static bool parse_bool_arg(const char **cursor, bool *value)
{
    char token[16] = {0};
    if (!parse_string_arg(cursor, token, sizeof(token)) || value == NULL) {
        return false;
    }
    if (strcasecmp(token, "1") == 0 || strcasecmp(token, "on") == 0 ||
        strcasecmp(token, "true") == 0 || strcasecmp(token, "yes") == 0) {
        *value = true;
        return true;
    }
    if (strcasecmp(token, "0") == 0 || strcasecmp(token, "off") == 0 ||
        strcasecmp(token, "false") == 0 || strcasecmp(token, "no") == 0) {
        *value = false;
        return true;
    }
    return false;
}

static bool parse_rest_arg(const char **cursor, char *value, size_t value_len)
{
    if (cursor == NULL || *cursor == NULL || value == NULL || value_len == 0) {
        return false;
    }
    skip_spaces(cursor);
    const char *start = *cursor;
    if (*start == '\0') {
        return false;
    }
    const char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    size_t len = (size_t)(end - start);
    if (len == 0 || len >= value_len) {
        return false;
    }
    memcpy(value, start, len);
    value[len] = '\0';
    *cursor = end;
    return true;
}


static const char *dura_error_code_from_message(const char *message)
{
    if (message == NULL) {
        return "UNKNOWN";
    }
    if (strstr(message, "unknown command") != NULL) return "UNKNOWN_COMMAND";
    if (strstr(message, "not the active BLE gateway") != NULL) return "NOT_ACTIVE_BLE_GATEWAY";
    if (strstr(message, "requires") != NULL || strstr(message, "expects") != NULL ||
        strstr(message, "must be") != NULL || strstr(message, "trailing garbage") != NULL) return "INVALID_ARGUMENT";
    if (strstr(message, "not found") != NULL) return "NOT_FOUND";
    if (strstr(message, "prerequisites not met") != NULL) return "PRECONDITION_FAILED";
    if (strstr(message, "already active") != NULL || strstr(message, "not ready") != NULL) return "INVALID_STATE";
    if (strstr(message, "timeout") != NULL) return "TIMEOUT";
    if (strstr(message, "alloc") != NULL || strstr(message, "No memory") != NULL) return "NO_MEM";
    return "COMMAND_FAILED";
}

static esp_err_t format_app_ok_json(const char *command, const char *message, char *json, size_t json_len)
{
    ESP_RETURN_ON_FALSE(command != NULL && json != NULL && json_len > 0, ESP_ERR_INVALID_ARG, TAG, "bad app ok json args");
    int written = dura_json_snprintf(json, json_len,
                           "{\"ok\":true,\"command\":\"%s\",\"message\":\"%s\"}",
                           command, message != NULL ? message : "ok");
    return dura_json_result(json, json_len, written);
}

static esp_err_t format_app_error_json(const char *command, const char *code, const char *message, esp_err_t esp_err_code, char *json, size_t json_len)
{
    ESP_RETURN_ON_FALSE(command != NULL && json != NULL && json_len > 0, ESP_ERR_INVALID_ARG, TAG, "bad app error json args");
    int written = dura_json_snprintf(json, json_len,
                           "{\"ok\":false,\"command\":\"%s\",\"error\":{\"code\":\"%s\",\"message\":\"%s\",\"esp_err\":\"0x%x\"}}",
                           command,
                           code != NULL ? code : "COMMAND_FAILED",
                           message != NULL ? message : "command failed",
                           esp_err_code);
    return dura_json_result(json, json_len, written);
}

static bool response_is_plain_error(const char *response)
{
    return response != NULL && strncmp(response, "error:", 6) == 0;
}

static bool response_is_plain_ok(const char *response)
{
    return response != NULL && strncmp(response, "ok:", 3) == 0;
}

static void wrap_ble_app_response(const char *command, char *response, size_t response_len, esp_err_t err)
{
    if (response == NULL || response_len == 0) return;
    if ((response[0] == '{' || response[0] == '[' || response[0] == '\0') && err != ESP_OK) {
        (void)dura_json_fail(response, response_len, err == ESP_ERR_NO_MEM ? -1 : -2);
        return;
    }
    if (response[0] == '\0' || response[0] == '{' || response[0] == '[') return;
    size_t original_len = strlen(response);
    char *original = malloc(original_len + 1);
    if (original == NULL) { (void)dura_json_fail(response, response_len, -1); return; }
    memcpy(original, response, original_len + 1);
    if (response_is_plain_error(original) || err != ESP_OK) {
        const char *message = response_is_plain_error(original) ? original + 6 : original;
        if (*message == ' ') ++message;
        (void)format_app_error_json(command, dura_error_code_from_message(original), message, err, response, response_len);
    } else if (response_is_plain_ok(original)) {
        const char *message = original + 3;
        if (*message == ' ') ++message;
        (void)format_app_ok_json(command, message, response, response_len);
    }
    free(original);
}

static bool local_liquid_is_configured(const dura_liquid_config_t *liquid)
{
    return liquid != NULL && liquid->liquid_name[0] != '\0' &&
           strcasecmp(liquid->liquid_name, "unassigned") != 0 &&
           liquid->viscosity_cP > 0.0f;
}

static esp_err_t format_provision_info_json(char *json, size_t json_len)
{
    dura_liquid_config_t liquid = {0};
    dura_peers_snapshot_t peers = {0};

    ESP_RETURN_ON_FALSE(json != NULL && json_len > 0, ESP_ERR_INVALID_ARG, TAG, "bad json buffer");
    ESP_RETURN_ON_ERROR(dura_recipe_get_local_liquid(&liquid), TAG, "get local liquid failed");
    ESP_RETURN_ON_ERROR(dura_peers_get_snapshot(&peers), TAG, "get peer snapshot failed");

    const bool configured = local_liquid_is_configured(&liquid);
    int written = dura_json_snprintf(json, json_len,
                           "{\"provisioning\":{\"state\":\"%s\",\"standalone_ready\":true,\"identity_configured\":%s,\"network_node_ready\":false,\"qr_dismiss_behavior\":\"any_button_to_home\",\"qr_payload_version\":1,\"qr_payload\":\"dura://claim?model=dura_m3000&mac=%02X%02X%02X%02X%02X%02X&fw=%s\",\"model\":\"dura_m3000\",\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"firmware\":\"%s\",\"local_liquid\":\"%s\",\"local_liquid_id\":%lu,\"assigned_system\":{\"id\":\"%s\",\"name\":\"%s\",\"configured\":%s},\"app_owned_fields\":[\"system_id\",\"system_name\",\"device_name\",\"liquid_profile_id\",\"calibration_profile\"],\"recipe_scope\":\"local_meter\",\"device_button_setup_required\":false}}",
                           configured ? "configured" : "unprovisioned",
                           configured ? "true" : "false",
                           peers.local_mac[0], peers.local_mac[1], peers.local_mac[2],
                           peers.local_mac[3], peers.local_mac[4], peers.local_mac[5],
                           DURA_METER_FIRMWARE_VERSION,
                           peers.local_mac[0], peers.local_mac[1], peers.local_mac[2],
                           peers.local_mac[3], peers.local_mac[4], peers.local_mac[5],
                           DURA_METER_FIRMWARE_VERSION,
                           liquid.liquid_name,
                           (unsigned long)liquid.calibration_id,
                           s_assigned_system_id,
                           s_assigned_system_name,
                           system_assignment_configured() ? "true" : "false");
    return dura_json_result(json, json_len, written);
}

static esp_err_t format_whoami_json(char *json, size_t json_len)
{
    dura_liquid_config_t liquid = {0};
    dura_peers_snapshot_t peers = {0};
    dura_coordinator_snapshot_t coordinator = {0};

    ESP_RETURN_ON_FALSE(json != NULL && json_len > 0, ESP_ERR_INVALID_ARG, TAG, "bad json buffer");
    ESP_RETURN_ON_ERROR(dura_recipe_get_local_liquid(&liquid), TAG, "get local liquid failed");
    ESP_RETURN_ON_ERROR(dura_peers_get_snapshot(&peers), TAG, "get peer snapshot failed");
    ESP_RETURN_ON_ERROR(dura_coordinator_get_snapshot(&coordinator), TAG, "get coordinator snapshot failed");

    const bool configured = local_liquid_is_configured(&liquid);
    const bool network_node_ready = false;
    int written = dura_json_snprintf(json, json_len,
                           "{\"firmware\":\"%s\",\"local_liquid\":\"%s\",\"local_liquid_id\":%lu,\"viscosity_cP\":%.2f,\"power_source\":\"%s\",\"external_power_present\":%s,\"ble_requires_external_power\":%s,\"boost\":{\"enable_source\":\"hardwired_3v3_digital\",\"enabled\":%s,\"controlled\":false},\"provisioning_state\":\"%s\",\"standalone_ready\":true,\"identity_configured\":%s,\"network_node_ready\":%s,\"device_button_setup_required\":false,\"local_mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"ble_gateway\":{\"active\":%s,\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"claim_id\":%lu,\"local\":%s},\"assigned_system\":{\"id\":\"%s\",\"name\":\"%s\",\"configured\":%s},\"peer_count\":%u,\"coordinator_state\":\"%s\",\"active_recipe\":\"%s\"}",
                           DURA_METER_FIRMWARE_VERSION,
                           liquid.liquid_name,
                           (unsigned long)liquid.calibration_id,
                           (double)liquid.viscosity_cP,
                           dura_board_power_source_name(),
                           dura_board_ext_power_present() ? "true" : "false",
                           CONFIG_DURA_BOARD_BLE_REQUIRES_EXT_POWER ? "true" : "false",
                           dura_board_boost_enabled() ? "true" : "false",
                           configured ? "configured" : "unprovisioned",
                           configured ? "true" : "false",
                           network_node_ready ? "true" : "false",
                           peers.local_mac[0], peers.local_mac[1], peers.local_mac[2],
                           peers.local_mac[3], peers.local_mac[4], peers.local_mac[5],
                           peers.ble_gateway_active ? "true" : "false",
                           peers.ble_gateway_mac[0], peers.ble_gateway_mac[1], peers.ble_gateway_mac[2],
                           peers.ble_gateway_mac[3], peers.ble_gateway_mac[4], peers.ble_gateway_mac[5],
                           (unsigned long)peers.ble_gateway_claim_id,
                           peers.ble_gateway_active && memcmp(peers.ble_gateway_mac, peers.local_mac, sizeof(peers.local_mac)) == 0 ? "true" : "false",
                           s_assigned_system_id,
                           s_assigned_system_name,
                           system_assignment_configured() ? "true" : "false",
                           (unsigned)peers.peer_count,
                           dura_coordinator_state_name(coordinator.state),
                           coordinator.active_recipe);
    return dura_json_result(json, json_len, written);
}

static esp_err_t format_sanity_check_json(uint32_t min_peers, char *json, size_t json_len)
{
    dura_liquid_config_t liquid = {0};
    dura_peers_snapshot_t peers = {0};
    dura_coordinator_snapshot_t coordinator = {0};
    dura_meter_snapshot_t meter = {0};

    ESP_RETURN_ON_FALSE(json != NULL && json_len > 0, ESP_ERR_INVALID_ARG, TAG, "bad json buffer");
    ESP_RETURN_ON_ERROR(dura_recipe_get_local_liquid(&liquid), TAG, "get local liquid failed");
    ESP_RETURN_ON_ERROR(dura_peers_get_snapshot(&peers), TAG, "get peer snapshot failed");
    ESP_RETURN_ON_ERROR(dura_coordinator_get_snapshot(&coordinator), TAG, "get coordinator snapshot failed");
    ESP_RETURN_ON_ERROR(dura_meter_get_snapshot(&meter), TAG, "get meter snapshot failed");

    const bool local_identity_configured = local_liquid_is_configured(&liquid);
    const bool now_network_required = min_peers > 0U;
    const bool local_identity_ok = !now_network_required || local_identity_configured;
    const bool peers_ok = !now_network_required ||
                          (peers.esp_now_ready && peers.peer_count >= (size_t)min_peers);
    const bool coordinator_ok = coordinator.state == DURA_COORDINATOR_IDLE && !coordinator.leader_active && coordinator.active_recipe[0] == '\0';
    const bool output_safe = !meter.pump_enabled && meter.batch_mode != DURA_BATCH_RUNNING;
    const bool pass = local_identity_ok && peers_ok && coordinator_ok && output_safe;

    int written = dura_json_snprintf(json, json_len,
                           "{\"sanity_check\":{\"status\":\"%s\",\"min_peers\":%lu,\"peer_network_required\":%s,\"peer_radio_ready\":%s,\"peer_count\":%u,\"local_liquid\":\"%s\",\"coordinator_state\":\"%s\",\"leader_active\":%s,\"active_recipe\":\"%s\",\"pump_enabled\":%s,\"checks\":{\"local_identity_configured\":%s,\"local_identity_required\":%s,\"local_identity\":%s,\"peers\":%s,\"coordinator_idle\":%s,\"output_safe\":%s}}}",
                           pass ? "PASS" : "FAIL",
                           (unsigned long)min_peers,
                           now_network_required ? "true" : "false",
                           peers.esp_now_ready ? "true" : "false",
                           (unsigned)peers.peer_count,
                           liquid.liquid_name,
                           dura_coordinator_state_name(coordinator.state),
                           coordinator.leader_active ? "true" : "false",
                           coordinator.active_recipe,
                           meter.pump_enabled ? "true" : "false",
                           local_identity_configured ? "true" : "false",
                           now_network_required ? "true" : "false",
                           local_identity_ok ? "true" : "false",
                           peers_ok ? "true" : "false",
                           coordinator_ok ? "true" : "false",
                           output_safe ? "true" : "false");
    return dura_json_result(json, json_len, written);
}

static esp_err_t format_feature_matrix_json(char *json, size_t json_len)
{
    ESP_RETURN_ON_FALSE(json != NULL && json_len > 0, ESP_ERR_INVALID_ARG, TAG, "bad json buffer");

    int written = dura_json_snprintf(json, json_len,
                           "{\"feature_matrix\":{"
                           "\"design\":\"one configurable meter controller\","
                           "\"capabilities\":["
                           "\"standalone_meter\","
                           "\"flow_a_b_counting\","
                           "\"batch_dispense\","
                           "\"calibration\","
                           "\"local_output_gate\","
                           "\"field_sense_reserved\","
                           "\"flow_ext_pulse_input_reserved\","
                           "\"aux_abs_legacy_if_reserved\","
                           "\"fault_shutdown\","
                           "\"lcd_128x64_ui\","
                           "\"four_key_ui\","
                           "\"nvs_persistence\","
                           "\"uart_service_console\","
                           "\"ble_config_shell\","
                           "\"local_recipe_batch\","
                           "\"rf_event_input_ready\"],"
                           "\"use_cases\":["
                           "\"legacy_single_meter\","
                           "\"multi_meter_recipe\","
                           "\"service_config_tool\","
                           "\"production_test_fixture\","
                           "\"remote_rf_variant\","
                           "\"dpm2000_external_flow_sensor\","
                           "\"abs_9v_dc_aux_interface\","
                           "\"customer_ui_variant\"],"
                           "\"rule\":\"transports_create_events;meter_state_owns_outputs\"}}"
                           );
    return dura_json_result(json, json_len, written);
}

static void append_test_failure(char *failures, size_t failures_len, const char *test_id)
{
    if (failures_len == 0 || test_id == NULL || test_id[0] == '\0') {
        return;
    }
    size_t used = strlen(failures);
    if (used >= failures_len - 1) {
        return;
    }
    int written = snprintf(failures + used, failures_len - used, "%s%s", used > 0 ? "," : "", test_id);
    (void)written;
}

#define DURA_SELF_TEST_CHECK(TEST_ID, EXPR) \
    do { \
        total++; \
        if (EXPR) { \
            passed++; \
        } else { \
            append_test_failure(failures, sizeof(failures), TEST_ID); \
        } \
    } while (0)

static esp_err_t run_self_tests(uint32_t meter_count, char *response, size_t response_len)
{
    if (meter_count == 0 || meter_count > (DURA_PEER_MAX_PEERS + 1U)) {
        snprintf(response, response_len, "error: run_tests expects meter_count 1..%u", (unsigned)(DURA_PEER_MAX_PEERS + 1U));
        return ESP_OK;
    }

    uint32_t total = 0;
    uint32_t passed = 0;
    char failures[256] = {0};
    esp_err_t err = ESP_OK;
    dura_meter_snapshot_t meter = {0};
    dura_meter_snapshot_t before = {0};
    dura_liquid_config_t liquid = {0};
    dura_recipe_t recipe = {0};
    dura_peers_snapshot_t peers = {0};
    const char *target_liquid = "unassigned";
    bool peer_recipe_started = false;

    ESP_LOGI(TAG, "self-test start: meters_expected=%lu", (unsigned long)meter_count);
    ESP_LOGI(TAG, "[selftest-expected] negative recipe/calibration/cleanup checks are intentionally exercised during this run");
    dura_meter_set_log_context("selftest-expected");
    dura_coordinator_set_log_context("selftest-expected");

    (void)dura_coordinator_abort();
    err = dura_meter_factory_defaults();
    DURA_SELF_TEST_CHECK("MTR-001", err == ESP_OK && dura_meter_get_snapshot(&meter) == ESP_OK &&
                         meter.screen == DURA_SCREEN_HOME && meter.selected_units == DURA_UNITS_GALLON &&
                         !meter.pump_enabled && !meter.fault_latched && meter.meter_total_counts == 0 &&
                         meter.batch_total_counts == 0 && meter.total_total_counts == 0 && meter.odometer_counts == 0);

    DURA_SELF_TEST_CHECK("CFG-DEFAULT", meter.meter_timeout_sec == 20u &&
                         meter.backlight_timeout_sec == 10u &&
                         meter.selected_language == DURA_LANGUAGE_ENGLISH && !meter.reset_to_zero &&
                         meter.selected_precal_profile == 0u && meter.quick_cal_reference == 17u &&
                         meter.precal_profiles[0].gallon_per_count > 0.0f &&
                         meter.precal_profiles[1].gallon_per_count == 0.0f);
    err = dura_meter_set_meter_timeout(19u);
    DURA_SELF_TEST_CHECK("CFG-RANGE-METER", err == ESP_ERR_INVALID_ARG);
    err = dura_meter_set_backlight_timeout(8u);
    DURA_SELF_TEST_CHECK("CFG-RANGE-BACKLIGHT", err == ESP_ERR_INVALID_ARG);
    err = dura_meter_set_language((dura_language_t)3);
    DURA_SELF_TEST_CHECK("CFG-RANGE-LANGUAGE", err == ESP_ERR_INVALID_ARG);
    err = dura_meter_apply_quick_cal(22u);
    DURA_SELF_TEST_CHECK("CFG-RANGE-QUICK", err == ESP_ERR_INVALID_ARG);
    err = dura_meter_select_precal_profile(1u);
    DURA_SELF_TEST_CHECK("CFG-EMPTY-PROFILE", err == ESP_ERR_NOT_FOUND);

    err = dura_meter_set_meter_timeout(120u);
    err = (err == ESP_OK) ? dura_meter_set_backlight_timeout(DURA_BACKLIGHT_TIMEOUT_OFF) : err;
    err = (err == ESP_OK) ? dura_meter_set_language(DURA_LANGUAGE_SPANISH) : err;
    err = (err == ESP_OK) ? dura_meter_set_reset_to_zero(true) : err;
    err = (err == ESP_OK) ? dura_meter_store_working_precal_profile(1u) : err;
    err = (err == ESP_OK) ? dura_meter_apply_quick_cal(21u) : err;
    DURA_SELF_TEST_CHECK("CFG-QUICK-APPLY", err == ESP_OK && dura_meter_get_snapshot(&meter) == ESP_OK &&
                         meter.quick_cal_reference == 21u &&
                         meter.working_coefficients.gallon_per_count > meter.precal_profiles[0].gallon_per_count);
    err = dura_meter_select_precal_profile(1u);
    DURA_SELF_TEST_CHECK("CFG-PROFILE-APPLY", err == ESP_OK && dura_meter_get_snapshot(&meter) == ESP_OK &&
                         meter.selected_precal_profile == 1u &&
                         fabsf(meter.working_coefficients.gallon_per_count -
                               meter.precal_profiles[1].gallon_per_count) < 0.0000001f);
    err = dura_meter_factory_defaults(); /* RAM only: verify the prior immediate saves reload. */
    err = (err == ESP_OK) ? dura_meter_load() : err;
    DURA_SELF_TEST_CHECK("CFG-SAVE-LOAD", err == ESP_OK && dura_meter_get_snapshot(&meter) == ESP_OK &&
                         meter.meter_timeout_sec == 120u && meter.backlight_timeout_sec == 9u &&
                         meter.selected_language == DURA_LANGUAGE_SPANISH && meter.reset_to_zero &&
                         meter.selected_precal_profile == 1u);
    err = dura_meter_factory_defaults();
    DURA_SELF_TEST_CHECK("CFG-RESTORE", err == ESP_OK);

    err = dura_meter_set_auto_batch(true);
    DURA_SELF_TEST_CHECK("BAT-001", err == ESP_OK && dura_meter_get_snapshot(&meter) == ESP_OK &&
                         meter.screen == DURA_SCREEN_BATCH && !meter.pump_enabled);
    err = dura_meter_set_auto_batch(false);
    DURA_SELF_TEST_CHECK("BAT-002", err == ESP_OK && dura_meter_get_snapshot(&meter) == ESP_OK &&
                         meter.screen == DURA_SCREEN_HOME && !meter.pump_enabled);

    err = dura_meter_start_batch(1.0f);
    (void)dura_meter_get_snapshot(&before);
    DURA_SELF_TEST_CHECK("BAT-003", err == ESP_OK && before.screen == DURA_SCREEN_BATCH &&
                         before.batch_mode == DURA_BATCH_RUNNING && before.pump_enabled &&
                         before.preset_batch_counts > 0);
    err = dura_meter_record_pulse(10);
    DURA_SELF_TEST_CHECK("BAT-005", err == ESP_OK && dura_meter_get_snapshot(&meter) == ESP_OK &&
                         meter.meter_total_counts == 10 && meter.batch_total_counts == 10 &&
                         meter.total_total_counts == 10 && meter.odometer_counts == 10);
    DURA_SELF_TEST_CHECK("BAT-006", meter.remaining_batch < before.remaining_batch);
    err = dura_meter_stop_batch();
    DURA_SELF_TEST_CHECK("BAT-004", err == ESP_OK && dura_meter_get_snapshot(&meter) == ESP_OK &&
                         !meter.pump_enabled && meter.batch_mode == DURA_BATCH_PAUSED);

    err = dura_meter_set_fault(7);
    DURA_SELF_TEST_CHECK("FLT-001", err == ESP_OK && dura_meter_get_snapshot(&meter) == ESP_OK &&
                         meter.fault_latched && meter.fault_code == 7 && !meter.pump_enabled);
    err = dura_meter_start_batch(1.0f);
    DURA_SELF_TEST_CHECK("FLT-002", err != ESP_OK && dura_meter_get_snapshot(&meter) == ESP_OK && !meter.pump_enabled);
    err = dura_meter_clear_fault();
    DURA_SELF_TEST_CHECK("FLT-003", err == ESP_OK && dura_meter_get_snapshot(&meter) == ESP_OK &&
                         !meter.fault_latched && meter.fault_code == 0);

    /* A stopped batch is intentionally paused and cannot enter calibration.
     * Restore the self-test's clean calibration precondition explicitly. */
    err = dura_meter_factory_defaults();
    DURA_SELF_TEST_CHECK("CAL-PRE", err == ESP_OK);
    err = dura_meter_start_calibration();
    DURA_SELF_TEST_CHECK("CAL-001", err == ESP_OK && dura_meter_get_snapshot(&meter) == ESP_OK &&
                         meter.screen == DURA_SCREEN_CALIBRATION && meter.calibration_mode == DURA_CAL_RUNNING &&
                         meter.pump_enabled);
    (void)dura_meter_record_pulse(10);
    err = dura_meter_stop_calibration();
    DURA_SELF_TEST_CHECK("CAL-002", err == ESP_OK && dura_meter_get_snapshot(&meter) == ESP_OK &&
                         !meter.pump_enabled && meter.calibration_mode == DURA_CAL_IDLE);
    err = dura_meter_start_calibration();
    (void)dura_meter_record_pulse(250);
    err = (err == ESP_OK) ? dura_meter_stop_calibration() : err;
    DURA_SELF_TEST_CHECK("CAL-003", err == ESP_OK && dura_meter_get_snapshot(&meter) == ESP_OK &&
                         !meter.pump_enabled && meter.calibration_mode == DURA_CAL_WAIT_MEASURED_AMOUNT);

    err = dura_recipe_get_local_liquid(&liquid);
    DURA_SELF_TEST_CHECK("ID-READ", err == ESP_OK && liquid.liquid_name[0] != '\0' && liquid.viscosity_cP > 0.0f);
    if (err == ESP_OK && liquid.liquid_name[0] != '\0' && strcasecmp(liquid.liquid_name, "unassigned") != 0) {
        target_liquid = liquid.liquid_name;
    }
    err = dura_recipe_save();
    DURA_SELF_TEST_CHECK("ID-SAVE", err == ESP_OK);
#if CONFIG_DURA_APP_M3000_LEGACY_ONLY
    err = ESP_OK;
#else
    err = dura_peers_broadcast_identity_update();
#endif
    DURA_SELF_TEST_CHECK("ID-BCAST", err == ESP_OK);

    err = dura_recipe_reset_default("SelfTestMissing", DURA_RECIPE_ORDER_CONFIGURED);
    DURA_SELF_TEST_CHECK("RCP-MISS-SETUP", err == ESP_OK);
    err = dura_recipe_add_default_ingredient("NoLiquid", 0.10f, 1);
    DURA_SELF_TEST_CHECK("RCP-MISS-ADD", err == ESP_OK);
    err = dura_coordinator_start_recipe_as_leader("NoSuchRecipe");
    DURA_SELF_TEST_CHECK("RCP-NOTFOUND", err == ESP_ERR_NOT_FOUND);
    err = dura_coordinator_start_recipe_as_leader("SelfTestMissing");
    DURA_SELF_TEST_CHECK("RCP-MISS-START", err == ESP_ERR_INVALID_STATE && dura_meter_get_snapshot(&meter) == ESP_OK &&
                         !meter.pump_enabled && meter.batch_mode != DURA_BATCH_RUNNING);

    err = dura_recipe_reset_default("SelfTest", DURA_RECIPE_ORDER_CONFIGURED);
    DURA_SELF_TEST_CHECK("RCP-001", err == ESP_OK && dura_recipe_get_default(&recipe) == ESP_OK &&
                         strcmp(recipe.name, "SelfTest") == 0 && recipe.ingredient_count == 0 &&
                         recipe.order == DURA_RECIPE_ORDER_CONFIGURED);

    err = dura_peers_get_snapshot(&peers);
#if CONFIG_DURA_APP_M3000_LEGACY_ONLY
    DURA_SELF_TEST_CHECK("PEER-RADIO-OFF", err == ESP_OK && !peers.esp_now_ready && peers.peer_count == 0U);
#else
    DURA_SELF_TEST_CHECK("PEER-READY", err == ESP_OK && peers.esp_now_ready);
#endif
    if (meter_count > 1U) {
        DURA_SELF_TEST_CHECK("PEER-COUNT", err == ESP_OK && peers.peer_count >= (size_t)(meter_count - 1U));
    }

    bool have_peer_target = false;
    for (size_t i = 0; i < peers.peer_count; ++i) {
        if (peers.peers[i].liquid_name[0] != '\0' && strcasecmp(peers.peers[i].liquid_name, "unassigned") != 0) {
            target_liquid = peers.peers[i].liquid_name;
            have_peer_target = true;
            break;
        }
    }
    if (meter_count > 1U) {
        DURA_SELF_TEST_CHECK("PEER-IDENTITY", have_peer_target);
    }
    err = dura_recipe_add_default_ingredient(target_liquid, 0.25f, 1);
    DURA_SELF_TEST_CHECK("RCP-002", err == ESP_OK && dura_recipe_get_default(&recipe) == ESP_OK &&
                         recipe.ingredient_count == 1 && strcmp(recipe.ingredients[0].liquid_name, target_liquid) == 0 &&
                         fabsf(recipe.ingredients[0].amount - 0.25f) < 0.001f && recipe.ingredients[0].sequence == 1);

    if (meter_count > 1U) {
        DURA_SELF_TEST_CHECK("PEER-TARGET", have_peer_target);
        err = have_peer_target ? dura_coordinator_start_recipe_as_leader("SelfTest") : ESP_ERR_NOT_FOUND;
        peer_recipe_started = (err == ESP_OK);
        DURA_SELF_TEST_CHECK("PEER-START", peer_recipe_started);

        err = dura_coordinator_abort();
        DURA_SELF_TEST_CHECK("PEER-CLEANUP", err == ESP_OK && dura_meter_get_snapshot(&meter) == ESP_OK &&
                             !meter.pump_enabled && meter.batch_mode != DURA_BATCH_RUNNING);
    }

    (void)dura_meter_factory_defaults();
    (void)dura_meter_save();
    dura_meter_set_log_context(NULL);
    dura_coordinator_set_log_context(NULL);

    const uint32_t failed = total - passed;
    const char *status = (failed == 0U) ? "PASS" : "FAIL";
    ESP_LOGI(TAG, "self-test complete: status=%s tests=%lu passed=%lu failed=%lu peer_target=%s peer_recipe_started=%s",
             status, (unsigned long)total, (unsigned long)passed, (unsigned long)failed,
             target_liquid, peer_recipe_started ? "true" : "false");

    int written = dura_json_snprintf(response, response_len,
             "{\"test_report\":{\"status\":\"%s\",\"meters_expected\":%lu,\"tests\":%lu,\"passed\":%lu,\"failed\":%lu,\"failures\":\"%s\",\"peer_target\":\"%s\",\"peer_recipe_started\":%s,\"notes\":\"cal_reject_expected;peer_cleanup_expected\"}}",
             status, (unsigned long)meter_count, (unsigned long)total, (unsigned long)passed,
             (unsigned long)failed, failures, target_liquid, peer_recipe_started ? "true" : "false");
    return dura_json_result(response, response_len, written);
}

#undef DURA_SELF_TEST_CHECK

#if !CONFIG_DURA_APP_M3000_LEGACY_ONLY
static bool mac_is_local(const uint8_t mac[6])
{
    dura_peers_snapshot_t peers = {0};
    return mac != NULL && dura_peers_get_snapshot(&peers) == ESP_OK &&
           memcmp(mac, peers.local_mac, sizeof(peers.local_mac)) == 0;
}

static uint32_t make_ble_gateway_claim_id(void)
{
    uint32_t claim_id = (uint32_t)(esp_timer_get_time() / 1000ULL);
    return claim_id == 0 ? 1U : claim_id;
}
#endif

static esp_err_t dura_status_handler(char *json, size_t json_len, void *user_ctx)
{
    (void)user_ctx;
    return dura_meter_format_status_json(json, json_len);
}

static void dura_ble_connected_handler(void *user_ctx)
{
    (void)user_ctx;
    (void)dura_board_set_ble_connected(true);
    s_local_ble_gateway_active = true;
#if CONFIG_DURA_APP_M3000_LEGACY_ONLY
    s_local_ble_gateway_claim_id = 0;
#else
    s_local_ble_gateway_claim_id = make_ble_gateway_claim_id();
    esp_err_t err = dura_peers_claim_ble_gateway(s_local_ble_gateway_claim_id);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BLE gateway claim failed 0x%x", err);
    }
    err = dura_peers_send_ping();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BLE gateway peer refresh ping failed 0x%x", err);
    }
#endif
}

static void dura_ble_disconnected_handler(void *user_ctx)
{
    (void)user_ctx;
    (void)dura_board_set_ble_connected(false);
#if !CONFIG_DURA_APP_M3000_LEGACY_ONLY
    if (s_local_ble_gateway_active && s_local_ble_gateway_claim_id != 0) {
        esp_err_t err = dura_peers_release_ble_gateway(s_local_ble_gateway_claim_id);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "BLE gateway release failed 0x%x", err);
        }
    }
#endif
    s_local_ble_gateway_active = false;
    s_local_ble_gateway_claim_id = 0;
}

#if !CONFIG_DURA_APP_M3000_LEGACY_ONLY
static void dura_ble_gateway_peer_event_handler(const dura_peer_event_t *event, void *user_ctx)
{
    (void)user_ctx;
    if (event == NULL || event->type != DURA_PEER_MSG_BLE_GATEWAY_CLAIM || mac_is_local(event->src_mac)) {
        return;
    }

    dura_peers_snapshot_t peers = {0};
    if (dura_peers_get_snapshot(&peers) != ESP_OK ||
        !peers.ble_gateway_active ||
        peers.ble_gateway_claim_id != event->claim_id ||
        memcmp(peers.ble_gateway_mac, event->src_mac, sizeof(peers.ble_gateway_mac)) != 0) {
        return;
    }

    bool connected = false;
    if (s_local_ble_gateway_active && gama_blefob_is_connected(&connected) == ESP_OK && connected) {
        ESP_LOGI(TAG, "Remote BLE gateway claim accepted from %02X:%02X:%02X:%02X:%02X:%02X claim=%lu; disconnecting local BLE",
                 event->src_mac[0], event->src_mac[1], event->src_mac[2],
                 event->src_mac[3], event->src_mac[4], event->src_mac[5],
                 (unsigned long)event->claim_id);
        s_local_ble_gateway_active = false;
        s_local_ble_gateway_claim_id = 0;
        (void)gama_blefob_disconnect("ble gateway taken over by another meter");
    }
}
#endif

static esp_err_t broadcast_identity_after_recipe_save(void)
{
    esp_err_t err = dura_recipe_save();
    if (err != ESP_OK) {
        return err;
    }
#if CONFIG_DURA_APP_M3000_LEGACY_ONLY
    return ESP_OK;
#else
    return dura_peers_broadcast_identity_update();
#endif
}

typedef struct {
    const char *name;
    const char *description;
    uint8_t local_sequence;
    uint8_t peer_sequence;
    float local_amount;
    float peer_amount;
} dura_test_recipe_preset_t;

static const dura_test_recipe_preset_t s_test_recipe_presets[] = {
    {
        .name = "TestMix1",
        .description = "two-step: local meter seq=1, first live peer seq=2",
        .local_sequence = 1,
        .peer_sequence = 2,
        .local_amount = 1.0f,
        .peer_amount = 1.0f,
    },
    {
        .name = "TestMix2",
        .description = "parallel: local meter seq=1, first live peer seq=1",
        .local_sequence = 1,
        .peer_sequence = 1,
        .local_amount = 1.0f,
        .peer_amount = 1.0f,
    },
    {
        .name = "TestMix3",
        .description = "reverse two-step: first live peer seq=1, local meter seq=2",
        .local_sequence = 2,
        .peer_sequence = 1,
        .local_amount = 1.0f,
        .peer_amount = 1.0f,
    },
    {
        .name = "TestMix4",
        .description = "amount ordering check: local amount=2.0 seq=1, first live peer amount=1.0 seq=2",
        .local_sequence = 1,
        .peer_sequence = 2,
        .local_amount = 2.0f,
        .peer_amount = 1.0f,
    },
};

static const dura_test_recipe_preset_t *find_test_recipe_preset(const char *name)
{
    for (size_t i = 0; i < sizeof(s_test_recipe_presets) / sizeof(s_test_recipe_presets[0]); ++i) {
        if (strcasecmp(name, s_test_recipe_presets[i].name) == 0) {
            return &s_test_recipe_presets[i];
        }
    }
    return NULL;
}

static bool liquid_name_is_usable(const char *name)
{
    return name != NULL && name[0] != '\0' && strcasecmp(name, "unassigned") != 0;
}

static esp_err_t run_test_recipe_preset(const dura_test_recipe_preset_t *preset, bool autorun, uint32_t autorun_pulses, char *response, size_t response_len)
{
    dura_liquid_config_t local = {0};
    dura_peers_snapshot_t peers = {0};
    dura_coordinator_snapshot_t coordinator = {0};
    const dura_peer_info_t *peer = NULL;
    bool aborted_previous = false;
    esp_err_t err = ESP_OK;

    ESP_RETURN_ON_FALSE(preset != NULL && response != NULL && response_len > 0,
                        ESP_ERR_INVALID_ARG, TAG, "bad test recipe args");
    ESP_RETURN_ON_ERROR(dura_recipe_get_local_liquid(&local), TAG, "get local liquid failed");
    ESP_RETURN_ON_ERROR(dura_peers_get_snapshot(&peers), TAG, "get peer snapshot failed");

    if (!liquid_name_is_usable(local.liquid_name)) {
        snprintf(response, response_len, "error: local liquid is not configured; run set_liquid <name> <cP> first");
        return ESP_OK;
    }

    for (size_t i = 0; i < peers.peer_count; ++i) {
        if (liquid_name_is_usable(peers.peers[i].liquid_name)) {
            peer = &peers.peers[i];
            break;
        }
    }
    if (peer == NULL) {
        snprintf(response, response_len, "error: %s needs one live peer with configured liquid; run peer_status", preset->name);
        return ESP_OK;
    }

    err = dura_coordinator_get_snapshot(&coordinator);
    if (err != ESP_OK) {
        snprintf(response, response_len, "error: %s could not read coordinator state 0x%x", preset->name, err);
        return ESP_OK;
    }
    if (coordinator.state != DURA_COORDINATOR_IDLE) {
        err = dura_coordinator_abort();
        if (err != ESP_OK) {
            snprintf(response, response_len, "error: %s could not abort active recipe 0x%x", preset->name, err);
            return ESP_OK;
        }
        aborted_previous = true;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    err = dura_recipe_reset_default(preset->name, DURA_RECIPE_ORDER_CONFIGURED);
    if (err == ESP_OK) {
        err = dura_recipe_add_default_ingredient(local.liquid_name, preset->local_amount, preset->local_sequence);
    }
    if (err == ESP_OK) {
        err = dura_recipe_add_default_ingredient(peer->liquid_name, preset->peer_amount, preset->peer_sequence);
    }
    if (err == ESP_OK) {
        err = dura_recipe_save();
    }
    if (err != ESP_OK) {
        snprintf(response, response_len, "error: %s setup failed 0x%x", preset->name, err);
        return ESP_OK;
    }

    if (autorun && autorun_pulses == 0) {
        snprintf(response, response_len, "error: %s auto test pulse count must be > 0", preset->name);
        return ESP_OK;
    }

    if (autorun) {
        err = dura_coordinator_enable_test_autorun(preset->name, autorun_pulses);
        if (err == ESP_OK) {
            err = dura_peers_send_test_autorun(preset->name, autorun_pulses);
        }
        if (err != ESP_OK) {
            snprintf(response, response_len, "error: %s auto test arm failed 0x%x", preset->name, err);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    err = dura_coordinator_start_recipe_as_leader(preset->name);
    if (err == ESP_ERR_INVALID_STATE) {
        snprintf(response, response_len,
                 "error: %s created but prerequisites failed; local=%s seq=%u peer=%s seq=%u; check recipe_detail and peer_status",
                 preset->name,
                 local.liquid_name,
                 preset->local_sequence,
                 peer->liquid_name,
                 preset->peer_sequence);
    } else if (err != ESP_OK) {
        snprintf(response, response_len, "error: %s created but start failed 0x%x", preset->name, err);
    } else {
        snprintf(response, response_len,
                 "%sok: %s created and %s\nlocal meter: %s amount=%.2f seq=%u\npeer meter: %s amount=%.2f seq=%u%s",
                 aborted_previous ? "previous recipe aborted\n" : "",
                 preset->name,
                 autorun ? "started with auto pulses" : "started",
                 local.liquid_name,
                 (double)preset->local_amount,
                 preset->local_sequence,
                 peer->liquid_name,
                 (double)preset->peer_amount,
                 preset->peer_sequence,
                 autorun ? "\nautorun: armed on leader and peers; no manual sim_pulse required" : "");
    }
    return ESP_OK;
}

static void refresh_peer_table_before_command(const char *reason)
{
    esp_err_t err = dura_peers_send_ping();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "peer refresh skipped before %s: ping failed 0x%x", reason ? reason : "command", err);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(DURA_PEER_DISCOVERY_SETTLE_MS));
}

static bool command_should_refresh_peers(const char *command)
{
#if CONFIG_DURA_APP_M3000_LEGACY_ONLY
    (void)command;
    return false;
#else
    if (command == NULL) {
        return false;
    }
    return strcmp(command, "whoami") == 0 ||
           strcmp(command, "peer_status") == 0 ||
           strcmp(command, "coordinator_status") == 0 ||
           command_args(command, "run_tests") != NULL ||
           command_args(command, "sanity_check") != NULL ||
           command_args(command, "test_recipe_auto") != NULL ||
           command_args(command, "test_recipe") != NULL ||
           command_args(command, "batch_start") != NULL ||
           command_args(command, "start_recipe") != NULL;
#endif
}

/* Only accepted operations count, not transport success or parse errors. */
static esp_err_t dura_command_activity(esp_err_t err)
{
    if (err == ESP_OK) dura_board_note_user_activity();
    return err;
}

static esp_err_t dura_execute_command(const char *command, char *response, size_t response_len, void *user_ctx)
{
    (void)user_ctx;
    esp_err_t err = ESP_OK;
    const char *args = NULL;

    if (strcmp(command, "help") == 0 || strcmp(command, "?") == 0) {
        snprintf(response, response_len,
                 "Dura M3000 local meter commands:\n"
                 "  help | ?                         Show this screen\n"
                 "  whoami                           Firmware and local meter identity\n"
                 "  provision_info                   Local BLE/app onboarding state\n"
                 "  sanity_check                      Legacy-safe local readiness\n"
                 "  run_tests <meter_count>           Run built-in self checks\n"
                 "\n"
                 "Meter setup/debug:\n"
                 "  liquid_list                      Show factory/custom liquid catalog\n"
                 "  liquid_select <id>               Select catalog liquid for this meter\n"
                 "  liquid_add <factor> [ref] <name> Add custom liquid profile\n"
                 "  liquid_delete <id>               Delete custom liquid profile\n"
                 "  set_liquid <name> <cP> [cal_id]   Set this meter identity manually\n"
                 "  meter_status | meter_config | meter_save | meter_load | meter_defaults\n"
                 "  ui_config                         Show low-battery/button-hold settings\n"
                 "  ui_config_set <threshold_pct> <blink_on_ms> <blink_off_ms> <max_rate_x>\n"
                 "  auto_batch on|off | start_batch <amount> | stop_batch\n"
                 "  start_cal | stop_cal | sim_pulse <count>\n"
                 "  lcd_debug | button_diag | deep_sleep | set_fault <code> | clear_fault\n"
                 "  open_pair <seconds>");
        return ESP_OK;
    }

    if (command_should_refresh_peers(command)) {
        refresh_peer_table_before_command(command);
    }

    if (strcmp(command, "whoami") == 0) {
        return format_whoami_json(response, response_len);
    }
    if (strcmp(command, "provision_info") == 0) {
        return format_provision_info_json(response, response_len);
    }
    if (strcmp(command, "feature_matrix") == 0) {
        return format_feature_matrix_json(response, response_len);
    }
    if (strcmp(command, "system_status") == 0) {
        int written = dura_json_snprintf(response, response_len,
          "{\"system\":{\"id\":\"%s\",\"name\":\"%s\",\"configured\":%s,\"scope_rule\":\"local_meter_only\"}}",
                               s_assigned_system_id, s_assigned_system_name,
                               system_assignment_configured() ? "true" : "false");
        return dura_json_result(response, response_len, written);
    }
    if ((args = command_args(command, "system_assign")) != NULL) {
        char system_id[32] = {0};
        char system_name[32] = {0};
        if (!parse_string_arg(&args, system_id, sizeof(system_id)) ||
            !parse_string_arg(&args, system_name, sizeof(system_name)) ||
            !no_extra_args(args)) {
            snprintf(response, response_len, "error: system_assign expects <id> <name_without_spaces>");
            return ESP_OK;
        }
        err = dura_command_activity(save_system_assignment(system_id, system_name));
        snprintf(response, response_len, err == ESP_OK ? "ok: system assigned" : "error: system_assign failed 0x%x", err);
        return ESP_OK;
    }
    if (strcmp(command, "system_clear") == 0) {
        err = dura_command_activity(clear_system_assignment());
        snprintf(response, response_len, err == ESP_OK ? "ok: system cleared" : "error: system_clear failed 0x%x", err);
        return ESP_OK;
    }
    if ((args = command_args(command, "sanity_check")) != NULL) {
#if CONFIG_DURA_APP_M3000_LEGACY_ONLY
        uint32_t min_peers = 0;
#else
        uint32_t min_peers = 1;
#endif
        skip_spaces(&args);
        if (*args != '\0' && !parse_uint_arg(&args, &min_peers, 10)) {
            snprintf(response, response_len, "error: sanity_check min_peers must be unsigned integer");
            return ESP_OK;
        }
        if (!no_extra_args(args)) {
            snprintf(response, response_len, "error: sanity_check has trailing garbage");
            return ESP_OK;
        }
        return format_sanity_check_json(min_peers, response, response_len);
    }
    if ((args = command_args(command, "run_tests")) != NULL) {
        uint32_t meter_count = 0;
        if (!parse_uint_arg(&args, &meter_count, 10) || !no_extra_args(args)) {
            snprintf(response, response_len, "error: run_tests expects <meter_count>");
            return ESP_OK;
        }
        return run_self_tests(meter_count, response, response_len);
    }
    if ((args = command_args(command, "test_recipe_auto")) != NULL) {
        char recipe_name[DURA_RECIPE_MAX_NAME_LEN] = {0};
        uint32_t pulse_count = 1000;
        const dura_test_recipe_preset_t *preset = NULL;
        if (!parse_string_arg(&args, recipe_name, sizeof(recipe_name))) {
            snprintf(response, response_len, "error: test_recipe_auto expects TestMix1|TestMix2|TestMix3|TestMix4 [pulses]");
            return ESP_OK;
        }
        skip_spaces(&args);
        if (*args != '\0' && !parse_uint_arg(&args, &pulse_count, 10)) {
            snprintf(response, response_len, "error: test_recipe_auto pulse count must be unsigned integer");
            return ESP_OK;
        }
        if (!no_extra_args(args) || pulse_count == 0) {
            snprintf(response, response_len, "error: test_recipe_auto expects optional pulse count > 0");
            return ESP_OK;
        }
        preset = find_test_recipe_preset(recipe_name);
        if (preset == NULL) {
            snprintf(response, response_len, "error: unknown test recipe '%s'; use TestMix1, TestMix2, TestMix3, or TestMix4", recipe_name);
            return ESP_OK;
        }
        return run_test_recipe_preset(preset, true, pulse_count, response, response_len);
    }
    if ((args = command_args(command, "test_recipe")) != NULL) {
        char recipe_name[DURA_RECIPE_MAX_NAME_LEN] = {0};
        const dura_test_recipe_preset_t *preset = NULL;
        if (!parse_string_arg(&args, recipe_name, sizeof(recipe_name)) || !no_extra_args(args)) {
            snprintf(response, response_len, "error: test_recipe expects TestMix1|TestMix2|TestMix3|TestMix4");
            return ESP_OK;
        }
        preset = find_test_recipe_preset(recipe_name);
        if (preset == NULL) {
            snprintf(response, response_len, "error: unknown test recipe '%s'; use TestMix1, TestMix2, TestMix3, or TestMix4", recipe_name);
            return ESP_OK;
        }
        return run_test_recipe_preset(preset, false, 0, response, response_len);
    }
#if CONFIG_DURA_BOARD_ENABLE_HARDWARE_TASK
    if (strcmp(command, "lcd_debug") == 0) {
#if !CONFIG_DURA_BOARD_LCD_HOLD_SPLASH_ON_BOOT
        err = dura_board_render_debug_screen();
        snprintf(response, response_len, err == ESP_OK ? "ok: lcd debug screen redrawn" : "error: lcd_debug failed 0x%x", err);
#else
        snprintf(response, response_len, "error: lcd debug disabled; CONFIG_DURA_BOARD_LCD_HOLD_SPLASH_ON_BOOT=y");
#endif
        return ESP_OK;
    }
    if (strcmp(command, "button_diag") == 0) {
        dura_board_dump_button_io_config("command");
        snprintf(response, response_len, "ok: button gpio diagnostics dumped to log");
        return ESP_OK;
    }
    if (strcmp(command, "deep_sleep") == 0) {
        snprintf(response, response_len, "entering deep_sleep; wake on any active-low button");
        return dura_board_enter_deep_sleep();
    }
#endif
    if (strcmp(command, "dura_status") == 0 || strcmp(command, "meter_status") == 0) {
        return dura_meter_format_status_json(response, response_len);
    }
    if (strcmp(command, "dura_config") == 0 || strcmp(command, "meter_config") == 0) {
        return dura_meter_format_config_json(response, response_len);
    }
    if (strcmp(command, "ui_config") == 0) {
        return dura_ui_config_format_json(response, response_len);
    }
    if ((args = command_args(command, "ui_config_set")) != NULL) {
        uint32_t threshold_percent = 0u;
        uint32_t blink_on_ms = 0u;
        uint32_t blink_off_ms = 0u;
        uint32_t max_rate_x = 0u;
        if (!parse_uint_arg(&args, &threshold_percent, 10) ||
            !parse_uint_arg(&args, &blink_on_ms, 10) ||
            !parse_uint_arg(&args, &blink_off_ms, 10) ||
            !parse_uint_arg(&args, &max_rate_x, 10) ||
            !no_extra_args(args) ||
            threshold_percent > UINT8_MAX || max_rate_x > UINT8_MAX) {
            snprintf(response, response_len,
                     "error: ui_config_set expects <threshold_pct> <blink_on_ms> <blink_off_ms> <max_rate_x>");
            return ESP_OK;
        }

        const dura_ui_runtime_config_t config = {
            .low_battery_threshold_percent = (uint8_t)threshold_percent,
            .button_hold_max_rate_x = (uint8_t)max_rate_x,
            .reserved = 0u,
            .low_battery_blink_on_ms = blink_on_ms,
            .low_battery_blink_off_ms = blink_off_ms,
        };
        err = dura_command_activity(dura_ui_config_set(&config));
        if (err == ESP_ERR_INVALID_ARG) {
            snprintf(response, response_len,
                     "error: ui_config_set ranges threshold=0..100 blink=100..60000 ms max_rate=1..100x");
            return ESP_OK;
        }
        if (err != ESP_OK) {
            snprintf(response, response_len, "error: ui_config_set persistence failed 0x%x", err);
            return ESP_OK;
        }
        return dura_ui_config_format_json(response, response_len);
    }
    if (strcmp(command, "meter_save") == 0) {
        err = dura_command_activity(dura_meter_save());
        snprintf(response, response_len, err == ESP_OK ? "ok: meter saved" : "error: meter_save failed 0x%x", err);
        return ESP_OK;
    }
    if (strcmp(command, "meter_load") == 0) {
        err = dura_command_activity(dura_meter_load());
        snprintf(response, response_len, err == ESP_OK ? "ok: meter loaded" : "error: meter_load failed 0x%x", err);
        return ESP_OK;
    }
    if (strcmp(command, "meter_defaults") == 0) {
        err = dura_command_activity(dura_meter_factory_defaults());
        if (err == ESP_OK) {
            err = dura_meter_save();
        }
        snprintf(response, response_len, err == ESP_OK ? "ok: meter defaults saved" : "error: meter_defaults failed 0x%x", err);
        return ESP_OK;
    }
    if (strcmp(command, "recipe_status") == 0) {
        return dura_recipe_format_status_json(response, response_len);
    }
    if (strcmp(command, "recipe_detail") == 0) {
        return dura_recipe_format_detail_json(response, response_len);
    }
    if (strcmp(command, "recipe_save") == 0) {
        err = dura_command_activity(broadcast_identity_after_recipe_save());
        snprintf(response, response_len, err == ESP_OK ? "ok: recipe saved" : "error: recipe_save failed 0x%x", err);
        return ESP_OK;
    }
    if (strcmp(command, "recipe_load") == 0) {
        err = dura_command_activity(dura_recipe_load());
        snprintf(response, response_len, err == ESP_OK ? "ok: recipe loaded" : "error: recipe_load failed 0x%x", err);
        return ESP_OK;
    }
    if (strcmp(command, "recipe_clear") == 0) {
        err = dura_command_activity(dura_recipe_clear_default());
        snprintf(response, response_len, err == ESP_OK ? "ok: recipe cleared" : "error: recipe_clear failed 0x%x", err);
        return ESP_OK;
    }
    if (strcmp(command, "factory_reset_recipe") == 0) {
        err = dura_command_activity(dura_recipe_factory_defaults());
        snprintf(response, response_len, err == ESP_OK ? "ok: recipe factory defaults saved" : "error: factory_reset_recipe failed 0x%x", err);
        return ESP_OK;
    }
    if (strcmp(command, "liquid_list") == 0) {
        return dura_recipe_format_liquid_catalog_json(response, response_len);
    }
    if ((args = command_args(command, "liquid_select")) != NULL) {
        uint32_t id = 0;
        if (!parse_uint_arg(&args, &id, 10) || id > UINT16_MAX || !no_extra_args(args)) {
            snprintf(response, response_len, "error: liquid_select expects <id>");
            return ESP_OK;
        }
        err = dura_command_activity(dura_recipe_select_liquid_profile((uint16_t)id));
        if (err == ESP_OK) {
#if CONFIG_DURA_APP_M3000_LEGACY_ONLY
            err = ESP_OK;
#else
            err = dura_peers_broadcast_identity_update();
#endif
        }
        snprintf(response, response_len, err == ESP_OK ? "ok: liquid selected" : "error: liquid_select failed 0x%x", err);
        return ESP_OK;
    }
    if ((args = command_args(command, "liquid_add")) != NULL) {
        float factor = 0.0f;
        uint32_t ref_num = 0;
        uint16_t id = 0;
        char name[DURA_RECIPE_MAX_LIQUID_NAME_LEN] = {0};
        if (!parse_float_arg(&args, &factor) || factor <= 0.0f) {
            snprintf(response, response_len, "error: liquid_add expects <factor> [ref] <name>");
            return ESP_OK;
        }
        skip_spaces(&args);
        const char *before_ref = args;
        if (parse_uint_arg(&args, &ref_num, 10)) {
            if (ref_num > UINT8_MAX) {
                snprintf(response, response_len, "error: liquid_add ref must be 0..255");
                return ESP_OK;
            }
            skip_spaces(&args);
            if (*args == '\0') {
                args = before_ref;
                ref_num = 0;
            }
        } else {
            args = before_ref;
            ref_num = 0;
        }
        if (!parse_rest_arg(&args, name, sizeof(name)) || !no_extra_args(args)) {
            snprintf(response, response_len, "error: liquid_add expects <factor> [ref] <name>");
            return ESP_OK;
        }
        err = dura_command_activity(dura_recipe_add_custom_liquid(name, factor, (uint8_t)ref_num, &id));
        if (err == ESP_OK) {
            snprintf(response, response_len, "ok: custom liquid added id=%u", (unsigned)id);
        } else {
            snprintf(response, response_len, "error: liquid_add failed 0x%x", err);
        }
        return ESP_OK;
    }
    if ((args = command_args(command, "liquid_delete")) != NULL) {
        uint32_t id = 0;
        if (!parse_uint_arg(&args, &id, 10) || id > UINT16_MAX || !no_extra_args(args)) {
            snprintf(response, response_len, "error: liquid_delete expects <custom_id>");
            return ESP_OK;
        }
        err = dura_command_activity(dura_recipe_delete_custom_liquid((uint16_t)id));
        snprintf(response, response_len, err == ESP_OK ? "ok: custom liquid deleted" : "error: liquid_delete failed 0x%x", err);
        return ESP_OK;
    }
    if ((args = command_args(command, "set_liquid")) != NULL) {
        dura_liquid_config_t liquid = {0};
        uint32_t calibration_id = 0;
        if (!parse_string_arg(&args, liquid.liquid_name, sizeof(liquid.liquid_name))) {
            snprintf(response, response_len, "error: set_liquid requires <name>");
            return ESP_OK;
        }
        if (strcasecmp(liquid.liquid_name, "unassigned") == 0) {
            snprintf(response, response_len, "error: set_liquid cannot use 'unassigned'; use factory_reset_recipe to reset");
            return ESP_OK;
        }
        if (!parse_float_arg(&args, &liquid.viscosity_cP) || liquid.viscosity_cP <= 0.0f) {
            snprintf(response, response_len, "error: set_liquid viscosity_cP must be positive number");
            return ESP_OK;
        }
        skip_spaces(&args);
        if (*args != '\0' && !parse_uint_arg(&args, &calibration_id, 10)) {
            snprintf(response, response_len, "error: set_liquid cal_id must be unsigned integer");
            return ESP_OK;
        }
        if (!no_extra_args(args)) {
            snprintf(response, response_len, "error: set_liquid has trailing garbage");
            return ESP_OK;
        }
        liquid.calibration_id = calibration_id;
        err = dura_command_activity(dura_recipe_set_local_liquid(&liquid));
        if (err == ESP_OK) {
#if CONFIG_DURA_APP_M3000_LEGACY_ONLY
            err = ESP_OK;
#else
            err = dura_peers_broadcast_identity_update();
#endif
        }
        snprintf(response, response_len, err == ESP_OK ? "ok: liquid set" : "error: set_liquid failed 0x%x", err);
        return ESP_OK;
    }
    if ((args = command_args(command, "recipe_create_simple")) != NULL) {
        char recipe_name[DURA_RECIPE_MAX_NAME_LEN] = {0};
        uint32_t id = 0;
        float amount = 0.0f;
        uint32_t sequence = 0;
        dura_liquid_profile_t profile = {0};
        if (!parse_string_arg(&args, recipe_name, sizeof(recipe_name))) {
            snprintf(response, response_len, "error: recipe_create_simple expects <name> <liquid_id> <amount> [seq]");
            return ESP_OK;
        }
        if (!parse_uint_arg(&args, &id, 10) || id > UINT16_MAX) {
            snprintf(response, response_len, "error: recipe_create_simple liquid_id must be 0..65535");
            return ESP_OK;
        }
        if (!parse_float_arg(&args, &amount) || amount <= 0.0f) {
            snprintf(response, response_len, "error: recipe_create_simple amount must be positive number");
            return ESP_OK;
        }
        skip_spaces(&args);
        if (*args != '\0' && !parse_uint_arg(&args, &sequence, 10)) {
            snprintf(response, response_len, "error: recipe_create_simple sequence must be unsigned integer");
            return ESP_OK;
        }
        if (sequence > UINT8_MAX) {
            snprintf(response, response_len, "error: recipe_create_simple sequence must be 0..255");
            return ESP_OK;
        }
        if (!no_extra_args(args)) {
            snprintf(response, response_len, "error: recipe_create_simple has trailing garbage");
            return ESP_OK;
        }
        err = dura_recipe_find_liquid_profile_by_id((uint16_t)id, &profile);
        if (err == ESP_OK) {
            err = dura_command_activity(dura_recipe_reset_default(recipe_name, DURA_RECIPE_ORDER_CONFIGURED));
        }
        if (err == ESP_OK) {
            err = dura_command_activity(dura_recipe_add_default_ingredient(profile.name, amount, (uint8_t)sequence));
        }
        if (err == ESP_OK) {
            err = dura_peers_broadcast_identity_update();
        }
        if (err == ESP_OK) {
            int written = dura_json_snprintf(response, response_len,
                                   "{\"ok\":true,\"command\":\"recipe_create_simple\",\"recipe\":{\"name\":\"%s\",\"order\":\"configured\",\"ingredient_count\":1,\"ingredient\":{\"liquid_id\":%lu,\"liquid\":\"%s\",\"amount\":%.2f,\"sequence\":%lu}}}",
                                   recipe_name, (unsigned long)id, profile.name, (double)amount,
                                   (unsigned long)(sequence == 0 ? 1 : sequence));
            return dura_json_result(response, response_len, written);
        }
        snprintf(response, response_len, "error: recipe_create_simple failed 0x%x", err);
        return ESP_OK;
    }
    if ((args = command_args(command, "recipe_new")) != NULL) {
        char recipe_name[DURA_RECIPE_MAX_NAME_LEN] = {0};
        if (!parse_string_arg(&args, recipe_name, sizeof(recipe_name)) || !no_extra_args(args)) {
            snprintf(response, response_len, "error: recipe_new requires single <name>");
            return ESP_OK;
        }
        err = dura_command_activity(dura_recipe_reset_default(recipe_name, DURA_RECIPE_ORDER_CONFIGURED));
        snprintf(response, response_len, err == ESP_OK ? "ok: recipe reset" : "error: invalid recipe name");
        return ESP_OK;
    }
    if ((args = command_args(command, "recipe_order")) != NULL) {
        char order[16] = {0};
        dura_recipe_t recipe;
        if (!parse_string_arg(&args, order, sizeof(order)) || !no_extra_args(args) ||
            (strcasecmp(order, "largest") != 0 && strcasecmp(order, "smallest") != 0 &&
             strcasecmp(order, "configured") != 0)) {
            snprintf(response, response_len, "error: recipe_order expects configured|largest|smallest");
            return ESP_OK;
        }
        err = dura_recipe_get_default(&recipe);
        if (err == ESP_OK) {
            if (strcasecmp(order, "largest") == 0) {
                recipe.order = DURA_RECIPE_ORDER_LARGEST_FIRST;
            } else if (strcasecmp(order, "smallest") == 0) {
                recipe.order = DURA_RECIPE_ORDER_SMALLEST_FIRST;
            } else {
                recipe.order = DURA_RECIPE_ORDER_CONFIGURED;
            }
            err = dura_command_activity(dura_recipe_set_default(&recipe));
        }
        snprintf(response, response_len, err == ESP_OK ? "ok: recipe order set" : "error: recipe_order failed 0x%x", err);
        return ESP_OK;
    }
    if ((args = command_args(command, "recipe_add")) != NULL) {
        char liquid_name[DURA_RECIPE_MAX_LIQUID_NAME_LEN] = {0};
        float amount = 0.0f;
        uint32_t sequence = 0;
        if (!parse_string_arg(&args, liquid_name, sizeof(liquid_name))) {
            snprintf(response, response_len, "error: recipe_add requires <liquid>");
            return ESP_OK;
        }
        if (!parse_float_arg(&args, &amount) || amount <= 0.0f) {
            snprintf(response, response_len, "error: recipe_add amount must be positive number");
            return ESP_OK;
        }
        skip_spaces(&args);
        if (*args != '\0' && !parse_uint_arg(&args, &sequence, 10)) {
            snprintf(response, response_len, "error: recipe_add sequence must be unsigned integer");
            return ESP_OK;
        }
        if (sequence > UINT8_MAX) {
            snprintf(response, response_len, "error: recipe_add sequence must be 0..255");
            return ESP_OK;
        }
        if (!no_extra_args(args)) {
            snprintf(response, response_len, "error: recipe_add has trailing garbage");
            return ESP_OK;
        }
        err = dura_command_activity(dura_recipe_add_default_ingredient(liquid_name, amount, (uint8_t)sequence));
        snprintf(response, response_len, err == ESP_OK ? "ok: ingredient added" : "error: recipe_add failed 0x%x", err);
        return ESP_OK;
    }
    if ((args = command_args(command, "recipe_add_id")) != NULL) {
        uint32_t id = 0;
        float amount = 0.0f;
        uint32_t sequence = 0;
        dura_liquid_profile_t profile = {0};
        if (!parse_uint_arg(&args, &id, 10) || id > UINT16_MAX) {
            snprintf(response, response_len, "error: recipe_add_id expects <id> <amount> [seq]");
            return ESP_OK;
        }
        if (!parse_float_arg(&args, &amount) || amount <= 0.0f) {
            snprintf(response, response_len, "error: recipe_add_id amount must be positive number");
            return ESP_OK;
        }
        skip_spaces(&args);
        if (*args != '\0' && !parse_uint_arg(&args, &sequence, 10)) {
            snprintf(response, response_len, "error: recipe_add_id sequence must be unsigned integer");
            return ESP_OK;
        }
        if (sequence > UINT8_MAX) {
            snprintf(response, response_len, "error: recipe_add_id sequence must be 0..255");
            return ESP_OK;
        }
        if (!no_extra_args(args)) {
            snprintf(response, response_len, "error: recipe_add_id has trailing garbage");
            return ESP_OK;
        }
        err = dura_recipe_find_liquid_profile_by_id((uint16_t)id, &profile);
        if (err == ESP_OK) {
            err = dura_command_activity(dura_recipe_add_default_ingredient(profile.name, amount, (uint8_t)sequence));
        }
        snprintf(response, response_len, err == ESP_OK ? "ok: ingredient added" : "error: recipe_add_id failed 0x%x", err);
        return ESP_OK;
    }
    if (strcmp(command, "peer_status") == 0) {
        return dura_peers_format_status_json(response, response_len);
    }
    if (strcmp(command, "peer_ping") == 0) {
        err = dura_peers_send_ping();
        snprintf(response, response_len, err == ESP_OK ? "ok: ping sent" : "error: peer_ping failed 0x%x", err);
        return ESP_OK;
    }
    if (strcmp(command, "peer_hello") == 0) {
        err = dura_peers_broadcast_hello();
        snprintf(response, response_len, err == ESP_OK ? "ok: hello sent" : "error: peer_hello failed 0x%x", err);
        return ESP_OK;
    }
    if (strcmp(command, "coordinator_status") == 0) {
        return dura_coordinator_format_status_json(response, response_len);
    }
    if ((args = command_args(command, "batch_start")) != NULL) {
        char recipe_name[DURA_RECIPE_MAX_NAME_LEN] = {0};
        float scale = 1.0f;
        if (!parse_string_arg(&args, recipe_name, sizeof(recipe_name))) {
            snprintf(response, response_len, "error: batch_start requires <recipe> [scale]");
            return ESP_OK;
        }
        skip_spaces(&args);
        if (*args != '\0' && !parse_float_arg(&args, &scale)) {
            snprintf(response, response_len, "error: batch_start scale must be positive number");
            return ESP_OK;
        }
        if (scale <= 0.0f || !no_extra_args(args)) {
            snprintf(response, response_len, "error: batch_start requires <recipe> [positive_scale]");
            return ESP_OK;
        }
        dura_coordinator_snapshot_t before = {0};
        (void)dura_coordinator_get_snapshot(&before);
        err = dura_command_activity(dura_coordinator_start_batch_as_leader(recipe_name, scale));
        if (err == ESP_ERR_NOT_FOUND) {
            snprintf(response, response_len, "error: recipe not found '%s'", recipe_name);
        } else if (err == ESP_ERR_INVALID_STATE && before.state != DURA_COORDINATOR_IDLE) {
            snprintf(response, response_len, "error: coordinator already active; state=%s active_recipe=%s", dura_coordinator_state_name(before.state), before.active_recipe);
        } else if (err == ESP_ERR_INVALID_STATE) {
            snprintf(response, response_len, "error: batch '%s' prerequisites not met; check recipe_detail and peer_status", recipe_name);
        } else {
            snprintf(response, response_len, err == ESP_OK ? "ok: batch started" : "error: batch_start failed 0x%x", err);
        }
        return ESP_OK;
    }
    if ((args = command_args(command, "start_recipe")) != NULL) {
        char recipe_name[DURA_RECIPE_MAX_NAME_LEN] = {0};
        if (!parse_string_arg(&args, recipe_name, sizeof(recipe_name)) || !no_extra_args(args)) {
            snprintf(response, response_len, "error: start_recipe requires single <name>");
            return ESP_OK;
        }
        dura_coordinator_snapshot_t before = {0};
        (void)dura_coordinator_get_snapshot(&before);
        err = dura_command_activity(dura_coordinator_start_recipe_as_leader(recipe_name));
        if (err == ESP_ERR_NOT_FOUND) {
            snprintf(response, response_len, "error: recipe not found '%s'", recipe_name);
        } else if (err == ESP_ERR_INVALID_STATE && before.state != DURA_COORDINATOR_IDLE) {
            snprintf(response, response_len, "error: coordinator already active; state=%s active_recipe=%s", dura_coordinator_state_name(before.state), before.active_recipe);
        } else if (err == ESP_ERR_INVALID_STATE) {
            snprintf(response, response_len, "error: recipe '%s' prerequisites not met; check recipe_detail and peer_status", recipe_name);
        } else {
            snprintf(response, response_len, err == ESP_OK ? "ok: recipe started" : "error: start_recipe failed 0x%x", err);
        }
        return ESP_OK;
    }
    if (strcmp(command, "batch_abort") == 0) {
        err = dura_command_activity(dura_coordinator_abort());
        snprintf(response, response_len, err == ESP_OK ? "ok: batch aborted" : "error: batch_abort failed 0x%x", err);
        return ESP_OK;
    }
    if (strcmp(command, "batch_pause") == 0) {
        err = dura_command_activity(dura_meter_pause_batch());
        snprintf(response, response_len, err == ESP_OK ? "ok: batch paused" : "error: batch_pause failed 0x%x", err);
        return ESP_OK;
    }
    if (strcmp(command, "batch_resume") == 0) {
        err = dura_command_activity(dura_meter_resume_batch());
        snprintf(response, response_len, err == ESP_OK ? "ok: batch resumed" : "error: batch_resume failed 0x%x", err);
        return ESP_OK;
    }
    if (strcmp(command, "abort_recipe") == 0) {
        err = dura_command_activity(dura_coordinator_abort());
        snprintf(response, response_len, err == ESP_OK ? "ok: recipe aborted" : "error: abort_recipe failed 0x%x", err);
        return ESP_OK;
    }
    if ((args = command_args(command, "auto_batch")) != NULL) {
        bool enabled = false;
        if (!parse_bool_arg(&args, &enabled) || !no_extra_args(args)) {
            snprintf(response, response_len, "error: auto_batch expects 0|1|off|on|false|true|no|yes");
            return ESP_OK;
        }
        err = dura_command_activity(dura_meter_set_auto_batch(enabled));
        if (err == ESP_OK) {
            snprintf(response, response_len, "ok: auto_batch %s", enabled ? "on" : "off");
        } else {
            snprintf(response, response_len, "error: auto_batch failed 0x%x", err);
        }
        return ESP_OK;
    }
    if ((args = command_args(command, "start_batch")) != NULL) {
        float amount = 0.0f;
        if (!parse_float_arg(&args, &amount) || amount <= 0.0f || !no_extra_args(args)) {
            snprintf(response, response_len, "error: start_batch amount must be positive number");
            return ESP_OK;
        }
        err = dura_command_activity(dura_meter_start_batch(amount));
        snprintf(response, response_len, err == ESP_OK ? "ok: batch started" : "error: start_batch failed 0x%x", err);
        return ESP_OK;
    }
    if (strcmp(command, "stop_batch") == 0) {
        err = dura_command_activity(dura_meter_stop_batch());
        snprintf(response, response_len, err == ESP_OK ? "ok: batch stopped" : "error: stop_batch failed 0x%x", err);
        return ESP_OK;
    }
    if (strcmp(command, "start_cal") == 0) {
        err = dura_command_activity(dura_meter_start_calibration());
        snprintf(response, response_len, err == ESP_OK ? "ok: calibration started" : "error: start_cal failed 0x%x", err);
        return ESP_OK;
    }
    if (strcmp(command, "stop_cal") == 0) {
        err = dura_command_activity(dura_meter_stop_calibration());
        snprintf(response, response_len, err == ESP_OK ? "ok: calibration stopped" : "error: stop_cal failed 0x%x", err);
        return ESP_OK;
    }
    if ((args = command_args(command, "sim_pulse")) != NULL) {
        uint32_t count = 0;
        if (!parse_uint_arg(&args, &count, 10) || !no_extra_args(args)) {
            snprintf(response, response_len, "error: sim_pulse count must be unsigned integer");
            return ESP_OK;
        }
        err = dura_meter_record_pulse(count);
        if (count > 0) (void)dura_command_activity(err);
        snprintf(response, response_len, err == ESP_OK ? "ok: pulse recorded" : "error: sim_pulse failed 0x%x", err);
        return ESP_OK;
    }
    if ((args = command_args(command, "set_fault")) != NULL) {
        uint32_t code = 0;
        if (!parse_uint_arg(&args, &code, 0) || !no_extra_args(args)) {
            snprintf(response, response_len, "error: set_fault code must be unsigned integer");
            return ESP_OK;
        }
        err = dura_command_activity(dura_meter_set_fault(code == 0 ? 1 : code));
        (void)gama_blefob_set_fault_latched(true);
        (void)gama_blefob_set_fault_code(code == 0 ? 1 : code);
        snprintf(response, response_len, err == ESP_OK ? "ok: fault set" : "error: set_fault failed 0x%x", err);
        return ESP_OK;
    }
    if (strcmp(command, "clear_fault") == 0) {
        err = dura_command_activity(dura_meter_clear_fault());
        (void)gama_blefob_set_fault_latched(false);
        (void)gama_blefob_set_fault_code(0);
        snprintf(response, response_len, err == ESP_OK ? "ok: fault cleared" : "error: clear_fault failed 0x%x", err);
        return ESP_OK;
    }
    if ((args = command_args(command, "open_pair")) != NULL) {
        uint32_t seconds = 0;
        if (!parse_uint_arg(&args, &seconds, 10) || !no_extra_args(args)) {
            snprintf(response, response_len, "error: open_pair seconds must be unsigned integer");
            return ESP_OK;
        }
        err = dura_command_activity(gama_blefob_open_pairing_window(seconds == 0 ? 60 : seconds));
        snprintf(response, response_len, err == ESP_OK ? "ok: pairing window opened" : "error: open_pair failed 0x%x", err);
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

static const char *dura_command_source_name(dura_command_source_t source)
{
    switch (source) {
    case DURA_COMMAND_SOURCE_UART:
        return "uart";
    case DURA_COMMAND_SOURCE_BLE:
        return "ble";
    default:
        return "unknown";
    }
}

static esp_err_t dura_submit_command(dura_command_source_t source,
                                     const char *command,
                                     char *response,
                                     size_t response_len,
                                     TickType_t timeout_ticks)
{
    ESP_RETURN_ON_FALSE(command != NULL && response != NULL && response_len > 0,
                        ESP_ERR_INVALID_ARG, TAG, "bad command submission args");

    response[0] = '\0';
    if (s_app_command_queue == NULL || s_app_command_reply_queue == NULL ||
        s_app_command_submit_mutex == NULL) {
        snprintf(response, response_len, "error: app command queue not ready");
        return ESP_ERR_INVALID_STATE;
    }
    if (strlen(command) >= DURA_APP_COMMAND_MAX_LEN) {
        snprintf(response, response_len, "error: command too long");
        return ESP_ERR_INVALID_SIZE;
    }
    if (xSemaphoreTake(s_app_command_submit_mutex, timeout_ticks) != pdTRUE) {
        snprintf(response, response_len, "error: app command submit busy");
        return ESP_ERR_TIMEOUT;
    }

    /* The reply route has process lifetime.  A single submitter owns it, and
     * request IDs let it discard a late reply from a previously timed-out call. */

    dura_command_request_t request = {
        .source = source,
        .request_id = ++s_app_command_next_request_id,
    };
    if (request.request_id == 0) request.request_id = ++s_app_command_next_request_id;
    strlcpy(request.command, command, sizeof(request.command));

    esp_err_t err = ESP_OK;
    dura_command_reply_t *reply = heap_caps_calloc(1, sizeof(*reply), MALLOC_CAP_8BIT);
    if (reply == NULL) {
        snprintf(response, response_len, "error: command reply alloc failed");
        err = ESP_ERR_NO_MEM;
        goto done;
    }

    /* Drain into the existing heap reply, never a response-sized BLE stack local. */
    while (xQueueReceive(s_app_command_reply_queue, reply, 0) == pdTRUE) {}

    if (xQueueSend(s_app_command_queue, &request, timeout_ticks) != pdTRUE) {
        snprintf(response, response_len, "error: app command queue full");
        err = ESP_ERR_TIMEOUT;
        goto done;
    }

    const TickType_t wait_started = xTaskGetTickCount();
    for (;;) {
        const TickType_t elapsed = xTaskGetTickCount() - wait_started;
        const TickType_t remaining = (elapsed < timeout_ticks) ? timeout_ticks - elapsed : 0;
        if (remaining == 0 || xQueueReceive(s_app_command_reply_queue, reply, remaining) != pdTRUE) {
            snprintf(response, response_len, "error: app command response timeout");
            err = ESP_ERR_TIMEOUT;
            goto done;
        }
        if (reply->request_id == request.request_id) break;
    }

    err = reply->err;
    strlcpy(response, reply->response[0] != '\0' ? reply->response : "ok", response_len);

done:
    free(reply);
    xSemaphoreGive(s_app_command_submit_mutex);
    return err;
}

static void app_command_task(void *arg)
{
    (void)arg;

    while (true) {
        dura_command_request_t request = {0};
        if (xQueueReceive(s_app_command_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        dura_command_reply_t *reply = heap_caps_calloc(1, sizeof(*reply), MALLOC_CAP_8BIT);
        if (reply == NULL) {
            ESP_LOGE(TAG, "app command reply allocation failed");
            continue;
        }
        ESP_LOGI(TAG, "%s command: %s", dura_command_source_name(request.source), request.command);
        reply->request_id = request.request_id;
        reply->err = dura_execute_command(request.command, reply->response, sizeof(reply->response), NULL);
        if (reply->err == ESP_ERR_NOT_FOUND) {
            snprintf(reply->response, sizeof(reply->response), "error: unknown command '%s'", request.command);
        } else if (reply->err != ESP_OK && reply->response[0] == '\0') {
            snprintf(reply->response, sizeof(reply->response), "error: command failed 0x%x", reply->err);
        }

        /* Process-lifetime queue: safe even if the original caller timed out.
         * A length-one overwrite also prevents orphan replies blocking work. */
        (void)xQueueOverwrite(s_app_command_reply_queue, reply);
        free(reply);
    }
}

static void dura_ble_user_activity_handler(void *user_ctx)
{
    (void)user_ctx;
    dura_board_note_user_activity();
}

static esp_err_t dura_ble_command_handler(const char *command, char *response, size_t response_len, void *user_ctx)
{
    (void)user_ctx;
    /* M3000 is the Bluetooth-only legacy-logic product.  Unlike V2, BLE commands
     * are always local and do not require ESP-NOW BLE-gateway election. */
    esp_err_t err = dura_submit_command(DURA_COMMAND_SOURCE_BLE, command, response, response_len,
                                        pdMS_TO_TICKS(DURA_APP_COMMAND_TIMEOUT_MS));
    wrap_ble_app_response(command, response, response_len, err);
    return err;
}

#if !CONFIG_DURA_APP_QEMU_NO_RADIO
static esp_err_t dura_legacy_notify_snapshot(const dura_board_ui_snapshot_t *snapshot)
{
    if (snapshot == NULL) return ESP_ERR_INVALID_ARG;

    const dura_legacy_snapshot_t legacy = {
        .screen_id = snapshot->legacy_screen_id,
        .menu_id = snapshot->legacy_menu_id,
        .calibration_counts = snapshot->calibration_counts,
        .meter_total = snapshot->meter_total,
        .remaining_batch = snapshot->remaining_batch,
        .batch_amount = snapshot->batch_amount,
        .calibration_measured_amount = snapshot->calibration_measured_amount,
        .selected_units = snapshot->selected_units,
        .backlight_timeout_sec = snapshot->backlight_timeout_sec,
        .show_batch_total = snapshot->show_batch_total,
    };
    uint8_t frame[DURA_LEGACY_NOTIFICATION_MAX_LEN];
    size_t frame_len = 0;
    esp_err_t err = dura_legacy_serialize_snapshot(&legacy, frame, sizeof(frame), &frame_len);
    if (err != ESP_OK) return err;
    return gama_blefob_notify_binary(frame, frame_len);
}

static void dura_legacy_ble_ui_change_handler(const dura_board_ui_snapshot_t *snapshot,
                                              void *context)
{
    (void)context;
    esp_err_t err = dura_legacy_notify_snapshot(snapshot);
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "legacy UI notification failed 0x%x", err);
    }
}

static esp_err_t dura_legacy_ble_binary_write_handler(const uint8_t *data, size_t size,
                                                      void *user_ctx)
{
    (void)user_ctx;

    /* Counted legacy frames retain binary precedence, even when malformed bytes
     * happen to be printable. Text uses the generic shell and its registered
     * application fallback; product/RF behavior remains application-owned. */
    size_t text_size = size;
    const bool counted = data != NULL && size > 0u && (size_t)data[0] + 2u == size;
    if (data != NULL && !counted && text_size > 0u) {
        if (data[text_size - 1] == '\n') {
            --text_size;
            if (text_size && data[text_size - 1] == '\r') --text_size;
        } else if (data[text_size - 1] == '\r') --text_size;
    }
    bool printable_text = data != NULL && text_size > 0u && text_size < DURA_APP_COMMAND_MAX_LEN && !counted;
    for (size_t i = 0; printable_text && i < text_size; ++i) {
        printable_text = data[i] >= 0x20u && data[i] <= 0x7eu;
    }
    if (printable_text) {
        char command[DURA_APP_COMMAND_MAX_LEN];
        memcpy(command, data, text_size);
        command[text_size] = '\0';
        return gama_blefob_dispatch_text(command);
    }

    dura_legacy_action_t action;
    esp_err_t err = dura_legacy_parse(data, size, &action);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "legacy binary frame rejected len=%u: %s",
                 (unsigned)size, esp_err_to_name(err));
        if (data != NULL && size > 0u) {
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, size, ESP_LOG_WARN);
        }
        return err;
    }

    if (action.type == DURA_LEGACY_ACTION_BUTTON) {
        dura_button_t button;
        switch (action.button_position) {
        case DURA_LEGACY_BUTTON_POSITION_1: button = DURA_BUTTON_UP; break;
        case DURA_LEGACY_BUTTON_POSITION_2: button = DURA_BUTTON_DOWN; break;
        case DURA_LEGACY_BUTTON_POSITION_3: button = DURA_BUTTON_SELECT; break;
        case DURA_LEGACY_BUTTON_POSITION_4: button = DURA_BUTTON_BACK; break;
        default: return ESP_ERR_INVALID_ARG; /* Parser already rejects this. */
        }
        return dura_board_enqueue_button(button);
    }

    dura_board_ui_snapshot_t snapshot;
    err = dura_board_get_ui_snapshot(&snapshot);
    if (err != ESP_OK) return err;
    return dura_legacy_notify_snapshot(&snapshot);
}

static esp_err_t dura_legacy_ble_rf_button_handler(uint32_t button, bool active,
                                                   void *user_ctx)
{
    (void)user_ctx;
    if (!active) return ESP_OK;

    dura_button_t local_button;
    switch (button) {
    case DURA_LEGACY_BUTTON_POSITION_1: local_button = DURA_BUTTON_UP; break;
    case DURA_LEGACY_BUTTON_POSITION_2: local_button = DURA_BUTTON_DOWN; break;
    case DURA_LEGACY_BUTTON_POSITION_3: local_button = DURA_BUTTON_SELECT; break;
    case DURA_LEGACY_BUTTON_POSITION_4: local_button = DURA_BUTTON_BACK; break;
    default: return ESP_ERR_INVALID_ARG;
    }
    return dura_board_enqueue_button(local_button);
}
#endif

static void start_app_command_processor(void)
{
    s_app_command_queue = xQueueCreate(DURA_APP_COMMAND_QUEUE_LEN, sizeof(dura_command_request_t));
    s_app_command_reply_queue = xQueueCreate(1, sizeof(dura_command_reply_t));
    s_app_command_submit_mutex = xSemaphoreCreateMutex();
    if (s_app_command_queue == NULL || s_app_command_reply_queue == NULL ||
        s_app_command_submit_mutex == NULL) {
        ESP_LOGE(TAG, "app command routing primitives create failed");
        return;
    }

    BaseType_t task_ok = xTaskCreate(app_command_task, "dura_app_commands",
                                     DURA_APP_COMMAND_STACK_SIZE, NULL, 6, NULL);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "app command processor task create failed");
        vQueueDelete(s_app_command_queue);
        vQueueDelete(s_app_command_reply_queue);
        vSemaphoreDelete(s_app_command_submit_mutex);
        s_app_command_queue = NULL;
        s_app_command_reply_queue = NULL;
        s_app_command_submit_mutex = NULL;
        return;
    }
    ESP_LOGI(TAG, "app command processor started queue_len=%u stack=%u",
             (unsigned)DURA_APP_COMMAND_QUEUE_LEN, (unsigned)DURA_APP_COMMAND_STACK_SIZE);
}

static void serial_console_task(void *arg)
{
    (void)arg;

    char command[DURA_APP_COMMAND_MAX_LEN] = {0};
    char *response = heap_caps_calloc(1, DURA_APP_RESPONSE_MAX_LEN, MALLOC_CAP_8BIT);
    if (response == NULL) {
        ESP_LOGE(TAG, "serial console response buffer alloc failed");
        vTaskDelete(NULL);
        return;
    }
    size_t command_len = 0;

    printf("\r\nDura M3000 serial console ready. Type help for local meter commands.\r\n> ");
    fflush(stdout);

    while (true) {
        int input = fgetc(stdin);
        if (input == EOF) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        uint8_t ch = (uint8_t)input;
        if (ch == '\r' || ch == '\n') {
            if (command_len == 0) {
                printf("\r\n> ");
                fflush(stdout);
                continue;
            }

            command[command_len] = '\0';
            memset(response, 0, DURA_APP_RESPONSE_MAX_LEN);
            ESP_LOGI(TAG, "uart received command: %s", command);
            esp_err_t err = dura_submit_command(DURA_COMMAND_SOURCE_UART, command, response, DURA_APP_RESPONSE_MAX_LEN,
                                                pdMS_TO_TICKS(DURA_APP_COMMAND_TIMEOUT_MS));
            if (err == ESP_ERR_NOT_FOUND) {
                snprintf(response, DURA_APP_RESPONSE_MAX_LEN, "error: unknown command '%s'", command);
            } else if (err != ESP_OK && response[0] == '\0') {
                snprintf(response, DURA_APP_RESPONSE_MAX_LEN, "error: command failed 0x%x", err);
            }

            printf("\r\n%s\r\n> ", response[0] != '\0' ? response : "ok");
            fflush(stdout);
            command_len = 0;
            command[0] = '\0';
            continue;
        }

        if (ch == 0x08 || ch == 0x7f) {
            if (command_len > 0) {
                command_len--;
                command[command_len] = '\0';
                printf("\b \b");
                fflush(stdout);
            }
            continue;
        }

        if (ch >= 0x20 && ch <= 0x7e) {
            if (command_len < sizeof(command) - 1) {
                command[command_len++] = (char)ch;
                putchar((int)ch);
                fflush(stdout);
            }
        }
    }
}

static void start_serial_console(void)
{
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    BaseType_t task_ok = xTaskCreate(serial_console_task, "dura_serial_console",
                                     DURA_SERIAL_CONSOLE_STACK_SIZE, NULL, 5, NULL);
    if (task_ok != pdPASS) {
        log_heap_state("serial_console_task_create_failed");
        ESP_LOGE(TAG, "serial command console task create failed");
        return;
    }
    ESP_LOGI(TAG, "serial command console started stack=%u", (unsigned)DURA_SERIAL_CONSOLE_STACK_SIZE);
}

void app_main(void)
{
    init_nvs();
    ESP_ERROR_CHECK(dura_ui_config_init());
    load_system_assignment();
    log_module_info();

    ESP_ERROR_CHECK(dura_meter_init());
    ESP_ERROR_CHECK(dura_recipe_init());
    ESP_ERROR_CHECK(dura_coordinator_init());
    ESP_ERROR_CHECK(dura_peers_init());
    ESP_ERROR_CHECK(dura_board_init_power_sense());
#if !CONFIG_DURA_APP_M3000_LEGACY_ONLY
    ESP_ERROR_CHECK(dura_peers_register_event_callback(dura_ble_gateway_peer_event_handler, NULL));
#endif

#if CONFIG_DURA_BOARD_ENABLE_HARDWARE_TASK
    ESP_ERROR_CHECK(dura_board_init());
    ESP_ERROR_CHECK(dura_board_start());
    ESP_LOGI(TAG, "Dura board hardware task enabled; pump output %s",
             CONFIG_DURA_BOARD_ENABLE_PUMP_OUTPUT ? "enabled" : "forced off");
#else
    ESP_LOGI(TAG, "Dura board hardware task disabled; battery measurement unavailable");
#endif

    start_app_command_processor();
    start_serial_console();

#if CONFIG_DURA_APP_QEMU_NO_RADIO
    ESP_LOGW(TAG, "QEMU no-radio mode active; BLE shell init skipped");
#else
    if (CONFIG_DURA_BOARD_BLE_REQUIRES_EXT_POWER && !dura_board_ext_power_present()) {
        ESP_LOGW(TAG, "backup battery only; BLE init skipped because external power is absent");
    } else {
        gama_blefob_config_t ble_config;
        gama_blefob_config_init_default(&ble_config);
        ble_config.device_name = "Dura HF Meter M3000";
        ble_config.project_id = "DURA_M3000";
        ble_config.module_name = "Dura-M3000-BLE";
        ble_config.module_version = "proto-0007-0008";
        ble_config.firmware_version = DURA_METER_FIRMWARE_VERSION;
        ble_config.init_nvs_if_needed = false;
        ble_config.advertise_on_boot = true;
        ble_config.security_mode = GAMA_BLEFOB_SECURITY_PAIR_REQUIRED;
        ble_config.command_handler = dura_ble_command_handler;
        ble_config.user_activity_handler = dura_ble_user_activity_handler;
        ble_config.binary_write_handler = dura_legacy_ble_binary_write_handler;
        ble_config.binary_write_user_ctx = NULL;
        ble_config.rf_button_handler = dura_legacy_ble_rf_button_handler;
        ble_config.rf_button_user_ctx = NULL;
        ble_config.status_handler = dura_status_handler;
        ble_config.connected_handler = dura_ble_connected_handler;
        ble_config.disconnected_handler = dura_ble_disconnected_handler;

        ESP_ERROR_CHECK(gama_blefob_init(&ble_config));
#if CONFIG_DURA_BOARD_ENABLE_HARDWARE_TASK
        ESP_ERROR_CHECK(dura_board_set_ui_change_callback(
            dura_legacy_ble_ui_change_handler, NULL));
        dura_board_ui_snapshot_t initial_snapshot;
        ESP_ERROR_CHECK(dura_board_get_ui_snapshot(&initial_snapshot));
        esp_err_t initial_snapshot_err = dura_legacy_notify_snapshot(&initial_snapshot);
        if (initial_snapshot_err != ESP_OK && initial_snapshot_err != ESP_ERR_NOT_SUPPORTED &&
            initial_snapshot_err != ESP_ERR_INVALID_STATE) {
            ESP_ERROR_CHECK(initial_snapshot_err);
        }
#endif
        ESP_LOGI(TAG, "BLE shell and strict legacy Dura binary protocol initialized");
    }
#endif
#if CONFIG_DURA_APP_QEMU_NO_RADIO
    vTaskDelay(portMAX_DELAY);
#endif
}
