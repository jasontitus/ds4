#ifndef BENCH_TIMING_H
#define BENCH_TIMING_H

#if !defined(_POSIX_C_SOURCE) || _POSIX_C_SOURCE < 200809L
#  undef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>
#include <time.h>

static inline uint64_t bench_now_ns(void) {
    struct timespec ts;
#if defined(CLOCK_MONOTONIC_RAW)
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

#endif
