// GIL holder identity test bench.
//
// gil::lock() is on the path of every trap. It no longer calls pthread_self()
// (on pte: pthread_getspecific -> pte_osTlsGetValue -> vitasdk_get_tls_data ->
// sceKernelGetTLSAddr, an inter-module call): instead it publishes a stack
// mark, which the reader translates into a label via the
// gil::register_stack() table.
//
// This test answers the questions that replacement raises:
//   1. does the translation name the RIGHT thread, and stay SILENT when it
//      doesn't know (unregistered thread) — naming the neighbor would be the
//      "diagnostic that lies";
//   2. does assert_held() still detect a real fault, including the strong
//      case "the GIL is held, but by SOMEONE ELSE"?
//   3. does a RECYCLED stack lend its name? That's the only false name this
//      mechanism can produce (an unregistered thread running on a dead
//      thread's still-registered stack), and it's exactly what the
//      "unregister" half of gil.h's invariant closes. glibc recycles its
//      stacks, so the host can exercise this case — pte can't (see gil.h).
//
// Host only (fork + SIGABRT). The mechanism under test is identical
// on-target: disjoint stacks and the address of a local. So this does not
// replace rt_boot_arm_check.sh — it isolates what that script cannot show.
//
//   g++ -std=gnu++17 -O2 -I src tools/torture/gil_ident.cpp src/runtime/gil.cpp \
//       -lpthread -o /tmp/gil_ident && /tmp/gil_ident
//
// Do not build with -DNDEBUG: phase 2 relies on assertions.
#include "runtime/gil.h"
#include <pthread.h>
#include <unistd.h>
#include <csignal>
#include <sys/wait.h>
#include <cstdio>
#include <cstdint>
#include <atomic>
using namespace d2rt;

static std::atomic<int> g_fail{0}, g_checks{0};
static void expect(bool ok, const char* what) {
    g_checks++;
    if (!ok) { g_fail++; std::printf("  ECHEC: %s\n", what); }
}
// Declared window, well under a host stack's 8MiB: a strict subset, same as
// on-target (gil.h).
static const uintptr_t SPAN = 240 * 1024;
static void reg(uint32_t tag) { char ici; uintptr_t hi = (uintptr_t)&ici; gil::register_stack(hi - SPAN, hi, tag); }

// ---- phase 1: who holds the GIL? --------------------------------------------

// Variable depth: the mark published by lock() CHANGES from one acquisition
// to the next for the same thread; the label must not.
static void take_and_check(uint32_t mytag, int depth) {
    if (depth > 0) { volatile char pad[512]; pad[0] = (char)depth; take_and_check(mytag, depth - 1); (void)pad; return; }
    gil::lock();
    gil::Probe p; gil::probe(&p);
    expect(p.held, "GIL vu pris par son detenteur");
    expect(p.owner_tag == mytag, "etiquette = le fil qui tient VRAIMENT le GIL");
    gil::assert_held();
    gil::unlock();
}

// Single-thread: shim attribution must survive a Release/relock taken at a
// DIFFERENT depth (so under a different mark) — that's exactly what a raw
// mark comparison would lose, and why probe() compares resolved labels
// instead.
static void shim_across_release(int depth) {
    if (depth > 0) { volatile char pad[512]; pad[0] = (char)depth; shim_across_release(depth - 1); (void)pad; return; }
    gil::lock();
    gil::note_shim_enter(0x7f001234, "banc!shim");
    gil::Probe p; gil::probe(&p);
    expect(p.in_shim, "shim attribue a la prise initiale");
    { gil::Release r; }
    gil::probe(&p);
    expect(p.owner_tag == 9, "etiquette stable apres Release/relock");
    expect(p.in_shim && p.shim && p.shim[0] == 'b', "shim toujours attribue apres Release/relock");
    gil::note_shim_exit();
    gil::unlock();
}

static void* worker(void* a) {
    uint32_t tag = (uint32_t)(uintptr_t)a;
    reg(tag);
    for (int i = 0; i < 200; ++i) take_and_check(tag, i % 5);
    return nullptr;
}
// Deliberately unregistered thread: it must NEVER inherit another thread's name.
static void* stranger(void*) {
    for (int i = 0; i < 50; ++i) {
        gil::lock();
        gil::Probe p; gil::probe(&p);
        expect(p.held, "GIL pris par le fil anonyme");
        expect(p.owner_tag == 0, "fil non enregistre => AUCUN nom (pas celui du voisin)");
        gil::unlock();
    }
    return nullptr;
}

