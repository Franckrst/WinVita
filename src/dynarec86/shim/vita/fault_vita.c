/* fault_vita.c — user-mode abort handler for the Vita build, through the
 * kubridge kernel plugin (optional: without it nothing here is armed and a
 * fault goes straight to the kernel's crash dump, exactly as before).
 *
 * What it buys:
 *   1. a DURABLE record of the guest state at the faulting instruction --
 *      the eight live x86 registers are r4..r11 at that very instruction,
 *      and the host pc names the dynablock, hence the exact x86 address --
 *      written by dyn86_fault_handle() before the kernel's dump runs;
 *   2. the SMC write barrier: a guest store into a page holding translated
 *      code lands here (protectDB made the page read-only through the real
 *      mprotect in mman_vita.c); the blocks are marked dirty, the page
 *      reopened, and the store resumes -- box86's own contract, which the
 *      Vita build had no way to honour without a fault handler.
 *
 * Declining: the kernel resumes at the (unchanged) context when the handler
 * returns, i.e. it re-faults. To hand the fault to the default path cleanly,
 * the handler is RELEASED first, then returns: the re-fault reaches the
 * kernel's own handler -> psp2dmp -> the crash reporter, unchanged. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psp2/kernel/threadmgr.h>
#include "kubridge_min.h"

void wx86_vita_progress_c(const char* msg);
int  dyn86_vita_kubridge(void);                       /* mman_vita.c */

typedef struct dyn86_fault {
    uint32_t type, pc, lr, sp, r0, r1, r2, far, fsr;
    uint32_t live[8];          /* r4..r11 = EAX ECX EDX EBX ESP EBP ESI EDI */
    uint32_t x86insn, gaddr;   /* filled by dyn86_fault_handle: faulting x86 instruction, guest fault address */
} dyn86_fault_t;
/* cpu_box86.cpp: 1 = handled (resume the context as is), 0 = decline,
 * 2 = deliver to the guest's exception filter (redirect to seh_tramp). */
int  dyn86_fault_handle(dyn86_fault_t* f);
void dyn86_seh_deliver(const dyn86_fault_t* f);      /* cpu_box86.cpp, never returns */

static __thread int t_in_fault = 0;
static __thread int t_seh_active = 0;                /* the filter is running on this thread */
static __thread dyn86_fault_t t_seh_rec;
static uint32_t g_faults_seen = 0;
/* Counters read by the port's periodic MEM line; bumped in dyn86_fault_handle
 * through the accessor (a C++ unit in an anonymous namespace cannot bind a
 * plain extern variable with C linkage, but a function is fine). */
uint32_t dyn86_smc_faults = 0;
uint32_t dyn86_fault_records = 0;
static uint32_t g_counters[2];
uint32_t* dyn86_fault_counters(void) { return g_counters; }

/* The guest's top-level filter (SetUnhandledExceptionFilter), a guest VA. */
static uint32_t g_seh_filter = 0;
void     dyn86_set_seh_filter(uint32_t va) { g_seh_filter = va; }
uint32_t dyn86_seh_filter_get(void)         { return g_seh_filter; }
/* Port hooks for the delivery: `exit_requested` says the guest asked to
 * leave (ExitProcess from its __except block: the port is shutting down and
 * this thread must simply park), `terminate` is the port's own clean exit
 * for the case where no handler claimed the exception. */
static int  (*g_exit_requested)(void) = 0;
static void (*g_terminate)(const char* why) = 0;
static void (*g_before_dispatch)(uint32_t gaddr) = 0;
static uint32_t g_sentinel = 0;   /* the bridge's sentinel VA: a return address that means "finished" */
void dyn86_seh_set_hooks(int (*exit_requested)(void), void (*terminate)(const char*), void (*before_dispatch)(uint32_t), uint32_t sentinel_va) {
    g_exit_requested = exit_requested; g_terminate = terminate; g_before_dispatch = before_dispatch; g_sentinel = sentinel_va; }
uint32_t dyn86_seh_sentinel(void) { return g_sentinel; }
int  dyn86_seh_exit_requested(void) { return g_exit_requested ? g_exit_requested() : 0; }
void dyn86_seh_terminate(const char* why) { if (g_terminate) g_terminate(why); }
void dyn86_seh_before_dispatch(uint32_t gaddr) { if (g_before_dispatch) g_before_dispatch(gaddr); }

