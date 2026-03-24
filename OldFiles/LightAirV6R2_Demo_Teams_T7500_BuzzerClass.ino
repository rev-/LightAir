
// change log 01/04/2025: add support for rlm, glm, blim as limit values for each color, after white diffuser calibration

//COLOR CALIBRATION FOR AVERY DENNISON T7500 TARGETS! USING GP340 WILL REQUIRE OTHER VALUES!
// 2025 Team is made with a base of AveryDennison T7500B with 3M electroCut overlays (colors 2-6) or car headlight tint from aliExpress ( Yiwu Zhouping auto supplies) (colors 7-9)

// SPI mode 0 ?

// TO LISTEN DATA:
// socat - udp-listen:44444


// V6R2 Pinout (AKA V6R1)

//  GPIO  |  FUNCTION
//        |
//  8     |   I2C_SDA
//  3     |   I2C_SCL
//        |
//  13    |   SPI_CS_ADC
//  14    |   SPI_MOSI
//  21    |   SPI_MISO
//  47    |   SPI_CLK
//        |
//  6     |   SWITCHOFF
//        |
//  9     |   AFE_ON
//        |
//  38    |   LEDON
//        |
//  4     |   SPK
//  -     |   VOLUME
//        |
//  43    |   (TX)
//  44    |   (RX)
//        |
//  7     |   SW_R1
//  17    |   SW_R2
//  16    |   SW_C1
//  15    |   SW_C2
//  18    |   SW_C3
//        |
//  12    |   VIB
//        |


