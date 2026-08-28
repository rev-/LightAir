#pragma once
typedef int gpio_num_t;

// Host stub: pin writes go nowhere, reads report low.
static inline int gpio_set_level(gpio_num_t, int) { return 0; }
static inline int gpio_get_level(gpio_num_t)      { return 0; }
