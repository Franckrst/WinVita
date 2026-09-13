// Oracle for src/dynarec86/alt_table.c.
//
// What it proves, and what it doesn't.
//
// It proves two things about the body the library ships:
//   1. No bound: 1000 distinct alternates are all set, all read back, and
//      dyn86_alt_lost() stays at 0.
//   2. A failure is visible: when allocation is refused, the set is refused
//      (the count doesn't advance), dyn86_alt_lost() advances, and the log
//      gets a line naming the loss.
//
// Fault injection: alt_table.c is compiled with -Dmalloc=wx86_selftest_malloc
// (see tools/selftest.sh) — the allocator is this file's, and it can refuse
// on demand. The fake targets the allocator, not the function under test:
// dyn86_set_alternate's body is the library's, line for line. Caveat: the
// tested object differs from the shipped one by that one flag — fault
// injection without recompilation isn't possible on an allocation path.
//
// Negative control: the test verifies the fault actually changes the
// verdict (without it, lost==0; with it, lost>0, and lost stops climbing
// once the fault is removed). A test that gives the same result either way
// proves nothing.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

extern "C" {
    void dyn86_set_alternate(uintptr_t from, uintptr_t to);
    int  dyn86_alt_count(void);
    int  dyn86_alt_lost(void);
    extern uintptr_t *dyn86_alt_from;
    extern uintptr_t *dyn86_alt_to;

    // The allocator alt_table.c sees (-Dmalloc=wx86_selftest_malloc).
    int  g_refuser = 0;
    void* wx86_selftest_malloc(size_t n) { return g_refuser ? nullptr : std::malloc(n); }

    // Stand-in for the console log: a line counter here.
    int  g_lignes = 0;
    char g_derniere[128] = {0};
    void wx86_vita_progress_c(const char* msg) {
        ++g_lignes;
        std::snprintf(g_derniere, sizeof g_derniere, "%s", msg ? msg : "(null)");
    }
}

static int erreurs = 0, verifs = 0;
static void ok(bool c, const char* quoi) {
    ++verifs;
    if (!c) { std::printf("  ECHEC: %s\n", quoi); ++erreurs; }
}

static uintptr_t cible(int i) { return (uintptr_t)0x00400000u + (uintptr_t)i * 16u; }

int main() {
    // ---- 1. no bound: 1000 sets, 1000 lookups ------------------------------
    const int N = 1000;
    for (int i = 0; i < N; ++i) dyn86_set_alternate(cible(i), cible(i) + 8u);
    ok(dyn86_alt_count() == N, "les 1000 alternates sont poses");
    ok(dyn86_alt_lost() == 0,  "aucun alternate perdu");
    ok(g_lignes == 0,          "aucune ligne de journal sur le chemin nominal");
    int retrouves = 0;
    for (int i = 0; i < N; ++i)
        for (int k = 0; k < dyn86_alt_count(); ++k)
            if (dyn86_alt_from[k] == cible(i)) {
                if (dyn86_alt_to[k] == cible(i) + 8u) ++retrouves;
                break;
            }
    ok(retrouves == N, "les 1000 redirections se relisent a la bonne valeur");

    // Re-setting an already-present key updates in place without using new space.
    const int avant = dyn86_alt_count();
    dyn86_set_alternate(cible(7), 0xDEADBEEFu);
    ok(dyn86_alt_count() == avant, "re-poser une cle connue n'ajoute pas d'entree");
    bool maj = false;
    for (int k = 0; k < dyn86_alt_count(); ++k)
        if (dyn86_alt_from[k] == cible(7)) { maj = (dyn86_alt_to[k] == 0xDEADBEEFu); break; }
    ok(maj, "re-poser une cle connue met la valeur a jour");

    // ---- 2. a failure is visible -------------------------------------------
    const int nAvant     = dyn86_alt_count();
    const int perduAvant = dyn86_alt_lost();
    g_refuser = 1;
    int poses = 0;
    // At most one growth step separates two capacities, so a handful of sets
    // reaches the current capacity; this tries far more than enough.
    for (int i = 0; i < 4096; ++i) {
        dyn86_set_alternate(cible(100000 + i), 1u);
        if (dyn86_alt_lost() > perduAvant) break;
        ++poses;
    }
    g_refuser = 0;
    ok(dyn86_alt_lost() > perduAvant, "une allocation refusee est COMPTEE");
    ok(g_lignes > 0,                  "une allocation refusee est JOURNALISEE");
    ok(std::strstr(g_derniere, "PERDU") != nullptr, "la ligne nomme la perte");
    ok(dyn86_alt_count() == nAvant + poses,
       "une pose refusee n'entre pas dans la table");

    // ---- 3. negative control: without the fault, the verdict differs ------
    const int perduApres = dyn86_alt_lost();
    for (int k = 0; k < 256; ++k) dyn86_set_alternate(cible(200000 + k), 2u);
    ok(dyn86_alt_lost() == perduApres,
       "faute retiree : plus aucune perte (le filet ne crie pas tout seul)");

    std::printf("alt_table: %d verifications, %d echec(s)\n", verifs, erreurs);
    return erreurs ? 1 : 0;
}
