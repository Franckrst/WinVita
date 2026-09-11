// bench_gtc.c — micro-benchmark isolating the GetTickCount trap cost.
// Calls GetTickCount N times in a tight loop; the ON-vs-OFF wall-clock delta
// (D2_DISABLE_INTRINSICS) is the per-call intrinsic saving. Load time cancels
// in the delta. Build like torture.exe:
//   i686-w64-mingw32-gcc -m32 -O1 -nostartfiles -e _bench_entry \
//       -o bench_gtc.exe bench_gtc.c -lkernel32
#include <windows.h>

// N is passed at compile time (-DBENCH_N=...) so one source serves any size.
#ifndef BENCH_N
#define BENCH_N 20000000u
#endif

void __attribute__((noreturn)) bench_entry(void)
{
    volatile unsigned acc = 0;
    unsigned i;
    for (i = 0; i < (unsigned)BENCH_N; ++i)
        acc += GetTickCount();
    // Return the low byte so the run can't be optimized to nothing.
    ExitProcess(acc & 0xFF);
    for (;;) {}
}
