#include "SpiAdcSensor.h"
#include <math.h>

void SpiAdcSensor::begin(SpiAdcBus& bus, uint8_t cmd, const char* unit) {
    _bus  = &bus;
    _cmd  = cmd;
    _unit = unit;
}

bool SpiAdcSensor::read(float& out) const {
    if (!_bus) return false;
    uint16_t raw;
    if (!_bus->readChannel(_cmd, raw)) return false;
    float v = convert(raw);
    if (isnan(v)) return false;
    out = v;
    return true;
}