/*
The MIaET License (MIT)
Copyright (c) 2015 thewknd
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

// libraries used in project

#include <SPI.h>
#include <Arduino.h>

#include <ESP32DMASPIMaster.h>

#include <Keypad.h>

#include "Wire.h"
#include "SSD1306Wire.h"

#include <esp_now.h>
#include <WiFi.h>

#include <Preferences.h>

#include "Buzzer.h"
#include "SoundCtrl.h"

//Shutdown/keepalive //In V6 direct control by pushbutton
// #define SHTDWN    32


//I2C
#define I2C_SDA 8
#define I2C_SCL 3
#define TWI_FREQ 400000L  //Fast mode I2C

//SPI  // pins according to hardware configuration of V6
#define SPI_CS 13
#define SPI_MOSI 14
#define SPI_MISO 21
#define SPI_CLK 47

#define SIN_MOSI 38

//Power control
#define SWITCHOFF 6  // hold down for a few ms for hard switchoff
#define AFE_ON 9     // powers the whole analog part

//ADC SPI controls, according to datasheet

//Bit 7 (MSB)  Bit 6  Bit 5  Bit 4   Bit 3  Bit 2  Bit 1   Bit 0
//DONTC        DONTC  ADD2   ADD1    ADD0   DONTC  DONTC   DONTC
#define ADC_READ_BAT 0b0000000000000000   // Read battery sensor channel
#define ADC_READ_PDT 0b0000100000000000   // Read photodiode temperature
#define ADC_READ_LEDT 0b0001000000000000  // Read LED board temperature
#define ADC_READ_R 0b0001100000000000     // Read R photodiode channel
#define ADC_READ_G 0b0010000000000000     // Read G photodiode channel
#define ADC_READ_B 0b0010100000000000     // Read B photodiode channel

#define ADC_CLK 16000000  // 16MHz max

//LED
#define LEDON 38

//Triggers
#define TRIG_1 11
#define TRIG_2 10

//Vibration
#define VIB 12


// other controls
#define REP_TIMES 20  // Recog sequence repetition times
#define MULTITIMES 3  // Repetitions of recog sequence per lighting.
#define NUM_SIGMA 3   // directly applied to calibration sigma

#define LIMITDEV 0.1  // actual accepted error is 2*LIMITDEV

ESP32DMASPI::Master master;
ESP32DMASPI::Master sinMaster;

boolean restart = false;
boolean debug = true;
boolean comm = true;

int note_freq[4] = { 1000, 2000, 3000, 4000 };
int note_dur[4] = { 25, 25, 25, 25 };

uint8_t *spi_master_tx_buf;
uint8_t *spi_master_rx_buf1;

static const uint32_t SIN_BUFFER_SIZE = 10 * 120;
static const uint32_t GOERTZ_PERIOD = SIN_BUFFER_SIZE / 6;
static const uint32_t SIN_MAG = 512;
static const uint32_t SPI_BUFFER_OFFSET = 4 * 0;  // Must be multiple of 4. Caution to delete excess readings!
static const uint32_t SPI_BUFFER_SIZE = SIN_BUFFER_SIZE + SPI_BUFFER_OFFSET;

uint8_t *sin_buf;

int32_t sintab[GOERTZ_PERIOD];  // Period depends on measures done

uint8_t sin_buf_data[SIN_BUFFER_SIZE] = { 0x0, 0x40, 0x10, 0x4, 0x1, 0x0, 0x40, 0x10, 0x4, 0x1,
                                          0x0, 0x40, 0x10, 0x4, 0x1, 0x0, 0x40, 0x10, 0x4, 0x1,
                                          0x0, 0x40, 0x10, 0x4, 0x1, 0x0, 0x40, 0x10, 0x4, 0x1,
                                          0x0, 0x40, 0x10, 0x4, 0x1, 0x0, 0x40, 0x10, 0x4, 0x1,
                                          0x0, 0x80, 0x40, 0x20, 0x10, 0x8, 0x4, 0x2, 0x2, 0x1,
                                          0x0, 0x80, 0x40, 0x20, 0x10, 0x8, 0x4, 0x2, 0x2, 0x1,
                                          0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1,
                                          0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1,
                                          0x1, 0x2, 0x4, 0x8, 0x8, 0x10, 0x20, 0x20, 0x40, 0x81,
                                          0x2, 0x4, 0x10, 0x20, 0x41, 0x2, 0x4, 0x10, 0x20, 0x41,
                                          0x2, 0x8, 0x20, 0x82, 0x8, 0x20, 0x41, 0x4, 0x10, 0x41,
                                          0x4, 0x10, 0x42, 0x8, 0x21, 0x4, 0x10, 0x42, 0x8, 0x21,
                                          0x4, 0x21, 0x4, 0x21, 0x4, 0x21, 0x4, 0x21, 0x4, 0x21,
                                          0x8, 0x42, 0x10, 0x84, 0x21, 0x8, 0x42, 0x10, 0x84, 0x21,
                                          0x8, 0x42, 0x21, 0x8, 0x84, 0x22, 0x10, 0x84, 0x42, 0x11,
                                          0x8, 0x88, 0x88, 0x44, 0x44, 0x22, 0x22, 0x21, 0x11, 0x11,
                                          0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
                                          0x11, 0x11, 0x22, 0x22, 0x22, 0x44, 0x44, 0x48, 0x88, 0x89,
                                          0x12, 0x24, 0x48, 0x91, 0x22, 0x44, 0x89, 0x12, 0x24, 0x89,
                                          0x12, 0x44, 0x91, 0x24, 0x49, 0x12, 0x44, 0x91, 0x24, 0x49,
                                          0x12, 0x49, 0x24, 0x92, 0x49, 0x12, 0x49, 0x24, 0x92, 0x49,
                                          0x24, 0x92, 0x52, 0x49, 0x25, 0x24, 0x92, 0x52, 0x49, 0x25,
                                          0x24, 0xA4, 0x92, 0x92, 0x52, 0x4A, 0x49, 0x49, 0x29, 0x25,
                                          0x25, 0x29, 0x29, 0x4A, 0x4A, 0x52, 0x92, 0x94, 0x94, 0xA5,
                                          0x29, 0x4A, 0x94, 0xA5, 0x4A, 0x52, 0xA5, 0x2A, 0x52, 0x95,
                                          0x2A, 0x55, 0x2A, 0x55, 0x2A, 0x55, 0x2A, 0x55, 0x2A, 0x55,
                                          0x2A, 0xA9, 0x55, 0x4A, 0xAA, 0x55, 0x52, 0xAA, 0xA5, 0x55,
                                          0x2A, 0xAA, 0xA5, 0x55, 0x55, 0x2A, 0xAA, 0xA5, 0x55, 0x55,
                                          0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
                                          0x55, 0x55, 0x5A, 0xAA, 0xAB, 0x55, 0x55, 0x5A, 0xAA, 0xAB,
                                          0x55, 0x6A, 0xB5, 0x56, 0xAB, 0x55, 0x6A, 0xB5, 0x56, 0xAB,
                                          0x56, 0xAD, 0x5A, 0xB5, 0x6B, 0x56, 0xAD, 0x5A, 0xB5, 0x6B,
                                          0x5A, 0xD6, 0xB5, 0xAD, 0x6B, 0x5A, 0xD6, 0xB5, 0xAD, 0x6B,
                                          0x5B, 0x5B, 0x5B, 0x5B, 0x5B, 0x5B, 0x5B, 0x5B, 0x5B, 0x5B,
                                          0x5B, 0x6B, 0x6D, 0x6D, 0xAD, 0xB5, 0xB6, 0xB6, 0xDA, 0xDB,
                                          0x6B, 0x6D, 0xB6, 0xDB, 0x6D, 0xB6, 0xDB, 0x6D, 0xB6, 0xDB,
                                          0x6D, 0xB7, 0x6D, 0xB7, 0x6D, 0xB7, 0x6D, 0xB7, 0x6D, 0xB7,
                                          0x6E, 0xDB, 0xB7, 0x6E, 0xDD, 0xBB, 0x76, 0xED, 0xDB, 0xB7,
                                          0x6E, 0xEE, 0xED, 0xDD, 0xDD, 0xBB, 0xBB, 0xBB, 0x77, 0x77,
                                          0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77,
                                          0x77, 0x7B, 0xBD, 0xDE, 0xEF, 0x77, 0x7B, 0xBD, 0xDE, 0xEF,
                                          0x7B, 0xDE, 0xF7, 0xBD, 0xEF, 0x7B, 0xDE, 0xF7, 0xBD, 0xEF,
                                          0x7B, 0xDF, 0x7B, 0xDF, 0x7B, 0xDF, 0x7B, 0xDF, 0x7B, 0xDF,
                                          0x7D, 0xF7, 0xDF, 0x7D, 0xFB, 0xEF, 0xBE, 0xFB, 0xEF, 0xBF,
                                          0x7D, 0xFB, 0xF7, 0xDF, 0xBF, 0x7D, 0xFB, 0xF7, 0xDF, 0xBF,
                                          0x7E, 0xFD, 0xFB, 0xFB, 0xF7, 0xEF, 0xEF, 0xDF, 0xBF, 0x7F,
                                          0x7F, 0xBF, 0xBF, 0xDF, 0xEF, 0xF7, 0xFB, 0xFD, 0xFE, 0xFF,
                                          0x7F, 0xDF, 0xF7, 0xFD, 0xFF, 0x7F, 0xDF, 0xF7, 0xFD, 0xFF,
                                          0x7F, 0xEF, 0xFE, 0xFF, 0xDF, 0xFB, 0xFF, 0xBF, 0xF7, 0xFF,
                                          0x7F, 0xFB, 0xFF, 0xDF, 0xFF, 0x7F, 0xFB, 0xFF, 0xDF, 0xFF,
                                          0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 0xFF,
                                          0x7F, 0xFF, 0xF7, 0xFF, 0xFF, 0x7F, 0xFF, 0xF7, 0xFF, 0xFF,
                                          0x7F, 0xFF, 0xFF, 0xEF, 0xFF, 0xFF, 0xFB, 0xFF, 0xFF, 0xFF,
                                          0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF,
                                          0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF,
                                          0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                          0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                          0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                          0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                          0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                          0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                          0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                          0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                          0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                          0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                          0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                          0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF,
                                          0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF,
                                          0x7F, 0xFF, 0xFF, 0xEF, 0xFF, 0xFF, 0xFB, 0xFF, 0xFF, 0xFF,
                                          0x7F, 0xFF, 0xF7, 0xFF, 0xFF, 0x7F, 0xFF, 0xF7, 0xFF, 0xFF,
                                          0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 0xFF, 0x7F, 0xFF,
                                          0x7F, 0xFB, 0xFF, 0xDF, 0xFF, 0x7F, 0xFB, 0xFF, 0xDF, 0xFF,
                                          0x7F, 0xEF, 0xFE, 0xFF, 0xDF, 0xFB, 0xFF, 0xBF, 0xF7, 0xFF,
                                          0x7F, 0xDF, 0xF7, 0xFD, 0xFF, 0x7F, 0xDF, 0xF7, 0xFD, 0xFF,
                                          0x7F, 0xBF, 0xBF, 0xDF, 0xEF, 0xF7, 0xFB, 0xFD, 0xFE, 0xFF,
                                          0x7E, 0xFD, 0xFB, 0xFB, 0xF7, 0xEF, 0xEF, 0xDF, 0xBF, 0x7F,
                                          0x7D, 0xFB, 0xF7, 0xDF, 0xBF, 0x7D, 0xFB, 0xF7, 0xDF, 0xBF,
                                          0x7D, 0xF7, 0xDF, 0x7D, 0xFB, 0xEF, 0xBE, 0xFB, 0xEF, 0xBF,
                                          0x7B, 0xDF, 0x7B, 0xDF, 0x7B, 0xDF, 0x7B, 0xDF, 0x7B, 0xDF,
                                          0x7B, 0xDE, 0xF7, 0xBD, 0xEF, 0x7B, 0xDE, 0xF7, 0xBD, 0xEF,
                                          0x77, 0x7B, 0xBD, 0xDE, 0xEF, 0x77, 0x7B, 0xBD, 0xDE, 0xEF,
                                          0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77,
                                          0x6E, 0xEE, 0xED, 0xDD, 0xDD, 0xBB, 0xBB, 0xBB, 0x77, 0x77,
                                          0x6E, 0xDB, 0xB7, 0x6E, 0xDD, 0xBB, 0x76, 0xED, 0xDB, 0xB7,
                                          0x6D, 0xB7, 0x6D, 0xB7, 0x6D, 0xB7, 0x6D, 0xB7, 0x6D, 0xB7,
                                          0x6B, 0x6D, 0xB6, 0xDB, 0x6D, 0xB6, 0xDB, 0x6D, 0xB6, 0xDB,
                                          0x5B, 0x6B, 0x6D, 0x6D, 0xAD, 0xB5, 0xB6, 0xB6, 0xDA, 0xDB,
                                          0x5B, 0x5B, 0x5B, 0x5B, 0x5B, 0x5B, 0x5B, 0x5B, 0x5B, 0x5B,
                                          0x5A, 0xD6, 0xB5, 0xAD, 0x6B, 0x5A, 0xD6, 0xB5, 0xAD, 0x6B,
                                          0x56, 0xAD, 0x5A, 0xB5, 0x6B, 0x56, 0xAD, 0x5A, 0xB5, 0x6B,
                                          0x55, 0x6A, 0xB5, 0x56, 0xAB, 0x55, 0x6A, 0xB5, 0x56, 0xAB,
                                          0x55, 0x55, 0x5A, 0xAA, 0xAB, 0x55, 0x55, 0x5A, 0xAA, 0xAB,
                                          0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
                                          0x2A, 0xAA, 0xA5, 0x55, 0x55, 0x2A, 0xAA, 0xA5, 0x55, 0x55,
                                          0x2A, 0xA9, 0x55, 0x4A, 0xAA, 0x55, 0x52, 0xAA, 0xA5, 0x55,
                                          0x2A, 0x55, 0x2A, 0x55, 0x2A, 0x55, 0x2A, 0x55, 0x2A, 0x55,
                                          0x29, 0x4A, 0x94, 0xA5, 0x4A, 0x52, 0xA5, 0x2A, 0x52, 0x95,
                                          0x25, 0x29, 0x29, 0x4A, 0x4A, 0x52, 0x92, 0x94, 0x94, 0xA5,
                                          0x24, 0xA4, 0x92, 0x92, 0x52, 0x4A, 0x49, 0x49, 0x29, 0x25,
                                          0x24, 0x92, 0x52, 0x49, 0x25, 0x24, 0x92, 0x52, 0x49, 0x25,
                                          0x12, 0x49, 0x24, 0x92, 0x49, 0x12, 0x49, 0x24, 0x92, 0x49,
                                          0x12, 0x44, 0x91, 0x24, 0x49, 0x12, 0x44, 0x91, 0x24, 0x49,
                                          0x12, 0x24, 0x48, 0x91, 0x22, 0x44, 0x89, 0x12, 0x24, 0x89,
                                          0x11, 0x11, 0x22, 0x22, 0x22, 0x44, 0x44, 0x48, 0x88, 0x89,
                                          0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
                                          0x8, 0x88, 0x88, 0x44, 0x44, 0x22, 0x22, 0x21, 0x11, 0x11,
                                          0x8, 0x42, 0x21, 0x8, 0x84, 0x22, 0x10, 0x84, 0x42, 0x11,
                                          0x8, 0x42, 0x10, 0x84, 0x21, 0x8, 0x42, 0x10, 0x84, 0x21,
                                          0x4, 0x21, 0x4, 0x21, 0x4, 0x21, 0x4, 0x21, 0x4, 0x21,
                                          0x4, 0x10, 0x42, 0x8, 0x21, 0x4, 0x10, 0x42, 0x8, 0x21,
                                          0x2, 0x8, 0x20, 0x82, 0x8, 0x20, 0x41, 0x4, 0x10, 0x41,
                                          0x2, 0x4, 0x10, 0x20, 0x41, 0x2, 0x4, 0x10, 0x20, 0x41,
                                          0x1, 0x2, 0x4, 0x8, 0x8, 0x10, 0x20, 0x20, 0x40, 0x81,
                                          0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1,
                                          0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1,
                                          0x0, 0x80, 0x40, 0x20, 0x10, 0x8, 0x4, 0x2, 0x2, 0x1,
                                          0x0, 0x80, 0x40, 0x20, 0x10, 0x8, 0x4, 0x2, 0x2, 0x1,
                                          0x0, 0x40, 0x10, 0x4, 0x1, 0x0, 0x40, 0x10, 0x4, 0x1,
                                          0x0, 0x40, 0x10, 0x4, 0x1, 0x0, 0x40, 0x10, 0x4, 0x1,
                                          0x0, 0x40, 0x10, 0x4, 0x1, 0x0, 0x40, 0x10, 0x4, 0x1

};

// LOG2(REP_TIMES)+LOG2(ADCREP)+12(ADC RESOLUTION) < 32 !!
// best : LOG2(REP_TIMES)+LOG2(ADCREP)+12 = 32
// Actual: 5 + 6 + 12 = 23. May have more repetition times

uint8_t readr1 = 24;  //SPI ADC control for R channel in uint8_t, first byte
uint8_t readg1 = 32;  //SPI ADC control for G channel in uint8_t, first byte
uint8_t readb1 = 40;  //SPI ADC control for B channel in uint8_t, first byte
// The other byte is always 0


// Temperature sensor LUTs

//To calculate and then multiply with actual 3.3V read from the ADC!
int ledtemplut[128] = { -40, -35, -26, -19, -15, -11, -8, -6, -3, -1, 1, 3, 4, 6, 7, 9, 10, 11, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 25, 26, 27, 28, 29, 29, 30, 31, 32, 32, 33, 34, 34, 35, 36, 37, 37, 38, 39, 39, 40, 40, 41, 42, 42, 43, 44, 44, 45, 46, 46, 47, 47, 48, 49, 49, 50, 50, 51, 52, 52, 53, 53, 54, 55, 55, 56, 56, 57, 58, 58, 59, 60, 60, 61, 61, 62, 63, 63, 64, 65, 65, 66, 66, 67, 68, 68, 69, 70, 70, 71, 72, 73, 73, 74, 75, 75, 76, 77, 78, 78, 79, 80, 81, 81, 82, 83, 84, 85, 86, 87, 87, 88, 89, 90, 91, 92 };
int pdtemplut[128] = { 200, 169, 141, 127, 117, 110, 104, 99, 95, 92, 88, 86, 83, 81, 79, 77, 75, 73, 71, 70, 69, 67, 66, 65, 63, 62, 61, 60, 59, 58, 57, 56, 55, 54, 54, 53, 52, 51, 50, 50, 49, 48, 48, 47, 46, 45, 45, 44, 44, 43, 42, 42, 41, 41, 40, 39, 39, 38, 38, 37, 37, 36, 36, 35, 35, 34, 34, 33, 33, 32, 32, 31, 31, 30, 30, 29, 29, 28, 28, 27, 27, 27, 26, 26, 25, 25, 24, 24, 23, 23, 23, 22, 22, 21, 21, 20, 20, 19, 19, 19, 18, 18, 17, 17, 16, 16, 15, 15, 15, 14, 14, 13, 13, 12, 12, 11, 11, 10, 10, 9, 9, 8, 8, 7, 7, 6, 6, 5 };

int gamestats[5] = { 0, 0, 0, 0, 0 };
int gametime = 60 * 15;  // game time in seconds
int gamestartime = 0;

int sinIter = 0;
int arrayiter = 0;
int satCount = 0;

// I2C line dedicated to LCD
SSD1306Wire lcd(0x3c, I2C_SDA, I2C_SCL);  // ADDRESS, SDA, SCL
uint32_t lcdcursor = 0;


// some flags for flow control
boolean justshot = false;
boolean pairsuccess = false;
boolean sentsuccess = false;
boolean commShot = false;
boolean mute = false;

boolean juston = true;

uint32_t startime = 0;

int id = 0;


//Numbers and names of players

const int playnum = 16;  // Theoretical maximum 20, but 16 keeps messaging easier

const uint8_t broadCastAddress[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
uint8_t newMAC[] = { 0x1A, 0x17, 0xA1, 0x00, 0x00, 0x00 };
//                  0x1A  0x17  0xA1  f
//                  Role: 01 = TOTEM ; 00 = proj

int advnum;
char searchchar[5] = "_00-";
char advname[10] = "";
char players[playnum][12] = {
  { "00-NONE" },
  { "01-Clear" },
  { "02-Green" },
  { "03-Yellow" },
  { "04-Blue" },
  { "05-Orange" },
  { "06-Red" },
  { "07-Lime" },
  { "08-Magenta" },
  { "09-Purple" },
  { "10-Unknown" },
  { "11-Unknown" },
  { "12-Unknown" },
  { "13-Unknown" },
  { "14-Unknown" },
  { "15-Unknown" }
};



// calibration hitbox, will be filled with calibration values stored in non-volatile memory
// keeps 4 lines for each player and is used to check whether read color falls in one of the boxes

// For test reasons we use hit boxes extrapolated from the unions of calibration boxes in CalibSheets. See files "HitBoxes_V6R2" and "CalibSheet_V6R2_*_BiancoCentrato"

float hitBox[playnum][4] = {

  { 0, 0, 0, 0 },              //0
  { 0.4, 0.25, 0.58, 0.42 },   //1 Clear
  { 0.1, 0.0, 0.78, 0.62 },    //2 Green
  { 0.65, 0.5, 1, 0.9 },       //3 Yellow
  { 0.1, 0, 0.32, 0.15 },      //4 Blue
  { 0.77, 0.62, 0.84, 0.70 },  //5 Orange
  { 0.95, 0.87, 1, 0.0 },      //6 Red
  { 0.5, 0.3, 0.82, 0.7 },     //7 Lime
  { 0.6, 0.4, 0.24, 0.10 },    //8 Magenta
  { 0.4, 0.28, 0.36, 0.24 },   //9 Purple
  { -10, -10, -10, -10 },      //10
  { -10, -10, -10, -10 },      //11
  { -10, -10, -10, -10 },      //12
  { -10, -10, -10, -10 },      //13
  { -10, -10, -10, -10 },      //14
  { -10, -10, -10, -10 },      //15

};

// variables for sensors
int pdtemp;
int ledtemp;
float volt;
uint16_t bat;
uint16_t ntc;
uint16_t lastsens = 0;
uint16_t senstime = 2000;

uint32_t r = 0;
uint32_t g = 0;
uint32_t b = 0;

uint32_t rv;
uint32_t gv;
uint32_t bv;

long long rawsum = 0;
long long sum = 0;
long long rout = 0;
long long gout = 0;
long long bout = 0;

uint32_t rcal = 0;
uint32_t gcal = 0;
uint32_t bcal = 0;
float rstd = 0;
float gstd = 0;
float bstd = 0;
float sigma_r, sigma_r2 = 0;
float sigma_ang, sigma_ang2 = 0;
uint32_t limpow, rlim, glim, blim = 0;  //limits from white diffuser at close distance
uint32_t delta = 0;
uint32_t threshold = 0;
uint32_t reptimes = 0;

float rfact;
float bfact;

uint32_t freqbin;
uint32_t phase;

float outr = 0.0;
float outg = 0.0;
float outb = 0.0;
float outsum = 0.0;
float angsum = 0.0;
float outang = 0.0;

int vpow;
int deltaBat;

// game parameters
const int loadtime = 10000;  //ms
const int loadammo = 50;     //ammo charged each loadtime
int ammo = 50;               //starting ammo
int ammotic = 0;
int ammotac = 0;
boolean load = true;

int points = 0;

unsigned int downtime = 10;  //in seconds. Must be >5
boolean down = false;
boolean recog = false;

unsigned int tic = 0;
unsigned int tac = 0;


// ESP-NOW (telecommunication) parameters
String buf = "";
String found_SSID = "";
String found_BSSIDstr = "";
String myname = "";
uint32_t mycolor = 0;
String SSID = "LightAirV6_";
char PEER_SSID[40] = "LightAirV6_";
uint8_t hitmsg;

// Global copy of peer
#define NUMPEERS 20
esp_now_peer_info_t peer[NUMPEERS] = {};
int PeerCnt = 0;
int PeerNumb = 0;
// fixed number of peer
// "1-Clear", "2-Green", "3-Yellow", "4-Blue", "5-Orange", "6-Red"
#define CHANNEL 1
#define PRINTSCANRESULTS 1
#define DELETEBEFOREPAIR 0


// Keypad setup
String keyStr = "";
int keycount = 0;
char inkey;
char key;
const byte KROWS = 2;  //two rows
const byte KCOLS = 3;  //three columns
char keys[KROWS][KCOLS] = {
  { '<', '^', '>' },
  { 'A', 'V', 'B' }
};
byte rowPins[KROWS] = { 7, 17 };       //connect to the row pinouts of the keypad
byte colPins[KCOLS] = { 16, 15, 18 };  //connect to the column pinouts of the keypad

bool pressedkeys[6] = { false, false, false, false, false, false };
bool releasedkeys[6] = { false, false, false, false, false, false };
bool holdkeys[6] = { false, false, false, false, false, false };

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, KROWS, KCOLS);


// Sound setup

SoundCtrl soundCtrl = SoundCtrl();

// Init ESP Now with fallback -- code copied from official ESP-NOW examples, with debug commented and minor changes. Skip to "END ESP-NOW"
void InitESPNow() {

  if (esp_now_init() != ESP_OK) {
    if (debug) Serial.println("Error initializing Esp Now");
    //  ESP_BT.println("ESPNow Init Success");
    //ESP.restart();
    return;
  }
  if (debug) Serial.println("Trying to start callback functions");
  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);
}

void addBroadCast() {
  // memset(&peer, 0, sizeof(peer)); // clear slaves
  for (int ii = 0; ii < 6; ii++) {
    peer[0].peer_addr[ii] = (uint8_t)0xff;
  }

  PeerCnt += 1;
  peer[0].channel = CHANNEL;
  peer[0].encrypt = 0;

  manageSlave();
}

// Check if the slave is already paired with the master.
// If not, pair the slave with master
void manageSlave() {
  if (PeerCnt > 0) {
    for (int i = 0; i < playnum; i++) {
      const esp_now_peer_info_t *peertry = &peer[i];
      const uint8_t *peertry_addr = peer[i].peer_addr;
      // ESP_BT.print("Processing: ");
      // for (int ii = 0; ii < 6; ++ii ) {
      //  ESP_BT.print((uint8_t) peertry_addr[ii], HEX);
      //  if (ii != 5) ESP_BT.print(":");
      // }
      // ESP_BT.print(" Status: ");
      // check if the peer exists
      bool exists = esp_now_is_peer_exist(peertry_addr);
      if (exists) {
        // Slave already paired.
        // ESP_BT.println("Already Paired");
      } else {
        // Slave not paired, attempt pair
        esp_err_t addStatus = esp_now_add_peer(peertry);
        if (addStatus == ESP_OK) {
          // Pair success
          // ESP_BT.println("Pair success");
        } else if (addStatus == ESP_ERR_ESPNOW_NOT_INIT) {
          // How did we get so far!!
          // ESP_BT.println("ESPNOW Not Init");
        } else if (addStatus == ESP_ERR_ESPNOW_ARG) {
          // ESP_BT.println("Add Peer - Invalid Argument");
        } else if (addStatus == ESP_ERR_ESPNOW_FULL) {
          // ESP_BT.println("Peer list full");
        } else if (addStatus == ESP_ERR_ESPNOW_NO_MEM) {
          // ESP_BT.println("Out of memory");
        } else if (addStatus == ESP_ERR_ESPNOW_EXIST) {
          // ESP_BT.println("Peer Exists");
        } else {
          // ESP_BT.println("Not sure what happened");
        }
        delay(100);
      }
    }
  } else {
    // No slave found to process
    //ESP_BT.println("No Slave found to process");
  }
}

// END ESP-NOW


// load calibration values, from the NVM (Non Volatile Memory) to an array

void loadCalibration() {
  const String defString = "NONE";
  const float defFloat = -10;
  const uint32_t defInt = 0;
  String ckey = "";
  ckey.reserve(256);

  Preferences calibration;
  calibration.begin("calibration", false);

  //rcal, gcal, bcal from tested values instead of calibration procedure!

  myname = calibration.getString("myname", defString);
  mycolor = calibration.getUInt("mycolor", defFloat);
  rcal = calibration.getUInt("rcal", defInt) * MULTITIMES;
  gcal = calibration.getUInt("gcal", defInt) * MULTITIMES;
  bcal = calibration.getUInt("bcal", defInt) * MULTITIMES;

  limpow = calibration.getUInt("limpow", defInt) * MULTITIMES;

  rlim = calibration.getUInt("rlim", defInt) * MULTITIMES;
  glim = calibration.getUInt("glim", defInt) * MULTITIMES;
  blim = calibration.getUInt("blim", defInt) * MULTITIMES;

  rstd = calibration.getFloat("rstd", defFloat) * NUM_SIGMA;
  gstd = calibration.getFloat("gstd", defFloat) * NUM_SIGMA;
  bstd = calibration.getFloat("bstd", defFloat) * NUM_SIGMA;

  sigma_r = (rstd - gstd - bstd);
  sigma_r2 = pow(sigma_r, 2);
  sigma_ang = gstd - bstd;
  sigma_ang2 = pow(sigma_ang, 2);

  rfact = calibration.getFloat("rfact", defFloat);
  bfact = calibration.getFloat("bfact", defFloat);


  freqbin = calibration.getUInt("freqbin", defInt);
  phase = calibration.getUInt("phase", defInt);

  reptimes = calibration.getUInt("reptimes", defInt);

  calibration.end();


  // MANUAL CALIBRATION VALUES

  /*

  myname = "Red";
  mycolor = 6;

  rfact = 3.567;
  bfact = 1.34;

  rcal = 16251634;
  gcal = 42013856;
  bcal = 38622372;

  limpow = 500000;

  rstd = 201786.44;
  gstd = 115805.50;
  bstd = 136820.75;

  phase = 28;
  */

  if (debug) {
    Serial.println("**CALIBRATION**");
    Serial.print("myname=");
    Serial.println(myname);
    Serial.print("mycolor=");
    Serial.println(mycolor);
    Serial.print("rcal=");
    Serial.println(rcal);
    Serial.print("gcal=");
    Serial.println(gcal);
    Serial.print("bcal=");
    Serial.println(bcal);
    Serial.print("limpow=");
    Serial.println(limpow);
    Serial.print("rstd=");
    Serial.println(rstd);
    Serial.print("gstd=");
    Serial.println(gstd);
    Serial.print("bstd=");
    Serial.println(bstd);
    Serial.print("rfact=");
    Serial.println(rfact);
    Serial.print("bfact=");
    Serial.println(bfact);
    Serial.print("phase=");
    Serial.println(phase);
  }
}


