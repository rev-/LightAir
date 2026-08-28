#pragma once
#include <stdint.h>
#include <stddef.h>

// Enough of the ESP-IDF SPI master API for the host build: the firmware
// sources that reach the host suite (SpiAdcBus, SpiAdcSensor) must compile
// and link, but no transfer ever happens — every call is a no-op that
// reports success and leaves the receive buffer untouched.

#ifndef ESP_OK
typedef int esp_err_t;
#define ESP_OK 0
#endif

typedef int  spi_host_device_t;
typedef void* spi_device_handle_t;

#define SPI_DMA_CH_AUTO 3

typedef struct {
    int mosi_io_num;
    int miso_io_num;
    int sclk_io_num;
    int quadwp_io_num;
    int quadhd_io_num;
    int max_transfer_sz;
} spi_bus_config_t;

typedef struct {
    int clock_speed_hz;
    int mode;
    int spics_io_num;
    int queue_size;
} spi_device_interface_config_t;

typedef struct {
    int         flags;
    size_t      length;
    size_t      rxlength;
    const void* tx_buffer;
    void*       rx_buffer;
} spi_transaction_t;

static inline esp_err_t spi_bus_initialize(spi_host_device_t,
                                           const spi_bus_config_t*, int) { return ESP_OK; }
static inline esp_err_t spi_bus_add_device(spi_host_device_t,
                                           const spi_device_interface_config_t*,
                                           spi_device_handle_t* out) {
    if (out) *out = (spi_device_handle_t)1;
    return ESP_OK;
}
static inline esp_err_t spi_device_transmit(spi_device_handle_t,
                                            spi_transaction_t*) { return ESP_OK; }
