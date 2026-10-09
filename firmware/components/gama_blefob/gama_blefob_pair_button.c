#include "gama_blefob_internal.h"

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
void pair_button_task(void *arg)
{
	(void)arg;

#if CONFIG_BLE_SHELL_PAIR_BUTTON_GPIO >= 0
	int last_level;
	int stable_level;
	TickType_t last_change_tick;
	const gpio_num_t pair_gpio = (gpio_num_t)CONFIG_BLE_SHELL_PAIR_BUTTON_GPIO;

	gpio_config_t io_conf = {
		.pin_bit_mask = 1ULL << CONFIG_BLE_SHELL_PAIR_BUTTON_GPIO,
		.mode = GPIO_MODE_INPUT,
#ifdef CONFIG_BLE_SHELL_PAIR_BUTTON_PULLUP
		.pull_up_en = GPIO_PULLUP_ENABLE,
#else
		.pull_up_en = GPIO_PULLUP_DISABLE,
#endif
#ifdef CONFIG_BLE_SHELL_PAIR_BUTTON_PULLDOWN
		.pull_down_en = GPIO_PULLDOWN_ENABLE,
#else
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
#endif
		.intr_type = GPIO_INTR_DISABLE,
	};
	ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&io_conf));
	last_level = gpio_get_level(pair_gpio);
	stable_level = last_level;
	last_change_tick = xTaskGetTickCount();

	while (1) {
		int level = gpio_get_level(pair_gpio);
		if (level != last_level) {
			last_level = level;
			last_change_tick = xTaskGetTickCount();
		}
		if (level != stable_level && (xTaskGetTickCount() - last_change_tick) > pdMS_TO_TICKS(CONFIG_BLE_SHELL_PAIR_BUTTON_DEBOUNCE_MS)) {
			stable_level = level;
			bool pressed = CONFIG_BLE_SHELL_PAIR_BUTTON_ACTIVE_LOW ? (stable_level == 0) : (stable_level != 0);
			if (pressed) {
				ble_shell_open_pairing_window(s_cfg.pair_window_seconds, "pair button opened BLE window");
			}
		}
		vTaskDelay(pdMS_TO_TICKS(CONFIG_BLE_SHELL_PAIR_BUTTON_POLL_MS));
	}
#else
	ESP_LOGI(TAG, "Pair button disabled; set CONFIG_BLE_SHELL_PAIR_BUTTON_GPIO to enable it");
	vTaskDelete(NULL);
#endif
}
#endif
