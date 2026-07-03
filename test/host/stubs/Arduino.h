// Host-test stub of the Arduino core.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
extern uint32_t g_millis;
static inline uint32_t millis() { return g_millis; }
static inline void delay(uint32_t) {}
#define PROGMEM
