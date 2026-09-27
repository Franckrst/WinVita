// Console services for the Vita (see vita_host.h).
#include "platform/vita_host.h"

// This entire file only exists on-target: off console there are no cores to
// place threads on, no Sony clock, and no durable log to maintain. Same
// convention as platform/vita_audio.cpp.
#ifdef __vita__

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <pthread.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>

// ===========================================================================
//  DURABLE PROGRESS LOG
// ===========================================================================
// Static lock with a constant initializer, so no lazy init and no
// __cxa_guard_acquire — the class of trap the build script's `nm` guard
// exists to catch.
static pthread_mutex_t g_progress_mx = PTHREAD_MUTEX_INITIALIZER;   // the buffer (and, in sync mode, the file)
static pthread_mutex_t g_progress_io = PTHREAD_MUTEX_INITIALIZER;   // the file: held across sceIo calls
static SceUID   g_progress_fd = -1;          // open log (under g_progress_io), -1 = closed
static uint64_t g_progress_opened_us = 0;    // when it was (re)opened
static __thread uint64_t t_progress_us = 0;  // per-thread time spent logging

// ASYNCHRONOUS MODE (wx86_vita_progress_async_start, after env.txt is read).
// Measured on console, Act V, 26/09/2026: with a synchronous sceIoWrite per
// line the game thread still lost 30-107 ms on the frames where a periodic
// report published its 20-40 lines (journal= on the lente# lines), even with
// the file kept open. Lines now go into this buffer — a memcpy under a lock
// no one holds across I/O — and a writer thread flushes it every 50 ms.
// What stays synchronous, so nothing that explains a death is left behind:
//   - every line before the start (boot, env.txt), and all of them under
//     WX86_JOURNAL_SYNC=1;
//   - wx86_vita_progress_flush(): d2_crashlog, the native fault handler
//     (flush_c), the engine's own fatal _exit/abort sites;
//   - wx86_vita_progress_stop(): the ordered teardown before
//     sceKernelExitProcess drains, joins the writer and goes back to
//     synchronous writes (atexit handlers never run on _exit/_Exit/abort or
//     sceKernelExitProcess, so they cannot be the safety net);
//   - a full buffer: the writer flushes it in place, never drops a line.
// A process killed without any of those loses at most the last ~50 ms of
// lines to the file — and they are still in this buffer, i.e. in the dump.
// 64 KiB: a burst of 40 periodic lines is ~8 KiB; a full buffer falls back
// to synchronous writes anyway.
#define WX86_JB_SIZE (64u * 1024u)                      // power of two
static char     g_jb[WX86_JB_SIZE];
static uint32_t g_jbHead = 0, g_jbTail = 0;             // monotonic byte counters (under g_progress_mx)
static bool     g_jbAsync = false;
static volatile int g_jbStop = 0;
static pthread_t g_jbThread;
// Owner of g_progress_io (thread id, 0 = free): lets the fault-handler flush
// tell "THIS thread holds the file" (skip, or it deadlocks) from "another
// thread is mid-write" (wait for it, bounded).
static volatile SceUID g_progress_io_owner = 0;
static inline void io_lock()   { pthread_mutex_lock(&g_progress_io); g_progress_io_owner = sceKernelGetThreadId(); }
static inline void io_unlock() { g_progress_io_owner = 0; pthread_mutex_unlock(&g_progress_io); }

