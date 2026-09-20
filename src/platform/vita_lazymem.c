/* src/platform/vita_lazymem.c — see vita_lazymem.h for the contract.
 *
 * Structure, per chunk of 64 KiB: ONE state byte, moved only by CAS.
 *
 *     0 RESERVED   address space only, no physical page
 *     1 BUSY       one thread is backing it right now
 *     2 COMMITTED  backed AND zeroed
 *
 * Why a state byte and not a lock: the safety net runs in a fault handler,
 * on whatever thread faulted, possibly while that thread holds the newlib
 * heap lock or the GIL. It may not take a lock of its own, allocate, or
 * print. A CAS plus a spin on BUSY is the whole synchronisation, and the
 * loser of a race simply waits for the winner's zero-fill to be visible —
 * without it, two threads could both zero a chunk and the second memset
 * would erase what the first one's caller already wrote.
 *
 * The kernel does NOT hand back zeroed pages (neither does a Vita memblock),
 * so the chunk is zeroed here, inside the BUSY window. Win32 guarantees
 * MEM_COMMIT reads as zero and the guest relies on it.
 */
#include "platform/vita_lazymem.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __vita__
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include "platform/vita_kubridge.h"
#else
#include <sched.h>
#include <sys/mman.h>
#endif

/* Engine log sink (platform/vita_host.h): C++ header, C unit — declared here
 * like mman_vita.c does. On hardware there is no stderr, and a silent
 * fallback to the eager path would look like "the lazy arena worked". */
void wx86_vita_progress_c(const char* msg);

static void lz_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void lz_log(const char* fmt, ...) {
    char m[192];
    va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(m, sizeof m, fmt, ap);
    __builtin_va_end(ap);
    wx86_vita_progress_c(m);
    fprintf(stderr, "[lazymem] %s\n", m);
}

/* ---- state ---------------------------------------------------------------- */

enum { ST_RESERVED = 0, ST_BUSY = 1, ST_COMMITTED = 2 };
enum { WHY_FIXED = 0, WHY_EXPLICIT, WHY_ENGINE, WHY_NET };

uint8_t* wx86_lazymem_state = NULL;      /* NULL = lazy mode off (hot path) */
static uintptr_t g_base;                 /* host address of guest VA 0      */
static uint64_t  g_span;                 /* usable bytes behind g_base      */
static uint32_t  g_nchunk;
static uint64_t  g_reserved;             /* with the alignment slack        */
static uint64_t  g_committed, g_peak, g_budget;
static uint32_t  g_cnt[4];               /* chunks backed, per WHY_*        */
static uint32_t  g_released, g_refused, g_net_refused;
static uint32_t  g_net_va, g_net_fsr, g_eng_va;
static uintptr_t g_last_replay;          /* loop guard in the net           */
static uint32_t  g_lazy_lo, g_lazy_hi;   /* consumer-managed guest range    */
static int       g_mode = -1;            /* -1 = env not read yet           */
#ifdef __vita__
static SceUID    g_uid = -1;
static Wx86KuExcpHandler g_old_dabt;
static uint32_t  g_net_foreign;          /* foreign (non-arena) DABTs the net has waved through */
#endif

int wx86_lazymem_mode(void) {
    if (g_mode < 0) {
        const char* e = getenv("WX86_ARENA_LAZY");
        if (!e || !*e || (e[0] == '0' && !e[1])) g_mode = WX86_LAZY_OFF;
        else if (e[0] == 's' || e[0] == 'S')     g_mode = WX86_LAZY_STRICT;
        else                                      g_mode = WX86_LAZY_NET;
    }
    return g_mode;
}
void wx86_lazymem_force_off(void) { g_mode = WX86_LAZY_OFF; }

/* ---- platform primitives -------------------------------------------------- */

/* Backs ONE chunk. Returns 0 on success. Never zeroes: the caller does, so
 * the zero-fill stays inside the BUSY window. */
static int plat_commit(uintptr_t addr) {
#ifdef __vita__
    return kuKernelMemCommit((void*)addr, WX86_LAZY_CHUNK,
                             WX86_KU_PROT_READ | WX86_KU_PROT_WRITE, NULL) < 0 ? -1 : 0;
#else
    return mprotect((void*)addr, WX86_LAZY_CHUNK, PROT_READ | PROT_WRITE);
#endif
}

static void plat_release(uintptr_t addr) {
#ifdef __vita__
    kuKernelMemDecommit((void*)addr, WX86_LAZY_CHUNK);
#else
    /* MADV_DONTNEED first: on an anonymous private mapping it returns the
     * pages AND guarantees the next read is zero, which is what the Vita's
     * decommit does. PROT_NONE then restores the "unbacked" behaviour a
     * kernel copy must trip over. */
    madvise((void*)addr, WX86_LAZY_CHUNK, MADV_DONTNEED);
    mprotect((void*)addr, WX86_LAZY_CHUNK, PROT_NONE);
#endif
}