// downseq is what happens when a player is hit. May elaborate to something more complex

void downseq() {

  soundCtrl.playDown();

  gamestats[4] += 1;

  for (int k = downtime; k >= 0; k = k - 1) {
    lcd.clear();
    buf = "";
    buf.concat("HIT BY:");
    lcd.drawString(0, 10, buf);
    buf = "";
    buf.concat(players[advnum]);
    lcd.drawString(0, 20, buf);
    buf = "";
    if (k > 0) buf.concat("WAIT:");
    else buf.concat("GO TO BASE");
    lcd.drawString(0, 40, buf);
    buf = "";
    if (k > 0) buf.concat(k);
    lcd.drawString(60, 40, buf);
    buf = "";
    lcd.display();
    delay(1000);
  }

  //setup lit signalling message
  hitmsg = mycolor * 15;  // From:
  hitmsg = hitmsg + 0;    // To: Base

  esp_now_unregister_recv_cb();
  esp_now_register_recv_cb(OnDataRecvDown);
  int downmsgtimer = millis();

  while (down) {

    if ((millis() - downmsgtimer) > 2000) {  // Autosend Sos packet every 2 seconds
      downmsgtimer = millis();
      sendData(hitmsg);
    }
  }

  soundCtrl.playUp();

  esp_now_unregister_recv_cb();
  esp_now_register_recv_cb(OnDataRecv);
  ammo = loadammo;
  down = false;
}

