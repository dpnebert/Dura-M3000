#include "dura_battery.h"

static dura_battery_measurement_t s_measurement;

#if CONFIG_DURA_BOARD_ENABLE_HARDWARE_TASK

#include "dura_board.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"

#define DURA_BATTERY_DISCARD_SAMPLES 4u
#define DURA_BATTERY_AVERAGE_SAMPLES 16u
#define DURA_BATTERY_SAMPLE_PERIOD_MS 5000u

static const char *TAG = "dura_battery";
static adc_oneshot_unit_handle_t s_adc_unit;
static adc_cali_handle_t s_adc_cali;
static adc_channel_t s_adc_channel;
static bool s_initialized;
static int64_t s_ready_at_us;
static int64_t s_next_sample_at_us;

static void set_unavailable(void)
{
    s_measurement = (dura_battery_measurement_t){0};
}

esp_err_t dura_battery_init(void)
{
    if (s_initialized) return ESP_OK;
    set_unavailable();

    adc_unit_t unit_id;
    adc_channel_t channel;
    esp_err_t err = adc_oneshot_io_to_channel(DURA_GPIO_BATTERY_ADC, &unit_id, &channel);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "battery ADC GPIO resolution unavailable: %s", esp_err_to_name(err));
        return err;
    }
    if (unit_id != ADC_UNIT_1 || channel != ADC_CHANNEL_0) {
        ESP_LOGW(TAG, "battery ADC route mismatch: gpio=%d unit=%d channel=%d",
                 (int)DURA_GPIO_BATTERY_ADC, (int)unit_id, (int)channel);
        return ESP_ERR_INVALID_STATE;
    }

    const adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = unit_id,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    err = adc_oneshot_new_unit(&unit_cfg, &s_adc_unit);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "battery ADC unit unavailable: %s", esp_err_to_name(err));
        return err;
    }

    const adc_oneshot_chan_cfg_t channel_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(s_adc_unit, channel, &channel_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "battery ADC channel unavailable: %s", esp_err_to_name(err));
        (void)adc_oneshot_del_unit(s_adc_unit);
        s_adc_unit = NULL;
        return err;
    }

    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = unit_id,
        .chan = channel,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "battery ADC calibration unavailable: %s", esp_err_to_name(err));
        (void)adc_oneshot_del_unit(s_adc_unit);
        s_adc_unit = NULL;
        return err;
    }

    s_adc_channel = channel;
    s_initialized = true;
    s_ready_at_us = esp_timer_get_time() + ((int64_t)DURA_BATTERY_ADC_WAKE_SETTLE_MS * 1000);
    s_next_sample_at_us = s_ready_at_us;
    ESP_LOGI(TAG,
             "battery ADC ready: gpio=%d unit=ADC1 channel=0 attenuation=12dB discard=%u average=%u period_ms=%u",
             (int)DURA_GPIO_BATTERY_ADC, DURA_BATTERY_DISCARD_SAMPLES,
             DURA_BATTERY_AVERAGE_SAMPLES, DURA_BATTERY_SAMPLE_PERIOD_MS);
    return ESP_OK;
}

void dura_battery_poll(void)
{
    if (!s_initialized) {
        set_unavailable();
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (now < s_ready_at_us || now < s_next_sample_at_us) return;
    s_next_sample_at_us = now + ((int64_t)DURA_BATTERY_SAMPLE_PERIOD_MS * 1000);

    int raw;
    for (uint32_t i = 0; i < DURA_BATTERY_DISCARD_SAMPLES; ++i) {
        if (adc_oneshot_read(s_adc_unit, s_adc_channel, &raw) != ESP_OK) {
            set_unavailable();
            ESP_LOGW(TAG, "battery ADC discard read failed; measurement unavailable");
            return;
        }
    }

    uint64_t pad_mv_sum = 0u;
    for (uint32_t i = 0; i < DURA_BATTERY_AVERAGE_SAMPLES; ++i) {
        int pad_mv;
        esp_err_t err = adc_oneshot_read(s_adc_unit, s_adc_channel, &raw);
        if (err == ESP_OK) err = adc_cali_raw_to_voltage(s_adc_cali, raw, &pad_mv);
        if (err != ESP_OK || pad_mv < 0) {
            set_unavailable();
            ESP_LOGW(TAG, "battery ADC calibrated read failed; measurement unavailable");
            return;
        }
        pad_mv_sum += (uint32_t)pad_mv;
    }

    const uint32_t average_pad_mv = (uint32_t)(pad_mv_sum / DURA_BATTERY_AVERAGE_SAMPLES);
    const uint32_t battery_mv = dura_battery_apply_divider_mv(average_pad_mv);
    s_measurement.valid = true;
    s_measurement.millivolts = battery_mv;
    s_measurement.percent = dura_battery_percent_from_mv(battery_mv);
}

#else

esp_err_t dura_battery_init(void)
{
    s_measurement = (dura_battery_measurement_t){0};
    return ESP_ERR_NOT_SUPPORTED;
}

void dura_battery_poll(void)
{
    s_measurement = (dura_battery_measurement_t){0};
}

#endif

dura_battery_measurement_t dura_battery_get_measurement(void)
{
    return s_measurement;
}