static void plat_pause(void) {
#ifdef __vita__
    sceKernelDelayThread(50);
#else
    sched_yield();
#endif
}

/* ---- chunk state machine -------------------------------------------------- */

static void peak_update(uint64_t v) {
    uint64_t p = __atomic_load_n(&g_peak, __ATOMIC_RELAXED);
    while (v > p && !__atomic_compare_exchange_n(&g_peak, &p, v, 1,
                                                 __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

/* 1 = the chunk is backed on return, 0 = refused (budget or kernel). */
static int chunk_commit(uint32_t i, int why) {
    for (;;) {
        uint8_t s = __atomic_load_n(&wx86_lazymem_state[i], __ATOMIC_ACQUIRE);
        if (s == ST_COMMITTED) return 1;
        if (s == ST_BUSY) { plat_pause(); continue; }
        uint8_t expect = ST_RESERVED;
        if (!__atomic_compare_exchange_n(&wx86_lazymem_state[i], &expect, ST_BUSY, 1,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            continue;                                   /* someone else got it */
        /* The budget protects everything allocated AFTER the arena (JIT pool,
         * vitaGL, the C heap): they are sized at startup and would simply be
         * refused by the kernel if the guest ate the free memory first.
         * WHY_FIXED is exempt — those chunks are the engine's own layout,
         * paid in full by the eager path too. */
        uint64_t now = __atomic_add_fetch(&g_committed, (uint64_t)WX86_LAZY_CHUNK, __ATOMIC_ACQ_REL);
        if (why != WHY_FIXED && g_budget && now > g_budget) {
            __atomic_sub_fetch(&g_committed, (uint64_t)WX86_LAZY_CHUNK, __ATOMIC_ACQ_REL);
            __atomic_store_n(&wx86_lazymem_state[i], ST_RESERVED, __ATOMIC_RELEASE);
            __atomic_add_fetch(&g_refused, 1u, __ATOMIC_RELAXED);
            if (why == WHY_NET) __atomic_add_fetch(&g_net_refused, 1u, __ATOMIC_RELAXED);
            return 0;
        }
        const uintptr_t addr = g_base + (uintptr_t)i * WX86_LAZY_CHUNK;
        if (plat_commit(addr) != 0) {
            __atomic_sub_fetch(&g_committed, (uint64_t)WX86_LAZY_CHUNK, __ATOMIC_ACQ_REL);
            __atomic_store_n(&wx86_lazymem_state[i], ST_RESERVED, __ATOMIC_RELEASE);
            __atomic_add_fetch(&g_refused, 1u, __ATOMIC_RELAXED);
            if (why == WHY_NET) __atomic_add_fetch(&g_net_refused, 1u, __ATOMIC_RELAXED);
            return 0;
        }
        memset((void*)addr, 0, WX86_LAZY_CHUNK);
        peak_update(now);
        // An engine-side commit means a shim dereferenced guest memory the
        // engine had not backed: never fatal (it is backed now), but it is
        // the address to explain, so keep the last one.
        if (why == WHY_ENGINE) g_eng_va = (uint32_t)(i * WX86_LAZY_CHUNK);
        __atomic_add_fetch(&g_cnt[why], 1u, __ATOMIC_RELAXED);
        __atomic_store_n(&wx86_lazymem_state[i], ST_COMMITTED, __ATOMIC_RELEASE);
        return 1;
    }
}

static int range_commit(uint32_t va, uint32_t n, int why) {
    if (!n) return 1;
    uint64_t end = (uint64_t)va + n;
    if (end > g_span) end = g_span;
    if ((uint64_t)va >= end) return 1;
    const uint32_t first = va / WX86_LAZY_CHUNK;
    const uint32_t last  = (uint32_t)((end - 1) / WX86_LAZY_CHUNK);
    int ok = 1;
    for (uint32_t i = first; i <= last; ++i)
        if (!chunk_commit(i, why)) ok = 0;
    return ok;
}

/* ---- public API ----------------------------------------------------------- */

int wx86_arena_lazy(void) { return wx86_lazymem_state != NULL; }

int wx86_arena_commit(uint32_t va, uint32_t n) {
    if (!wx86_lazymem_state) return 1;
    return range_commit(va, n, WHY_EXPLICIT);
}

int wx86_arena_commit_fixed(uint32_t va, uint32_t n) {
    if (!wx86_lazymem_state) return 1;
    /* Cpu::map over the consumer-managed range must NOT back it: that range
     * is the whole point (a Win32 program reserves it and commits a fraction). */
    const uint64_t lo = va, hi = (uint64_t)va + n;
    if (g_lazy_hi <= g_lazy_lo || hi <= g_lazy_lo || lo >= g_lazy_hi)
        return range_commit(va, n, WHY_FIXED);              /* no overlap */
    int ok = 1;
    if (lo < g_lazy_lo) ok &= range_commit((uint32_t)lo, (uint32_t)(g_lazy_lo - lo), WHY_FIXED);
    if (hi > g_lazy_hi) ok &= range_commit(g_lazy_hi, (uint32_t)(hi - g_lazy_hi), WHY_FIXED);
    return ok;
}

int wx86_arena_ensure(uint32_t va, uint32_t n) {
    if (!wx86_lazymem_state) return 1;
    if (!n) n = 1;
    uint64_t end = (uint64_t)va + n;
    if (end > g_span) end = g_span;
    if ((uint64_t)va >= end) return 1;
    const uint32_t first = va / WX86_LAZY_CHUNK, last = (uint32_t)((end - 1) / WX86_LAZY_CHUNK);
    for (uint32_t i = first; i <= last; ++i)
        if (__atomic_load_n(&wx86_lazymem_state[i], __ATOMIC_ACQUIRE) != ST_COMMITTED)
            return range_commit(va, (uint32_t)(end - va), WHY_ENGINE);
    return 1;
}

void wx86_arena_release(uint32_t va, uint32_t n) {
    if (!wx86_lazymem_state || !n) return;
    uint64_t end = (uint64_t)va + n;
    if (end > g_span) end = g_span;
    /* Whole chunks only: a chunk shared with a live neighbour must stay. The
     * guest region hands out 64 KiB-aligned blocks, so this is the common case. */
    uint64_t lo = ((uint64_t)va + WX86_LAZY_CHUNK - 1) & ~(uint64_t)(WX86_LAZY_CHUNK - 1);
    uint64_t hi = end & ~(uint64_t)(WX86_LAZY_CHUNK - 1);
    for (uint64_t a = lo; a + WX86_LAZY_CHUNK <= hi; a += WX86_LAZY_CHUNK) {
        const uint32_t i = (uint32_t)(a / WX86_LAZY_CHUNK);
        uint8_t expect = ST_COMMITTED;
        if (!__atomic_compare_exchange_n(&wx86_lazymem_state[i], &expect, ST_BUSY, 1,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            continue;                                   /* not backed, or busy */
        plat_release(g_base + (uintptr_t)a);
        __atomic_sub_fetch(&g_committed, (uint64_t)WX86_LAZY_CHUNK, __ATOMIC_ACQ_REL);
        __atomic_add_fetch(&g_released, 1u, __ATOMIC_RELAXED);
        __atomic_store_n(&wx86_lazymem_state[i], ST_RESERVED, __ATOMIC_RELEASE);
    }
}

void wx86_arena_set_lazy_range(uint32_t va, uint32_t n) { g_lazy_lo = va; g_lazy_hi = va + n; }
void wx86_arena_set_commit_budget(uint64_t bytes) { g_budget = bytes; }

int wx86_lazymem_fault(uintptr_t addr) {
    uint8_t* st = wx86_lazymem_state;
    if (!st) return 0;
    if (addr < g_base || addr - g_base >= g_span) return 0;   /* not our arena */
    const uint32_t i = (uint32_t)((addr - g_base) / WX86_LAZY_CHUNK);
    g_net_va = (uint32_t)(addr - g_base);
    if (g_mode == WX86_LAZY_STRICT) return 0;                 /* audit mode: die */
    if (__atomic_load_n(&st[i], __ATOMIC_ACQUIRE) == ST_COMMITTED) {
        /* Either a racing thread backed it between the abort and here (replay
         * is right), or the page is inaccessible for another reason (the
         * dynarec's write protection) and replaying would spin forever. Allow
         * exactly one replay per address. */
        uintptr_t prev = __atomic_exchange_n(&g_last_replay, addr, __ATOMIC_ACQ_REL);
        return prev != addr;
    }
    __atomic_store_n(&g_last_replay, (uintptr_t)0, __ATOMIC_RELEASE);
    return chunk_commit(i, WHY_NET);
}

void wx86_lazymem_stats(Wx86LazyStats* o) {
    if (!o) return;
    memset(o, 0, sizeof *o);
    o->reserved = g_reserved;
    o->committed = __atomic_load_n(&g_committed, __ATOMIC_RELAXED);
    o->peak = __atomic_load_n(&g_peak, __ATOMIC_RELAXED);
    o->budget = g_budget;
    o->chunks_fixed = g_cnt[WHY_FIXED];
    o->chunks_explicit = g_cnt[WHY_EXPLICIT];
    o->chunks_engine = g_cnt[WHY_ENGINE];
    o->chunks_net = g_cnt[WHY_NET];
    o->chunks_released = g_released;
    o->refused = g_refused;
    o->net_refused = g_net_refused;
    o->net_last_va = g_net_va;
    o->net_last_fsr = g_net_fsr;
    o->eng_last_va = g_eng_va;
}

/* ---- foreign-fault repeat tracking (see lazy_dabt below for the full story) ----
 *
 * Deliberately built OUTSIDE any #ifdef __vita__: it has no Vita dependency
 * (uintptr_t/uint32_t/atomics/plat_pause are already portable in this file),
 * so it compiles -- and can be unit-tested -- on desktop and qemu-arm too,
 * even though its only real caller (lazy_dabt) is Vita-only. That is what
 * makes tools/lazymem_net_selftest.cpp possible: kubridge, and any closed
 * component that could refault at a fixed (address, PC) forever, exist only
 * on real hardware, so the bound below cannot be exercised end to end off
 * console -- but the counting primitive it is built on can be, exactly.
 *
 * WX86_NET_MAX_REPLAY: how many times in a row the SAME (host fault
 * address, faulting PC) is tolerated before giving up on it. Chosen as 3:
 * 1 (first sighting: log + re-arm, the previous fix's behaviour, unchanged)
 * + 1 free retry for a genuinely transient condition (the kind the previous
 * fix's own comment already expected: "the replay either succeeds... or the
 * kernel's default handling produces its usual crash") + 1 more margin
 * before concluding it is truly stuck -- without letting the count grow
 * large enough to matter either way. Verified against kubridge's own kernel
 * source (exceptions.S / exceptions_bootstrap.S, same source read for the
 * commit this extends) before picking a number: EVERY occurrence, transient
 * or not, pays a full kernel exception round trip -- context save/restore of
 * 13 GPRs plus 32 VFP/NEON doubles, a spinlock taken with IRQs suspended
 * (ksceKernelSpinlockLowLockCpuSuspendIntr), a mode switch to user and back
 * through the exceptionBootstrap trampoline -- plus, in THIS handler
 * specifically, an unconditional kuKernelRegisterExceptionHandler call and an
 * fprintf to stderr (lz_log). None of that is free, but none of it is what
 * makes a tight loop dangerous either: what matters is that it is BOUNDED.
 * Even if a future change made every step here as fast as physically
 * possible, 3 iterations of a kernel exception round trip is microseconds to
 * low milliseconds on this CPU -- nowhere near any plausible watchdog window
 * -- while the previous, unbounded version could run for as long as the
 * faulting address kept faulting, which is the failure mode believed to have
 * caused the two full console reboots this fix responds to (see ROADMAP.md,
 * phase 5, point 4bis: kernel-level infinite replay is exactly the shape of
 * bug a hardware watchdog exists to catch).
 *
 * A table, not a single global counter: g_last_replay above already exists
 * for a DIFFERENT purpose (an already-COMMITTED arena chunk that refaults
 * for some other reason), and reusing it here would conflate two unrelated
 * fault classes through one shared slot -- exactly the kind of cross-thread
 * false positive/negative its own comment already warns about for its own,
 * narrower use. This is a small, separate, fixed-size table instead:
 *   - Address ALONE is not enough to identify "the same fault coming back":
 *     two different call sites, at two different moments, can legitimately
 *     fault on the same reused host page. Comparing the faulting PC too is
 *     what tells "the same instruction refaulting" apart from coincidence.
 *   - Fixed size (WX86_NET_TRACK_N slots), round-robin eviction when full:
 *     correctness for the case this guards against (ONE address stuck in a
 *     loop) does not depend on which slot holds it, and a genuinely stuck
 *     fault reappears on the very next call regardless of which slot it
 *     lands in -- eviction caused by unrelated, CONCURRENT foreign faults on
 *     other addresses can only delay reaching the limit for a given address
 *     (its counter restarts at 1 if evicted and then hit again), never
 *     prevent it, since the count keeps climbing back up every time that
 *     address keeps refaulting. Honest limit, stated plainly: if
 *     WX86_NET_TRACK_N or more DISTINCT foreign addresses were all looping
 *     at once (never observed, and would itself be an extraordinary
 *     situation), the table could keep resetting several of their counters
 *     and delay each one's individual bail-out past WX86_NET_MAX_REPLAY
 *     calls to that specific address -- still bounded (every slot is reused
 *     at least once every WX86_NET_TRACK_N insertions), just not by exactly
 *     that number in that pathological case.
 *   - A short spinlock guards the table, not lock-free atomics: this path
 *     already gave up async-signal-safety the moment lz_log() started
 *     calling fprintf() (previous commit), so a brief, uncontended spin here
 *     adds no new class of risk.
 */
#define WX86_NET_TRACK_N    8
#define WX86_NET_MAX_REPLAY 3
typedef struct { uintptr_t far_; uint32_t pc; uint32_t n; } Wx86NetTrackSlot;
static Wx86NetTrackSlot g_net_track[WX86_NET_TRACK_N];
static volatile uint8_t g_net_track_busy;
static uint32_t         g_net_track_next;   /* round-robin eviction cursor */

/* Returns how many times (far_, pc) has now been seen IN A ROW (1 on first
 * sighting this "streak"). Never allocates; blocks at most as long as the
 * tiny critical section below (an array scan/update, no I/O). */
static uint32_t net_track_hit(uintptr_t far_, uint32_t pc) {
    while (__atomic_test_and_set(&g_net_track_busy, __ATOMIC_ACQUIRE)) plat_pause();
    int slot = -1;
    for (int i = 0; i < WX86_NET_TRACK_N; ++i)
        if (g_net_track[i].n && g_net_track[i].far_ == far_ && g_net_track[i].pc == pc) { slot = i; break; }
    if (slot < 0) {
        for (int i = 0; i < WX86_NET_TRACK_N; ++i)
            if (!g_net_track[i].n) { slot = i; break; }
        if (slot < 0) { slot = (int)(g_net_track_next % WX86_NET_TRACK_N); ++g_net_track_next; }
        g_net_track[slot].far_ = far_;
        g_net_track[slot].pc = pc;
        g_net_track[slot].n = 0;
    }
    uint32_t n = ++g_net_track[slot].n;
    __atomic_clear(&g_net_track_busy, __ATOMIC_RELEASE);
    return n;
}

/* TEST-ONLY entry points (tools/lazymem_net_selftest.cpp). No engine path
 * calls these -- lazy_dabt is the only real caller of net_track_hit, and it
 * is Vita-only. They exist because lazy_dabt itself cannot be exercised off
 * real hardware: there is no kubridge, and no closed component able to
 * refault deterministically at a fixed address+PC, under qemu-arm or on
 * desktop. This lets the counting primitive the bail-out decision is built
 * on be proven correct on every platform that builds this file -- strictly
 * better than leaving it entirely unverified until console access is
 * available again. What this does NOT prove, and cannot: the Vita-only
 * kuKernelRegisterExceptionHandler/kuKernelReleaseExceptionHandler calls
 * around it in lazy_dabt, or real kernel replay timing. Those stay
 * unverified before an actual console run. */
uint32_t wx86_lazymem_test_net_hit(uintptr_t far_, uint32_t pc) { return net_track_hit(far_, pc); }
uint32_t wx86_lazymem_test_net_max_replay(void) { return WX86_NET_MAX_REPLAY; }
uint32_t wx86_lazymem_test_net_track_n(void) { return WX86_NET_TRACK_N; }
void wx86_lazymem_test_net_reset(void) {
    while (__atomic_test_and_set(&g_net_track_busy, __ATOMIC_ACQUIRE)) plat_pause();
    memset(g_net_track, 0, sizeof g_net_track);
    g_net_track_next = 0;
    __atomic_clear(&g_net_track_busy, __ATOMIC_RELEASE);
}

/* ---- reservation ---------------------------------------------------------- */

#ifdef __vita__
/* Safety net (R1 of docs/audits/ram/kubridge.md). The JIT writes straight
 * through membase with no check, so a chunk the engine failed to commit
 * would be a hard data abort. This commits it and returns, which makes
 * kubridge reload the context and re-execute the faulting instruction.
 * Every hit is a MISSED commit somewhere in the engine — hence the counter.
 *
 * PROCESS-WIDE, NOT ENGINE-SCOPED: kuKernelRegisterExceptionHandler installs
 * this handler for the whole process, on every thread that takes a data
 * abort — not just the emulated CPU. A fault from anywhere else (a closed
 * Sony component, a future native shim, anything) reaches this function too,
 * and wx86_lazymem_fault() correctly says "not mine" for it.
 *
 * Bug found and fixed here (2026-09-20): the old "no previous handler" branch
 * called kuKernelReleaseExceptionHandler and nothing else, which unregisters
 * the handler for the REST OF THE PROCESS's LIFETIME, permanently and with
 * no log line. The very first foreign fault anywhere in the process — kernel
 * PID 0x... aside, this includes libshacccg/GXM during vitaGL init — tore
 * down the net for good; every genuine lazy-arena fault afterwards (normal
 * operation of this mechanism) then had no handler to catch it and fell
 * straight into the kernel's default handling: a hard, silent crash with no
 * "stop:" line and an unfamiliar psp2core-SceKernelProcess.spsp2dmp dump —
 * exactly what was observed on console. Confirmed by code audit; kubridge
 * and Sony's tools are not at fault (see ROADMAP.md, phase 5).
 *
 * Verified against kubridge's own kernel source (bythos14/kubridge,
 * src/exceptions.S + src/exceptions_bootstrap.S) before writing this fix:
 *   - LReturnToExceptionHandler calls GetExceptionHandler() to look up the
 *     per-process handler table EXACTLY ONCE per fault, at the very top of
 *     dispatch, before the user handler ever runs.
 *   - The chosen handler runs in user mode (exceptionBootstrap: blx r1),
 *     then signals "done" with `udf #0`, which re-enters the kernel at
 *     UndefExceptionHandler_lvl0 and falls into LRestoreExceptionContext.
 *     That path reloads the ORIGINAL saved context (untouched since before
 *     the jump to user mode) and rfe's straight back to the faulting PC —
 *     UNCONDITIONALLY. It does NOT re-read the handler table.
 *   - The table is consulted again only at the top of the NEXT fault's
 *     dispatch (a fresh call to GetExceptionHandler).
 * Consequence: what this function does to the registration BEFORE returning
 * has no effect at all on how THIS fault replays — that was already decided
 * the moment kubridge picked us to run. It only affects which handler (if
 * any) sees the fault AFTER that replay: either the same address faulting
 * again (a deterministic foreign fault) or a completely unrelated one on any
 * thread.
 *
 * Fix: re-register ourselves immediately, in the same breath as noticing the
 * foreign fault, instead of releasing and leaving the net down. Because the
 * lookup that matters for THIS replay already happened (see above), a
 * genuinely fatal, non-reproducible foreign abort still resolves exactly as
 * before: the replay either succeeds (transient condition) or the kernel's
 * default handling produces its usual crash and dump when it faults again
 * with nobody left to hand it to a second time in a row — we are simply no
 * longer the reason nobody is left. What changes is that a one-off foreign
 * fault can never again permanently blind the net for the rest of the
 * process.
 *
 * UPDATE (2026-09-20, same day, after this exact residual risk materialised):
 * the paragraph above accepted "a foreign address that refaults identically
 * forever now loops instead of crashing" as a strictly-better trade-off, on
 * the reasoning that a detectable hang beats a silent kernel crash. That
 * reasoning under-weighted WHERE the hang runs: this replay loop is inside
 * the KERNEL's own exception dispatch, not user-space, so nothing in the
 * process (or even a user-space watchdog) can ever observe or interrupt it.
 * A loop with no possible exit is exactly the shape of fault a hardware
 * watchdog exists to catch by rebooting the whole device. Two console test
 * runs with WX86_ARENA_LAZY=1 after this fix shipped each ended in a full
 * console reboot, one right after the other — the log froze at a different,
 * early point each time (consistent with a genuinely async foreign fault,
 * not a deterministic one always hitting the same spot), and the user
 * physically observed the console restart, not just the app dying. This is
 * not proven at the kernel level (no console access is available while this
 * follow-up is being written — see ROADMAP.md, phase 5), but it is by far
 * the most coherent explanation on the facts, and the residual risk this
 * comment already named is a mechanism fully capable of producing exactly
 * that symptom. Treated as confirmed for engineering purposes; see the
 * bound added below and ROADMAP.md for the full writeup and its honest
 * uncertainty.
 *
 * Fix: bound how many times in a row the SAME foreign fault (same host
 * address AND same PC — see the net_track_hit block above this function for
 * why both, and the full reasoning for the limit and the table) is allowed
 * to replay before this handler deliberately lets it crash instead. Below
 * the limit, behaviour is UNCHANGED from the paragraph above: re-register
 * (no window where the net is down) and log. At the limit, this handler
 * does the one thing proven above to make a replay actually terminal
 * instead of a no-op for the fault in hand: it releases the handler and
 * does NOT re-register. Because the replay of THIS fault was already
 * decided before this call even started running (see above), releasing now
 * has zero effect on it; what it does is ensure that when this exact
 * address faults again at this exact PC — guaranteed, since by definition
 * it got here by doing exactly that WX86_NET_MAX_REPLAY times running —
 * GetExceptionHandler() finds nothing registered and the kernel's own
 * default handling takes over: the clean crash and dump this file has
 * always aimed to preserve for a fault that was never going to resolve.
 *
 * This is NOT the bug this file already fixed once, and the difference is
 * exact and important: the earlier bug released on the FIRST foreign fault
 * ever seen, unconditionally, disarming the net for the rest of the
 * process's life before a single genuine lazy-arena fault had a chance to
 * occur. This release only ever happens after WX86_NET_MAX_REPLAY identical
 * occurrences of ONE specific (address, PC) pair — every other foreign
 * fault, and every subsequent occurrence of a DIFFERENT (address, PC) pair,
 * still gets the full re-register-and-log treatment from the paragraph
 * above, unaffected.
 *
 * Does the net need to re-arm for foreign faults at OTHER addresses after
 * this release, in case the process somehow survives? On this platform,
 * the answer verified in the kernel source is that it practically cannot
 * come up: a user-mode data abort with no handler is fatal to the WHOLE
 * PROCESS, not just the faulting thread (LFatalError explicitly calls
 * sceKernelExitProcess on the sibling stack-corruption path, and nothing
 * reviewed in exceptions.S / exceptions_bootstrap.S offers a thread-only
 * recovery route). Re-registering immediately after this release, hoping to
 * cover that near-impossible survival case, would defeat the entire fix: it
 * would let THIS exact fault be caught again on its very next occurrence,
 * resurrecting the infinite loop this bound exists to end — release is the
 * mechanism that turns "replay" into "crash" for this address, and it only
 * works if it is not immediately undone. Explicit, honest limitation: if
 * Vita/kubridge semantics are ever wrong about this, or change in the
 * future, the net stays down for the rest of that process's life after this
 * release, same failure mode as the bug fixed earlier — accepted here only
 * because the alternative (never bounding the loop at all) is the confirmed,
 * worse, console-rebooting failure this exact fix responds to.
 *
 * No call to kuKernelReleaseExceptionHandler EXCEPT at the bound above: a
 * bare release+re-register pair below the limit would open a real (if
 * brief) window — both calls take the same per-process spinlock kubridge's
 * own GetExceptionHandler() takes on every thread's fault dispatch — during
 * which a genuinely-ours lazy fault on another thread could see no handler
 * installed. Simply not releasing below the limit removes that window
 * entirely. `old` is passed as NULL on the re-registration below the limit:
 * g_old_dabt must keep the value captured once at startup (any handler that
 * existed before ours), never the "previous" value of lazy_dabt itself, or
 * the chain above would loop back into this function.
 */
static void lazy_dabt(Wx86KuExcpContext* ctx) {
    g_net_fsr = ctx->fsr;
    if (wx86_lazymem_fault(ctx->far_)) return;
    if (g_old_dabt) { g_old_dabt(ctx); return; }
    uint32_t n = __atomic_add_fetch(&g_net_foreign, 1u, __ATOMIC_RELAXED);
    uint32_t hits = net_track_hit((uintptr_t)ctx->far_, (uint32_t)ctx->pc);
    if (hits >= WX86_NET_MAX_REPLAY) {
        /* Give up on THIS (address, PC) pair: release, do not re-register.
         * The next occurrence of this exact, by-now-proven-stuck fault has
         * no handler to catch it and crashes cleanly (see comment above). */
        kuKernelReleaseExceptionHandler(WX86_KU_EXCP_DATA_ABORT);
        lz_log("arene paresseuse : faute HORS arene REPETEE A L'IDENTIQUE "
               "(adresse hote=0x%08x pc=0x%08x fsr=0x%08x, %u fois de suite) — "
               "ABANDON : filet DESARME pour CETTE faute precise, plantage propre "
               "attendu a la prochaine repetition (#%u cumule)",
               (unsigned)ctx->far_, (unsigned)ctx->pc, (unsigned)ctx->fsr,
               (unsigned)hits, (unsigned)n);
        return;
    }
    int rc = kuKernelRegisterExceptionHandler(WX86_KU_EXCP_DATA_ABORT, lazy_dabt, NULL, NULL);
    /* Unconditional — not gated behind a diagnostic knob. The old code was
     * silent here, which is exactly what let the permanent-disarm bug hide
     * across three separate code audits: nothing in any log said the net
     * had ever seen a foreign fault, let alone that it had gone dark. */
    lz_log("arene paresseuse : faute HORS arene (adresse hote=0x%08x pc=0x%08x fsr=0x%08x) — "
           "filet %s (repetition %u/%u, #%u cumule)",
           (unsigned)ctx->far_, (unsigned)ctx->pc, (unsigned)ctx->fsr,
           rc < 0 ? "RE-ENREGISTREMENT ECHOUE, DESARME" : "re-enregistre",
           (unsigned)hits, (unsigned)WX86_NET_MAX_REPLAY, (unsigned)n);
}
#endif

int wx86_lazymem_probe(uint64_t bytes) {
    if (wx86_lazymem_mode() == WX86_LAZY_OFF) return 0;
#ifdef __vita__
    void* addr = NULL;
    SceUID uid = kuKernelMemReserve(&addr, (SceSize)bytes, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW);
    if (uid <= 0 || !addr) {
        lz_log("arene paresseuse INDISPONIBLE : kuKernelMemReserve(%u Mo) = 0x%08X "
               "(kubridge bythos >= 0.3 requis) — chemin classique",
               (unsigned)(bytes >> 20), (unsigned)uid);
        return 0;
    }
    void* base = NULL;
    int ok = sceKernelGetMemBlockBase(uid, &base) >= 0 && base == addr;
    /* Prove the commit path too: with a weak stub, a missing NID only shows
     * up when the call is actually made. */
    if (ok && kuKernelMemCommit(addr, WX86_LAZY_CHUNK,
                                WX86_KU_PROT_READ | WX86_KU_PROT_WRITE, NULL) < 0) ok = 0;
    if (ok) {
        *(volatile uint32_t*)addr = 0xA5A5A5A5u;
        ok = *(volatile uint32_t*)addr == 0xA5A5A5A5u;
        kuKernelMemDecommit(addr, WX86_LAZY_CHUNK);
    }
    sceKernelFreeMemBlock(uid);
    if (!ok) {
        lz_log("arene paresseuse INDISPONIBLE : engagement de sonde refuse — chemin classique");
        return 0;
    }
    lz_log("arene paresseuse disponible (sonde %u Mo reservee puis liberee)", (unsigned)(bytes >> 20));
    return 1;
#else
    void* p = mmap(NULL, (size_t)bytes, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        lz_log("arene paresseuse INDISPONIBLE : mmap(%u Mo, PROT_NONE) refuse", (unsigned)(bytes >> 20));
        return 0;
    }
    munmap(p, (size_t)bytes);
    return 1;
#endif
}

void* wx86_lazymem_reserve(uint64_t bytes, uint64_t align, uint64_t* reserved_out) {
    if (wx86_lazymem_mode() == WX86_LAZY_OFF) return NULL;
    if (!align) align = WX86_LAZY_CHUNK;
    /* The slack is address space, not memory: a whole `align` of it costs
     * nothing here, where the eager path pays 1 to 16 MiB of real RAM. */
    uint64_t total = (bytes + align + 0xFFFFFull) & ~0xFFFFFull;
    uintptr_t raw = 0;
#ifdef __vita__
    void* addr = NULL;
    SceUID uid = kuKernelMemReserve(&addr, (SceSize)total, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW);
    if (uid <= 0 || !addr) {
        lz_log("arene paresseuse : reservation de %u Mo REFUSEE (0x%08X) — chemin classique",
               (unsigned)(total >> 20), (unsigned)uid);
        return NULL;
    }
    g_uid = uid;
    raw = (uintptr_t)addr;
#else
    void* p = mmap(NULL, (size_t)total, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        lz_log("arene paresseuse : mmap de %u Mo REFUSE — chemin classique", (unsigned)(total >> 20));
        return NULL;
    }
    raw = (uintptr_t)p;
#endif
    const uintptr_t base = (raw + (uintptr_t)align - 1) & ~((uintptr_t)align - 1);
    g_nchunk = (uint32_t)((bytes + WX86_LAZY_CHUNK - 1) / WX86_LAZY_CHUNK);
    uint8_t* st = (uint8_t*)calloc(g_nchunk, 1);
    if (!st) {
#ifdef __vita__
        sceKernelFreeMemBlock(uid); g_uid = -1;
#else
        munmap((void*)raw, (size_t)total);
#endif
        lz_log("arene paresseuse : table d'etat (%u Ko) refusee — chemin classique", g_nchunk >> 10);
        return NULL;
    }
    g_base = base;
    g_span = bytes;
    g_reserved = total;
    if (reserved_out) *reserved_out = total;
#ifdef __vita__
    if (wx86_lazymem_mode() == WX86_LAZY_NET) {
        int rc = kuKernelRegisterExceptionHandler(WX86_KU_EXCP_DATA_ABORT, lazy_dabt, &g_old_dabt, NULL);
        if (rc < 0)
            lz_log("arene paresseuse : filet DABT NON arme (0x%08X) — un engagement manque devient un plantage", (unsigned)rc);
    }
#endif
    /* Published LAST: it is the flag every hot path tests, and nothing may
     * see a half-initialised arena. */
    __atomic_store_n(&wx86_lazymem_state, st, __ATOMIC_RELEASE);
    lz_log("arene paresseuse armee : %u Mo d'espace d'adresses (dont %u Ko de marge d'alignement, 0 octet physique), "
           "morceaux de %u Ko, filet %s",
           (unsigned)(total >> 20), (unsigned)((base - raw) >> 10), WX86_LAZY_CHUNK >> 10,
           wx86_lazymem_mode() == WX86_LAZY_STRICT ? "DESARME (strict)" : "arme");
    return (void*)base;
}
