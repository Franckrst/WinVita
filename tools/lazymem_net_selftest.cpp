// tools/lazymem_net_selftest.cpp — proves the bound on src/platform/vita_lazymem.c's
// foreign-fault repeat counter (net_track_hit / WX86_NET_MAX_REPLAY), on a
// platform that cannot exercise the real thing.
//
// Why this tool exists: the only real caller of net_track_hit is lazy_dabt,
// the Vita-only kubridge data-abort handler (#ifdef __vita__). Reproducing
// what it guards against — a foreign host address that refaults forever at
// the exact same PC — needs a closed component that misbehaves in exactly
// that deterministic way, or real kubridge kernel replay semantics. Neither
// exists on desktop or under qemu-arm: there is nothing in this project able
// to fault the same way twice in a row without our own code doing it on
// purpose, and kubridge itself is real-hardware only. So the counting
// primitive the bail-out decision is BUILT ON is deliberately compiled
// unconditionally (no #ifdef __vita__ — see vita_lazymem.c), and this
// program calls it directly through the TEST-ONLY entry points declared in
// vita_lazymem.h (wx86_lazymem_test_net_*).
//
// What this proves: the (address, PC) table counts consecutive identical
// faults correctly, tells apart "same address, different PC" from a real
// repeat, reaches the documented threshold (WX86_NET_MAX_REPLAY) exactly
// when expected, survives concurrent hits from multiple threads without
// losing updates, and does not corrupt or crash under table overflow
// (more distinct addresses looping at once than there are slots).
// What this does NOT prove, and cannot from here: that lazy_dabt's
// Vita-only kuKernelRegisterExceptionHandler/kuKernelReleaseExceptionHandler
// calls around this counter behave as documented, or real kubridge replay
// timing. Both remain unverified before an actual console run — see
// ROADMAP.md, phase 5.
#include "platform/vita_lazymem.h"

#include <cstdio>
#include <cstdint>
#include <thread>
#include <vector>

namespace {
int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) { std::printf("  ECHEC: %s\n", what); ++failures; }
}
}  // namespace

int main() {
    const uint32_t MAX = wx86_lazymem_test_net_max_replay();
    const uint32_t N   = wx86_lazymem_test_net_track_n();
    std::printf("lazymem_net: WX86_NET_MAX_REPLAY=%u WX86_NET_TRACK_N=%u\n",
                (unsigned)MAX, (unsigned)N);
    check(MAX >= 2 && MAX <= 8, "WX86_NET_MAX_REPLAY dans une plage raisonnable (2..8)");
    check(N >= MAX, "la table a au moins autant de cases que le seuil");

    // ---- (a) consecutive hits at the SAME (addr, pc) count up 1, 2, 3, ... ----
    wx86_lazymem_test_net_reset();
    const uintptr_t A1 = 0x41414000u;
    const uint32_t  P1 = 0x00810000u;
    for (uint32_t i = 1; i <= MAX + 2; ++i) {
        uint32_t n = wx86_lazymem_test_net_hit(A1, P1);
        char buf[96];
        std::snprintf(buf, sizeof buf, "hit #%u de (addr,pc) identique renvoie %u (attendu %u)", i, n, i);
        check(n == i, buf);
    }
    check(wx86_lazymem_test_net_hit(A1, P1) == MAX + 3,
          "le compteur continue de grimper au-dela du seuil (la decision d'abandon "
          "vit dans lazy_dabt, pas dans le compteur lui-meme)");

    // ---- (b) same address, DIFFERENT pc: must NOT be treated as a repeat ----
    wx86_lazymem_test_net_reset();
    check(wx86_lazymem_test_net_hit(A1, P1) == 1, "premiere vue (A1,P1)");
    check(wx86_lazymem_test_net_hit(A1, P1) == 2, "deuxieme vue (A1,P1)");
    check(wx86_lazymem_test_net_hit(A1, P1 + 4) == 1,
          "MEME adresse, PC different -> compteur reparti a 1 (l'adresse seule ne suffit pas)");
    check(wx86_lazymem_test_net_hit(A1, P1) == 3,
          "(A1,P1) original toujours suivi independamment apres l'entree (A1,P1+4)");

    // ---- (c) different address, same pc: also not a repeat of (A1, P1) -----
    wx86_lazymem_test_net_reset();
    check(wx86_lazymem_test_net_hit(A1, P1) == 1, "premiere vue (A1,P1)");
    check(wx86_lazymem_test_net_hit(A1 + 0x10000, P1) == 1,
          "adresse differente, MEME PC -> compteur separe (pas confondu avec (A1,P1))");
    check(wx86_lazymem_test_net_hit(A1, P1) == 2, "(A1,P1) inchange par l'entree voisine");

    // ---- (d) threshold reached at exactly the documented count --------------
    wx86_lazymem_test_net_reset();
    const uintptr_t A2 = 0x52520000u;
    const uint32_t  P2 = 0x00900000u;
    uint32_t last = 0;
    for (uint32_t i = 1; i < MAX; ++i) {
        last = wx86_lazymem_test_net_hit(A2, P2);
        char buf[80];
        std::snprintf(buf, sizeof buf, "hit #%u < seuil (%u)", i, MAX);
        check(last < MAX, buf);
    }
    last = wx86_lazymem_test_net_hit(A2, P2);
    check(last == MAX, "le seuil est atteint exactement au Nieme hit identique consecutif");

    // ---- (e) table overflow: more distinct looping addresses than slots -----
    // Must not crash and must not silently merge unrelated addresses: each
    // of the first N distinct (addr, pc) pairs still reports count==1 on its
    // first sighting even after N+2 distinct pairs have been inserted.
    wx86_lazymem_test_net_reset();
    for (uint32_t i = 0; i < N + 2; ++i) {
        uint32_t n = wx86_lazymem_test_net_hit(0x60000000u + i * 0x10000u, 0x00A00000u);
        check(n == 1, "chaque NOUVELLE adresse, meme au-dela de la capacite de la table, "
                      "part a 1 (pas de fusion silencieuse entre deux adresses distinctes)");
    }

    // ---- (f) thread safety: concurrent hits on the SAME (addr, pc) ----------
    // No lost updates: the final count must equal the exact number of calls
    // made, across threads, or the spinlock around the table is broken.
    wx86_lazymem_test_net_reset();
    const uintptr_t A3 = 0x73730000u;
    const uint32_t  P3 = 0x00B00000u;
    const int THREADS = 8, PER_THREAD = 500;
    {
        std::vector<std::thread> ts;
        for (int t = 0; t < THREADS; ++t)
            ts.emplace_back([&]() {
                for (int i = 0; i < PER_THREAD; ++i) wx86_lazymem_test_net_hit(A3, P3);
            });
        for (auto& t : ts) t.join();
    }
    uint32_t final_n = wx86_lazymem_test_net_hit(A3, P3);
    char buf[128];
    std::snprintf(buf, sizeof buf,
                  "compteur concurrent exact apres %d fils x %d coups (+1 de verification) = %u, attendu %u",
                  THREADS, PER_THREAD, final_n, (unsigned)(THREADS * PER_THREAD + 1));
    check(final_n == (uint32_t)(THREADS * PER_THREAD + 1), buf);

    if (failures) {
        std::printf("lazymem_net: %d ECHEC(S)\n", failures);
        return 1;
    }
    std::printf("lazymem_net: tous les cas passent (comptage, PC discriminant, seuil, "
                "debordement de table, acces concurrent)\n");
    return 0;
}
