#pragma once

// ================================================================
// player_pins.h — hardware pin assignments for the LightAir player
// PCB (V6R2 board).
//
// Pin assignments verified against LightAirV6R2_Demo_Teams_T7500.ino
// and EnlightDefaults (src/config.h).
// ================================================================

// ---- I2C (OLED display) ----
static constexpr int PLAYER_I2C_SDA =  8;
static constexpr int PLAYER_I2C_SCL =  3;

// ---- SPI (Enlight ADC — MCP3204 or equivalent) ----
static constexpr int PLAYER_ADC_CS   = 13;  // chip select
static constexpr int PLAYER_ADC_SDO  = 14;  // MOSI / FAR LED sine output
static constexpr int PLAYER_ADC_SDI  = 21;  // MISO / ADC data in
static constexpr int PLAYER_ADC_CLK  = 47;  // SPI clock

// ---- Enlight LED drive ----
// FAR  LED: FSPI MOSI repurposed as PDM output (same pin as ADC_SDO above,
//           but on a second SPI host).
static constexpr int PLAYER_LED_SDO  = 38;  // FAR  LED data (LEDON on V6R2)
// NEAR LED: FSPI MISO repurposed as PDM output.
static constexpr int PLAYER_LED_SDI_OUT = 36;  // NEAR LED data

// ---- Power / AFE ----
static constexpr int PLAYER_SWITCHOFF =  6;  // power rail switch-off
static constexpr int PLAYER_AFE_ON    =  9;  // analogue front-end enable

// ---- Audio ----
static constexpr int PLAYER_SPK =  4;  // passive buzzer / speaker

// ---- Vibration ----
static constexpr int PLAYER_VIB = 12;  // motor vibration

// ---- 2×3 matrix keypad ----
//   Row pins (driven LOW one at a time during scan):
static constexpr uint8_t PLAYER_SW_R1 =  7;   // row 0: < ^ >
static constexpr uint8_t PLAYER_SW_R2 = 17;   // row 1: A V B
//   Column pins (INPUT_PULLUP):
static constexpr uint8_t PLAYER_SW_C1 = 16;   // col 0: < / A
static constexpr uint8_t PLAYER_SW_C2 = 15;   // col 1: ^ / V
static constexpr uint8_t PLAYER_SW_C3 = 18;   // col 2: > / B

// ---- Trigger buttons ----
static constexpr int PLAYER_TRIG_1 = 11;  // primary   trigger
static constexpr int PLAYER_TRIG_2 = 10;  // secondary trigger
