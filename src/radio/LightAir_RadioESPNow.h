#pragma once
#include "LightAir_RadioTransport.h"
#include <esp_now.h>
#include <WiFi.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#define ESPNOW_RECV_QUEUE   16   // receive ring-buffer depth
#define ESPNOW_MAX_PKT_LEN  250  // ESP-NOW hard limit

// ----------------------------------------------------------------
// LightAir_RadioESPNow
//
// ESP-NOW concrete transport.  Receives packets via the ESP-NOW
// callback (WiFi task, core 0) into a spinlock-protected ring buffer;
// LightAir_Radio drains it through receive() on the main loop.
//
// Usage:
//   LightAir_RadioESPNow  transport;
//   LightAir_Radio        radio(transport, playerId, token, role, team);
//   radio.begin();
// ----------------------------------------------------------------
class LightAir_RadioESPNow : public LightAir_RadioTransport {
public:
    // channel: Wi-Fi channel used for all ESP-NOW peers (default 1).
    explicit LightAir_RadioESPNow(uint8_t channel = 1);

    bool begin(const uint8_t selfMac[6]) override;
    bool send(const uint8_t mac[6], const uint8_t* data, size_t len) override;
    bool receive(uint8_t* data, int& len, int maxLen, int8_t& rssi)  override;

private:
    uint8_t _channel;

    // SPSC ring buffer — written by ESP-NOW callback (core 0),
    //                     read by receive()           (core 1).
    portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
    struct Entry { uint8_t data[ESPNOW_MAX_PKT_LEN]; int len; int8_t rssi; };
    Entry        _queue[ESPNOW_RECV_QUEUE];
    volatile int _head = 0;
    volatile int _tail = 0;

    static LightAir_RadioESPNow* _instance;
    static void onRecv(const esp_now_recv_info_t* recv_info, const uint8_t* data, int len);
};
