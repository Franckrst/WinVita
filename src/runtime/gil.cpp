// src/runtime/gil.cpp — see gil.h.
//
// PTHREAD_MUTEX_INITIALIZER static init is valid on BOTH pthreads we link:
// glibc (host/qemu checks) and VitaSDK's pte, where pthread_mutex_t is a
// pointer and the initializer is the sentinel ((pthread_mutex_t)-1) that
// pthread_mutex_lock lazily materializes via pte_mutex_check_need_init
// (verified in libpthread.a). custommem.c runs pthread_mutex_init at startup
// only because it wants an ERRORCHECK attr — not because static init breaks.
// pte's DEFAULT type is non-recursive, matching the never-nest contract.
#include "runtime/gil.h"
#include <cassert>
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace d2rt { namespace gil {

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
// write precedes every pthread_create (NativeScheduler ctor) — plain bool is
// race-free by construction; do not "fix" with a toggle.
static bool g_active = false;
// « Le GIL est pris » pour assert_held() : écrit UNIQUEMENT sous g_mu (posé
// après lock / effacé avant unlock), donc un booléen nu suffit — le mutex EST
// la synchronisation. L'IDENTITÉ du détenteur, elle, n'est plus ici : elle est
// publiée en marque de pile (g_pub_owner_mark, plus bas), et un fil garé dans
// pthread_cond_wait laisse une marque périmée derrière lui (le cv a rendu le
// mutex tout seul) — sans conséquence : la prise suivante l'écrase, et le
// dormeur se réinscrit par mark_owned() au retour de son attente.
static bool g_owned = false;

// ---- Observabilité (gil.h, audit §19) --------------------------------------
// Publié par le DÉTENTEUR seul, en relaxed : trois stores à la prise, un au
// relâchement. Pas de barrière — un lecteur qui voit un état d'il y a quelques
// nanosecondes le sait (il compare des relevés espacés de 200 ms ou 10 s).
// `g_pub_held` est distinct de `g_pub_owner_mark` À DESSEIN : un détenteur peut
// très bien n'être identifiable par personne (fil hôte non enregistré, table
// pleine), et « détenteur inconnu » ne doit jamais se lire « GIL libre ».
static std::atomic<bool>      g_pub_held{false};
// MARQUE DE PILE du détenteur (gil.h), pas un pthread_t : lock() est sur le
// chemin de CHAQUE trap et pthread_self() y coûtait une chaîne TLS complète
// (pthread_getspecific -> pte_osTlsGetValue -> sceKernelGetTLSAddr, appel
// INTER-MODULE). 0 = personne n'a encore pris le GIL.
static std::atomic<uintptr_t> g_pub_owner_mark{0};
static std::atomic<uint32_t>  g_pub_acq{0};
// Corps de shim en cours (un seul à la fois : c'est ce que le GIL garantit).
static std::atomic<uintptr_t> g_shim_owner{0};   // 0 = personne dans un shim
static std::atomic<uint32_t>  g_shim_va{0};
static std::atomic<const char*> g_shim_tag{nullptr};

// acq : mono-écrivain (le détenteur), donc load+store relaxed — pas de RMW
// atomique (ldrex/strex) sur le chemin chaud.
static inline void pub_take(uintptr_t mark) {
    g_pub_acq.store(g_pub_acq.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    g_pub_owner_mark.store(mark, std::memory_order_relaxed);
    g_pub_held.store(true, std::memory_order_relaxed);
}
static inline void pub_drop() { g_pub_held.store(false, std::memory_order_relaxed); }

// ---- Table d'identité (gil.h) ----------------------------------------------
// Cases jamais RÉUTILISÉES (un désenregistrement éteint la case, il ne la rend
// pas) : elles sont lues SANS VERROU par le chien de garde, le battement famine
// et assert_held(). 132 = kFlatMax (128 fils invités publiés) + le main + marge.
//
// AUCUNE BARRIÈRE, ni à l'écriture ni à la lecture — et ce n'est pas un oubli.
// La version précédente prenait un `acquire` PAR CASE (un `dmb ish` par tour de
// boucle au désassemblage) pour garantir que `lo`/`hi` soient visibles dès que
// `tag` l'est. Cette garantie n'est pas nécessaire, parce que chaque champ d'une
// case ne change qu'UNE FOIS (0 du BSS -> sa valeur), puis au plus une fois vers
// 0 à l'extinction. Le lecteur ne peut donc jamais lire de valeur « à moitié
// écrite » : il lit l'ancienne (0) ou la nouvelle. Il suffit alors que 0 soit
// REFUSANT sur les deux champs qui décident d'une correspondance :
//
//   * `tag == 0`  -> case ignorée ;
//   * `lo == 0`   -> case ignorée (garde explicite ci-dessous) ;
//   * `hi == 0`   -> `mark <= 0` est faux, une marque de pile n'est jamais 0.
//
// Toute visibilité PARTIELLE d'une case donne donc un RATÉ (« je ne sais pas »,
// étiquette 0, la ligne dit « hote »), jamais une correspondance fausse : c'est
// exactement le sens d'erreur que gil.h impose. `lo` est écrit APRÈS `hi` et
// effacé AVANT `tag` pour cette raison — c'est lui la garde.
// Les champs restent atomiques (relaxed) pour que la lecture concurrente d'une
// écriture ne soit pas une course au sens du langage ; sur ARMv7 un load/store
// relaxed d'un mot aligné est un `ldr`/`str` nu, donc coût nul.
static constexpr unsigned kStackSlots = 132;
struct StackSlot {
    std::atomic<uintptr_t> lo, hi;   // lo == 0 => case morte ou pas encore prête
    std::atomic<uint32_t>  tag;      // 0 = emplacement pas encore prêt / éteint
};
static StackSlot g_slots[kStackSlots];
static std::atomic<unsigned> g_slot_res{0};   // emplacements RÉSERVÉS

int register_stack(uintptr_t lo, uintptr_t hi, uint32_t tag) {
    if (!lo || lo >= hi || tag == 0) return -1;       // rien à dire vaut mieux qu'un nom faux
    unsigned i = g_slot_res.fetch_add(1, std::memory_order_relaxed);
    if (i >= kStackSlots) return -1;                  // table pleine : ce fil restera « hote »
    g_slots[i].hi.store(hi, std::memory_order_relaxed);
    g_slots[i].lo.store(lo, std::memory_order_relaxed);   // la garde, écrite APRÈS hi
    g_slots[i].tag.store(tag, std::memory_order_relaxed);
    return (int)i;
}

// Éteint la case (elle n'est pas rendue : voir plus haut, la sûreté vient de
// l'écriture-unique). À appeler par le fil lui-même, après sa dernière prise du
// GIL — c'est la moitié de l'invariant qui empêche un fil mort de prêter son
// nom à un vivant non enregistré tournant sur sa pile recyclée (gil.h).
void unregister_stack(int slot) {
    if (slot < 0 || (unsigned)slot >= kStackSlots) return;
    g_slots[slot].lo.store(0, std::memory_order_relaxed);   // coupe la correspondance D'ABORD
    g_slots[slot].tag.store(0, std::memory_order_relaxed);
}

// Marque -> étiquette, DEUX marques en UN SEUL parcours. UNE SEULE
// correspondance donne une réponse ; zéro (fil non enregistré) ou PLUSIEURS
// (piles recyclées, intervalles qui se recouvrent) rendent 0 = « je ne sais
// pas ». Jamais de nom faux (gil.h). Hors chemin chaud : appelée par les
// LECTEURS, et par assert_held() en build de debug — qui a besoin des deux
// marques (la sienne et celle du détenteur) et ne paie donc qu'un parcours au
// lieu des deux d'avant. Une marque nulle ne peut pas correspondre (lo > 0).
static void resolve2(uintptr_t ma, uintptr_t mb, uint32_t* ta, uint32_t* tb) {
    uint32_t fa = 0, fb = 0; unsigned ha = 0, hb = 0;
    unsigned n = g_slot_res.load(std::memory_order_relaxed);
    if (n > kStackSlots) n = kStackSlots;
    for (unsigned i = 0; i < n; ++i) {
        uint32_t t = g_slots[i].tag.load(std::memory_order_relaxed);
        if (!t) continue;                             // réservée, pas encore remplie, ou éteinte
        uintptr_t lo = g_slots[i].lo.load(std::memory_order_relaxed);
        if (!lo) continue;                            // la garde (cf. plus haut)
        uintptr_t hi = g_slots[i].hi.load(std::memory_order_relaxed);
        if (ma >= lo && ma <= hi) { fa = t; ++ha; }
        if (mb >= lo && mb <= hi) { fb = t; ++hb; }
    }
    *ta = (ha == 1) ? fa : 0;
    *tb = (hb == 1) ? fb : 0;
}

// ---- D2_GILCHECK : verification d'IDENTITE du detenteur, ETEINTE PAR DEFAUT --
// Le controle (2) de assert_held() (« c'est bien MOI qui tiens le GIL »)
// parcourt la table d'identite (resolve2) a CHAQUE shim de synchronisation, et
// le binaire livre n'a pas -DNDEBUG : il payait donc ce parcours en production
// pour un invariant qui ne se casse que pendant un chantier de scheduler.
// D2_GILCHECK=1 le rallume tel quel. Le controle (1) — assert(g_owned), une
// simple lecture de booleen — reste TOUJOURS actif : c'est lui qui attrape le
// vrai defaut (« personne ne tient le GIL »), et il ne coute rien.
// PAS de -DNDEBUG global : d'autres invariants du projet en dependent.
// Initialisation DYNAMIQUE au demarrage du programme (portee namespace) : pas
// de variable de garde thread-safe sur le chemin chaud, contrairement a un
// static de fonction. assert_held() n'est appele qu'une fois le runtime lance,
// donc bien apres l'initialisation statique.
static bool g_gilcheck = [] {
    const char* v = std::getenv("D2_GILCHECK");
    return v && std::strcmp(v, "0") != 0;
}();
void set_identity_check(bool on) { g_gilcheck = on; }   // TESTS (cf. gil.h)

bool active() { return g_active; }
void enable() { assert(!g_active); g_active = true; }   // called-ONCE contract
// `ici` n'est JAMAIS lu : seule son ADRESSE compte — un point dans la pile
// hôte du fil appelant, donc son identité, obtenue sans le moindre appel
// (cf. gil.h). C'est tout ce que ce chemin, parcouru à CHAQUE trap, publie.
void lock()   { char ici; pthread_mutex_lock(&g_mu); g_owned = true; pub_take((uintptr_t)&ici); }
void unlock() { g_owned = false; pub_drop(); pthread_mutex_unlock(&g_mu); }
bool try_lock() {
    char ici;
    if (pthread_mutex_trylock(&g_mu) != 0) return false;
    g_owned = true; pub_take((uintptr_t)&ici); return true;
}
pthread_mutex_t* mutex() { return &g_mu; }
void assert_held() {
    if (!g_active) return;
    assert(g_owned);                         // (1) le GIL est pris — vrai défaut si non
#ifndef NDEBUG
    // (2) et c'est BIEN MOI qui le tiens. Exact dès que l'appelant est un fil
    // ENREGISTRÉ, ce que sont tous les appelants réels (le main dans run() et
    // les runners) ; un fil non enregistré ne peut pas être départagé — on ne
    // fabrique alors aucun verdict plutôt qu'un faux. Tout est sous #ifndef
    // NDEBUG : coût nul quand les assertions sont coupées.
    // UN SEUL parcours de table pour les deux marques, et AUCUNE barrière
    // dedans (cf. resolve2). Ce chemin n'est PAS froid — au moins un
    // assert_held() par shim de synchronisation — et le binaire livré n'a pas
    // -DNDEBUG : il est donc désormais derrière D2_GILCHECK, ÉTEINT PAR
    // DÉFAUT (cf. g_gilcheck plus haut). Son coût croissait avec le nombre de
    // fils enregistrés.
    if (!g_gilcheck) return;                 // ETEINT PAR DEFAUT (cf. g_gilcheck)
    char ici;
    uint32_t me = 0, owner = 0;
    resolve2((uintptr_t)&ici, g_pub_owner_mark.load(std::memory_order_relaxed), &me, &owner);
    assert(!me || owner == me);
#endif
}
void mark_owned() { char ici; g_owned = true; pub_take((uintptr_t)&ici); }
void mark_released() { pub_drop(); }   // observabilité seule : g_owned est laissé tel quel (cf. gil.h)

void note_shim_enter(uint32_t trap_va, const char* tag) {
    if (!g_active) return;                       // coop : inerte (un test, pas d'écriture)
    g_shim_va.store(trap_va, std::memory_order_relaxed);
    g_shim_tag.store(tag, std::memory_order_relaxed);
    // Réutilise la marque déjà publiée par lock() : la recalculer ici coûterait
    // une écriture de plus par trap, et la RÉSOUDRE (resolve2) un parcours de
    // table sur le chemin chaud. La résolution est faite par probe().
    g_shim_owner.store(g_pub_owner_mark.load(std::memory_order_relaxed), std::memory_order_relaxed);
}
void note_shim_exit() {
    if (!g_active) return;
    g_shim_owner.store(0, std::memory_order_relaxed);
}

void probe(Probe* p) {
    p->active = g_active; p->held = false; p->owner_tag = 0; p->acq = 0;
    p->in_shim = false; p->shim_va = 0; p->shim = nullptr;
    if (!g_active) return;                       // coop : rien à dire, et rien ne s'imprime
    p->held      = g_pub_held.load(std::memory_order_relaxed);
    uintptr_t om = g_pub_owner_mark.load(std::memory_order_relaxed);
    uintptr_t so = g_shim_owner.load(std::memory_order_relaxed);
    p->acq       = g_pub_acq.load(std::memory_order_relaxed);
    uint32_t t_om = 0, t_so = 0;
    resolve2(om, so, &t_om, &t_so);              // résolution CÔTÉ LECTEUR (gil.h), un seul parcours
    p->owner_tag = t_om;
    if (!p->held) return;
    // Le shim n'est attribué QUE si son détenteur est celui qui tient le GIL
    // maintenant (cf. gil.h : un shim qui relâche puis reprend le GIL). Deux
    // marques du MÊME fil diffèrent (profondeurs d'appel différentes), d'où la
    // comparaison sur les ÉTIQUETTES résolues ; l'égalité brute des marques
    // couvre en plus le cas d'un fil non enregistré (étiquette 0 des deux
    // côtés, qu'il ne faut surtout pas prendre pour une correspondance).
    if (so && (so == om || (p->owner_tag && t_so == p->owner_tag))) {
        p->in_shim = true;
        p->shim_va = g_shim_va.load(std::memory_order_relaxed);
        p->shim    = g_shim_tag.load(std::memory_order_relaxed);
    }
}

}} // namespace d2rt::gil
