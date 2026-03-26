#pragma once

// ================================================================
// totem_pins.h — hardware pin assignments for the LightAir totem PCB.
//
// Totem board:
//   WS2812B LED strip (NeoPixel-compatible, single-wire data).
//   Discrete RGB LED with common enable (common-cathode or anode).
// ================================================================

// ---- WS2812B LED strip ----
static constexpr int     TOTEM_PIN_DATA = 13;  // WS2812B data line
static constexpr uint8_t TOTEM_NUM_LEDS = 13;  // strip length

// ---- Discrete RGB indicator LED ----
static constexpr int TOTEM_PIN_COMM = 18;  // common enable (cathode/anode)
static constexpr int TOTEM_PIN_R    =  4;  // red   channel
static constexpr int TOTEM_PIN_G    = 19;  // green channel
static constexpr int TOTEM_PIN_B    = 21;  // blue  channel
