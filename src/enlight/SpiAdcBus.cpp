#include "SpiAdcBus.h"
#include <string.h>
#include "esp_log.h"

static const char* TAG = "SpiAdcBus";

bool SpiAdcBus::begin(spi_host_device_t host, int mosi, int miso, int clk,
                       int cs, int clock_hz, size_t max_dma_len) {
    spi_bus_config_t bc = {};
    bc.mosi_io_num    = mosi;
    bc.miso_io_num    = miso;
    bc.sclk_io_num    = clk;
    bc.quadwp_io_num  = -1;
    bc.quadhd_io_num  = -1;
    bc.max_transfer_sz = (int)max_dma_len;

    if (spi_bus_initialize(host, &bc, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "bus init failed");
        return false;
    }

    spi_device_interface_config_t dc = {};
    dc.clock_speed_hz = clock_hz;
    dc.mode           = 0;
    dc.spics_io_num   = cs;
    dc.queue_size     = 1;

    if (spi_bus_add_device(host, &dc, &_dev) != ESP_OK) {
        ESP_LOGE(TAG, "add device failed");
        return false;
    }
    return true;
}

bool SpiAdcBus::readChannel(uint8_t cmd, uint16_t& value_out) {
    // The ADC is pipelined: the channel selected in one 16-clock frame is
    // converted and shifted out during the *next* frame.  Enlight compensates
    // for the same delay with ADC_PIPELINE_DELAY when it decodes its DMA
    // buffer; here the command is simply clocked twice in one transaction —
    // the first frame returns whatever conversion was already in flight, the
    // second one carries the channel asked for.
    __attribute__((aligned(4))) uint8_t tx[4] = { cmd, 0x00, cmd, 0x00 };
    __attribute__((aligned(4))) uint8_t rx[4] = { 0,   0,   0,   0    };

    spi_transaction_t t = {};
    t.length    = 32;
    t.rxlength  = 32;
    t.tx_buffer = tx;
    t.rx_buffer = rx;

    if (spi_device_transmit(_dev, &t) != ESP_OK) return false;
    value_out = (((uint16_t)rx[2] << 8) | rx[3]) & 0x0FFF;
    return true;
}
