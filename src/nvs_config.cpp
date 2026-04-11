#include "nvs_config.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char* TAG = "EnlightNVS";

#define NVS_GET_U32(h,k,d,def) \
    if(nvs_get_u32(h,k,&(d))!=ESP_OK){(d)=(def); \
    ESP_LOGW(TAG,"%s missing, default %lu",(k),(unsigned long)(def));}
#define NVS_GET_FLOAT(h,k,d,def) \
    {size_t _sz=sizeof(float); float _f; \
    if(nvs_get_blob(h,k,&_f,&_sz)!=ESP_OK){(d)=(def); \
    ESP_LOGW(TAG,"%s missing, default %.4f",(k),(double)(def));}else{(d)=_f;}}

static bool nvs_init_once() {
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS corrupt -- erasing");
        if (nvs_flash_erase() != ESP_OK) return false;
        r = nvs_flash_init();
    }
    return r == ESP_OK;
}

bool player_config_load(PlayerConfig& cfg) {
    if (!nvs_init_once()) return false;
    cfg.id       = 0xFF;
    cfg.hardware = DeviceHardware::PLAYER;  // safe default: boot as player
    nvs_handle_t h;
    if (nvs_open(CALIB_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return true; // defaults OK
    nvs_get_u8(h, CAL_KEY_ID, &cfg.id);
    uint8_t hw = (uint8_t)DeviceHardware::PLAYER;
    nvs_get_u8(h, CAL_KEY_HARDWARE, &hw);
    cfg.hardware = (DeviceHardware)hw;
    nvs_close(h);
    return true;
}

bool player_config_save(const PlayerConfig& cfg) {
    if (!nvs_init_once()) return false;
    nvs_handle_t h;
    if (nvs_open(CALIB_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_set_u8(h, CAL_KEY_ID,       cfg.id);
    nvs_set_u8(h, CAL_KEY_HARDWARE, (uint8_t)cfg.hardware);
    esp_err_t e = nvs_commit(h); nvs_close(h);
    return e == ESP_OK;
}

bool player_config_save_id(uint8_t id) {
    if (!nvs_init_once()) return false;
    nvs_handle_t h;
    if (nvs_open(CALIB_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_set_u8(h, CAL_KEY_ID, id);
    esp_err_t e = nvs_commit(h); nvs_close(h);
    return e == ESP_OK;
}

bool player_config_save_hardware(DeviceHardware hw) {
    if (!nvs_init_once()) return false;
    nvs_handle_t h;
    if (nvs_open(CALIB_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_set_u8(h, CAL_KEY_HARDWARE, (uint8_t)hw);
    esp_err_t e = nvs_commit(h); nvs_close(h);
    return e == ESP_OK;
}

bool enlight_calib_load(EnlightCalib& cal) {
    if (!nvs_init_once()) return false;
    nvs_handle_t h;
    bool ok = (nvs_open(CALIB_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK);
    if (ok) {
        NVS_GET_U32  (h, CAL_KEY_RCAL,           cal.rcal,        0);
        NVS_GET_U32  (h, CAL_KEY_GCAL,           cal.gcal,        0);
        NVS_GET_U32  (h, CAL_KEY_BCAL,           cal.bcal,        0);
        NVS_GET_U32  (h, CAL_KEY_RCAL_NEAR,      cal.rcalNear,    0);
        NVS_GET_U32  (h, CAL_KEY_GCAL_NEAR,      cal.gcalNear,    0);
        NVS_GET_U32  (h, CAL_KEY_BCAL_NEAR,      cal.bcalNear,    0);
        NVS_GET_U32  (h, CAL_KEY_LIMPOW,         cal.limpow,      0);
        NVS_GET_U32  (h, CAL_KEY_PHASE_OFF,      cal.phaseOff,    0);
        NVS_GET_FLOAT(h, CAL_KEY_RFACT,          cal.rfact,       1.0f);
        NVS_GET_FLOAT(h, CAL_KEY_BFACT,          cal.bfact,       1.0f);
        NVS_GET_FLOAT(h, CAL_KEY_NEAR_RATIO_MAX, cal.nearRatioMax, 1e9f);
        NVS_GET_U32  (h, CAL_KEY_MAX_NEAR_WHITE, cal.maxNearWhite, 0);
        NVS_GET_U32  (h, CAL_KEY_MAX_FAR_WHITE,  cal.maxFarWhite,  0);
        nvs_close(h);
    } else {
        cal = {};
        cal.limpow = 0;
        cal.rfact = cal.bfact = 1.0f;
        cal.nearRatioMax = 1e9f;
        cal.maxNearWhite = 0;
        cal.maxFarWhite  = 0;
        ESP_LOGW(TAG, "Calibration namespace missing -- using sentinels");
    }
    return true;
}

bool enlight_calib_save(const EnlightCalib& cal) {
    if (!nvs_init_once()) return false;
    nvs_handle_t h;
    if (nvs_open(CALIB_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_set_u32 (h, CAL_KEY_RCAL,           cal.rcal);
    nvs_set_u32 (h, CAL_KEY_GCAL,           cal.gcal);
    nvs_set_u32 (h, CAL_KEY_BCAL,           cal.bcal);
    nvs_set_u32 (h, CAL_KEY_RCAL_NEAR,      cal.rcalNear);
    nvs_set_u32 (h, CAL_KEY_GCAL_NEAR,      cal.gcalNear);
    nvs_set_u32 (h, CAL_KEY_BCAL_NEAR,      cal.bcalNear);
    nvs_set_u32 (h, CAL_KEY_LIMPOW,         cal.limpow);
    nvs_set_u32 (h, CAL_KEY_PHASE_OFF,      cal.phaseOff);
    nvs_set_blob(h, CAL_KEY_RFACT,          &cal.rfact,        sizeof(float));
    nvs_set_blob(h, CAL_KEY_BFACT,          &cal.bfact,        sizeof(float));
    nvs_set_blob(h, CAL_KEY_NEAR_RATIO_MAX, &cal.nearRatioMax, sizeof(float));
    nvs_set_u32 (h, CAL_KEY_MAX_NEAR_WHITE, cal.maxNearWhite);
    nvs_set_u32 (h, CAL_KEY_MAX_FAR_WHITE,  cal.maxFarWhite);
    esp_err_t e = nvs_commit(h); nvs_close(h);
    return e == ESP_OK;
}