static int phase_identite() {
    gil::enable(); reg(9);
    for (int d = 0; d < 6; ++d) shim_across_release(d);
    pthread_t th[5];
    for (int i = 0; i < 4; ++i) pthread_create(&th[i], nullptr, worker, (void*)(uintptr_t)(i + 1));
    pthread_create(&th[4], nullptr, stranger, nullptr);
    for (int i = 0; i < 200; ++i) take_and_check(9, i % 5);
    for (int i = 0; i < 5; ++i) pthread_join(th[i], nullptr);
    std::printf("  phase 1 (identite) : %d verifications, %d echec(s)\n", g_checks.load(), g_fail.load());
    return g_fail ? 1 : 0;
}

// ---- phase 2: does assert_held() still detect it? ---------------------------

static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  c = PTHREAD_COND_INITIALIZER;
static bool held = false;
static void* holder(void*) {
    reg(2);
    gil::lock();
    pthread_mutex_lock(&m); held = true; pthread_cond_signal(&c); pthread_mutex_unlock(&m);
    sleep(5); gil::unlock(); return nullptr;
}
static int enfant_defaut(int which) {
    gil::enable(); reg(1);
    if (which == 0) { gil::assert_held(); return 0; }        // (A) GIL free
    pthread_t t; pthread_create(&t, nullptr, holder, nullptr);
    pthread_mutex_lock(&m); while (!held) pthread_cond_wait(&c, &m); pthread_mutex_unlock(&m);
    gil::assert_held();                                      // (B) held by thr2
    return 0;
}
static int phase_assert() {
    int bad = 0;
    for (int w = 0; w < 2; ++w) {
        pid_t p = fork();
        if (p == 0) _exit(enfant_defaut(w));
        int st = 0; waitpid(p, &st, 0);
        bool aborted = WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT;
        std::printf("  phase 2 cas %c (%s) : %s\n", 'A' + w,
                    w ? "GIL tenu par un AUTRE fil" : "GIL libre",
                    aborted ? "assertion DECLENCHEE (attendu)" : "PAS d'assertion (DEFAUT MANQUE)");
        if (!aborted) bad = 1;
    }
    return bad;
}

// ---- phase 3: a RECYCLED stack doesn't lend its name -------------------------
// The ephemeral thread registers, takes the GIL, unregisters, and dies. The
// squatter, created right after, most likely inherits its stack (glibc's
// stack cache) and does not register — it must stay anonymous. Without the
// unregister step, it would carry the dead thread's name — the "diagnostic
// that lies".
static std::atomic<uintptr_t> g_markA{0}, g_markB{0};
static void* ephemere(void*) {
    char ici; uintptr_t hi = (uintptr_t)&ici;
    g_markA.store(hi, std::memory_order_relaxed);
    int s = gil::register_stack(hi - SPAN, hi, 77);
    gil::lock(); gil::Probe p; gil::probe(&p);
    expect(p.owner_tag == 77, "fil ephemere nomme DE SON VIVANT");
    gil::unlock();
    gil::unregister_stack(s);
    return nullptr;
}
static void* squatteur(void*) {
    char ici; g_markB.store((uintptr_t)&ici, std::memory_order_relaxed);
    gil::lock(); gil::Probe p; gil::probe(&p);
    expect(p.owner_tag == 0, "pile RECYCLEE : le squatteur n'herite PAS du nom du mort");
    gil::unlock();
    return nullptr;
}
static int phase_recyclage() {
    int before = g_fail.load();
    pthread_t a, b;
    pthread_create(&a, nullptr, ephemere, nullptr);  pthread_join(a, nullptr);
    pthread_create(&b, nullptr, squatteur, nullptr); pthread_join(b, nullptr);
    unsigned long A = (unsigned long)g_markA.load(), B = (unsigned long)g_markB.load();
    unsigned long d = A > B ? A - B : B - A;
    // If the stack was NOT reused, the case isn't exercised: this says so
    // rather than counting a free pass — absence of proof is information,
    // never a free pass.
    std::printf("  phase 3 (pile recyclee) : marques %#lx / %#lx, ecart %lu -> %s\n", A, B, d,
                d < (unsigned long)SPAN ? "pile REUTILISEE, cas EXERCE"
                                        : "pile NON reutilisee : cas NON EXERCE, ce PASS ne prouve rien");
    return g_fail.load() != before;
}

int main() {
    // assert_held()'s identity check is off by default: it costs a table
    // walk per sync shim in production (D2_GILCHECK). This test exists to
    // exercise it, so it's armed explicitly here, before any fork or thread.
    gil::set_identity_check(true);
    // Phases 1/3 and 2 are separated by fork(): enfant_defaut() calls
    // gil::enable() again, whose contract is "once per process."
    int bad = phase_assert();
    bad |= phase_identite();
    bad |= phase_recyclage();
    std::printf("  total : %d verifications, %d echec(s)\n", g_checks.load(), g_fail.load());
    std::printf("%s\n", bad ? "ECHEC" : "PASS : identite du detenteur + detection des defauts + pile recyclee");
    return bad;
}
