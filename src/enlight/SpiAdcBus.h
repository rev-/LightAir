#pragma once
#include "driver/spi_master.h"
#include <stddef.h>
#include <stdint.h>

// Owns the SPI bus initialisation and the single spi_device_handle_t for the
// shared ADC IC (TI ADC128S102, 8-channel 12-bit; see player_pins.h).  Must be constructed and begin()-ed before any client
// (Enlight, sensors, SpiExternal) calls spi_bus_add_device() on the same host.
//
// Enlight calls getHandle() to obtain the handle for its bulk DMA transactions.
// Sensor objects call readChannel() for single-shot 12-bit reads.
// All transactions are serialised automatically by the ESP-IDF per-host bus lock.
class SpiAdcBus {
public:
    // Initialise the SPI host and register the ADC device.
    // max_dma_len: largest single DMA transfer in bytes (set to Enlight's _adcBufBytes).
    bool begin(spi_host_device_t host, int mosi, int miso, int clk,
               int cs, int clock_hz, size_t max_dma_len);

    // Enlight stores this handle and uses it directly for DMA transactions.
    spi_device_handle_t getHandle() const { return _dev; }

    // Single-shot 12-bit read.  cmd = channel-select command byte for the ADC IC.
    // Clocks the command twice, because the part is pipelined: the first frame
    // only returns the conversion already in flight.
    // Note the sensors sitting on these channels are powered from the AFE rail
    // — a read taken with Enlight's AFE_ON low returns nothing useful.
    // Returns false on SPI error.
    bool readChannel(uint8_t cmd, uint16_t& value_out);

private:
    spi_device_handle_t _dev = nullptr;
};
