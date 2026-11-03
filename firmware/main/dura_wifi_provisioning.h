#pragma once

#include "esp_err.h"

esp_err_t dura_wifi_init_and_provision(void);
esp_err_t dura_wifi_reset_provisioning(void);
esp_err_t dura_wifi_start_provisioning(void);
