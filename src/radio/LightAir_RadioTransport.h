#pragma once
#include <stdint.h>
#include <stddef.h>

// ----------------------------------------------------------------
// LightAir_RadioTransport
//
// Platform-agnostic transport interface used by LightAir_Radio.
// Implementations handle physical layer details (MAC management,
// hardware init, packet buffering).  The managing layer works
// exclusively through this interface.
//
// Concrete implementations:
//   LightAir_RadioESPNow   — ESP-NOW on ESP32
//   (test doubles, BLE, UART, ...)
// ----------------------------------------------------------------
class LightAir_RadioTransport {
public:
    virtual ~LightAir_RadioTransport() = default;

    // Initialise the physical layer; set selfMac as the device address.
    virtual bool begin(const uint8_t selfMac[6]) = 0;

    // Send len bytes to mac.  Peer registration is handled internally.
    // Returns false on error.
    virtual bool send(const uint8_t mac[6],
                      const uint8_t* data, size_t len) = 0;

    // Pull the next received packet into data (capacity maxLen bytes).
    // Sets len to the number of bytes written.
    // Sets rssi to the measured signal strength (dBm); 0 if unavailable.
    // Returns false when the receive queue is empty.
    virtual bool receive(uint8_t* data, int& len, int maxLen, int8_t& rssi) = 0;
};