// File side, g_progress_io held. The descriptor stays open and is reopened
// at most once a second: an FTP reader during play never sees the file more
// than ~1 s behind, and one open/close per line (the original cost) is gone.
static void jb_write_file(const char* p, uint32_t n) {
    const uint64_t now = sceKernelGetProcessTimeWide();
    if (g_progress_fd >= 0 && now - g_progress_opened_us >= 1000000ull) {
        sceIoClose(g_progress_fd); g_progress_fd = -1;
    }
    if (g_progress_fd < 0) {
        g_progress_fd = sceIoOpen(wx86_vita_progress_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
        g_progress_opened_us = now;
    }
    if (g_progress_fd >= 0 && n && sceIoWrite(g_progress_fd, p, (SceSize)n) < 0) {
        sceIoClose(g_progress_fd); g_progress_fd = -1;   // retried (reopened) on the next write
    }
}
// Drains the buffer to the file, in order. g_progress_io held by the caller,
// g_progress_mx NOT held (taken here only to copy a chunk out).
static void jb_drain_io_held() {
    static char chunk[16u * 1024u];
    for (;;) {
        pthread_mutex_lock(&g_progress_mx);
        uint32_t n = g_jbHead - g_jbTail;
        if (n > sizeof chunk) n = sizeof chunk;
        const uint32_t off = g_jbTail & (WX86_JB_SIZE - 1u);
        const uint32_t a = (off + n > WX86_JB_SIZE) ? WX86_JB_SIZE - off : n;
        std::memcpy(chunk, g_jb + off, a);
        std::memcpy(chunk + a, g_jb, n - a);
        g_jbTail += n;
        pthread_mutex_unlock(&g_progress_mx);
        if (!n) return;
        jb_write_file(chunk, n);
    }
}

void wx86_vita_progress_flush() {
    if (!wx86_vita_progress_path) return;
    io_lock();
    jb_drain_io_held();
    io_unlock();
}
// Fault-handler / fatal-exit variant. If THIS thread already holds the file
// (a fault inside the log code itself) the flush is skipped — waiting would
// deadlock, and the lines stay in the buffer, i.e. in the dump. If ANOTHER
// thread holds it (the writer mid-sceIoWrite), wait for it, bounded
// (~300 ms), instead of silently leaving the CRASH line behind.
extern "C" void wx86_vita_progress_flush_c(void) {
    if (!wx86_vita_progress_path) return;
    if (g_progress_io_owner == sceKernelGetThreadId()) return;
    for (int i = 0; i < 300; ++i) {
        if (pthread_mutex_trylock(&g_progress_io) == 0) {
            g_progress_io_owner = sceKernelGetThreadId();
            jb_drain_io_held();
            io_unlock();
            return;
        }
        sceKernelDelayThread(1000);
    }
}

static void* jb_writer(void*) {
    while (!g_jbStop) { sceKernelDelayThread(50000); wx86_vita_progress_flush(); }
    return nullptr;
}
// Ordered teardown: drain, stop and JOIN the writer, then back to
// synchronous writes and close the file. No thread of ours may still be
// alive at sceKernelExitProcess.
void wx86_vita_progress_stop() {
    if (g_jbAsync) {
        g_jbStop = 1;
        pthread_join(g_jbThread, nullptr);
        pthread_mutex_lock(&g_progress_mx);
        g_jbAsync = false;
        pthread_mutex_unlock(&g_progress_mx);
    }
    wx86_vita_progress_close();
}
void wx86_vita_progress_async_start() {
    if (g_jbAsync || !wx86_vita_progress_path) return;
    const char* e = getenv("WX86_JOURNAL_SYNC");
    if (e && e[0] && !(e[0] == '0' && !e[1])) {
        wx86_vita_progress("journal: SYNCHRONE (WX86_JOURNAL_SYNC=1) — une ecriture par ligne sur le fil appelant");
        return;
    }
    g_jbStop = 0;
    if (pthread_create(&g_jbThread, nullptr, jb_writer, nullptr) != 0) {
        wx86_vita_progress("journal: fil d'ecriture NON cree — reste synchrone");
        return;
    }
    pthread_mutex_lock(&g_progress_mx);
    g_jbAsync = true;
    pthread_mutex_unlock(&g_progress_mx);
    wx86_vita_progress("journal: ASYNCHRONE (tampon 64 Kio, vidage 50 ms ; synchrone sur crash/sortie ; WX86_JOURNAL_SYNC=1 pour revenir)");
}

void wx86_vita_progress(const char* msg) {
    // No path means no log. Guessing one would let two ports write to the
    // same file, exactly what the lock below exists to prevent. This only
    // happens if a port omitted the definition, in which case the link
    // fails — the right moment to find out.
    if (!wx86_vita_progress_path) return;
    // Raw sceIo, NOT stdio. This runs on several host threads (watchdog,
    // starvation net, flush thread...) while the game thread drives newlib's
    // FILE*/descriptor tables through the Win32 file shims. A console dump of
    // 2026-09-25 died in newlib's descriptor layer (__vita_fd_grab, NULL
    // entry) right after a shim fopen that overlapped a log write from the
    // starvation thread: the freshly opened FILE* read as an error, then
    // aborted on its own descriptor. A SceUID never touches those tables,
    // whatever newlib's locking actually guarantees.
    const uint64_t t0 = sceKernelGetProcessTimeWide();
    char line[2048];   // some reports (r60, the core table) exceed 1 KiB
    int n = std::snprintf(line, sizeof line, "[%4u.%02us] %s\n", (unsigned)(t0 / 1000000ull), 0u, msg);
    if (n > (int)sizeof line - 1) { n = (int)sizeof line - 1; line[n - 1] = '\n'; }
    if (n <= 0) return;
    pthread_mutex_lock(&g_progress_mx);
    if (g_jbAsync && g_jbHead - g_jbTail + (uint32_t)n <= WX86_JB_SIZE) {
        const uint32_t off = g_jbHead & (WX86_JB_SIZE - 1u);
        const uint32_t a = (off + (uint32_t)n > WX86_JB_SIZE) ? WX86_JB_SIZE - off : (uint32_t)n;
        std::memcpy(g_jb + off, line, a);
        std::memcpy(g_jb, line + a, (uint32_t)n - a);
        g_jbHead += (uint32_t)n;
        pthread_mutex_unlock(&g_progress_mx);
    } else {
        // Synchronous: before the start, under WX86_JOURNAL_SYNC, or a full
        // buffer — drained first, so the file keeps the lines in order.
        pthread_mutex_unlock(&g_progress_mx);
        io_lock();
        jb_drain_io_held();
        jb_write_file(line, (uint32_t)n);
        io_unlock();
    }
    t_progress_us += sceKernelGetProcessTimeWide() - t0;
}

// Drops the cached descriptor. Call it only while the log is synchronous
// (before async_start, or after stop): with the writer running it could
// reopen the old path between this close and a rename.
void wx86_vita_progress_close() {
    io_lock();
    jb_drain_io_held();
    if (g_progress_fd >= 0) { sceIoClose(g_progress_fd); g_progress_fd = -1; }
    io_unlock();
}

// Time the CALLING thread has spent in wx86_vita_progress, lock wait
// included (us, cumulative). Read by the frame profile (journal= on the
// lente# lines) from the game thread.
extern "C" uint64_t wx86_vita_progress_self_us(void) { return t_progress_us; }

// C-linkage alias so the dynarec's C code (weak reference) can reach it.
extern "C" void wx86_vita_progress_c(const char* m) { wx86_vita_progress(m); }

// ===========================================================================
//  REAL SLEEP
// ===========================================================================
// The monotonic clock belongs in runtime/host_clock.h since it's meaningful
// off console too; keeping a copy here would duplicate it in the engine.
void wx86_vita_sleep_ms(uint32_t ms) { if (ms) sceKernelDelayThread(ms * 1000u); }

// ===========================================================================
//  HOST THREAD PLACEMENT ACROSS USER CORES
// ===========================================================================
namespace {
struct CoreEnt { char nom[14]; int uid; unsigned wanted; int rc; };
// Sized with headroom above what a single consumer typically registers — a
// thread that isn't registered disappears from the one line that shows which
// thread lives on which core.
CoreEnt g_core[16];
int     g_coreN = 0;

// Read once. Default is "222".
const char* core_scheme() {
    static const char* s_sch = nullptr;
    static bool s_done = false;
    if (!s_done) {
        s_done = true;
        const char* e = getenv("WX86_COEURS");
        if (!e) e = getenv("D2_COEURS");
        bool ok = e && e[0] && e[1] && e[2] && !e[3];
        for (int i = 0; ok && i < 3; ++i) if (e[i] < '0' || e[i] > '3') ok = false;
        if (e && e[0] && !ok) {
            char m[120];
            std::snprintf(m, sizeof m,
                "coeurs: WX86_COEURS=\"%s\" INVALIDE (3 chiffres 0..3 attendus) — repartition par defaut 222", e);
            wx86_vita_progress(m);
        }
        s_sch = ok ? e : "222";
    }
    return s_sch;
}
} // namespace

int wx86_vita_core_mask(int who) {
    if (who < 0 || who > 2) return SCE_KERNEL_CPU_MASK_USER_2;
    const char c = core_scheme()[who];
    // Heartbeat on USER_0 is accepted but flagged: this kernel is RUN-TO-BLOCK,
    // so an anti-starvation heartbeat sharing a core with guest runner threads
    // won't get scheduled once a runner stops blocking — exactly when it's
    // needed. A silently inert safety net is the worst failure mode here.
    if (who == 2 && c == '0') {
        static bool s_said = false;
        if (!s_said) { s_said = true; wx86_vita_progress(
            "coeurs: ATTENTION battement anti-famine demande sur USER_0 — sous RUN-TO-BLOCK"
            " le filet ne sera PAS elu quand un runner invite cesse de bloquer (filet inerte)"); }
    }
    // Digit `3` requests the fourth core (0x00080000). If the kernel refuses
    // it, the pin's rc is published in the "cores:" line and the thread stays
    // where it was. Don't rely on it until that line's probe reports rc0 on
    // hardware.
    return c == '0' ? SCE_KERNEL_CPU_MASK_USER_0
         : c == '1' ? SCE_KERNEL_CPU_MASK_USER_1
         : c == '3' ? WX86_CPU_MASK_USER_3
                    : SCE_KERNEL_CPU_MASK_USER_2;
}

void wx86_vita_core_register(const char* nom, int uid, unsigned wanted, int pin_rc) {
    if (g_coreN >= (int)(sizeof g_core / sizeof g_core[0])) return;
    CoreEnt& e = g_core[g_coreN++];
    std::snprintf(e.nom, sizeof e.nom, "%s", nom ? nom : "?");
    e.uid = uid; e.wanted = wanted; e.rc = pin_rc;
}

// Number of host threads registered so far. Used as context before creating
// a thread: an exhausted thread quota and exhausted memory can't be told
// apart from sceKernelCreateThread's rc alone.
int wx86_vita_core_count() { return g_coreN; }

extern "C" int wx86_vita_pin_self(int mask, unsigned* relu) {
    const SceUID me = sceKernelGetThreadId();
    const int rc = sceKernelChangeThreadCpuAffinityMask(me, mask);
    const int r  = sceKernelGetThreadCpuAffinityMask(me);
    if (relu) *relu = (r < 0) ? 0u : (unsigned)r;
    return rc;
}

extern "C" int wx86_vita_pin_thread(int thread_uid, int mask, unsigned* relu) {
    const int rc = sceKernelChangeThreadCpuAffinityMask((SceUID)thread_uid, mask);
    // Read back only if the caller asked for it — see vita_host.h.
    if (relu) { const int r = sceKernelGetThreadCpuAffinityMask((SceUID)thread_uid);
                *relu = (r < 0) ? 0u : (unsigned)r; }
    return rc;
}

namespace {
// Fourth-core probe, run once on the first 10-second window.
//
// It creates a thread but never starts it, so it only measures what the
// kernel ACCEPTS (rc of ChangeThreadCpuAffinityMask + mask read back), never
// whether a core actually executes something. A thread started on a
// nonexistent core would never get scheduled, and cleaning it up would need
// a bounded WaitThreadEnd plus an abandon — a needless risk for a question
// the rc already answers. `activeCpuMask` is read for reference, since it's
// the kernel's own count of active cores.
//
// Three masks are tried: USER_2 (a witness mask known to be accepted —
// without it, an `rc<0` on 0x80000 wouldn't be distinguishable from a broken
// probe), 0x00080000 (the 4th core alone), and 0x000F0000 (all four).
void core_probe_cpu3() {
    SceKernelSystemInfo si; std::memset(&si, 0, sizeof si); si.size = sizeof si;
    const int rsys = sceKernelGetSystemInfo(&si);
    SceUID th = sceKernelCreateThread("probe_c3", [](SceSize, void*) -> int { return 0; },
                                      0x10000100, 0x1000, 0, 0, nullptr);
    if (th < 0) {
        char m[112];
        std::snprintf(m, sizeof m,
            "coeurs: sonde 4e coeur — CreateThread KO (rc=0x%08x), activeCpuMask=0x%08x (rc=0x%08x)",
            (unsigned)th, rsys < 0 ? 0u : (unsigned)si.activeCpuMask, (unsigned)rsys);
        wx86_vita_progress(m);
        return;
    }
    const unsigned masks[3] = { SCE_KERNEL_CPU_MASK_USER_2, WX86_CPU_MASK_USER_3, 0x000F0000u };
    int  rc[3]; unsigned relu[3];
    for (int i = 0; i < 3; ++i) {
        rc[i]   = sceKernelChangeThreadCpuAffinityMask(th, (int)masks[i]);
        relu[i] = (unsigned)sceKernelGetThreadCpuAffinityMask(th);
    }
    sceKernelDeleteThread(th);          // never started, so Delete alone is enough
    char m[224];
    std::snprintf(m, sizeof m,
        "coeurs: sonde 4e coeur — activeCpuMask=0x%08x (rc=0x%08x) |"
        " temoin 0x40000 rc=0x%08x relu=0x%x | 0x80000 rc=0x%08x relu=0x%x |"
        " 0xF0000 rc=0x%08x relu=0x%x",
        rsys < 0 ? 0u : (unsigned)si.activeCpuMask, (unsigned)rsys,
        (unsigned)rc[0], relu[0], (unsigned)rc[1], relu[1], (unsigned)rc[2], relu[2]);
    wx86_vita_progress(m);
    // A mask that reads back as 0 proves nothing on this firmware — only the
    // rc distinguishes accepted from refused, and the 0x40000 witness confirms
    // whether the probe itself works.
    if (rc[0] < 0)
        wx86_vita_progress("coeurs: sonde 4e coeur NON CONCLUANTE — le temoin USER_2 lui-meme est refuse");
    else if (rc[1] >= 0)
        wx86_vita_progress("coeurs: 4e coeur (0x80000) ACCEPTE par le noyau — reste a prouver qu'un fil y COURT"
                           " (chiffre 3 de WX86_COEURS, puis lire le champ d= de la ligne coeurs:)");
    else
        wx86_vita_progress("coeurs: 4e coeur (0x80000) REFUSE par le noyau — trois coeurs user, pas quatre");
}
} // namespace

// Published for each host thread:
//   v=<n>   requested core (0/1/2/3), derived from the mask
//   rc      sceKernelChangeThreadCpuAffinityMask's rc (0 = ok)
//   m=<hex> mask read back by sceKernelGetThreadCpuAffinityMask
//   c=<n>   currentCpuId (sceKernelGetThreadInfo)
//   d=<n>   lastExecutedCpuId — the field that actually settles it
//
// Caveat: on this firmware, sceKernelGetThreadCpuAffinityMask reads back
// 0x00000000 even for a thread pinned successfully. So "m=0x0" is not proof
// of failure — the line also publishes the request's rc and, more
// importantly, lastExecutedCpuId, which says which physical core the thread
// actually ran on.
void wx86_vita_core_window_line() {
    if (!g_coreN) return;
    static bool s_probed = false;
    if (!s_probed) { s_probed = true; core_probe_cpu3(); }
    char buf[224]; int used = std::snprintf(buf, sizeof buf, "coeurs:");
    int inline_n = 0;
    for (int i = 0; i < g_coreN; ++i) {
        const CoreEnt& e = g_core[i];
        int want = (e.wanted & SCE_KERNEL_CPU_MASK_USER_0) ? 0
                 : (e.wanted & SCE_KERNEL_CPU_MASK_USER_1) ? 1
                 : (e.wanted & SCE_KERNEL_CPU_MASK_USER_2) ? 2
                 : (e.wanted & WX86_CPU_MASK_USER_3)       ? 3 : -1;
        int relu = (e.uid > 0) ? sceKernelGetThreadCpuAffinityMask(e.uid) : 0;
        SceKernelThreadInfo ti; std::memset(&ti, 0, sizeof ti); ti.size = sizeof ti;
        int rti = (e.uid > 0) ? sceKernelGetThreadInfo(e.uid, &ti) : -1;
        char one[96];
        std::snprintf(one, sizeof one, " %s=v%d/rc%d/m0x%x/c%d/d%d",
                      e.nom, want, e.rc < 0 ? -1 : 0, (unsigned)relu,
                      rti < 0 ? -1 : (int)ti.currentCpuId,
                      rti < 0 ? -1 : (int)ti.lastExecutedCpuId);
        const int need = (int)std::strlen(one);
        if (used + need >= (int)sizeof buf - 1 || inline_n >= 3) {
            wx86_vita_progress(buf);
            used = std::snprintf(buf, sizeof buf, "coeurs:"); inline_n = 0;
        }
        std::memcpy(buf + used, one, (size_t)need + 1); used += need; ++inline_n;
    }
    if (used > 7) wx86_vita_progress(buf);
}

#else  // ---------------------------------------------------------------------
//  OFF CONSOLE: the log exists but does nothing
// ---------------------------------------------------------------------------
// The engine's generic body (bridge, cpu_box86, both schedulers, the memory
// mapper) logs its progress, and that body also compiles for the qemu/desktop
// harness, where there is no console and no durable file to maintain.
//
// These two no-ops stand in for all of the portability needed: callers link
// strongly with no null check, and off console nothing happens.
//
// wx86_vita_progress_path is deliberately not read here, so a port has
// nothing to define for its desktop build to link.

void wx86_vita_progress(const char*) {}
extern "C" void wx86_vita_progress_c(const char*) {}
extern "C" void wx86_vita_progress_flush_c(void) {}

#endif // __vita__