void endGame() {

  restart = false;
  lcd.clear();
  buf = "";
  buf = "GAME OVER";  //HW version
  lcd.drawString(0, 0, buf);
  buf = "";
  buf.concat("|IDN ");  //Game
  buf.concat(gamestats[0]);
  lcd.drawString(0, 20, buf);
  buf = "";
  buf.concat("|TIM ");  //Game
  buf.concat(gamestats[1]);
  lcd.drawString(50, 20, buf);
  buf = "";
  buf.concat("|NRG ");  //Game
  buf.concat(gamestats[2]);
  lcd.drawString(0, 30, buf);
  buf = "";
  buf.concat("|LIT ");  //Game
  buf.concat(gamestats[3]);
  lcd.drawString(50, 30, buf);
  buf = "";
  buf.concat("|DWN ");  //Game
  buf.concat(gamestats[4]);
  lcd.drawString(0, 40, buf);
  buf = "";
  lcd.display();


  soundCtrl.playEndGame();

  while (!restart) {
    delay(10);
    if (keypad.getKeys()) {
      for (int i = 0; i < LIST_MAX; i++)  // Scan the whole key list.
      {
        // if ( (keypad.key[i].stateChanged) && keypad.key[i].kstate == HOLD && keypad.key[i].kchar=='V')  restart = true;
        if ((keypad.key[i].stateChanged) && keypad.key[i].kstate == HOLD) {
          restart = true;
          lcd.clear();
          buf.concat("RESTART");
          lcd.drawString(45, 20, buf);
          buf = "";
          lcd.display();
          soundCtrl.playUp();

          lcd.clear();
          lcd.display();
          for (int giter = 0; giter < 5; giter++) gamestats[giter] = 0;
          gamestats[0] = mycolor;
          gamestats[1] = gametime;
          gamestartime = millis();
          if (debug) {
            Serial.println("Return");
          }
          return;
        }
      }
    }
    if (debug) {
      Serial.println("EndGame loop");
    }
  }
  if (debug) {
    Serial.println("EndGame last line");
  }
}



