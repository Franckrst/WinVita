/* src/platform/vita_lazymem.h — lazy (on-demand) backing for the guest arena.
 *
 * The single-arena model (cpu_box86.cpp) buys ONE host block covering the
 * whole guest span and addresses it as H(va) = va + membase. On a machine
 * with no virtual memory, every RESERVED byte of that block is real RAM,
 * even though a Win32 program reserves far more than it commits: measured on
 * the Wine trace of the guest this engine was written for, 193 MiB reserved
 * for 149 MiB ever touched, at a 64 KiB granularity.
 *
 * This unit splits the two: address space is reserved once, physical pages
 * are attached per 64 KiB CHUNK when someone actually commits (or touches)
 * them. Nothing here knows the guest program: it is the Win32 memory
 * CONTRACT (reserve != commit) expressed against the two platforms the
 * engine runs on.
 *
 *   * PS Vita: kuKernelMemReserve/MemCommit/MemDecommit (kubridge, bythos14
 *     fork >= v0.3 — see vita_kubridge.h). A data abort on a reserved but
 *     uncommitted chunk is caught by a user exception handler that commits
 *     the chunk and lets the instruction re-execute. That handler is a SAFETY
 *     NET, not the mechanism: the JIT writes through membase with no check,
 *     so a missed commit would otherwise be a hard crash.
 *   * Host/qemu-arm: mmap(PROT_NONE) + mprotect(RW) per chunk, and the same
 *     net inside the SIGSEGV handler. This is what makes the mechanism
 *     testable without hardware: an uncommitted page behaves exactly as on
 *     the Vita, including a kernel write into it (read(2)) failing with
 *     EFAULT instead of silently short-reading.
 *
 * OFF BY DEFAULT. WX86_ARENA_LAZY=1 arms it, =strict arms it WITHOUT the net
 * (a fault dies, for audits). Absent the variable, not a single byte of
 * behaviour changes: cpu_box86 takes its usual mmap path and every entry
 * point below is an inert no-op.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Commit granularity. 64 KiB is Win32's own allocation granularity (so a
 * chunk never straddles two reservations), it is the guest region's
 * alignment, and it keeps the kernel-side page list ~15x shorter than 4 KiB
 * chunks would while still recovering ~85 % of the reservation waste. */
#define WX86_LAZY_CHUNK 0x10000u

/* Modes of WX86_ARENA_LAZY. Read once, lazily: on Vita the variable arrives
 * from env.txt, which platform_init applies after static initialisation. */
enum { WX86_LAZY_OFF = 0, WX86_LAZY_NET = 1, WX86_LAZY_STRICT = 2 };
int  wx86_lazymem_mode(void);
/* Refuses lazy mode for the rest of the session (a failed probe, a consumer
 * that decided against it). Must be called before the arena is created. */
void wx86_lazymem_force_off(void);

/* Reserve + commit probe of `bytes` worth of address space, immediately
 * undone. Answers "can this platform do lazy at this size, right now" —
 * on Vita it also proves the kubridge imports resolved, since an unresolved
 * weak stub fails here instead of at the first commit. 1 = yes. */
int  wx86_lazymem_probe(uint64_t bytes);

/* Reserves [base, base+bytes) of address space, aligned to `align` (the
 * alignment slack costs NO physical memory, unlike an ordinary block).
 * Returns the aligned base, or NULL. `reserved_out` receives the whole
 * reservation size including the slack. Also installs the safety net. */
void* wx86_lazymem_reserve(uint64_t bytes, uint64_t align, uint64_t* reserved_out);

/* Guest-address API used by the engine. All are no-ops (returning "backed")
 * when lazy mode is off, so call sites need no #ifdef.
 *
 * The arena's guest VA 0 is the aligned base above, so a guest VA maps to a
 * chunk index by a shift — no lookup. */
int  wx86_arena_lazy(void);                          /* 1 = lazy arena live   */
int  wx86_arena_commit(uint32_t va, uint32_t n);     /* explicit (VirtualAlloc, va_alloc) */
int  wx86_arena_commit_fixed(uint32_t va, uint32_t n); /* Cpu::map/set_trap: never refused */
void wx86_arena_release(uint32_t va, uint32_t n);    /* whole chunks only     */
/* Engine-side dereference (hostptr/read/write): guarantees backing BEFORE a
 * kernel copy is aimed at the range, because a kernel write into an
 * unbacked page fails as a short read instead of taking our net. */
int  wx86_arena_ensure(uint32_t va, uint32_t n);
/* Range the consumer manages itself (the VirtualAlloc region): Cpu::map
 * leaves it uncommitted instead of backing what it maps. */
void wx86_arena_set_lazy_range(uint32_t va, uint32_t n);
/* Ceiling on TOTAL committed bytes. Beyond it, an explicit commit fails
 * honestly (the caller answers ERROR_NOT_ENOUGH_MEMORY) rather than letting
 * the kernel refuse a vitaGL or JIT allocation later. 0 = no ceiling. */
void wx86_arena_set_commit_budget(uint64_t bytes);

/* Safety net. Returns 1 when the fault was ours and the instruction may be
 * re-executed. Async-signal-safe: no allocation, no locks, no printf. */
int  wx86_lazymem_fault(uintptr_t host_addr);

typedef struct Wx86LazyStats {
    uint64_t reserved;       /* address space held (with the alignment slack) */
    uint64_t committed;      /* bytes currently backed                        */
    uint64_t peak;           /* high-water of the above                       */
    uint64_t budget;         /* ceiling, 0 = none                             */
    uint32_t chunks_fixed;   /* chunks backed by Cpu::map / set_trap          */
    uint32_t chunks_explicit;/* by VirtualAlloc(MEM_COMMIT) / va_alloc        */
    uint32_t chunks_engine;  /* by an engine dereference (should stay small)  */
    uint32_t chunks_net;     /* by the safety net (a MISSED commit: expect 0) */
    uint32_t chunks_released;
    uint32_t refused;        /* commits refused by the budget                 */
    uint32_t net_refused;    /* refused INSIDE the net: the next stop is death*/
    uint32_t net_last_va;    /* guest VA of the last net fault                */
    uint32_t net_last_fsr;   /* Vita: FSR of the last net fault               */
    uint32_t eng_last_va;    /* guest VA of the last engine-side commit       */
} Wx86LazyStats;
void wx86_lazymem_stats(Wx86LazyStats* out);

/* Fast path for the hot engine calls: NULL when lazy mode is off, so one
 * predictable load+branch answers "nothing to do". */
extern uint8_t* wx86_lazymem_state;

/* TEST-ONLY. Not part of the engine contract, not called by any real code
 * path: exposes the bounded foreign-fault repeat counter that the Vita-only
 * safety net (vita_lazymem.c, lazy_dabt) bails out on, so
 * tools/lazymem_net_selftest.cpp can prove the counting/threshold/eviction
 * logic on desktop and qemu-arm -- platforms with no kubridge and no closed
 * component able to refault deterministically the way lazy_dabt itself
 * guards against, so lazy_dabt cannot be exercised there at all. Always
 * compiled (no #ifdef __vita__): the logic itself has no Vita dependency. */
uint32_t wx86_lazymem_test_net_hit(uintptr_t host_addr, uint32_t pc);
uint32_t wx86_lazymem_test_net_max_replay(void);
uint32_t wx86_lazymem_test_net_track_n(void);
void     wx86_lazymem_test_net_reset(void);

#ifdef __cplusplus
}
#endif
