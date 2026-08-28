#include "SpiExternal.h"
#include <string.h>
#include "esp_log.h"

static const char* TAG = "SpiExternal";

bool SpiExternal::begin(spi_host_device_t host, int cs_pin, int clock_hz,
                         uint8_t spi_mode) {
    if (cs_pin < 0) return true;  // no hardware assigned yet — no-op

    spi_device_interface_config_t dc = {};
    dc.clock_speed_hz = clock_hz;
    dc.mode           = spi_mode;
    dc.spics_io_num   = cs_pin;
    dc.queue_size     = 1;

    if (spi_bus_add_device(host, &dc, &_dev) != ESP_OK) {
        ESP_LOGE(TAG, "add device failed");
        return false;
    }
    return true;
}

bool SpiExternal::write(const uint8_t* data, size_t len) {
    if (!_dev) return false;

    spi_transaction_t t = {};
    t.length    = len * 8;
    t.tx_buffer = data;

    return spi_device_transmit(_dev, &t) == ESP_OK;
}

bool SpiExternal::transfer(const uint8_t* tx, uint8_t* rx, size_t len) {
    if (!_dev) return false;

    spi_transaction_t t = {};
    t.length    = len * 8;
    t.rxlength  = len * 8;
    t.tx_buffer = tx;
    t.rx_buffer = rx;

    return spi_device_transmit(_dev, &t) == ESP_OK;
}
