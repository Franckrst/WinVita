// src/runtime/host_clock.cpp — voir host_clock.h.
//
// Corps identique a celui que les deux portages hebergeaient chacun de leur
// cote (rt_now_us de d2vita, d2vita_now_us de carn-vita) et a celui de
// prof.cpp, qui n'etait compile qu'avec -DPROF_COUNTERS et ne pouvait donc
// pas servir au reste du moteur.
#include "runtime/host_clock.h"

#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#else
#include <time.h>
#endif

uint64_t wx86_now_us() {
#ifdef __vita__
    // Le noyau Sony compte deja en microsecondes.
    return (uint64_t)sceKernelGetProcessTimeWide();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
#endif
}

uint64_t wx86_now_ms() { return wx86_now_us() / 1000ull; }
