#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Call begin before startup can block; finish only after controller checks. */
esp_err_t dura_ota_boot_begin(void);
bool dura_ota_boot_pending(void);
esp_err_t dura_ota_boot_finish(bool controller_healthy);
bool dura_ota_update_allowed(void);
esp_err_t dura_ota_start_polling(void);