void sendData(uint8_t data) {

  esp_err_t result = esp_now_send(peer[0].peer_addr, &data, sizeof(data));
  //  const uint8_t *peer_addr = peer[targetnum].peer_addr;
  //  // ESP_BT.print("Sending: "); ESP_BT.println(data);
  //  esp_err_t result = esp_now_send(peer_addr, &data, sizeof(data));

  // ESP_BT.print("Send Status: ");
  if (result == ESP_OK) {
    sentsuccess = true;
  } else if (result == ESP_ERR_ESPNOW_NOT_INIT) {
    // How did we get so far!!
    // ESP_BT.println("ESPNOW not Init.");
  } else if (result == ESP_ERR_ESPNOW_ARG) {
    // ESP_BT.println("Invalid Argument");
  } else if (result == ESP_ERR_ESPNOW_INTERNAL) {
    // ESP_BT.println("Internal Error");
  } else if (result == ESP_ERR_ESPNOW_NO_MEM) {
    // ESP_BT.println("ESP_ERR_ESPNOW_NO_MEM");
  } else if (result == ESP_ERR_ESPNOW_NOT_FOUND) {
    // ESP_BT.println("Peer not found.");
  } else {
    // ESP_BT.println("Not sure what happened");
  }
}


// functions to send and receive data via ESP-NOW

void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  if (debug) {
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
    Serial.print("Last Packet Sent to: ");
    Serial.println(macStr);
    Serial.print("Last Packet Send Status: ");
    Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Delivery Success" : "Delivery Fail");
  }
}

// callback when data is recv from peer
void OnDataRecv(const uint8_t *mac_addr, const uint8_t *data, int data_len) {

  if (debug) {
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
    Serial.print("Last Packet Recv from: ");
    Serial.println(macStr);
    Serial.print("Last Packet Recv Data: ");
    Serial.println(data[0]);
    Serial.println("");
  }
  //  for (int h=0;h<10;h++){
  //    advname[h] = players[(data[0]-1)/10][h];
  //  }
  //  ESP_BT.print("From name = ");
  //  ESP_BT.println(advname);
  //
  //  ESP_BT.print("To name = ");
  //  ESP_BT.println(players[data[0]%10]);

  if ((comm == true) && (data[0] % 15 == mycolor) && (data[0] / 15 != 0)) {  // Ignore messages from TOTEM (sender = 0)
    down = true;
    advnum = data[0] / 15;
    //    ESP_BT.print("Received data ");
    //    ESP_BT.print(data[0]);
    //    ESP_BT.print(" so hit by ");
    //    ESP_BT.println(players[data[0]/10]);
  }
}


void OnDataRecvDown(const uint8_t *mac_addr, const uint8_t *data, int data_len) {  // When the user is down, change callback to only read totem respawn messages

  if (debug) {
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
    Serial.print("Last Down Packet Recv from: ");
    Serial.println(macStr);
    Serial.print("Last Down Packet Recv Data: ");
    Serial.println(data[0]);
    Serial.println("");
  }
  //  for (int h=0;h<10;h++){
  //    advname[h] = players[(data[0]-1)/10][h];
  //  }
  //  ESP_BT.print("From name = ");
  //  ESP_BT.println(advname);
  //
  //  ESP_BT.print("To name = ");
  //  ESP_BT.println(players[data[0]%10]);

  if ((comm == true) && (data[0] % 15 == mycolor) && (data[0] / 15 == 0)) {
    down = false;
    //    ESP_BT.print("Received data ");
    //    ESP_BT.print(data[0]);
    //    ESP_BT.print(" so hit by ");
    //    ESP_BT.println(players[data[0]/10]);
  }
}


