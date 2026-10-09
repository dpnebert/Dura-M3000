#include "dura_ui_config.h"
#include "dura_json_builder.h"

#include <stdbool.h>
#include <stdio.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "sdkconfig.h"

#define DURA_UI_CONFIG_NVS_NAMESPACE "dura_ui"
#define DURA_UI_CONFIG_NVS_KEY "runtime"
#define DURA_UI_CONFIG_MAGIC 0x44554943u /* DUIC */
#define DURA_UI_CONFIG_SCHEMA_VERSION 1u

static const char *TAG = "dura_ui_config";
static portMUX_TYPE s_config_mux = portMUX_INITIALIZER_UNLOCKED;
static dura_ui_runtime_config_t s_config;
static bool s_initialized;

typedef struct {
    uint32_t magic;
    uint16_t schema_version;
    uint16_t payload_size;
    dura_ui_runtime_config_t config;
} dura_ui_persisted_config_t;

static dura_ui_runtime_config_t compile_time_defaults(void)
{
    const dura_ui_runtime_config_t defaults = {
        .low_battery_threshold_percent = CONFIG_DURA_BOARD_LOW_BATTERY_THRESHOLD_PERCENT,
        .button_hold_max_rate_x = CONFIG_DURA_BOARD_BUTTON_HOLD_MAX_RATE_X,
        .reserved = 0u,
        .low_battery_blink_on_ms = CONFIG_DURA_BOARD_LOW_BATTERY_BLINK_ON_MS,
        .low_battery_blink_off_ms = CONFIG_DURA_BOARD_LOW_BATTERY_BLINK_OFF_MS,
    };
    return defaults;
}

esp_err_t dura_ui_config_validate(const dura_ui_runtime_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is null");
    ESP_RETURN_ON_FALSE(
        config->low_battery_threshold_percent <= DURA_UI_CONFIG_LOW_BATTERY_THRESHOLD_PERCENT_MAX,
        ESP_ERR_INVALID_ARG, TAG, "low-battery threshold out of range");
    ESP_RETURN_ON_FALSE(
        config->low_battery_blink_on_ms >= DURA_UI_CONFIG_BLINK_MS_MIN &&
        config->low_battery_blink_on_ms <= DURA_UI_CONFIG_BLINK_MS_MAX,
        ESP_ERR_INVALID_ARG, TAG, "blink-on duration out of range");
    ESP_RETURN_ON_FALSE(
        config->low_battery_blink_off_ms >= DURA_UI_CONFIG_BLINK_MS_MIN &&
        config->low_battery_blink_off_ms <= DURA_UI_CONFIG_BLINK_MS_MAX,
        ESP_ERR_INVALID_ARG, TAG, "blink-off duration out of range");
    ESP_RETURN_ON_FALSE(
        config->button_hold_max_rate_x >= DURA_UI_CONFIG_BUTTON_HOLD_MAX_RATE_X_MIN &&
        config->button_hold_max_rate_x <= DURA_UI_CONFIG_BUTTON_HOLD_MAX_RATE_X_MAX,
        ESP_ERR_INVALID_ARG, TAG, "button hold maximum rate out of range");
    return ESP_OK;
}

esp_err_t dura_ui_config_init(void)
{
    dura_ui_runtime_config_t loaded = compile_time_defaults();
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(DURA_UI_CONFIG_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        dura_ui_persisted_config_t persisted = {0};
        size_t size = sizeof(persisted);
        err = nvs_get_blob(nvs, DURA_UI_CONFIG_NVS_KEY, &persisted, &size);
        nvs_close(nvs);
        if (err == ESP_OK) {
            if (size == sizeof(persisted) &&
                persisted.magic == DURA_UI_CONFIG_MAGIC &&
                persisted.schema_version == DURA_UI_CONFIG_SCHEMA_VERSION &&
                persisted.payload_size == sizeof(persisted.config) &&
                dura_ui_config_validate(&persisted.config) == ESP_OK) {
                loaded = persisted.config;
            } else {
                ESP_LOGW(TAG, "persisted UI config invalid; using sdkconfig defaults");
            }
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "UI config read failed (%s); using sdkconfig defaults",
                     esp_err_to_name(err));
        }
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "UI config namespace open failed (%s); using sdkconfig defaults",
                 esp_err_to_name(err));
    }

    portENTER_CRITICAL(&s_config_mux);
    s_config = loaded;
    s_initialized = true;
    portEXIT_CRITICAL(&s_config_mux);
    ESP_LOGI(TAG, "UI config: threshold=%u%% blink=%u/%u ms max_rate=%ux",
             (unsigned)loaded.low_battery_threshold_percent,
             (unsigned)loaded.low_battery_blink_on_ms,
             (unsigned)loaded.low_battery_blink_off_ms,
             (unsigned)loaded.button_hold_max_rate_x);
    return ESP_OK;
}

esp_err_t dura_ui_config_get(dura_ui_runtime_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is null");
    portENTER_CRITICAL(&s_config_mux);
    if (!s_initialized) {
        portEXIT_CRITICAL(&s_config_mux);
        return ESP_ERR_INVALID_STATE;
    }
    *config = s_config;
    portEXIT_CRITICAL(&s_config_mux);
    return ESP_OK;
}

esp_err_t dura_ui_config_set(const dura_ui_runtime_config_t *config)
{
    ESP_RETURN_ON_ERROR(dura_ui_config_validate(config), TAG, "invalid UI config");

    portENTER_CRITICAL(&s_config_mux);
    const bool initialized = s_initialized;
    portEXIT_CRITICAL(&s_config_mux);
    ESP_RETURN_ON_FALSE(initialized, ESP_ERR_INVALID_STATE, TAG, "UI config not initialized");

    const dura_ui_persisted_config_t persisted = {
        .magic = DURA_UI_CONFIG_MAGIC,
        .schema_version = DURA_UI_CONFIG_SCHEMA_VERSION,
        .payload_size = sizeof(*config),
        .config = *config,
    };
    nvs_handle_t nvs = 0;
    ESP_RETURN_ON_ERROR(nvs_open(DURA_UI_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &nvs),
                        TAG, "open UI config NVS");
    esp_err_t err = nvs_set_blob(nvs, DURA_UI_CONFIG_NVS_KEY, &persisted, sizeof(persisted));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    ESP_RETURN_ON_ERROR(err, TAG, "persist UI config");

    portENTER_CRITICAL(&s_config_mux);
    s_config = *config;
    portEXIT_CRITICAL(&s_config_mux);
    return ESP_OK;
}

esp_err_t dura_ui_config_format_json(char *response, size_t response_len)
{
    ESP_RETURN_ON_FALSE(response != NULL && response_len > 0u,
                        ESP_ERR_INVALID_ARG, TAG, "response buffer invalid");
    dura_ui_runtime_config_t config;
    ESP_RETURN_ON_ERROR(dura_ui_config_get(&config), TAG, "get UI config");
    const int written = dura_json_snprintf(
        response, response_len,
        "{\"low_battery_threshold_percent\":%u,"
        "\"low_battery_blink_on_ms\":%u,"
        "\"low_battery_blink_off_ms\":%u,"
        "\"button_hold_max_rate_x\":%u}",
        (unsigned)config.low_battery_threshold_percent,
        (unsigned)config.low_battery_blink_on_ms,
        (unsigned)config.low_battery_blink_off_ms,
        (unsigned)config.button_hold_max_rate_x);
    return dura_json_result(response, response_len, written);
}
