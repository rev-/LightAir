#pragma once
typedef struct { int dummy; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
static inline void taskENTER_CRITICAL(portMUX_TYPE*) {}
static inline void taskEXIT_CRITICAL(portMUX_TYPE*) {}