void setup() {

  if (debug) {
    Serial.setTimeout(10);
    Serial.begin(115200);
  }

  //buffers for Strings
  buf.reserve(256);
  buf = "";
  keyStr.reserve(128);
  keyStr = "";
  found_SSID.reserve(256);
  found_BSSIDstr.reserve(256);
  SSID.reserve(50);
  myname.reserve(20);
  myname = "";


  // CHIP ID AND CALIBRATION VALUES

  loadCalibration();
  gamestats[0] = mycolor;
  gamestats[1] = gametime;

  newMAC[5] = mycolor;

  esp_base_mac_addr_set(newMAC);

  //  for(int i=0; i<17; i=i+8) {
  //    id |= ((ESP.getEfuseMac() >> (40 - i)) & 0xff) << i;
  //  }



  if (mycolor < 10) SSID.concat("0");
  SSID.concat(mycolor);
  SSID.concat("-");
  SSID.concat(myname);


  //SPI DMA



  // END chip ID and calibration

  //SPI DMA
  // to use DMA buffer, use these methods to allocate buffer


  for (int i = 0; i < GOERTZ_PERIOD; i++) {
    sintab[i] = round((SIN_MAG * sin((i + phase) * (2 * PI) / (GOERTZ_PERIOD * 1.0))));
  }

  spi_master_tx_buf = master.allocDMABuffer(SPI_BUFFER_SIZE * REP_TIMES);
  spi_master_rx_buf1 = master.allocDMABuffer(SPI_BUFFER_SIZE * REP_TIMES);

  sin_buf = sinMaster.allocDMABuffer(SIN_BUFFER_SIZE * REP_TIMES);

  for (int i = 0; i < SIN_BUFFER_SIZE * REP_TIMES; i++) {
    sin_buf[i] = ~sin_buf_data[i % SIN_BUFFER_SIZE];  //Negate to account for pinOutMatrix inversion
  }

  //memset(sin_buf, 0, SIN_BUFFER_SIZE);  // Transform the sine buffer in 0 (show not working time)
  delay(200);  // Don't know if required or sufficient ***

  // set buffer data...
  spi_master_tx_buf[0] = readr1;
  spi_master_tx_buf[1] = 0 & 0xFF;  // first request is ignored
  spi_master_tx_buf[2] = readr1;
  spi_master_tx_buf[3] = 0 & 0xFF;  // relaunch to add to 4byte multiple
  spi_master_tx_buf[4] = readr1;
  spi_master_tx_buf[5] = 0 & 0xFF;  // align to 6 byte

  for (int i = 6; i < SPI_BUFFER_SIZE * REP_TIMES; i = i + 6) {

    spi_master_tx_buf[i] = readg1;
    spi_master_tx_buf[i + 1] = 0 & 0xFF;
    spi_master_tx_buf[i + 2] = readb1;
    spi_master_tx_buf[i + 3] = 0 & 0xFF;
    spi_master_tx_buf[i + 4] = readr1;
    spi_master_tx_buf[i + 5] = 0 & 0xFF;
  }

  memset(spi_master_rx_buf1, 0, SPI_BUFFER_SIZE * REP_TIMES);

  delay(200);  // Don't know if required or sufficient ***

  master.setDataMode(SPI_MODE0);                           // the correct one is SPI_MODE2
  master.setFrequency(16000000);                           // dMax 16MHz due to ADC limitations
  master.setMaxTransferSize(SPI_BUFFER_SIZE * REP_TIMES);  // default: 4092 bytes

  sinMaster.setDataMode(SPI_MODE0);                           // whatever. What we really do here is simulate a PDM
  sinMaster.setFrequency(16000000);                           // Up to 80MHz
  sinMaster.setMaxTransferSize(SIN_BUFFER_SIZE * REP_TIMES);  // default: 4092 bytes

  // begin() after setting
  // note: the default pins are different depending on the board
  // please refer to README Section "SPI Buses and SPI Pins" for more details

  // TEST REMOVE ***
  master.begin(HSPI, SPI_CLK, SPI_MISO, SPI_MOSI, SPI_CS);  // pins from standard to actually used

  pinMode(SIN_MOSI, OUTPUT);
  digitalWrite(SIN_MOSI, LOW);

  sinMaster.begin(FSPI, -1, -1, SIN_MOSI, -1);              // begin just before use, because while registered the MOSI is HIGH in idle state
  pinMatrixOutAttach(SIN_MOSI, FSPID_IN_IDX, true, false);  // Last argument might be used to gate the signal

  //END SPI DMA


  // PIN setup

  //Shutdown/keepalive // removed in V6
  //  pinMode(SHTDWN, INPUT_PULLUP);
  //  adcAttachPin(SHTDWN);
  //  analogSetAttenuation(ADC_11db); //up to 3.3V = 2048

  //I2C
  pinMode(I2C_SDA, OUTPUT);
  pinMode(I2C_SCL, OUTPUT);

  //Triggers
  pinMode(TRIG_1, INPUT_PULLUP);
  pinMode(TRIG_2, INPUT_PULLUP);

  //Power
  pinMode(AFE_ON, OUTPUT);
  pinMode(SWITCHOFF, INPUT);

  //Vibration
  pinMode(VIB, OUTPUT);
  digitalWrite(VIB, LOW);

  //Keypad
  keypad.setDebounceTime(50);



  //ESP NOW SETUP

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  Serial.println("Try Init Esp Now");
  InitESPNow();
  Serial.println("Esp Now initialized");

  addBroadCast();

  //LCD initialization

  lcd.init();
  lcd.flipScreenVertically();
  lcd.clear();

  //Welcome screen

  buf = "";
  buf = "LightAir V6";  //HW version
  if (debug) buf.concat(" DEBUG");
  lcd.drawString(0, 0, buf);
  buf = "";
  buf.concat("Joined Team ");  //Game
  buf.concat(mycolor % 2);     //
  lcd.drawString(0, 10, buf);
  buf = "";
  buf.concat("Ready!");
  lcd.drawString(0, 20, buf);
  buf = "";
  lcd.display();

  delay(2000);

  // Search for other players by SSID names and show their short names on LCD
  // 10s loop to allow unsyncronized login

  // MAYBE A BETTER IDEA IS TO ALWAYS USE BROADCAST PACKETS!
  // That will require no initial MAC sharing and should result in longer radio range
  // Downside is the traffic may collide. Maybe a time division multiplexing can be used?

  //for (int i=0;i<1;i++){
  //    ScanForSlave();
  //    // If Slave is found, it would be populate in `slave` variable
  //    // We will check if `slave` is defined and then we proceed further
  //    if (PeerCnt > 0) { // check if slave channel is defined
  //      // `slave` is defined
  //      // Add slave as peer if it has not been added already
  //      manageSlave();
  //      // pair success or already paired
  //      // Send data to device
  //    } else {
  //      // No slave found to process
  //    }
  //    delay (1000);
  //    lcd.display(); //Show results for each loop
  //  }

  lcd.clear();
}



