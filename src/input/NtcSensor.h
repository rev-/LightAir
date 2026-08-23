#pragma once
#include "SpiAdcSensor.h"
#include <math.h>

// NTC thermistor temperature sensor using a resistive voltage divider.
//
// Two supply configurations are supported:
//   Constant supply  — NTC divider powered from the same rail as the ADC reference
//                      (supply_v == adc_ref, the common case; vref cancels out).
//   Variable supply  — NTC divider powered from a rail that differs from the ADC
//                      reference (e.g. battery voltage).  Pass a pointer to a float
//                      that is updated externally before each read().
//
// Circuit topology (ntc_high = true, default):
//   Vcc ── [NTC] ── ADC_pin ── [R_fixed] ── GND
// Circuit topology (ntc_high = false):
//   Vcc ── [R_fixed] ── ADC_pin ── [NTC] ── GND
//
// Conversion formula (Steinhart-Hart B-parameter equation):
//   R_ntc = R_fixed * (supply/adc_ref * 4095 - raw) / raw   [ntc_high=true]
//   T_K   = 1 / (1/T0_K + ln(R_ntc/R0) / beta)
//   T_C   = T_K - 273.15
class NtcSensor : public SpiAdcSensor {
public:
    // Constant supply (equals adc_ref by default — vref cancels in R_ntc formula).
    NtcSensor(float r_fixed, float r0, float beta,
              float supply_v = 3.3f, float adc_ref = 3.3f,
              float t0_c = 25.0f, bool ntc_high = true)
        : _rFixed(r_fixed), _r0(r0), _beta(beta),
          _t0k(t0_c + 273.15f), _adcRef(adc_ref),
          _supplyConst(supply_v), _psupply(nullptr), _ntcHigh(ntc_high) {}

    // Variable supply (battery-referenced).  psupply must remain valid for lifetime.
    NtcSensor(float r_fixed, float r0, float beta,
              const float* psupply, float adc_ref = 3.3f,
              float t0_c = 25.0f, bool ntc_high = true)
        : _rFixed(r_fixed), _r0(r0), _beta(beta),
          _t0k(t0_c + 273.15f), _adcRef(adc_ref),
          _supplyConst(0.0f), _psupply(psupply), _ntcHigh(ntc_high) {}

protected:
    float convert(uint16_t raw) const override {
        if (raw == 0 || raw == 4095) return NAN;
        float supply = _psupply ? *_psupply : _supplyConst;
        if (supply <= 0.0f || _adcRef <= 0.0f) return NAN;
        // Normalised full-scale counts adjusted for supply/reference ratio.
        float k = supply / _adcRef * 4095.0f;
        float r_ntc = _ntcHigh
            ? _rFixed * (k - (float)raw) / (float)raw
            : _rFixed * (float)raw / (k - (float)raw);
        if (r_ntc <= 0.0f) return NAN;
        float t_k = 1.0f / (1.0f / _t0k + logf(r_ntc / _r0) / _beta);
        return t_k - 273.15f;
    }

private:
    float        _rFixed, _r0, _beta, _t0k, _adcRef, _supplyConst;
    const float* _psupply;
    bool         _ntcHigh;
};