/* Reopen a guarded range (the port's test knob uses it: a page it made
 * unreadable to provoke a fault must be readable again before the game's own
 * handlers run, or their first API call re-faults). */
int dyn86_vita_unguard(uintptr_t host_addr, size_t len) {
    if (!dyn86_vita_kubridge()) return 0;
    uintptr_t lo = host_addr & ~(uintptr_t)0xFFF, hi = (host_addr + len + 0xFFF) & ~(uintptr_t)0xFFF;
    return kuKernelMemProtect((void*)lo, (SceSize)(hi - lo), KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE) < 0 ? 0 : 1;
}

static void release_all(void) {
    kuKernelReleaseExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT);
    kuKernelReleaseExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT);
    kuKernelReleaseExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION);
}

/* Win32 fidelity: a guest access violation is DELIVERED to the game's
 * top-level filter as EXCEPTION_ACCESS_VIOLATION with a CONTEXT, the way
 * Windows does. The handler cannot run x86 code itself (it sits on the
 * faulting thread in an abort frame), so it redirects the thread's pc to
 * this plain C function (ARM state: the engine is built -marm), which runs
 * once the kernel has resumed the thread and hands over to
 * dyn86_seh_deliver() (cpu_box86.cpp). Never returns. */
static void seh_tramp(void) {
    dyn86_seh_deliver(&t_seh_rec);
    for (;;) sceKernelDelayThread(1000000);
}

static void on_fault(KuKernelExceptionContext* c) {
    if (t_in_fault || t_seh_active) {          /* a fault inside the handler or inside the filter: get out of the way */
        release_all();
        return;
    }
    t_in_fault = 1;
    ++g_faults_seen;
    dyn86_fault_t f;
    memset(&f, 0, sizeof f);
    f.type = c->exceptionType; f.pc = c->pc; f.lr = c->lr; f.sp = c->sp;
    f.r0 = c->r0; f.r1 = c->r1; f.r2 = c->r2; f.far = c->FAR; f.fsr = c->FSR;
    f.live[0] = c->r4; f.live[1] = c->r5; f.live[2] = c->r6;  f.live[3] = c->r7;
    f.live[4] = c->r8; f.live[5] = c->r9; f.live[6] = c->r10; f.live[7] = c->r11;
    const int verdict = dyn86_fault_handle(&f);
    t_in_fault = 0;
    if (verdict == 2) {
        t_seh_rec = f;
        t_seh_active = 1;
        c->pc = (uint32_t)(uintptr_t)seh_tramp;
        c->SPSR &= ~0x20u;                     /* ARM state */
        return;
    }
    /* Decline: every type is released so the re-fault -- and anything the
     * dying process raises after it -- meets the kernel's path. */
    if (verdict != 1) release_all();
}

int dyn86_vita_fault_install(void) {
    if (!dyn86_vita_kubridge()) {
        wx86_vita_progress_c("faute: kubridge absent — pas de handler utilisateur, une faute invitee va droit au dump noyau");
        return 0;
    }
    int rc = 0;
    static const uint32_t kinds[3] = { KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT,
                                       KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT,
                                       KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION };
    for (int i = 0; i < 3; ++i) {
        KuKernelExceptionHandler old = 0;
        int r = kuKernelRegisterExceptionHandler(kinds[i], on_fault, &old, 0);
        if (r < 0) rc = r;
    }
    char m[160];
    snprintf(m, sizeof m, rc < 0 ? "faute: handler kubridge REFUSE (rc=0x%08x) — repli dump noyau"
                                 : "faute: handler kubridge arme (data/prefetch/undef) — etat x86 durable, barriere SMC, livraison SEH au filtre du jeu (rc=0x%08x)",
             (unsigned)rc);
    wx86_vita_progress_c(m);
    return rc < 0 ? 0 : 1;
}

uint32_t dyn86_vita_faults_seen(void) { return g_faults_seen; }
uint32_t dyn86_vita_smc_faults(void)   { dyn86_smc_faults = g_counters[0]; return g_counters[0]; }
uint32_t dyn86_vita_fault_records(void){ dyn86_fault_records = g_counters[1]; return g_counters[1]; }
