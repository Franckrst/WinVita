// src/runtime/gil.h — the runtime Global Interpreter-style Lock (native
// scheduler only; spec 2026-08-28 D3).
//
// One process-wide pthread_mutex serializing everything that runs OUTSIDE
// translated guest code: shim bodies, intrinsics, SEH dispatch, scheduler
// state. Translated guest code runs WITHOUT it (the game's own x86 `lock`
// prefixes are translated to real ldrex/strex). It is taken at the CpuBox86
// trap dispatch — NOT in Bridge::trap_handler, because Tier-1 intrinsics
// bypass the Bridge (cpu_box86.cpp try_intrinsic).
//
// It doubles as the native scheduler's lock: NativeScheduler::wait() blocks
// with pthread_cond_wait(cv, gil::mutex()) — atomically releasing the GIL
// while the thread sleeps. This gives the timeout-vs-completion exclusion
// (the GQCS phantom-packet bug) for free. NON-recursive: never nest lock().
//
// Inert until enable(): the cooperative backend never calls enable(), so its
// deterministic single-runner path never touches the mutex.
#pragma once
#include <pthread.h>
#include <cstdint>

namespace d2rt { namespace gil {

bool active();
void enable();                 // called ONCE, by the native scheduler ctor
void lock();
void unlock();
// Prise NON bloquante (coeur2, D2_FILSTAT) : vrai = pris, memes marques que
// lock() ; faux = occupe, rien n'est touche. Sert a COMPTER la contention du
// GIL par fil sans changer le chemin par defaut (cpu_box86.cpp TrapGuard).
bool try_lock();
pthread_mutex_t* mutex();      // for pthread_cond_wait integration

// Debug owner tracking (cheap: plain assignments protected by the mutex
// itself — set after lock, cleared before unlock; never atomics).
// assert_held() asserts the CALLING host thread owns the GIL; no-op while
// inactive. Called at the top of every NativeScheduler method whose contract
// requires the lock (wait_common/wake_check_all/create_thread/notify).
// Deux moitiés : (1) le GIL est pris — vraie pour TOUT appelant ; (2) et c'est
// bien moi — muette pour un fil NON enregistré (cf. la table plus bas). La
// couverture perdue face à l'ancien pthread_equal(g_owner, pthread_self()) est
// donc exactement celle des fils non enregistrés ; pour tous les autres (main
// dans run(), runners) le cas fort « tenu par quelqu'un d'AUTRE » déclenche
// toujours. La moitié (2) est sous #ifndef NDEBUG.
void assert_held();
// Rallume/éteint le contrôle (2) d'assert_held() — « c'est bien MOI qui tiens
// le GIL », qui parcourt la table d'identité. En production il est ÉTEINT par
// défaut (il coûtait un parcours par shim de synchronisation) et se rallume
// par D2_GILCHECK=1, lu UNE fois au démarrage. Cette fonction existe pour les
// TESTS, qui doivent l'armer sans dépendre de l'environnement ; l'appeler avant
// de lancer des fils. Le contrôle (1) — assert(g_owned) — reste inconditionnel.
void set_identity_check(bool on);
// pthread_cond_wait(cv, mutex()) releases and reacquires the mutex BEHIND
// lock()/unlock()'s back: the last unlocker cleared the owner flag, so a
// waiter must re-stamp itself when the wait returns or a later assert_held()
// in the same shim body (wait-then-notify) fires falsely. Call with the
// mutex held, right after the cond-wait loop exits.
void mark_owned();
// Symétrique de mark_owned() pour l'OBSERVABILITÉ (§19) : à appeler JUSTE
// AVANT une boucle de cond-wait. Sans elle, le détenteur publié resterait le
// fil parti dormir dans le cond (qui a rendu le mutex sans passer par
// unlock()), et le lecteur croirait à une prise éternelle : le mensonge exact
// que cette instrumentation existe pour ne pas produire. Ne touche PAS au
// marquage historique de assert_held() (dont le comportement est inchangé).
void mark_released();

// ---- Observabilité (audit T12 §19) — QUI tient le GIL, et depuis quand -----
//
// Le gel total de la session console 7 (§18.7) est resté une HYPOTHÈSE faute
// d'une seule trace de l'état du GIL : `sautees-gil=2893` disait que le filet
// anti-famine renonçait, jamais QUI le bloquait. Ces champs le disent.
//
// Écrits UNIQUEMENT par le détenteur, à la prise et au relâchement, en
// atomiques relaxed (pas de verrou, pas de barrière). Lus par le chien de
// garde et le battement famine SANS prendre le GIL — un lecteur qui bloque
// sur le verrou qu'il observe ne rapporte rien.
//
// AUCUNE HORLOGE n'est lue à la prise, délibérément : sur Vita clock_gettime
// est un appel noyau et le GIL est pris à CHAQUE trap d'import (rt_boot met
// déjà en cache rt_now_ms pour exactement cette raison — « D2 polls the clock
// ~26k/s »). L'ancienneté de la prise est donc DÉRIVÉE PAR LE LECTEUR : deux
// relevés successifs portant le même compteur `acq` prouvent que la prise dure
// depuis au moins l'intervalle qui les sépare. Ce que publie ce module est
// toujours une BORNE INFÉRIEURE (préfixe `>=` à l'impression), jamais une
// durée gonflée par une estampille périmée.
//
// Identité du détenteur : une MARQUE DE PILE (l'adresse d'une variable locale
// de lock(), donc un point dans la pile hôte du détenteur), traduite en
// étiquette par le lecteur. PAS de pthread_t : sur pte, pthread_self() descend
// dans pthread_getspecific -> pte_osTlsGetValue -> sceKernelGetTLSAddr, la MÊME
// chaîne noyau qu'un accès emutls — un appel INTER-MODULE par prise du GIL,
// donc par trap. La marque coûte une instruction (add rX, sp, #n) et aucun
// appel ; la traduction marque -> étiquette est faite PAR LE LECTEUR (probe(),
// hors chemin chaud), à partir de la table déclarée ci-dessous.
struct Probe {
    bool        active;    // le GIL existe (backend natif) — faux sous coop
    bool        held;      // pris à l'instant du relevé
    uint32_t    owner_tag; // étiquette du détenteur (id de fil invité), 0 = INCONNU
    uint32_t    acq;       // nombre total de prises depuis le boot
    bool        in_shim;   // le détenteur est DANS un corps de shim du Bridge
    uint32_t    shim_va;   // VA du trap (mappable par D2_DUMPTRAPS=1)
    const char* shim;      // "ws2_32.dll!#18" — chaîne stable (clé de slot_by_tag_)
};
void probe(Probe* out);        // ne bloque jamais ; tout à zéro si inactif

// ---- Table d'identité SANS recherche TLS -----------------------------------
//
// La seule identité qu'un fil porte GRATUITEMENT (dans un registre, sans appel)
// est son pointeur de pile. Chaque fil susceptible de prendre le GIL déclare
// donc UNE FOIS, DEPUIS LUI-MÊME, un intervalle [lo,hi] de SA pile hôte et
// l'étiquette à afficher (id de fil invité, jamais 0) :
//
//     char ici;                                     // son adresse = la marque
//     int s = gil::register_stack((uintptr_t)&ici - SPAN, (uintptr_t)&ici, t->id);
//     ...                                           // vie du fil
//     gil::unregister_stack(s);                     // APRÈS sa dernière prise
//
// INVARIANT QUI PORTE TOUTE LA SÛRETÉ, et que rien d'autre ne garde :
//   *** tout fil qui appelle gil::lock() doit s'être enregistré avant, et
//       s'être désenregistré après sa DERNIÈRE prise. ***
// Sans lui, un fil non enregistré qui tournerait sur une pile RECYCLÉE d'un fil
// mort encore inscrit hériterait de SON étiquette : le seul nom faux que ce
// mécanisme puisse produire. Les deux moitiés comptent — l'enregistrement pour
// que le fil vivant soit nommé, le désenregistrement pour qu'un mort ne prête
// pas son nom. (Sur cible aujourd'hui pte ne recycle aucune pile en session :
// pte_osThreadExit est un sceKernelExitThread nu, et DeleteThread n'est atteint
// que par detach. Le désenregistrement ne corrige donc rien d'atteignable — il
// ferme le flanc pour que l'invariant se garde tout seul.)
// L'unique preneur volontairement NON enregistré est le fil main AVANT run()
// (gil::Guard des bancs FAMINETEST/SELECTTEST de rt_boot) : sa pile est
// disjointe de toute pile pte, il s'affiche « hote », et assert_held() ne
// prétend alors rien de plus que « le GIL est pris ».
//
// L'intervalle déclaré doit être un SOUS-ENSEMBLE STRICT de la pile réelle : le
// sens sûr de l'erreur est « je ne sais pas » (marque hors de tout intervalle,
// étiquette 0, la ligne dit « hote »), jamais « je nomme le fil d'à côté ».
// La résolution refuse d'ailleurs de répondre dès que DEUX intervalles
// contiennent la marque (piles recyclées après un exit) : ambiguïté => 0.
// Un fil non enregistré n'est jamais nommé — c'est exactement ce que faisait
// déjà un fil hôte sans pthread_t connu.
//
// COÛT DE LA RÉSOLUTION, dit sans détour : c'est un parcours linéaire de la
// table, SANS barrière et SANS appel (la sûreté vient de l'écriture-unique des
// cases, pas d'un acquire — voir gil.cpp). Sa durée croît avec le nombre de
// fils invités JAMAIS enregistrés depuis le boot (les cases ne sont pas
// réutilisées), plafonné à 132. Elle n'est PAS mesurée. Hors chemin de trap
// (lecteurs + assert_held), jamais dans lock().
//
// Rend l'index de la case, ou -1 si le fil n'a pas pu être inscrit (table
// pleine, arguments refusés) — dans ce cas il restera « hote », ce qui est le
// sens sûr. unregister_stack(-1) est un no-op.
int  register_stack(uintptr_t lo, uintptr_t hi, uint32_t tag);
void unregister_stack(int slot);

// Marquage du corps de shim en cours (Bridge::trap_handler, sous GIL).
// UN SEUL enregistrement global suffit : le GIL garantit qu'un seul fil est
// dans un corps de shim à la fois. Il mémorise son détenteur, donc un shim
// qui RELÂCHE le GIL (gil::Release autour d'un ::poll) puis le reprend après
// qu'un autre fil soit passé n'induit pas le lecteur en erreur : les deux
// détenteurs diffèrent, le lecteur n'attribue alors aucun shim. Inerte tant
// que le GIL est inactif (coop) — un test d'un booléen global.
// NOTE (marques de pile) : le détenteur mémorisé ici est la MARQUE publiée par
// lock(), qui change d'une prise à l'autre pour un MÊME fil (profondeur d'appel
// différente). La comparaison est donc faite par probe() sur les ÉTIQUETTES
// résolues, pas sur les marques brutes — sans quoi un shim qui relâche puis
// reprend le GIL perdrait son attribution alors que rien n'a changé.
void note_shim_enter(uint32_t trap_va, const char* tag);
void note_shim_exit();

// RAII: take the GIL iff active (trap dispatch, host-side entry points).
struct Guard {
    bool a;
    Guard()  { a = active(); if (a) lock(); }
    ~Guard() { if (a) unlock(); }
    Guard(const Guard&) = delete;              // a copied Guard double-unlocks
    Guard& operator=(const Guard&) = delete;
};
// RAII inverse: RELEASE the GIL around a blocking host call (::poll, long
// translation, sleep) so other guest threads keep running (spec D3).
struct Release {
    bool a;
    Release()  { a = active(); if (a) unlock(); }
    ~Release() { if (a) lock(); }
    Release(const Release&) = delete;          // a copied Release double-locks
    Release& operator=(const Release&) = delete;
};

}} // namespace d2rt::gil
