#pragma once
#include "esp_err.h"
#include "esp_log.h"
#define ESP_RETURN_ON_FALSE(cond, code, ...) do { if (!(cond)) return (code); } while(0)
#define ESP_RETURN_ON_ERROR(expr, ...) do { int err_ = (expr); if (err_ != ESP_OK) return err_; } while(0)
