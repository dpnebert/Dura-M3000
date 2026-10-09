#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
typedef int nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
int nvs_open(const char*, int, nvs_handle_t*);
int nvs_get_u32(nvs_handle_t, const char*, uint32_t*);
int nvs_get_u8(nvs_handle_t, const char*, uint8_t*);
int nvs_get_blob(nvs_handle_t, const char*, void*, size_t*);
int nvs_set_u32(nvs_handle_t, const char*, uint32_t);
int nvs_set_u8(nvs_handle_t, const char*, uint8_t);
int nvs_set_blob(nvs_handle_t, const char*, const void*, size_t);
int nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
