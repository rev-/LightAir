#pragma once
#include "driver/spi_master.h"
#include <stddef.h>
#include <stdint.h>

// Second SPI2 device with a dedicated CS pin.
//
// The SPI2 bus must already be initialised by SpiAdcBus::begin() before
// calling begin() here.  This class only calls spi_bus_add_device() — NOT
// spi_bus_initialize() — so the host and shared MOSI/CLK lines are reused.
// The ESP-IDF per-host bus lock serialises transactions from this device
// with those from Enlight and SpiAdcBus::readChannel() automatically.
//
// begin() is a no-op when cs_pin == -1 (placeholder for future hardware).
class SpiExternal {
public:
    bool begin(spi_host_device_t host, int cs_pin, int clock_hz,
               uint8_t spi_mode = 0);

    // Half-duplex write.  Returns false if begin() was skipped or SPI error.
    bool write(const uint8_t* data, size_t len);

    // Full-duplex transfer.  tx and rx must both be len bytes.
    bool transfer(const uint8_t* tx, uint8_t* rx, size_t len);

private:
    spi_device_handle_t _dev = nullptr;
};