void loop() {

  delay(5);  // fast. Little rest between cycles


  /*
  // Update LCD. This way, that happens every iteration
  // May be optimized only refreshing new values when required (change in ammo, sens or points)
  // better to define in an outer function with a flag for refresh 
  lcd.clear();

  if(((millis()-gamestartime)/1000) > gametime)   endGame();
  
  if (ammo==0){
      buf = "";
      buf.concat("LOADING  |");
      for (int numbars=0; numbars<10; numbars++){
        if(numbars< ((10*(millis()-ammotic))/loadtime )) buf.concat("#");
        else buf.concat("_");
      }
      buf.concat("|");
      
//      buf.concat((float)(((millis()-ammotic)*100)/loadtime));
//      buf.concat("%");
      lcd.drawString(0,20, buf);
      lcd.display();
  }
  
  lcd.drawString(0, 30, "NRG:");
  buf="";
  buf.concat(ammo);
  lcd.drawString(40, 30, buf);
  buf="";
  buf.concat("*");
  buf.concat(points);
  lcd.drawString(90, 30, buf);
  buf="";
  buf.concat("TIME: ");
  if(gametime > ((millis()-gamestartime)/1000)) buf.concat(gametime-((millis()-gamestartime)/1000));
  
  lcd.drawString(0, 40, buf);
  buf="";
  lcd.drawString(0, 0, keyStr);
  buf="";
  
  lcd.display();

  delay(5);
  */

  // if the TRIGGER button is not pressed (normally HIGH) or the player is unable to shoot, it goes in the rest loop
  // keypad is read, sensors are read, ammo are reloaded etc.
  // when rest loop conditions are not met, the next step is recog, which is simply part of the main loop.
  // as the recog operation also flags "justshot", there is at least one iteration of rest loop for each recog to upload ammo on LCD
  while (!down && ((digitalRead(TRIG_1) == 1) || (ammo == 0) || (justshot))) {  // rest loop

    justshot = false;  //flags entrance to rest loop

    delay(5);  // otherwise the watchdog get starved!
    // Keypad check. For now keyboard does not trigger any effect but it would be used to navigate a menu or let the player make some choices (class, power ups, ... )
    if (keypad.getKeys()) {
      for (int i = 0; i < LIST_MAX; i++)  // Scan the whole key list.
      {
        if ((keypad.key[i].stateChanged) && (keypad.key[i].kstate != IDLE))  // Only find keys that have changed state.
        {
          if (debug) {
            Serial.print("Keypad press detected");
            Serial.print(keypad.key[i].kchar);
            Serial.print(" ");
            Serial.println(keypad.key[i].kstate);
          }
          switch (keypad.key[i].kstate) {  // Report active key state : IDLE, PRESSED, HOLD, or RELEASED
            //case PRESSED:
            //
            //break;
            case HOLD:
              keyStr = "";
              if (comm && keypad.key[i].kchar == 'A') {
                keyStr.concat("OUT");
                comm = false;
                if (debug) {
                  Serial.println("Key A hold: Out of game");
                }
              }

              if (!comm & keypad.key[i].kchar == 'B') {
                keyStr = "";
                comm = true;
                if (debug) {
                  Serial.println("Key B hold: In game");
                }
              }
              break;
              //case RELEASED:
              //keyStr.concat("R");
              //break;
              //case IDLE:
              //msg = " IDLE.";
          }
          //keyStr.concat(keypad.key[i].kchar);
        }
      }

      lcd.drawString(0, 0, keyStr);
      lcd.display();
    }

    // Read sensors. *** COMMENTED BECAUSE OF INTEREFERENCE WITH SHOT

    //    if (((millis()-lastsens) > senstime) ){   //Far from recog
    //      lastsens = millis();
    //
    //      delay(5);
    //      SPI.beginTransaction(SPISettings(ADC_CLK, MSBFIRST, SPI_MODE2));
    //      digitalWrite(SPI_CS, LOW);
    //      SPI.transfer16(ADC_READ_BAT); // Setup to read sens chan
    //      bat = SPI.transfer16(ADC_READ_BAT);
    //      SPI.endTransaction();
    //      volt = (bat/4095.0)*3.2*2;  //Battery is linked to a voltage partitioner that halves its voltage
    //
    //
    //      delay(5);
    //      SPI.beginTransaction(SPISettings(ADC_CLK, MSBFIRST, SPI_MODE2));
    //      digitalWrite(SPI_CS, LOW);
    //      SPI.transfer16(ADC_READ_LEDT); // Setup to read LED NTC
    //      ntc = SPI.transfer16(ADC_READ_LEDT); // Actually read LED NTC
    //
    //      SPI.endTransaction();
    //      ledtemp = ledtemplut[ntc>>5]; // LUTs have 128 positions (=7bits) so just take the ADC reading that is 12 bit down to 7bits
    //
    //      digitalWrite(SPI_CS, LOW);
    //      SPI.transfer16(ADC_READ_PDT); // Setup to read photodiode NTC
    //      ntc = SPI.transfer16(ADC_READ_PDT); // Actually read PD NTC
    //      SPI.endTransaction();
    //      pdtemp = pdtemplut[ntc>>5]; // LUTs have 128 positions (=7bits) so just take the ADC reading that is 12 bit down to 7bits
    //
    //    }

    // ammo load routine
    ammotac = millis() - ammotic;
    if ((!load) && (ammotac > loadtime)) {

      ammo = loadammo;  // replenish to default value. also possible ammo+loadammo to load over original value
      load = true;      // flags full load status
    }


    if (((millis() - gamestartime) / 1000) > gametime) {
      endGame();
      if (debug) {
        Serial.println("EndGame finished");
      }
    }
    lcd.clear();
    if (ammo == 0) {
      buf = "";
      buf.concat("LOADING  |");
      for (int numbars = 0; numbars < 10; numbars++) {
        if (numbars < ((10 * (millis() - ammotic)) / loadtime)) buf.concat("#");
        else buf.concat("_");
      }
      buf.concat("|");

      //      buf.concat((float)(((millis()-ammotic)*100)/loadtime));
      //      buf.concat("%");
      lcd.drawString(0, 20, buf);
      lcd.display();
    }

    lcd.drawString(0, 0, keyStr);

    lcd.drawString(0, 30, "NRG:");
    buf = "";
    buf.concat(ammo);
    lcd.drawString(40, 30, buf);
    buf = "";
    buf.concat("*");
    buf.concat(points);
    lcd.drawString(90, 30, buf);
    buf = "";
    buf.concat("TIME: ");
    if (gametime > ((millis() - gamestartime) / 1000)) buf.concat(gametime - ((millis() - gamestartime) / 1000));
    else endGame();
    lcd.drawString(0, 40, buf);
    buf = "";
    lcd.drawString(0, 0, keyStr);
    buf = "";
    lcd.display();
  }


  if (down) {
    downseq();
  }



  // recog operations start here.
  // if there are no rest loop conditions, the normal progression of the program goes in recog

  juston = false;

  digitalWrite(AFE_ON, HIGH);
  delay(5);

  //SPK AND VIB ON HERE
  //ledcWrite(0, 2048);
  soundCtrl.playLit();
  digitalWrite(VIB, HIGH);

  ammotic = millis();
  load = false;
  ammo = ammo - 1;
  gamestats[2] += 1;

  if (debug) ammo++;

  sinIter = 0;

  satCount = 0;

  // resets default values for R, G, B and elaborated data from the RGB photodiode
  r = 0;
  g = 0;
  b = 0;

  rout = 0;
  gout = 0;
  bout = 0;

  outr = 0.0;
  outg = 0.0;
  outb = 0.0;
  outsum = 0.0;
  angsum = 0.0;
  outang = 0.0;

  for (int m = 0; m < MULTITIMES; m++) {

    arrayiter = 0;  //reset arrayiter for every repetition, but keep rout,gout,bout

    master.queue(spi_master_tx_buf, spi_master_rx_buf1, SPI_BUFFER_SIZE * REP_TIMES);
    
    sinMaster.transfer(sin_buf, NULL, SIN_BUFFER_SIZE * REP_TIMES);
    master.yield();

    for (int w = SPI_BUFFER_SIZE; w < SPI_BUFFER_SIZE * REP_TIMES; w = w + 6) {  // First 2 bytes have response in next iterations; last 4 bytes are only there for an issue



      if ((spi_master_rx_buf1[w] << 8 | spi_master_rx_buf1[w + 1] > 10) && ((spi_master_rx_buf1[w + 2] << 8 | spi_master_rx_buf1[w + 3]) < 10) && ((spi_master_rx_buf1[w + 4] << 8 | spi_master_rx_buf1[w + 5]) > 10)) {
        satCount++;  //saturation solving routine : do not count measures saturated in at least one channel. Then account for that in void removal (cosine of corresponding angle)
      } else {
        rout += (spi_master_rx_buf1[w] << 8 | spi_master_rx_buf1[w + 1]) * sintab[arrayiter % GOERTZ_PERIOD];
        gout += (spi_master_rx_buf1[w + 2] << 8 | spi_master_rx_buf1[w + 3]) * sintab[arrayiter % GOERTZ_PERIOD];
        bout += (spi_master_rx_buf1[w + 4] << 8 | spi_master_rx_buf1[w + 5]) * sintab[arrayiter % GOERTZ_PERIOD];
      }

      if (debug) {
        if ((spi_master_rx_buf1[w] << 8 | spi_master_rx_buf1[w + 1]) >= 4085 || (spi_master_rx_buf1[w] << 8 | spi_master_rx_buf1[w + 1]) < 10 || (spi_master_rx_buf1[w + 2] << 8 | spi_master_rx_buf1[w + 3]) >= 4085 || (spi_master_rx_buf1[w + 2] << 8 | spi_master_rx_buf1[w + 3]) < 10 || (spi_master_rx_buf1[w + 4] << 8 | spi_master_rx_buf1[w + 5]) >= 4085 || (spi_master_rx_buf1[w + 4] << 8 | spi_master_rx_buf1[w + 5]) < 10) {
          Serial.print("***Saturation/null detected at iter***");
          Serial.println(arrayiter);
        }
      }

      arrayiter++;
    }

    /*
      // THIS IS THE CORRECT WAY TO CONSIDER VOID RESCALE IN CASE OF SATURATION. IT SHOULD BE COMPUTED FOR EACH OF MULTITIMES SINCE IT'S NOT GRANTED ALL THE MULTITIMES GIVE SAME VALUE.
      // THEN VOID SCALE VALUES SHOULD BE SAVED IN AN ARRAY AND FURTHER DATA ELABORATION SHOULD BE MADE FOR EACH OF MULTITIMES.
      // IT'S AN HARDER BUT MORE FLEXIBLE SOLUTION. ALTHOUGH, EVEN NOT RESCALING VOID VALUES SHOULD GIVE GOOD READINGS SINCE IN CASE OF SATURATION THE SIGNAL IS VERY HIGH AND VOID VALUES NEGLIGIBLE. 
      if (satCount > 0) //  since there is saturation, void values has to be rescaled to the actual used section of the modulation sine
      {
        // integral of sin(x)+1 around (PI/2) = x-cos(x) | [PI/2+satCount/2 ; PI/2-satCount/2] 
        // ( (PI/2+((satCount/2)*(2*PI))/(GOERTZ_PERIOD*1.0))-(cos(PI/2+((satCount/2)*(2*PI))/(GOERTZ_PERIOD*1.0))) ) - ( (PI/2-((satCount/2)*(2*PI))/(GOERTZ_PERIOD*1.0))-(cos(PI/2-((satCount/2)*(2*PI))/(GOERTZ_PERIOD*1.0))) )
        // = [cos (PI/2+x) = -sin(x); cos(PI/2-x) = sin(x) ] = 2*(satCount/2)*2*PI/(GOERTZ_PERIOD*1.0) + sin(((satCount/2)*(2*PI))/(GOERTZ_PERIOD*1.0)) + sin (((satCount/2)*(2*PI))/(GOERTZ_PERIOD*1.0))
        // = 2*satCount*PI/(GOERTZ_PERIOD*1.0) + 2*sin((satCount*PI)/(GOERTZ_PERIOD*1.0))

        satFloat = satCount*PI/(GOERTZ_PERIOD*1.0);
        2*(satFloat+sin(satFloat))/(2*PI);

      }
      */

    // Timer to wait until the next sine will be in synch with previous ones
    // measure sinMaster.transfer time and then divide by REP_TIMES (number of repetitions of a full sine for each sin buf)
  }

  // in the end of the recog routine LED, SPK and VIB are turned down
  digitalWrite(LEDON, LOW);
  tac = micros() - startime;
  soundCtrl.stop();
  digitalWrite(VIB, LOW);
  digitalWrite(AFE_ON, LOW);


  // begin data elaboration. // THIS ONE TO COMPLETE!!! *** ***

  // remove calibration values, that are actually the values read in a blank (no target) recog
  if (debug) {
    Serial.print("raw out: \t");
    Serial.print(rout);
    Serial.print("\t");
    Serial.print(gout);
    Serial.print("\t");
    Serial.println(bout);
  }

  rawsum = rout + gout + bout;

  if (rout > rcal) rout = rout - rcal;
  else rout = 0;
  if (gout > gcal) gout = gout - gcal;
  else gout = 0;
  if (bout > bcal) bout = bout - bcal;
  else bout = 0;

  if (debug) {
    Serial.print("out-cal: \t");
    Serial.print(rout);
    Serial.print("\t");
    Serial.print(gout);
    Serial.print("\t");
    Serial.println(bout);
  }

  if (rawsum > (limpow)) {  // enough power to suppose a target is lit. Limpow is taken from white diffuser target at close distance during calibration.
                            // may be better to use separate color limits, that are rlim, glim, blim

    if (debug) {
      Serial.print("limpow passed: ");
      Serial.print(rawsum);
      Serial.print(">");
      Serial.println(limpow);
    }

    // extract color coordinates: normalized R and G/R "angle" are good because they usally fall in rectangular-shaped blobs,
    // so that classification only requires comparison of coordinates with constant values (vertical and horizontal lines)
    // may be optimized to use integers, as float cast requires some extra operation, but is still very fast

    outr = rout * 1.0 * rfact;  // coordinate transform to white-centered color space
    outb = bout * 1.0 * bfact;
    outg = gout * 1.0;
    outsum = outr + outb + outg;
    angsum = outg + outb;

    if (debug) {
      Serial.print("rSpread=");
      Serial.print((((angsum * gstd) - (outg * sigma_ang)) / (pow(angsum, 2) - sigma_ang2)));
      Serial.print("\t");
      Serial.print("angSpread=");
      Serial.print(((rstd * outsum) - (outr * sigma_r)) / (pow(outsum, 2) - pow(sigma_r, 2)));
      Serial.println();
    }

    outg = outg / outsum;
    outr = outr / outsum;

    if (outr < 1) {
      outang = outg / (1 - outr);
    } else {
      outang = 1;
    }



    if (debug) {
      // Serial.println("Variance test passed!");
      Serial.print("Color coordinates = ");
      Serial.print(outr);
      Serial.print(" ; ");
      Serial.println(outang);
    }

    recog = false;
    // classification procedure: a target is likely hit, so check in which box it falls
    // possibly optimizable with a binary search tree, but even in this raw form it's very fast

    lcd.clear();

    for (int pp = 1; pp <= 12; pp++) {  // just check whether the color coordinates fall in any of the hitboxes
      if ((outr <= hitBox[pp][0]) && (outr >= hitBox[pp][1]) && (outang <= hitBox[pp][2]) && (outang >= hitBox[pp][3])) {
        //if the color coordinates fall in the box related to a player, write that on LCD and send that player a message
        // THIS PART WOULD BE BETTER DONE WITH AN OBJECT, LIKE GAME.ELABHIT(PP)
        buf = "";
        buf.concat("**HIT**");
        buf.concat(" ");
        buf.concat(pp);
        buf.concat("/");
        buf.concat(playnum);
        lcd.drawString(0, 0, buf);
        buf = "";
        //buf.concat(outr);
        //lcd.drawString(0,15, buf);
        //buf="";
        //buf.concat(outang);
        //lcd.drawString(60,15, buf);
        //buf="";
        buf.concat(players[pp]);
        lcd.drawString(0, 30, buf);
        buf = "";
        buf.concat("MATCH");
        lcd.drawString(70, 30, buf);
        // lcd.display(); // At the end of recognition
        recog = true;
        gamestats[3] += 1;

        if (debug) {
          Serial.println(players[pp]);
        }

        //HIT SIGNAL FOR PLAYER pp
        //it just contains in a single byte the sender number and the receiver number

        if (comm) {

          hitmsg = mycolor * 15;
          hitmsg = hitmsg + pp;
          sendData(hitmsg);  // Send in broadcast, no second arg

          if (debug) {
            Serial.print("Sent ");
            Serial.print(hitmsg);
            Serial.println(" Broadcast");
          }


          points = points + 1;

          // check correct reception
          if (sentsuccess) {
            buf = "";
            buf = "OK";
            lcd.drawString(0, 50, buf);
            sentsuccess = false;
          } else {
            buf = "";
            buf = "COMM ERROR";
            lcd.drawString(0, 50, buf);
          }
          break;
        }
      }
    }


    lcd.display();

    // Long beep if recog actually happened. Also shows display for a long enough time
    if (recog) {
      soundCtrl.playLit();
      if (comm) delay(2000);  // More time on screen for HIT in game
    }
  }

  // clear some data
  justshot = true;


  recog = false;

  outr = 0.0;
  outang = 0.0;

  rout = 0;
  gout = 0;
  bout = 0;

  rawsum = 0;
  sum = 0;

  r = 0;
  g = 0;
  b = 0;

  vpow = 0;
  deltaBat = 0;
}
