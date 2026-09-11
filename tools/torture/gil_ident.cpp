// tools/torture/gil_ident.cpp — banc de l'IDENTITÉ du détenteur du GIL.
//
// gil::lock() est sur le chemin de CHAQUE trap. Il n'appelle plus pthread_self()
// (sur pte : pthread_getspecific -> pte_osTlsGetValue -> vitasdk_get_tls_data ->
// sceKernelGetTLSAddr, un appel INTER-MODULE) : il publie une MARQUE DE PILE,
// que le LECTEUR traduit en étiquette via la table gil::register_stack().
//
// Ce banc répond aux deux seules questions que ce remplacement pose :
//   1. la traduction nomme-t-elle le BON fil, et SE TAIT-elle quand elle ne sait
//      pas (fil non enregistré) — le « diagnostic qui ment » serait de nommer le
//      voisin ;
//   2. assert_held() détecte-t-il ENCORE un vrai défaut, y compris le cas fort
//      « le GIL est tenu, mais par QUELQU'UN D'AUTRE » ?
//   3. une pile RECYCLÉE prête-t-elle son nom ? C'est le SEUL nom faux que ce
//      mécanisme puisse produire (un fil non enregistré tournant sur la pile
//      d'un mort encore inscrit), et c'est ce que la moitié « désenregistrement »
//      de l'invariant de gil.h ferme. glibc recycle ses piles, donc l'hôte sait
//      exercer le cas — pte non (gil.h).
//
// HÔTE seulement (fork + SIGABRT). Le mécanisme testé est identique sur cible :
// des piles disjointes et l'adresse d'une locale. Il ne remplace donc PAS
// rt_boot_arm_check.sh, il isole ce que celui-ci ne peut pas montrer.
//
//   g++ -std=gnu++17 -O2 -I src tools/torture/gil_ident.cpp src/runtime/gil.cpp \
//       -lpthread -o /tmp/gil_ident && /tmp/gil_ident
//
// NE PAS compiler avec -DNDEBUG : la phase 2 attend les assertions.
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
// Fenêtre déclarée, très inférieure aux 8 MiB d'une pile hôte : SOUS-ENSEMBLE
// strict, comme sur cible (gil.h).
static const uintptr_t SPAN = 240 * 1024;
static void reg(uint32_t tag) { char ici; uintptr_t hi = (uintptr_t)&ici; gil::register_stack(hi - SPAN, hi, tag); }

// ---- phase 1 : qui tient le GIL ? ------------------------------------------

// Profondeur variable : la marque publiée par lock() CHANGE d'une prise à
// l'autre pour un même fil ; l'étiquette, elle, ne doit pas bouger.
static void take_and_check(uint32_t mytag, int depth) {
    if (depth > 0) { volatile char pad[512]; pad[0] = (char)depth; take_and_check(mytag, depth - 1); (void)pad; return; }
    gil::lock();
    gil::Probe p; gil::probe(&p);
    expect(p.held, "GIL vu pris par son detenteur");
    expect(p.owner_tag == mytag, "etiquette = le fil qui tient VRAIMENT le GIL");
    gil::assert_held();
    gil::unlock();
}

// MONO-FIL : l'attribution du shim doit survivre à un Release/relock pris à une
// AUTRE profondeur (donc sous une AUTRE marque) — c'est exactement ce qu'une
// comparaison brute des marques perdrait, et pourquoi probe() compare les
// étiquettes résolues.
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
// Fil DÉLIBÉRÉMENT non enregistré : il ne doit JAMAIS hériter du nom d'un autre.
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

// ---- phase 2 : assert_held() détecte-t-il encore ? --------------------------

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
    if (which == 0) { gil::assert_held(); return 0; }        // (A) GIL libre
    pthread_t t; pthread_create(&t, nullptr, holder, nullptr);
    pthread_mutex_lock(&m); while (!held) pthread_cond_wait(&c, &m); pthread_mutex_unlock(&m);
    gil::assert_held();                                      // (B) tenu par thr2
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

// ---- phase 3 : une pile RECYCLÉE ne prête pas son nom ----------------------
// Le fil éphémère s'enregistre, prend le GIL, se DÉSENREGISTRE, meurt. Le
// squatteur, créé juste après, hérite très probablement de sa pile (cache de
// piles de glibc) et ne s'enregistre PAS : il doit rester anonyme. Sans le
// désenregistrement, il porterait le nom du mort — le « diagnostic qui ment ».
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
    // Si la pile n'a PAS été réutilisée, le cas n'est pas exercé : on le DIT,
    // on ne compte pas un PASS gratuit (doctrine : l'absence de preuve est une
    // information, jamais un artefact).
    std::printf("  phase 3 (pile recyclee) : marques %#lx / %#lx, ecart %lu -> %s\n", A, B, d,
                d < (unsigned long)SPAN ? "pile REUTILISEE, cas EXERCE"
                                        : "pile NON reutilisee : cas NON EXERCE, ce PASS ne prouve rien");
    return g_fail.load() != before;
}

int main() {
    // Le contrôle d'IDENTITÉ d'assert_held() est éteint par défaut depuis qu'il
    // s'est avéré coûter un parcours de table par shim de synchronisation en
    // production (D2_GILCHECK). Ce test est justement là pour l'éprouver : on
    // l'arme explicitement, avant tout fork et tout fil.
    gil::set_identity_check(true);
    // Les phases 1/3 et 2 sont séparées par fork() : enfant_defaut() rappelle
    // gil::enable(), dont le contrat est « une seule fois par processus ».
    int bad = phase_assert();
    bad |= phase_identite();
    bad |= phase_recyclage();
    std::printf("  total : %d verifications, %d echec(s)\n", g_checks.load(), g_fail.load());
    std::printf("%s\n", bad ? "ECHEC" : "PASS : identite du detenteur + detection des defauts + pile recyclee");
    return bad;
}
