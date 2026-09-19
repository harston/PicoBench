// Host stub of pico/time.h: a fake clock that tests advance explicitly.
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef uint64_t absolute_time_t;
extern uint64_t g_host_time_us;
static inline absolute_time_t get_absolute_time(void) { return g_host_time_us; }
static inline int64_t absolute_time_diff_us(absolute_time_t from, absolute_time_t to) { return (int64_t)(to - from); }
