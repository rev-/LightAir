#pragma once
#include "nvs_flash.h"
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
enum nvs_open_mode_t { NVS_READONLY, NVS_READWRITE };
static inline esp_err_t nvs_open(const char*, nvs_open_mode_t, nvs_handle_t*) { return 1; }
static inline esp_err_t nvs_set_u8(nvs_handle_t, const char*, uint8_t) { return 0; }
static inline esp_err_t nvs_get_u8(nvs_handle_t, const char*, uint8_t*) { return 0; }
static inline esp_err_t nvs_commit(nvs_handle_t) { return 0; }
static inline void nvs_close(nvs_handle_t) {}
static inline esp_err_t nvs_set_u32(nvs_handle_t, const char*, uint32_t) { return 0; }
static inline esp_err_t nvs_get_u32(nvs_handle_t, const char*, uint32_t*) { return 0; }
