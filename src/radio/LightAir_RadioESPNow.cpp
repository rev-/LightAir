#include "LightAir_RadioESPNow.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include <string.h>

static const char* TAG = "ESPNow";

LightAir_RadioESPNow* LightAir_RadioESPNow::_instance = nullptr;

// ----------------------------------------------------------------
LightAir_RadioESPNow::LightAir_RadioESPNow(uint8_t channel)
    : _channel(channel) {}

bool LightAir_RadioESPNow::begin(const uint8_t selfMac[6]) {
    _instance = this;

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    if (esp_wifi_set_mac(WIFI_IF_STA, selfMac) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set MAC");
        return false;
    }
    ESP_LOGI(TAG, "MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             selfMac[0], selfMac[1], selfMac[2],
             selfMac[3], selfMac[4], selfMac[5]);

    if (esp_now_init() != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed");
        return false;
    }
    esp_now_register_recv_cb(onRecv);
    return true;
}

bool LightAir_RadioESPNow::send(const uint8_t mac[6],
                                 const uint8_t* data, size_t len) {
    if (!esp_now_is_peer_exist(mac)) {
        esp_now_peer_info_t info = {};
        memcpy(info.peer_addr, mac, 6);
        info.channel = _channel;
        info.encrypt = false;
        if (esp_now_add_peer(&info) != ESP_OK) {
            ESP_LOGW(TAG, "add_peer failed");
            return false;
        }
    }
    esp_err_t e = esp_now_send(mac, data, len);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_send err=%d", e);
        return false;
    }
    return true;
}

bool LightAir_RadioESPNow::receive(uint8_t* data, int& len, int maxLen, int8_t& rssi) {
    taskENTER_CRITICAL(&_mux);
    bool avail = (_head != _tail);
    if (avail) {
        int copyLen = (_queue[_tail].len < maxLen) ? _queue[_tail].len : maxLen;
        memcpy(data, _queue[_tail].data, copyLen);
        len  = copyLen;
        rssi = _queue[_tail].rssi;
        _tail = (_tail + 1) % ESPNOW_RECV_QUEUE;
    }
    taskEXIT_CRITICAL(&_mux);
    return avail;
}

// ----------------------------------------------------------------
// Static ESP-NOW callback — WiFi task, core 0
// ----------------------------------------------------------------
void LightAir_RadioESPNow::onRecv(const esp_now_recv_info_t* recv_info,
                                   const uint8_t* data, int len) {
    LightAir_RadioESPNow* self = _instance;
    if (!self || len <= 0 || len > ESPNOW_MAX_PKT_LEN) return;

    int8_t rssi = (recv_info && recv_info->rx_ctrl)
                  ? (int8_t)recv_info->rx_ctrl->rssi
                  : 0;

    taskENTER_CRITICAL(&self->_mux);
    int nextHead = (self->_head + 1) % ESPNOW_RECV_QUEUE;
    if (nextHead != self->_tail) {           // drop silently if full
        memcpy(self->_queue[self->_head].data, data, len);
        self->_queue[self->_head].len  = len;
        self->_queue[self->_head].rssi = rssi;
        self->_head = nextHead;
    }
    taskEXIT_CRITICAL(&self->_mux);
}
