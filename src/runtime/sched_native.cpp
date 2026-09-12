// src/runtime/sched_native.cpp — see sched_native.h and the spec (D1-D8).
#include "runtime/sched_native.h"

extern "C" { extern uint64_t dyn86_jp_run_us; uint64_t dyn86_jp_now_us(void); }
extern "C" {
    int      d2rt_wakeprof = 0;          // D2_WAKEPROF, arme par rt_boot
    uint64_t d2rt_wake_n = 0, d2rt_wake_us = 0, d2rt_wake_worst = 0;
    // ⚠️ Compteur BRUT des signaux emis. Sans lui, « n=0 » ne se distingue pas
    // d'un instrument mort — l'erreur la plus frequente de la journee.
    uint64_t d2rt_wake_sig = 0;
    // Compteur des sondages a delai zero servis SANS dormir : c'est la mesure
    // du gain, et sans lui on ne saurait pas si le correctif a pris.
    uint64_t d2rt_poll0_n = 0;
}
#include "runtime/prof.h"

#include <cstdio>
#include <vector>
#include <utility>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <sched.h>
#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
extern "C" void dyn86_vita_open_vm_thread(void);   // mman_vita.c — VM domain PAR FIL (DACR)
#endif

// JOURNAL, EPINGLAGE ET RECENSEMENT DES COEURS : tout cela appartient au MOTEUR
// (platform/vita_host.h), parce que le moteur cible la Vita.
//
// Cette unite est l'ordonnanceur par DEFAUT du projet : c'est elle qui epingle
// et recense les fils ouvriers. Elle declarait quatre symboles en lien FAIBLE
// au prefixe du PREMIER consommateur — et un commentaire nommait meme le
// fichier `vita_present.cpp`, qui appartient a ce consommateur. Un lien faible
// non resolu vaut NULL : tout portage dont les symboles ne portent pas ce
// prefixe obtenait, EN SILENCE et sans erreur de lien, un journal muet et des
// fils NON EPINGLES. L'epinglage n'est pas cosmetique (regles de placement
// mesurees sur materiel) et un filet inerte en silence est le mode de panne le
// plus cher de ce projet.
//
// Les trois services de coeurs restent CONSOLE-SEULEMENT : tous leurs appels
// ci-dessous vivent deja sous `#ifdef __vita__`, il n'y a ni coeur a repartir
// ni affinite a relire ailleurs. Le journal, lui, est appele depuis le corps
// generique : il a sa version no-op hors console (voir platform/vita_host.h).
#include "platform/vita_host.h"
// L'HORLOGE MONOTONE EST AU MOTEUR (meme constat que bridge.cpp) : ces appels
// passaient par `d2rt_wake_now_us`, que chaque portage definissait en une ligne
// comme un renvoi vers wx86_now_us.
#include "runtime/host_clock.h"
extern "C" { extern uint32_t d2rt_sw_seq; }   // tick_real cache invalidation (rt_boot)
extern "C" void dyn86_dump_xfer(void);

namespace d2rt {

using S = GuestThread::State;

static void progress(const char* m) { wx86_vita_progress_c(m); }

NativeScheduler::NativeScheduler(Cpu* cpu, Bridge* br,
                                 uint32_t stack_region, uint32_t stack_each,
                                 uint32_t tib_region)
    : cpu_(cpu), br_(br),
      stack_region_(stack_region), stack_each_(stack_each), stack_next_(stack_region),
      tib_region_(tib_region), tib_next_(tib_region) {
    gil::enable();
}

uint64_t NativeScheduler::now_ms() const {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

// ---- Observabilité GIL (audit §19) : les DEUX lecteurs, sans verrou --------

int NativeScheduler::gil_field(char* out, unsigned n) {
    gil::Probe p; gil::probe(&p);
    if (!p.active || n < 8) return 0;            // coop : champ ABSENT de la ligne alive:
    // Ancienneté DÉRIVÉE (gil.h) : tant que le compteur de prises ne bouge pas
    // et que le GIL reste pris, c'est LA MÊME prise. `since` est posé au
    // premier relevé qui la voit, donc la durée annoncée est une BORNE
    // INFÉRIEURE (au plus une période de chien de garde de moins que la
    // réalité) — jamais gonflée.
    static uint32_t last_acq = 0; static bool last_held = false; static uint64_t since = 0;
    uint64_t now = now_ms();
    if (!(p.held && last_held && p.acq == last_acq)) since = now;
    last_acq = p.acq; last_held = p.held;
    if (!p.held) return std::snprintf(out, n, " gil=-");
    char who[24];
    if (p.owner_tag) std::snprintf(who, sizeof who, "thr%u", p.owner_tag);
    else             std::snprintf(who, sizeof who, "hote");
    int w = std::snprintf(out, n, " gil=%s/>=%llums/acq=%u", who,
                          (unsigned long long)(now - since), p.acq);
    if (w > 0 && (unsigned)w < n && p.in_shim)
        w += std::snprintf(out + w, n - (unsigned)w, " dans=%s",
                           p.shim ? p.shim : "?");
    return w < 0 ? 0 : (w > (int)n ? (int)n : w);
}

// ---- Vivacité par runner (sched_native.h) ----------------------------------
//
// POURQUOI CE POINT DE MESURE : le compteur est dérivé du budget de blocs de
// l'emu, décrémenté par le prologue de CHAQUE bloc traduit. C'est donc la
// couture de traduction elle-même — le point le plus fréquent qu'un fil invité
// qui avance franchit, et le SEUL qui reste vrai quand le fil boucle sans
// appeler le moindre shim (le tourniquet InterlockedCompareExchange / select
// passe par des traps, mais un tourniquet purement traduit n'en passerait
// aucun). Un compteur de traps aurait ce trou aveugle ; celui-ci ne l'a pas,
// et il ne coûte RIEN sur le chemin chaud (Cpu::thread_emu_blocks : une
// addition par TRANCHE, jamais par bloc).
//
// CE QUE LE CHAMP DIT : « blocs traduits exécutés depuis le boot », MONOTONE
// (modulo 2^32 ; seules les différences sont lues, et une différence non
// signée reste exacte à travers le repli). Deux relevés suffisent :
//   * un compteur bouge alors que frames= est figé   => FAMINE ;
//   * AUCUN ne bouge                                 => tout le monde est arrêté.
//
// CE QUE LE CHAMP NE DIT PAS : « arrêté » n'est PAS « interbloqué ». Un fil
// garé dans WaitForSingleObject, dans recv, ou dans le select bloquant est
// sainement bloqué et son compteur ne bouge pas non plus (revue I2). D'où
// l'état publié JUSTE À CÔTÉ du compteur (R/B/P/N) : c'est lui qui sépare
// « tout le monde attend le réseau » (B partout, sain) de l'interblocage
// (R qui ne progresse pas, ou B sur des objets que personne ne signale).
// Un fil qui exécute très peu de blocs entre deux relevés reste indiscernable
// d'un fil arrêté à cette résolution.
int NativeScheduler::vivacity_field(char* out, unsigned n) {
    if (!cpu_ || !out || n < 8) return 0;
    // Une seule question au backend : s'il n'a pas de compteur de blocs, le
    // champ est ABSENT (rien n'est écrit dans `out`), jamais un zéro menteur.
    uint32_t probe = 0;
    if (!cpu_->thread_emu_blocks(nullptr, &probe)) return 0;
    // Place insuffisante : le champ reste PRÉSENT, avec une valeur avouée
    // inconnue. « Absent » veut dire « backend coop » et rien d'autre — un
    // champ qui disparaît parce que la ligne était pleine rendrait le silence
    // ambigu au pire moment (revue M6).
    if (n < 24) return std::snprintf(out, n, " run=?");
    char buf[256]; int w = 0; unsigned shown = 0, skipped = 0;
    // 24 = pire cas d'une entrée (",<id>:<E>:<10 chiffres>") — on ne coupe
    // jamais une entrée en deux, les non-listés partent dans « +k ». La même
    // constante garde l'entrée ET le suffixe « +k » (revue M7 : la garde
    // d'entrée et celle du suffixe étaient asymétriques).
    static const int kEntry = 24;
    uint32_t fn = flat_n_.load(std::memory_order_acquire);   // cliché plat, sans verrou
    for (uint32_t i = 0; i < fn && i < kFlatMax; ++i) {
        GuestThread* g = flat_[i];
        if (!g || g->state == S::Finished || !g->native) continue;
        if (shown >= kVivacityMaxThreads || w > (int)sizeof buf - kEntry) { ++skipped; continue; }
        uint32_t blocks = 0;
        cpu_->thread_emu_blocks(static_cast<NT*>(g->native)->emu, &blocks);  // relevé racy assumé
        // État à CÔTÉ du compteur : R=Running, B=Blocked, P=Ready (prêt),
        // N=New. Deux caractères, et « aucun compteur ne bouge » cesse d'être
        // ambigu entre attente saine et blocage pathologique.
        char st = g->state == S::Running ? 'R' : g->state == S::Blocked ? 'B'
                : g->state == S::Ready   ? 'P' : 'N';
        w += std::snprintf(buf + w, sizeof buf - (size_t)w, "%s%u:%c:%u",
                           shown ? "," : "", g->id, st, (unsigned)blocks);
        ++shown;
    }
    if (!shown) return 0;                       // aucun fil vivant : rien à dire
    if (skipped && w <= (int)sizeof buf - kEntry)
        w += std::snprintf(buf + w, sizeof buf - (size_t)w, "+%u", skipped);
    int r = std::snprintf(out, n, " run=%s", buf);
    if (r < 0) { out[0] = 0; return 0; }
    return (unsigned)r >= n ? (int)(n - 1) : r; // snprintf a tronqué ET terminé : pas de reliquat
}

// ---- Recensement des blocs PAR FIL (sched_native.h) ------------------------
//
// Appelé au démontage, fil main, aucun runner en vol : lecture sans verrou
// assumée et sans course réelle.
//
// TROIS PRÉCAUTIONS, parce que ce chiffre décide d'un chantier :
//
//  1. TOUS les fils, Finished compris. vivacity_field les saute (il répond à
//     « qui avance maintenant ») ; les omettre ICI ferait disparaître du total
//     un fil qui aurait tout fait puis serait sorti — la mesure mentirait par
//     omission exactement là où elle sert à décider.
//  2. Le main compte comme les autres. Il tourne sur l'emu de BASE
//     (`nt->emu == nullptr`), et thread_emu_blocks(nullptr) rend précisément
//     ce compteur-là. L'oublier attribuerait 0 % au fil qui, sous coop, fait
//     tout.
//  3. Le total est celui des fils LISTÉS, jamais un total séparé : un
//     pourcentage dont le dénominateur inclut des fils absents de la liste
//     serait invérifiable à la lecture.
int NativeScheduler::blocks_census(char* out, unsigned n) {
    if (!cpu_ || !out || n < 16) return 0;
    uint32_t probe = 0;
    if (!cpu_->thread_emu_blocks(nullptr, &probe)) return 0;   // backend sans compteur

    struct Ent { uint32_t id; uint32_t blocks; };
    Ent e[kFlatMax + 1]; unsigned ne = 0;
    unsigned long long total = 0;
    for (auto& up : threads_) {
        GuestThread* g = up.get();
        if (!g || ne >= kFlatMax + 1) break;
        // Un fil sans extension native n'a jamais couru sous ce backend : il
        // n'a pas de compteur, et lui en inventer un à 0 le ferait passer pour
        // un fil oisif alors qu'il est hors sujet.
        if (!g->native) continue;
        uint32_t b = 0;
        cpu_->thread_emu_blocks(static_cast<NT*>(g->native)->emu, &b);
        e[ne].id = g->id; e[ne].blocks = b; ++ne;
        total += b;
    }
    if (!ne || !total) return 0;

    // Tri décroissant (insertion : ne <= 129, au démontage).
    for (unsigned i = 1; i < ne; ++i) {
        Ent k = e[i]; int j = (int)i - 1;
        while (j >= 0 && e[j].blocks < k.blocks) { e[j + 1] = e[j]; --j; }
        e[j + 1] = k;
    }

    // « max= » d'abord : c'est le chiffre de décision, il ne doit pas pouvoir
    // être tronqué par la liste qui le suit.
    unsigned pct = (unsigned)((e[0].blocks * 200ull + total) / (2ull * total));
    int w = std::snprintf(out, n, "blocs/fil: total=%llu fils=%u max=%u%% (fil %u)",
                          total, ne, pct, e[0].id);
    if (w < 0 || (unsigned)w >= n) { out[n - 1] = 0; return (int)(n - 1); }
    for (unsigned i = 0; i < ne && i < 12; ++i) {
        int r = std::snprintf(out + w, n - (unsigned)w, "%s%u:%u",
                              i ? "," : " | ", e[i].id, e[i].blocks);
        if (r < 0 || (unsigned)(w + r) >= n) { out[w] = 0; break; }   // jamais d'entrée coupée
        w += r;
    }
    return w;
}

// ---- thread creation --------------------------------------------------------

GuestThread* NativeScheduler::set_main(uint32_t entry, uint32_t stack_top, uint32_t main_tib) {
    gil::Guard g;
    auto t = std::make_unique<GuestThread>();
    t->id = next_id_++;
    t->stack_top = stack_top; t->stack_base = stack_top - 0x200000;
    t->tib = main_tib; t->ctx.fs_base = main_tib;   // fs-direct backend only (asserted at selection)
    t->entry = entry; t->ctx.eip = entry; t->started = false; t->state = S::Ready;
    NT* n = new NT(); pthread_cond_init(&n->cv, nullptr);
    n->is_main = true; n->emu = nullptr;            // main runs on the BASE emu
    t->native = n;
    auto* p = t.get(); threads_.push_back(std::move(t));
    uint32_t fn = flat_n_.load(std::memory_order_relaxed);
    if (fn < kFlatMax) { flat_[fn] = p; flat_n_.store(fn + 1, std::memory_order_release); }
    main_ = p;
    return p;
}

GuestThread* NativeScheduler::create_thread(uint32_t entry, uint32_t param,
                                            uint32_t stack_size, bool suspended) {
    // Caller = a shim => GIL already held. Guest stack/TIB carving mirrors the
    // cooperative allocator (recycling deliberately NOT ported in etape 1: the
    // native map is append-only for the lock-free flat_ reader; revisit if a
    // long online session exhausts the region — the coop warning is kept).
    gil::assert_held();
    if (shutdown_) return nullptr;          // teardown race (K4): no new runners
    if ((uint32_t)threads_.size() >= thread_cap_) {
        std::fprintf(stderr, "[sched-native] thread cap %u reached — CreateThread fails cleanly\n", thread_cap_);
        return nullptr;
    }
    auto t = std::make_unique<GuestThread>();
    t->id = next_id_++;
    uint32_t ssize = stack_size ? ((stack_size + 0xFFFF) & ~0xFFFFu) : stack_each_;
    uint32_t sbase = stack_next_; stack_next_ += ssize + 0x10000;
    if (tib_region_ && stack_next_ > tib_region_ && !stack_overrun_warned_) {
        stack_overrun_warned_ = true;       // one-shot: no boot_progress spam
        progress("SCHED-NATIVE: worker stacks reached the TIB region (overrun risk)");
    }
    cpu_->map(sbase, ssize, nullptr, P_RW);
    t->stack_base = sbase; t->stack_top = sbase + ssize;
    uint32_t tib = tib_next_; tib_next_ += 0x1000;
    cpu_->map(tib, 0x1000, nullptr, P_RW);
    cpu_->write_u32(tib + 0x00, 0xFFFFFFFF);        // ExceptionList (end of SEH chain)
    cpu_->write_u32(tib + 0x04, t->stack_top);      // StackBase
    cpu_->write_u32(tib + 0x08, t->stack_base);     // StackLimit
    cpu_->write_u32(tib + 0x18, tib);               // fs-direct: Self = own address
    // TIB+0x24 (ClientId.UniqueThread) DELIBERATELY zero — Storm SMem pool
    // sizing regression, see sched_cooperative.cpp for the full story.
    t->tib = tib; t->ctx.fs_base = tib;
    t->entry = entry; t->param = param; t->ctx.eip = entry;
    t->started = false; t->state = suspended ? S::New : S::Ready;
    NT* n = new NT(); pthread_cond_init(&n->cv, nullptr);
    n->emu = cpu_->thread_emu_create();
    t->native = n;
    auto* p = t.get(); threads_.push_back(std::move(t));
    uint32_t fn = flat_n_.load(std::memory_order_relaxed);
    if (fn < kFlatMax) { flat_[fn] = p; flat_n_.store(fn + 1, std::memory_order_release); }
    pthread_attr_t at; pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256 * 1024);     // shim bodies use std::string/printf
    int rc = pthread_create(&n->pth, &at, runner_tramp, p);
    pthread_attr_destroy(&at);
    if (rc != 0) {
        std::fprintf(stderr, "[sched-native] pthread_create FAILED (rc=%d)\n", rc);
        p->state = S::Finished; return nullptr;
    }
    return p;
}

void NativeScheduler::resume(GuestThread* t) {
    // Caller = shim => GIL held.
    gil::assert_held();
    if (t && t->state == S::New) { t->state = S::Ready; pthread_cond_signal(&nt(t)->cv); }
}

// TLS current-thread pointer: set by run() (main) and runner() (workers).
static __thread GuestThread* t_cur = nullptr;
GuestThread* NativeScheduler::current() { return t_cur; }

// D2_YIELD : ce que fait Sleep(0)/yield() sous l'ordonnanceur natif.
//   0 (defaut) = sched_yield() de la pte Vita, dont le PLANCHER est ~1 ms.
//       Mesure console 06/09 : D2 appelle Sleep(0) apres CHAQUE paquet
//       serveur->client (0x44c711 / 0x44c740, ~2,1 par image) -> 2,6 ms par
//       image perdus dans « autre » (65 % du poste), 57 us sous qemu.
//   1 = sceKernelDelayThread(D2_YIELD_US, defaut 10) : cede le coeur a un pret
//       d'egale priorite sans le plancher de la pte.
//   3 = attente active bornee de D2_YIELD_US us, GIL relache, sans appel noyau.
//   2 = ne cede que le GIL (aucun appel noyau) : les fils invites sont
//       preemptifs sur Vita, un Sleep(0) qui rend la main tout de suite est
//       une semantique Windows valide.
static uint64_t now_us_mono() {
#ifdef __vita__
    return (uint64_t)sceKernelGetSystemTimeWide();
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
#endif
}
static int yield_mode() {
    static int m = -1;
    if (m < 0) { const char* e = getenv("WX86_YIELD"); if (!e) e = getenv("D2_YIELD"); m = (e && *e) ? atoi(e) : 0; if (m < 0 || m > 3) m = 0; }
    return m;
}
static unsigned yield_us() {
    static int us = -1;
    if (us < 0) { const char* e = getenv("WX86_YIELD_US"); if (!e) e = getenv("D2_YIELD_US"); us = (e && *e) ? atoi(e) : 10; if (us < 0) us = 10; }
    return (unsigned)us;
}
void NativeScheduler::yield() {
    // OS preemption makes cooperative yields advisory. Still honor the intent
    // (spin-break, SwitchToThread): give siblings the core for a beat.
    gil::Release r;
    switch (yield_mode()) {
#ifdef __vita__
      case 1: sceKernelDelayThread(yield_us()); break;
#endif
      case 2: break;
      case 3: {
        // MODE 3 (06/09) : attente ACTIVE bornee, GIL relache, AUCUN appel
        // noyau. Mesure console : sceKernelDelayThread(10 us) coute ~0,55 ms
        // (l'appel et la reelection, pas la duree) ; la boucle de jeu appelle
        // Sleep(0) 14 a 18 fois par image -> 8,8-10,7 ms perdus. Le mode 2
        // (retour immediat) supprimait ce cout mais aussi le STIMULATEUR : la
        // boucle passait de 7-9 a 9-32 tours par image et coutait plus cher.
        // Ici on laisse passer un temps REEL mais minuscule : la cadence est
        // conservee, le prix divise par ~25. Les autres fils invites sont de
        // vrais fils preemptifs : ils tournent pendant l'attente.
        const uint64_t t0 = now_us_mono();
        const uint64_t us = (uint64_t)yield_us();
        while (now_us_mono() - t0 < us) { __asm__ __volatile__("" ::: "memory"); }
        break; }
      default: sched_yield(); break;
    }
}

// ---- wait / notify (the heart; GIL held throughout except inside cond_wait) -

uint32_t NativeScheduler::wait_common(Waitable* w, uint32_t timeout_ms, uint32_t eax_on_timeout) {
    gil::assert_held();
    GuestThread* t = t_cur;
    // K1: TerminateThread landed while this thread was computing (its shim
    // marked us Finished from OUTSIDE): never block again — post a redirect so
    // this shim's trap tail sends the thread to the sentinel; finish_thread's
    // already-Finished guard then preserves TerminateThread's exit code.
    // Native kills at the NEXT WAIT (coop killed at the next slice — one notch
    // coarser, honest, accepted etape 1: a terminated compute-bound thread
    // runs until its next wait). K1(c): a thread terminated while BLOCKED
    // keeps its host pthread parked on its cv until shutdown (nothing
    // re-signals it — wake_check_all only walks Blocked threads). Bounded
    // leak, one 256 KiB host stack per kill — honest, etape 1.
    // Terminated-while-Running is the case handled below.
    if (t && t->state == S::Finished) { br_->redirect_next(br_->sentinel()); return 0; }
    if (!w) return 0;
    NT* n = nt(t);
    if (w->try_acquire(t)) return w->result();
    // ⚡ 08/09 : UN DELAI DE ZERO EST UN TEST, PAS UNE ATTENTE.
    // Windows : WaitForSingleObject(h, 0) non signale rend WAIT_TIMEOUT
    // INSTANTANEMENT. Nous partions dormir >= 1 ms (voir « ms = timeout_ms ?
    // timeout_ms : 1 » plus bas), regle RECOPIEE du coopératif ou elle est
    // necessaire : sous horloge virtuelle le temps n'avance que par les
    // echeances, et un sondage qui rend la main aussitot bloquerait l'horloge.
    // Le natif, lui, REFUSE D2_VIRTCLOCK au demarrage (rt_boot.cpp:5508,
    // « le natif est real-clock par construction ») : la regle n'y sert donc
    // a RIEN et coute plein pot.
    //
    // CE QUE CA COUTAIT, mesure console (Game+0xa83c sonde avec delai 0) :
    //     fenetre  cadence  sondages  signales  qui DORMENT  temps perdu
    //       208 s     25,4       270        19          251     ~0,25 s
    //       188 s     18,1      1122        79         1043     ~1,0 s
    //       178 s     14,4      1886        77         1809     ~1,8 s / 10 s
    // Soit 18 % de la fenetre passee a dormir sur des tests qui devraient etre
    // instantanes. D2_WAITPROF le disait deja sans que je le lise : « pire
    // 0ms->2ms ». Et c'est AUTO-AMPLIFIANT — plus c'est lent, plus d'objets
    // sont en attente, plus on sonde, plus on dort.
    //
    // D2_POLL0=0 restaure l'ancien comportement (A/B).
    if (timeout_ms == 0) {
        static int poll0 = -1;
        if (poll0 < 0) { const char* e = getenv("WX86_POLL0"); if (!e) e = getenv("D2_POLL0"); poll0 = (e && *e == '0') ? 0 : 1; }
        if (poll0) { ++d2rt_poll0_n; return eax_on_timeout; }
    }
    // Honest blocked-context snapshot for GetThreadContext/dumps (spec D7).
    cpu_->save_context(t->ctx);
    t->wait_obj = w;
    t->state = S::Blocked;
    n->woken = false;
    bool inf = (timeout_ms == 0xFFFFFFFFu);
    timespec abs;
    if (!inf) {
        // timeout 0 => 1 ms, replicating the cooperative deadline rule
        // (sched_cooperative.cpp wait(): virt_ms_ + (timeout ? timeout : 1)).
        uint32_t ms = timeout_ms ? timeout_ms : 1;
        // CLOCK_REALTIME because pthread_cond_timedwait measures the absolute
        // deadline against it — VERIFIED on both targets (T12, disassembly
        // audit of the VitaSDK pte binary, 2026-08-29; details in
        // docs/audit/t12_schedprobe_natif_vita.md):
        //   glibc (qemu): REALTIME base by default, re-evaluated on every
        //     wakeup — a clock step shifts pending timeouts;
        //   pte (Vita): cond_timedwait -> sem_timedwait -> pte_relmillisecs
        //     = abstime - ftime(), and ftime = clock_gettime(CLOCK_REALTIME);
        //     the RELATIVE delta then goes into sceKernelWaitSema (usec). The
        //     abs->rel conversion happens on EVERY (re)entry into the wait,
        //     so a clock step only skews conversions made after it, never
        //     waits already in flight.
        // TRAP — pthread_condattr_setclock(CLOCK_MONOTONIC): pte ACCEPTS it
        // (returns 0, stores the id in the attr) but the wait path NEVER
        // reads that attribute back — the call would be a silent no-op that
        // creates exactly the base/measurement mismatch it claims to avoid.
        // Do not use it; the shared REALTIME base on both sides is the only
        // coherent configuration here.
        clock_gettime(CLOCK_REALTIME, &abs);
        abs.tv_sec  += ms / 1000;
        abs.tv_nsec += (long)(ms % 1000) * 1000000L;
        if (abs.tv_nsec >= 1000000000L) { abs.tv_sec++; abs.tv_nsec -= 1000000000L; }
        // Filet famine (§3.2) : échéance publiée pour le détecteur (T3), base
        // now_ms()/MONOTONIC (l'abs REALTIME ci-dessus ne sert qu'au cond).
        // Posée ici sous GIL, effacée sous GIL après le réveil.
        n->deadline_ms = now_ms() + ms;
    }
    // pte lazy-init note (Vita): PTHREAD_MUTEX_INITIALIZER is a sentinel that
    // pthread_mutex_lock materializes on FIRST lock(); pthread_cond_wait on a
    // never-locked mutex would touch the un-materialized sentinel. Safe here
    // by construction: the ctor enable()d the GIL and run()/runner() lock()
    // it before any wait can execute — a lock() always precedes the first
    // cond_wait on this mutex.
    int rc = 0;
    gil::mark_released();               // observabilité (§19) : le cond rend le mutex ici
    while (!n->woken && !shutdown_) {
        rc = inf ? pthread_cond_wait(&n->cv, gil::mutex())
                 : pthread_cond_timedwait(&n->cv, gil::mutex(), &abs);
        if (rc == ETIMEDOUT && !n->woken) break;
    }
    if (d2rt_wakeprof && n->t_signal_us) {
        const uint64_t dt = wx86_now_us() - n->t_signal_us;
        n->t_signal_us = 0;
        ++d2rt_wake_n; d2rt_wake_us += dt;
        if (dt > d2rt_wake_worst) d2rt_wake_worst = dt;
    }
    // The cv released/reacquired the GIL mutex behind lock()/unlock()'s back:
    // re-stamp ownership so a later assert_held() in this shim body holds.
    gil::mark_owned();
    n->deadline_ms = 0;                 // filet famine : plus d'attente temporisée en cours
    t->wait_obj = nullptr;
    // K1: guarded transition (coop idiom, sched_cooperative.cpp:296) — never
    // clobber a Finished posted from OUTSIDE (TerminateThread) while blocked.
    if (t->state == S::Blocked) t->state = S::Running;
    if (n->woken) { wake_signal_++; wakes_total_++; d2rt_sw_seq++; return n->delivered_eax; }
    wake_timeout_++; wakes_total_++; d2rt_sw_seq++;
    return eax_on_timeout;      // shutdown_ also lands here: shim returns, next
                                // trap dispatch sees shutdown and stops the run
}

uint32_t NativeScheduler::wait(Waitable* w, uint32_t timeout_ms) {
    return wait_common(w, timeout_ms, 0x102);      // WAIT_TIMEOUT
}
void NativeScheduler::wait_noresult(Waitable* w, uint32_t timeout_ms) {
    (void)wait_common(w, timeout_ms, 0x102);       // shim supplies its own EAX
}
uint32_t NativeScheduler::wait_timeout_result(Waitable* w, uint32_t timeout_ms,
                                              uint32_t eax_on_timeout) {
    return wait_common(w, timeout_ms, eax_on_timeout);
}

void NativeScheduler::wake_check_all() {
    // GIL held. EXACT mirror of the cooperative pick_ready() wake loop:
    // id-creation order, try_acquire ON BEHALF of the waiter (KIocp writes the
    // waiter's guest out-params in there), deliver result(), signal.
    // K3 triage: native wakes on EACH notify; coop coalesced multiple
    // SetEvents per slice into one wake — a wake-count A/B diff between the
    // two backends is design, not a bug (D4).
    gil::assert_held();
    for (auto& up : threads_) {
        GuestThread* t = up.get();
        if (t->state != S::Blocked || !t->wait_obj) continue;
        Waitable* w = static_cast<Waitable*>(t->wait_obj);
        if (w->try_acquire(t)) {
            NT* n = nt(t);
            n->delivered_eax = w->result();
            n->woken = true;
            t->wait_obj = nullptr;      // consumed; wait_common finishes state
            // ⚡ 08/09 : LATENCE PURE D'ORDONNANCEMENT. « signalement -> vu »
            // mesure 171 us quand ca tourne et jusqu'a 282 ms quand ca rame,
            // mais ce chiffre melange NOTRE reveil et la cadence de sondage du
            // jeu. Ici on isole la moitie qui nous appartient : de
            // pthread_cond_signal() a la reprise EFFECTIVE du fil reveille
            // (qui doit reprendre le GIL). Aucune ambiguite d'interpretation.
            if (d2rt_wakeprof) { n->t_signal_us = wx86_now_us(); ++d2rt_wake_sig; }
            pthread_cond_signal(&n->cv);
        }
    }
}

void NativeScheduler::notify(Waitable*) {
    gil::assert_held();
    wake_check_all();
}

// ---- runner / lifecycle -----------------------------------------------------

void NativeScheduler::seed_first_run(GuestThread* t) {
    // GIL held; current host thread's emu already bound. Mirrors the
    // cooperative run_slice() first-run seeding byte for byte.
    cpu_->set_fs_base(t->tib);
    uint32_t esp = t->stack_top;
    esp -= 4; cpu_->write_u32(esp, t->param);        // arg to thread proc
    esp -= 4; cpu_->write_u32(esp, br_->sentinel()); // return -> thread exit
    cpu_->set_reg(R_ESP, esp);
    cpu_->set_reg(R_EIP, t->entry);
    // K2: power-on EFLAGS. A fresh thread emu is memset-zeroed (EFLAGS=0);
    // coop loaded 0x202 via load_context (the X86Context default).
    // set_reg(R_EFLAGS) also resets df=d_none — verified in cpu_box86.
    cpu_->set_reg(R_EFLAGS, 0x202);
    t->started = true;
}

void NativeScheduler::finish_thread(GuestThread* t, bool ok, const char* fault) {
    // GIL held. (The SEH dispatcher call below therefore runs GIL-held — that
    // is correct: it reads guest memory via shim-grade helpers.)
    // K1(b): TerminateThread already marked this thread Finished from OUTSIDE
    // and set its exit code — touch NEITHER state nor exit_code, just wake
    // joiners. A fault surfacing here was raced by the terminate: name it in
    // ONE line (no full dump, no state touch) so the log never hides it.
    if (t->state == S::Finished) {
        if (!ok)
            std::printf("  [sched-native] fault SUPPRESSED after external terminate: thr %u eip=%08x addr=%08x\n",
                        t->id, cpu_->reg(R_EIP), cpu_->fault_addr());
        wake_check_all(); return;
    }
    if (!ok) {
        uint32_t code = cpu_->fault_code();
        int act = (code && fault_disp_) ? fault_disp_(t, code, cpu_->fault_addr()) : 0;
        if (act == 1) {                  // resume (reserved) — dead path etape 1
            std::printf("  [sched-native] SEH resume (act=1): experimental, NOT supported under native — thread leaks\n");
            return;
        }
        t->exit_code = 0xC0000005; stop_reason_ = fault ? fault : "fault";
        std::printf("  [sched-native] thread %u FAULT: %s EIP=0x%08x faultAddr=0x%08x ESP=0x%08x EAX=0x%08x\n",
                    t->id, stop_reason_, cpu_->reg(R_EIP), cpu_->fault_addr(),
                    cpu_->reg(R_ESP), cpu_->reg(R_EAX));
        dyn86_dump_xfer();
        // CHAINE DE PILE : on releve les mots de la pile qui pointent DANS un
        // module charge — l'amorce d'une trace d'appels quand le cadre est
        // perdu. Jusqu'au 2026-09-12 les trois plages etaient ecrites en dur
        // (0x400000-0xA00000, 0x1900000-0x2100000, 0x30000000-0x31000000) :
        // c'etait le plan memoire du PREMIER consommateur, et sur un portage
        // dont les modules vivent ailleurs le dump sortait VIDE — un
        // diagnostic qui se tait au moment ou on en a le plus besoin. Le pont
        // connait la base et la taille de chaque module charge : on lui
        // demande, et le dump dit lequel.
        { uint32_t esp = cpu_->reg(R_ESP);
          const std::vector<std::pair<uint32_t,uint32_t>> mods =
              br_ ? br_->loaded_modules() : std::vector<std::pair<uint32_t,uint32_t>>();
          for (uint32_t off = 0; off < 0x100; off += 4) {
              uint32_t v = cpu_->read_u32(esp + off);
              for (size_t k = 0; k < mods.size(); ++k)
                  if (v >= mods[k].first && v - mods[k].first < mods[k].second) {
                      std::printf("      [esp+0x%02x] 0x%08x  (module @%08x +0x%x)\n",
                                  off, v, mods[k].first, (unsigned)(v - mods[k].first));
                      break; } } }
        char m[128]; std::snprintf(m, sizeof m, "NATIVE FAULT thr %u eip=%08x addr=%08x",
                                   t->id, cpu_->reg(R_EIP), cpu_->fault_addr());
        progress(m);
    } else {
        // A GENUINE thread exit stopped AT the Bridge sentinel (trap_handler
        // returned false there): EAX is the thread proc's return value. A
        // clean stop anywhere else only reaches here during shutdown teardown
        // (async quit=1 broadcast, no break marker) — EAX is then
        // mid-computation garbage: report exit_code 0, don't pretend the
        // thread returned.
        bool genuine = cpu_->reg(R_EIP) == br_->sentinel();
        t->exit_code = genuine ? cpu_->reg(R_EAX) : 0;
    }
    cpu_->save_context(t->ctx);                      // final honest snapshot
    t->state = S::Finished;
    wake_check_all();                                // KThread joiners
}

// ---- coeur2 : knobs (lus UNE fois) et epinglage par le fil lui-meme ---------
// WX86_COEUR_SERVEUR=<c> : 0/absent = OFF (topologie d'avant a l'octet pres) ;
// 1..3 = le fil serveur sur USER_c (3 = 4e coeur, CapUnlocker). Les autres
// fils invites restent sur USER_0. Identification : WX86_COEUR_SERVEUR_ID=<id>
// (prioritaire), sinon l'ADRESSE D'ENTREE invitee que le consommateur a donnee
// par set_server_thread_entry() — le moteur ne resout aucun nom de module et
// n'a donc plus de knob d'adresse RELATIVE : une RVA n'a de sens que rapportee
// a un module, et c'est le portage qui sait lequel. Sous qemu :
// WX86_QEMU_MONOCOEUR=<cpu> emule la topologie console par sched_setaffinity
// (main + runners sur <cpu>, serveur sur <cpu>+c) — sans lui, qemu garde ses
// fils libres sur tous les coeurs hote (c'est deja le regime des portes et
// bancs natifs qemu depuis l'etape 1).
static int coeur2_core() {
    static int c = -1;
    if (c < 0) { const char* e = getenv("WX86_COEUR_SERVEUR"); if (!e) e = getenv("D2_COEUR_SERVEUR"); c = (e && *e) ? atoi(e) : 0; if (c < 0 || c > 3) c = 0; }
    return c;
}
static uint32_t coeur2_id() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("WX86_COEUR_SERVEUR_ID"); if (!e) e = getenv("D2_COEUR_SERVEUR_ID"); v = (e && *e) ? atoi(e) : 0; if (v < 0) v = 0; }
    return (uint32_t)v;
}
static int qemu_monocoeur() {           // -1 = knob absent (aucune affinite hote)
    static int v = -2;
    if (v < -1) { const char* e = getenv("WX86_QEMU_MONOCOEUR"); if (!e) e = getenv("D2_QEMU_MONOCOEUR"); v = (e && *e) ? atoi(e) : -1; if (v < -1) v = -1; }
    return v;
}

bool NativeScheduler::is_server_thread(GuestThread* t) {
    if (!t) return false;
    if (const uint32_t id = coeur2_id()) return t->id == id;
    // server_entry_ est une adresse invitee ABSOLUE, posee par le consommateur.
    // 0 = il n'en a pose aucune : personne n'est le serveur, et la topologie
    // reste celle d'avant. Le moteur ne cherche pas a deviner.
    return server_entry_ && t->entry == server_entry_;
}

void NativeScheduler::pin_runner(GuestThread* t, bool is_main) {
    const int c = coeur2_core();
    const bool srv = !is_main && c > 0 && is_server_thread(t);
#ifdef __vita__
    SceUID self = sceKernelGetThreadId();
    if (!is_main) nt(t)->sce_uid = (int32_t)self;   // filet famine : diag + repli P4 (spec §5 T2)
    // Etape 1 topology (spec D6): every guest runner pinned on USER_0, priority
    // = present/watchdog (0x10000100), set from inside the thread itself.
    // KERNEL VERDICT for this topology (T12 console probe, 2026-08-29):
    // RUN-TO-BLOCK — the Vita kernel does NOT timeslice equal-priority
    // threads on one core; the first runner keeps the core until it blocks.
    // Anti-starvation nets are therefore REQUIRED before any online native
    // run (solo stays testable: D2 yields through its imports). Numbers and
    // method: docs/audit/t12_schedprobe_natif_vita.md.
    // coeur2 : le fil serveur (et lui seul) part sur USER_c ; SCE_KERNEL_CPU_MASK_USER_n = 0x10000 << n.
    const int mask = srv ? (int)(0x10000u << c) : (int)SCE_KERNEL_CPU_MASK_USER_0;
    const int rc = sceKernelChangeThreadCpuAffinityMask(self, mask);
    if (!is_main) sceKernelChangeThreadPriority(self, 0x10000100);
    else
        // Le main invite est enregistre dans la carte des coeurs comme TEMOIN de
        // USER_0 : la ligne « coeurs: » a besoin d'au moins un fil dont on sait ou
        // il doit etre pour que les autres lignes se lisent. Les runners ne sont
        // pas enregistres — ils naissent et meurent, le registre est fixe.
        wx86_vita_core_register("invite-main", (int)self, (unsigned)SCE_KERNEL_CPU_MASK_USER_0, rc);
    if (srv) {
        // Le serveur EST enregistre : la ligne coeurs: publiera son d= (dernier
        // coeur execute), la seule preuve qu'il COURT ailleurs (masque relu = 0
        // sur ce firmware, t12 §10.1).
        wx86_vita_core_register("serveur", (int)self, (unsigned)mask, rc);
        char m[160];
        std::snprintf(m, sizeof m, "coeur2: fil %u (entree=%08x) epingle sur USER_%d masque=0x%x rc=0x%08x — fil serveur sur son coeur",
                      t->id, t->entry, c, (unsigned)mask, (unsigned)rc);
        progress(m); std::printf("  %s\n", m);
    }
    // VM domain PAR FIL (cause racine des crashes rtc+3s de la session
    // console 3, relecture des dumps — commentaire complet dans
    // mman_vita.c) : ce fil va TRADUIRE, donc ECRIRE l'arene JIT ; sans
    // son propre OpenVMDomain (DACR = contexte de fil), son premier
    // allocBlock est un Data abort deterministe. La permission a pour portee
    // le FIL, pas le coeur (t12 §16, phase H) : elle suit le fil sur USER_c.
    dyn86_vita_open_vm_thread();
#else
    // qemu / hote : emulation de la topologie console sur demande seulement.
    const int base = qemu_monocoeur();
    if (base >= 0) {
        const int cpu = srv ? base + c : base;
        cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu, &set);
        const int rc = sched_setaffinity(0, sizeof set, &set);
        char m[160];
        std::snprintf(m, sizeof m, "coeur2: fil %u (entree=%08x) affinite hote cpu=%d rc=%d%s%s",
                      t->id, t->entry, cpu, rc, rc ? " (ECHEC : fil libre)" : "",
                      srv ? " — fil serveur sur son coeur" : "");
        std::printf("  %s\n", m); std::fflush(stdout);
    } else if (srv) {
        std::printf("  coeur2: fil %u (entree=%08x) reconnu SERVEUR — sans WX86_QEMU_MONOCOEUR aucune affinite hote (qemu : fils deja libres)\n",
                    t->id, t->entry);
    }
#endif
}

void* NativeScheduler::runner_tramp(void* p) {
    // The one pthread start argument carries the GuestThread*; the scheduler
    // singleton is reached through g_native_sched (sched_native.h — assigned
    // by make_scheduler before any create_thread).
    GuestThread* t = static_cast<GuestThread*>(p);
    g_native_sched->runner(t);
    return nullptr;
}

void NativeScheduler::runner(GuestThread* t) {
    // Epinglage + priorite + VM domain, PAR LE FIL LUI-MEME (coeur2 : le fil
    // serveur peut partir sur un autre coeur — pin_runner, knob D2_COEUR_SERVEUR).
    pin_runner(t, false);
    NT* n = nt(t);
    cpu_->thread_emu_bind(n->emu);
    t_cur = t;
    // Observabilité GIL (§19) : ce fil déclare l'intervalle de SA pile hôte,
    // AVANT sa première prise du GIL. C'est ce qui remplace le pthread_self()
    // que lock() payait à chaque trap (gil.h). `ici` n'est jamais lu — seule
    // son adresse compte, et elle est à quelques centaines d'octets du sommet
    // (deux trames : runner_tramp puis runner).
    int gslot;
    { char ici; uintptr_t hi = (uintptr_t)&ici;
      gslot = gil::register_stack(hi - kRunnerStackDeclared, hi, t->id); }
    // Désenregistrement à la SORTIE, par le fil lui-même et après sa dernière
    // prise du GIL : c'est la seconde moitié de l'invariant de gil.h. Sans lui,
    // un fil non enregistré qui tournerait plus tard sur cette pile recyclée
    // hériterait de l'étiquette du mort — le seul nom faux que le mécanisme
    // puisse produire. RAII pour couvrir les DEUX sorties de runner().
    struct Unreg { int s; ~Unreg() { gil::unregister_stack(s); } } unreg{gslot};
    gil::lock();
    if (t->state == S::New && !shutdown_) gil::mark_released();   // observabilité (§19)
    while (t->state == S::New && !shutdown_)         // CREATE_SUSPENDED
        pthread_cond_wait(&n->cv, gil::mutex());
    gil::mark_owned();                               // see wait_common
    if (shutdown_) { t->state = S::Finished; gil::unlock(); return; }
    seed_first_run(t);
    t->state = S::Running;
    run_guest(t);
    gil::unlock();
}

void NativeScheduler::run_guest(GuestThread* t) {
    // GIL held on entry. Translated code must run WITHOUT the GIL: release
    // around cpu_->run() — the trap dispatch re-takes it per shim (gil::Guard
    // in cpu_box86.cpp). No cooperative yields on this backend: waits block
    // inside the shim.
    uint32_t eip = cpu_->reg(R_EIP);
    const char* fault = nullptr;
    bool ok;
    {
        gil::Release r;
        for (;;) {
#ifdef PROF_COUNTERS
            // Comptabilite du temps invite, MIROIR de celle du coop
            // (sched_cooperative.cpp:272). Sans elle prof::run_ns reste a zero
            // sous D2SCHED=native et la ligne « budget: » du profil d'image
            // annonce dynarec=0.0 en versant tout dans « hors-run » — mesure du
            // 2026-09-04 sur console. run_ns est INCLUSIF (code traduit + corps
            // de shims executes dans la tranche), donc dynarec pur = run - trap.
            const uint64_t prof_t0 = d2rt::prof::now_ns();
#endif
            // ⚡ 08/09 : ALIMENTER dyn86_jp_run_us AUSSI SOUS L'ORDONNANCEUR
            // NATIF. Il n'etait incremente que dans sched_cooperative.cpp, et
            // en plus derriere le knob dyn86_jitprof. Sous D2SCHED=native — ce
            // que le jeu utilise — il valait donc TOUJOURS 0, et le profil
            // d'image affichait « run=0ms » sur CHAQUE image, y compris une a
            // 1241 ms. J'en avais conclu que le fil de jeu etait ARRETE ; il
            // executait en fait du code traduit, ce que le dump montrait deja
            // (PC du fil principal dans la region JIT). Un compteur mort ne se
            // lit pas « zero », il ne se lit PAS.
            const uint64_t run_t0 = dyn86_jp_now_us();
            ok = cpu_->run(eip, &fault);
            dyn86_jp_run_us += dyn86_jp_now_us() - run_t0;
#ifdef PROF_COUNTERS
            { const uint64_t dt = d2rt::prof::now_ns() - prof_t0;
              d2rt::prof::run_ns += dt; t->prof_ns += dt; }
#endif
            if (!ok) break;                              // fault
            if (cpu_->take_limit_hit()) {                // stop broadcast landed
                if (shutdown_) break;                    // inchangé, prioritaire
                // Filet anti-famine (§3.3) : un poke du battement (T3) a zéroé
                // le budget de CE fil → une sieste bornée, UNE par époque. On
                // est ici DANS le bloc gil::Release ci-dessus : la sieste
                // bloque hors GIL par construction, I1 intact (le GIL reste
                // prenable par le fil désaffamé). L'expiration SPONTANÉE du
                // budget 0x7FFFFFFF retombe sur époque==fam_ack → reprise
                // immédiate, le « spurious resume » d'aujourd'hui à l'identique.
                uint32_t e = fam_epoch_.load(std::memory_order_relaxed);
                NT* n = nt(t);
                if (fam_on_.load(std::memory_order_relaxed) && e != n->fam_ack) {
                    n->fam_ack = e;
                    fam_note_nap(t, cpu_->reg(R_EIP));       // journal + compteur (§3.6)
                    fam_nap();                               // DelayThread/nanosleep — HORS GIL
                }
                eip = cpu_->reg(R_EIP); continue;        // spurious: resume
            }
            if (br_->take_yield()) {                     // advisory yield (SwitchToThread)
                eip = cpu_->reg(R_EIP); sched_yield(); continue;
            }
            // Clean stop: ONLY a Bridge-sentinel return is a real thread exit
            // (trap_handler returned false at the sentinel slot, EIP == sentinel).
            // An async quit=1 with no marker (another thread's shutdown-grade
            // request_stop) must not "finish" a live thread: resume unless we
            // are actually tearing down.
            if (cpu_->reg(R_EIP) == br_->sentinel()) break;   // genuine finish
            if (shutdown_) break;
            eip = cpu_->reg(R_EIP); continue;
        }
    }
    finish_thread(t, ok, fault);
    t->slices++;
}

// ---- Filet anti-famine : helpers du mécanisme (spec §3.3/§3.6) --------------

// Sieste réelle : le SEUL point d'ordonnancement fiable sous run-to-block
// (T12 §1.3 — bloquer = céder le cœur au prochain prêt ; sonde v3 : SIESTE-
// CEDE confirmé console 2026-08-29). Appeler HORS GIL uniquement.
void NativeScheduler::fam_nap() {
#ifdef __vita__
    sceKernelDelayThread(fam_nap_us_);
#else
    timespec ts{ (time_t)(fam_nap_us_ / 1000000u), (long)(fam_nap_us_ % 1000000u) * 1000L };
    nanosleep(&ts, nullptr);
#endif
}

// Compteur + journal LIMITÉ (§3.6) : 1re sieste journalisée, puis 1 sur
// kFamineLogEvery — une famine longue ne doit pas noyer boot_progress.
// PLUS : la 1re sieste de CHAQUE fil est journalisée (NT::fam_napped, revue
// M2) — sans quoi un fil désaffamé entre deux multiples du modulo global
// resterait invisible. C'est la ligne qui NOMME le spinner (l'EIP de sortie
// de couture ≈ la boucle chaude). Appelé hors GIL par le runner lui-même :
// progress() (fopen append sur Vita) tolère la concurrence (régime watchdog) ;
// stdout en plus pour le banc qemu (progress y est un no-op).
void NativeScheduler::fam_note_nap(GuestThread* t, uint32_t eip) {
    uint32_t nth = fam_naps_.fetch_add(1, std::memory_order_relaxed) + 1;
    NT* n = nt(t);
    bool first_of_thread = !n->fam_napped;
    n->fam_napped = true;
    if (!first_of_thread && (nth % kFamineLogEvery) != 0) return;
    char m[64];
    std::snprintf(m, sizeof m, "famine: sieste thr %u eip=%08x", t->id, eip);
    std::printf("  %s\n", m);
    progress(m);
}

// ---- Battement famine (Tâche 3 : le détecteur qui ARME le mécanisme T2) ----

// Sommeil du battement — même paire d'appels que fam_nap(), mais durée libre :
// le battement dort par tranches <= 50 ms pour rester réactif au teardown
// (son join précède le join borné des runners, §3.5).
static void fam_sleep_us(uint32_t us) {
#ifdef __vita__
    sceKernelDelayThread(us);
#else
    timespec ts{ (time_t)(us / 1000000u), (long)(us % 1000000u) * 1000L };
    nanosleep(&ts, nullptr);
#endif
}

void NativeScheduler::arm_famine(const volatile int* frames, uint32_t window_ms,
                                 uint32_t nap_us) {
    fam_frame_     = frames;
    fam_window_ms_ = window_ms ? window_ms : kFamineWindowMsDefault;
    fam_nap_us_    = nap_us ? nap_us : kFamineNapUsDefault;
    // Armer la couture (T2) : époque encore 0 = déjà acquittée par tous les
    // fils par construction — aucune sieste tant que rien n'est détecté.
    fam_on_.store(true, std::memory_order_relaxed);
}

void* NativeScheduler::fam_tramp(void* self) {
    static_cast<NativeScheduler*>(self)->fam_beat();
    return nullptr;
}

// Boucle du détecteur (§3.2) : toutes les fam_window_ms_, lire wakes_total_ et
// le compteur d'images ; suspicion = les DEUX figés sur la fenêtre. Sur
// suspicion : trylock BRUT du GIL (précédent dump_threads_to) — JAMAIS
// gil::lock() ici, et donc aucun appel à une méthode sous gil::assert_held()
// (thread_by_id, live_thread_ids…) : threads_ est parcouru directement.
void NativeScheduler::fam_beat() {
#ifdef __vita__
    // Auto-rapport de vitalité (§3.5 amendé, audit §10.1/§10.6). Historique :
    // le self-pin USER_1 + relecture du masque (3c7eb43) avait DEUX défauts
    // prouvés sur console — (1) un fil captif dès la naissance n'exécute
    // jamais son épinglage (§10.3), réglé par l'épinglage CRÉATEUR de run()
    // (plan B) ; (2) la jambe « masque RELU » est inopérable :
    // sceKernelGetThreadCpuAffinityMask rend 0 même pour un fil épinglé avec
    // succès sur ce firmware (§10.1) — elle imprimerait une FAUSSE alarme
    // INVARIANT VIOLE à chaque boot. Remplacée par cette ligne : imprimée
    // PAR le battement, elle est la preuve qu'il court (un fil mort n'écrit
    // pas), et son absence est détectable post-mortem (stamp INERTE de
    // rt_boot). La priorité relue est informative — la mesure réelle du
    // régime (0x10000100 résolu), pas un invariant.
    // « USER_2 » ecrit en dur mentirait des que D2_COEURS bouge : le coeur
    // annonce est celui que le knob a REELLEMENT demande.
    { const int fm = wx86_vita_core_mask(2);
      char m[120];
      std::snprintf(m, sizeof m, "famine: battement arme (Sce brut, USER_%d createur [D2_COEURS], prio relue=%d)",
                    fm == SCE_KERNEL_CPU_MASK_USER_0 ? 0
                      : fm == SCE_KERNEL_CPU_MASK_USER_1 ? 1 : 2,
                    sceKernelGetThreadCurrentPriority());
      progress(m); }
#endif
    // Lectures volatiles cross-cœur, déchirure 64 bits tolérée — le régime du
    // heartbeat watchdog (sched_native.h, champ wakes_total_). Une déchirure
    // fait rater UNE fenêtre (valeurs différentes => pas de détection) : le
    // sens sûr.
    const volatile uint64_t* wp = (const volatile uint64_t*)&wakes_total_;
    uint64_t last_w = *wp;
    int      last_f = fam_frame_ ? *fam_frame_ : -1;
    uint64_t next   = now_ms() + fam_window_ms_;
    // Observabilité GIL (§19), état PROPRE à ce fil : le battement relève le
    // GIL à CHAQUE fenêtre (pas seulement sur suspicion), sinon l'ancienneté
    // de la prise démarrerait au premier gel constaté au lieu de la prise
    // elle-même. Deux relevés au même compteur de prises = la même prise.
    uint32_t gil_acq = 0; bool gil_held = false; uint64_t gil_since = 0;
    while (!fam_stop_.load(std::memory_order_relaxed)) {
        uint64_t now = now_ms();
        if (now < next) {
            uint64_t left = next - now;
            fam_sleep_us((uint32_t)((left > 50 ? 50 : left) * 1000));
            continue;
        }
        next = now + fam_window_ms_;
        if (shutdown_) break;               // teardown : plus de détection, le join arrive
        // ---- « fils: » (06/09) : BLOCS TRADUITS PAR FIL INVITE, par fenetre de 10 s.
        // Les chutes de fps en jeu reel montrent le fil principal en attente
        // (WFSO 11-14/img, libre 30-54 ms/pas) pendant que le pas de simulation
        // tombe a 12-15/s : c'est un AUTRE fil invite (le fil serveur du jeu, en
        // processus) qui tient le coeur. Aucun relevé ne le disait : voici le
        // proxy CPU par fil — le delta des blocs executes (thread_emu_blocks),
        // le meme compteur que le champ run= des detections, mais periodique.
        {
            static uint64_t fils_next = 0; static uint32_t prevBlk[16] = {0}; static uint32_t prevId[16] = {0};
            static uint32_t prevTr[16] = {0}, prevCo[16] = {0}, prevWu[16] = {0};
            static bool g_filstat_seen = false;
            if (cpu_ && now >= fils_next) {
                fils_next = now + 10000;
                char line[420]; int w = std::snprintf(line, sizeof line, "fils:");
                { uint32_t a = 0; g_filstat_seen = cpu_->thread_emu_filstat(nullptr, &a, nullptr, nullptr); }
                uint32_t fn = flat_n_.load(std::memory_order_acquire);
                for (uint32_t i = 0; i < fn && i < kFlatMax && i < 16; ++i) {
                    GuestThread* g = flat_[i];
                    if (!g || g->state == S::Finished || !g->native) continue;
                    uint32_t blocks = 0;
                    if (!cpu_->thread_emu_blocks(static_cast<NT*>(g->native)->emu, &blocks)) continue;
                    const uint32_t d = (prevId[i] == g->id) ? blocks - prevBlk[i] : 0;
                    prevBlk[i] = blocks;
                    const char st = g->state == S::Running ? 'R' : g->state == S::Blocked ? 'B' : g->state == S::Ready ? 'P' : 'N';
                    if (w < (int)sizeof line - 32)
                        w += std::snprintf(line + w, sizeof line - (size_t)w, " %u=%u%c", g->id, d / 10, st);
                    // coeur2 (D2_FILSTAT=1) : /t<traps/s>/c<prises contendues/s>/w<ms d'attente GIL par s>
                    // — la densite de traps du fil et ce que lui coute la serialisation des shims.
                    uint32_t tr = 0, co = 0, wu = 0;
                    if (cpu_->thread_emu_filstat(static_cast<NT*>(g->native)->emu, &tr, &co, &wu)) {
                        const uint32_t dt = (prevId[i] == g->id) ? tr - prevTr[i] : 0;
                        const uint32_t dc = (prevId[i] == g->id) ? co - prevCo[i] : 0;
                        const uint32_t dw = (prevId[i] == g->id) ? wu - prevWu[i] : 0;
                        prevTr[i] = tr; prevCo[i] = co; prevWu[i] = wu;
                        if (w < (int)sizeof line - 40)
                            w += std::snprintf(line + w, sizeof line - (size_t)w, "/t%u/c%u/w%u", dt / 10, dc / 10, dw / 10000);
                    }
                    prevId[i] = g->id;
                }
                std::snprintf(line + w, sizeof line - (size_t)w, " (blocs/s par fil%s)", g_filstat_seen ? " ; /t traps/s /c contendues/s /w ms-attente-GIL/s" : "");
                progress(line);
                std::printf("  %s\n", line); std::fflush(stdout);   // qemu : progress y est un no-op
            }
        }
        gil::Probe gp; gil::probe(&gp);     // lecture sans verrou (§19) — ne bloque jamais
        if (!(gp.held && gil_held && gp.acq == gil_acq)) gil_since = now;   // nouvelle prise
        gil_acq = gp.acq; gil_held = gp.held;
        uint64_t w = *wp;
        int      f = fam_frame_ ? *fam_frame_ : -1;
        bool frozen = (w == last_w) && (f == last_f);   // suspicion = les DEUX figés (§3.2)
        last_w = w; last_f = f;
        if (!frozen) continue;
        if (pthread_mutex_trylock(gil::mutex()) != 0) {
            // GIL occupé = un shim progresse (I1, §3.2) — pas de famine
            // TOTALE : sauter la fenêtre est CORRECT.
            //
            // MAIS l'abandon lui-même doit se VOIR. Historiquement cette
            // branche n'incrémentait qu'un compteur, publié seulement dans la
            // ligne « famine: detection » — qui n'est justement JAMAIS émise
            // quand on sort d'ici. Résultat : l'absence de ligne « famine » ne
            // prouvait rien (le filet pouvait renoncer 500 s de suite en
            // silence), le « diagnostic qui ment » que la maison proscrit.
            // Elle publie maintenant, à cadence bornée en TEMPS
            // (kFamineSkipReportMs, justification dans sched_native.h) : le
            // 1er saut tout de suite, puis un résumé au plus toutes les 5 s.
            uint32_t skips = fam_gil_skips_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (skips == 1 || now - fam_skip_last_ms_ >= kFamineSkipReportMs) {
                uint32_t since = skips - fam_skip_last_n_;
                fam_skip_last_ms_ = now; fam_skip_last_n_ = skips;
                char who[24];
                if (!gp.held)          std::snprintf(who, sizeof who, "libre-au-releve");
                else if (gp.owner_tag) std::snprintf(who, sizeof who, "thr %u", gp.owner_tag);
                else                   std::snprintf(who, sizeof who, "un fil hote");
                char line[420];   // + champ run= (jusqu'a 10 runners) : la ligne ne doit pas etre tronquee
                int lw = std::snprintf(line, sizeof line,     // lw : ne PAS masquer `w` (les wakes)
                    // Le titre dit ce qui s'est REELLEMENT passe (le trylock a
                    // echoue), et le detenteur est explicitement date « au
                    // releve » : le releve gil::probe est pris en HAUT de la
                    // fenetre, avant le trylock, donc il peut dire « libre »
                    // alors que le trylock a echoue une poignee de
                    // microsecondes plus tard. L'ancien titre « GIL occupe »
                    // contredisait alors son propre champ (revue M2).
                    "famine: fenetre SAUTEE (trylock GIL echoue) (sautees=%u, +%u depuis le dernier resume) "
                    "detenteur-au-releve=%s acq=%u%s%s — le filet ne desaffame pas pendant ce temps",
                    skips, since, who, gp.acq,
                    gp.in_shim ? " dans=" : "",
                    gp.in_shim ? (gp.shim ? gp.shim : "?") : "");
                // Vivacité par runner sur la MÊME ligne : c'est ce qui tranche
                // famine (un compteur bouge) / interblocage (aucun ne bouge)
                // sur les cibles sans ligne alive: (qemu).
                if (lw > 0 && (unsigned)lw < sizeof line)
                    vivacity_field(line + lw, (unsigned)(sizeof line - lw));
                std::printf("  %s\n", line);
                progress(line);
            }
            // ... SAUF si le détenteur ne bouge plus (§19). « Un shim
            // progresse » est une INTERPRÉTATION : elle tombe dès que la même
            // prise (même compteur acq) dure plus de kGilStuckMs. Le
            // battement le DIT, et ne fait toujours RIEN de plus — aucun
            // réveil, aucun poke, aucune sieste : le remède reste hors de
            // portée par construction, c'est précisément ce qu'il faut
            // mesurer avant de toucher au filet.
            uint64_t hold = gp.held ? (now - gil_since) : 0;
            if (gp.held && hold >= kGilStuckMs) {
                uint32_t nst = fam_gil_stuck_.fetch_add(1, std::memory_order_relaxed) + 1;
                if (nst == 1 || (nst % kFamineLogEvery) == 0) {   // même cadence que les autres flux (§3.6)
                    char who[24];
                    if (gp.owner_tag) std::snprintf(who, sizeof who, "thr %u", gp.owner_tag);
                    else              std::snprintf(who, sizeof who, "un fil hote");
                    char line[240];
                    if (gp.in_shim)
                        std::snprintf(line, sizeof line,
                            "famine: GIL tenu par %s depuis >=%llu ms dans %s "
                            "(trap=%08x acq=%u sautees=%u) — desaffamage impossible",
                            who, (unsigned long long)hold, gp.shim ? gp.shim : "?",
                            gp.shim_va, gp.acq, skips);
                    else
                        std::snprintf(line, sizeof line,
                            "famine: GIL tenu par %s depuis >=%llu ms hors shim "
                            "(acq=%u sautees=%u) — desaffamage impossible",
                            who, (unsigned long long)hold, gp.acq, skips);
                    std::printf("  %s\n", line);
                    progress(line);
                }
            }
            continue;
        }
        // Sous verrou : corroboration — comptage par état + attentes
        // temporisées expirées (NT::deadline_ms, base now_ms()/MONOTONIC,
        // lu ici seulement sous le (try)lock — contrat sched_native.h)...
        uint32_t running = 0, expired = 0;
        uint64_t nowl = now_ms();
        for (auto& up : threads_) {
            GuestThread* g = up.get();
            if (g->state == S::Running) ++running;
            else if (g->state == S::Blocked) {
                NT* n = static_cast<NT*>(g->native);
                if (n->deadline_ms && nowl > n->deadline_ms + kFamineExpiredMarginMs)
                    ++expired;
            }
        }
        // Numéro de détection tiré MAINTENANT : il décide si la ligne sera
        // imprimée, donc si le relevé de vivacité doit être fait du tout (revue
        // M4 — il tournait jusqu'à 50 fois/s au banc 20 ms pour rien).
        uint32_t nth = fam_detections_.fetch_add(1, std::memory_order_relaxed) + 1;
        bool logit = (nth == 1) || (nth % kFamineLogEvery) == 0;
        // Vivacité par runner RELEVÉE MAINTENANT, avant le poke. Le compteur
        // étant devenu monotone (revue I1), le poke ne le fausse plus — mais il
        // REMET LES FILS EN MOUVEMENT : un relevé pris après décrirait le
        // filet, pas le gel. Mesuré ici, imprimé plus bas.
        char viv[224]; viv[0] = 0;
        if (logit) vivacity_field(viv, sizeof viv);
        // ... puis ORDRE CONTRACTUEL (sched_native.h, a1fa90d) : publier
        // l'époque AVANT de poker les budgets — la couture lit l'ÉPOQUE ...
        fam_epoch_.fetch_add(1, std::memory_order_release);
        // ... et poke des SEULS fils Running (§3.2 : les Blocked, le noyau
        // les réveille ; épargner leur emu évite une sieste parasite au
        // réveil). nt->emu nullptr = emu de base (main) — contrat T2.
        for (auto& up : threads_) {
            GuestThread* g = up.get();
            if (g->state != S::Running) continue;
            cpu_->nudge_thread_budget(static_cast<NT*>(g->native)->emu);
        }
        // Flux détection rate-limité kFamineLogEvery (§3.6), indépendant du
        // flux siestes. Ligne formatée sous verrou (peu cher), émise après.
        char line[480];
        if (logit)
            // Le champ run= porte le relevé PRÉ-POKE ci-dessus : au moment
            // précis où le gel est constaté, il dit qui avance encore
            // (famine) ou personne (interblocage).
            std::snprintf(line, sizeof line,
                "famine: detection #%u (fenetre=%ums frames=%d wakes=%llu running=%u expirees=%u sautees-gil=%u)%s",
                nth, fam_window_ms_, f, (unsigned long long)w, running, expired,
                fam_gil_skips_.load(std::memory_order_relaxed), viv);
        pthread_mutex_unlock(gil::mutex());
        if (logit) { std::printf("  %s\n", line); progress(line); }  // stdout (qemu) + boot_progress (Vita)
    }
}

void NativeScheduler::exit_current(uint32_t code) {
    // ExitThread arrives via redirect_next(sentinel) and lands in run_guest's
    // sentinel finish; TerminateThread marks the TARGET in its shim. This entry
    // point only records the code — NEVER cpu_->request_stop(): that broadcast
    // is shutdown-grade and monotonic in native (see the cpu.h contract).
    if (t_cur) t_cur->exit_code = code;
}

void NativeScheduler::run() {
    // MAIN runs on the calling host thread (spec: run() blocks until exit).
    // Battement famine (§3.1) : démarré ICI, seulement si arm_famine() a été
    // appelé (rt_boot, branche native, D2_FAMINE). Tout échec rend le filet
    // inerte — dit, jamais silencieux (« diagnostic qui ment »).
    if (fam_on_.load(std::memory_order_relaxed)) {
#ifdef __vita__
        // Plan B (spec §3.5, ouvert par l'audit §10.3, cause §10.6) : thread
        // Sce BRUT. Le chemin pte faisait naître le battement à la priorité
        // Sce 191 — la MOINS urgente (pte_new pose la prio pte MIN 128,
        // mapping 319−x) — ET jetait le rc du sceKernelStartThread : captif
        // dès la naissance, invisible. Ici, la discipline v4-C prouvée sur
        // console : priorité EXPLICITE 0x10000100 (le régime du present,
        // prouvé vivant pendant les wedges), épinglage USER_2 par le
        // CRÉATEUR avant le start (le cœur prouvé vivant — §10.4 ; USER_1
        // suspect, arithmétique a+c §10.1 ; contention negligeable : le
        // present dort dans WaitSema entre les images et le battement ne
        // court que quelques µs par fenêtre — tous des bloqueurs, et v3
        // SIESTE-CEDE prouve qu'un DelayThread élit un prêt d'égale
        // priorité). rc de create/pin/start TOUS testés : un pin raté n'est
        // PAS fatal (aff reste 0, le fil démarre et dira sa position) ; un
        // create/start raté rend le filet INERTE — dit. Surface pthread de
        // fam_beat depuis un fil non-pte : VÉRIFIÉE au désassembleur (§10.6,
        // trylock/unlock GIL kind 0 sans TLS pte ; sommeil déjà
        // sceKernelDelayThread ; progress/printf exercés par le watchdog).
        // 64 Kio de pile : fam_beat formate + fopen (progress) — le 16 Kio
        // du watchdog est déjà à l'aise, on prend une marge x4 (les 0x4000
        // des sondes ne font que spinner).
        auto tramp = [](SceSize, void* argp) -> int {
            // AUTO-EPINGLAGE (wx86_vita_pin_self) : le masque pose par le
            // createur ne tient pas apres le start (05/09).
            {
                const unsigned fm = (unsigned)wx86_vita_core_mask(2);
                unsigned relu = 0; const int rc = wx86_vita_pin_self((int)fm, &relu);
                char m[112]; std::snprintf(m, sizeof m,
                    "famine: battement auto-epinglage masque=0x%x rc=0x%08x relu=0x%x", fm, (unsigned)rc, relu);
                wx86_vita_progress_c(m);
            }
            (*(NativeScheduler* const*)argp)->fam_beat();
            return 0; };
        SceUID th = sceKernelCreateThread("d2_famine", tramp, 0x10000100, 64 * 1024, 0, 0, nullptr);
        if (th < 0) {
            fam_on_.store(false, std::memory_order_relaxed);
            char m[112];
            std::snprintf(m, sizeof m, "famine: battement CreateThread KO (rc=0x%08x) — filet INERTE", (unsigned)th);
            std::printf("  %s\n", m); progress(m);
        } else {
            // D2_COEURS position 2. Defaut « 2 » = USER_2, donc l'appel d'avant
            // a l'octet pres. Ce fil est le SEUL des trois auxiliaires qui ne
            // doit JAMAIS atterrir sur USER_0 : verdict noyau RUN-TO-BLOCK
            // (t12_schedprobe §1), et le battement existe precisement pour le
            // cas ou un runner invite de USER_0 ne bloque plus. wx86_vita_core_mask
            // accepte la valeur mais la DENONCE dans le journal.
            const unsigned fmask = (unsigned)wx86_vita_core_mask(2);
            int prc = sceKernelChangeThreadCpuAffinityMask(th, (int)fmask);
            wx86_vita_core_register("battement", (int)th, fmask, prc);
            if (prc < 0) {
                char m[176];
                std::snprintf(m, sizeof m,
                    "famine: epinglage masque 0x%x KO (rc=0x%08x) — battement demarre non epingle (temoin : absence de la ligne 'battement arme')", fmask, (unsigned)prc);
                progress(m);
            }
            NativeScheduler* self_p = this;
            int src = sceKernelStartThread(th, sizeof self_p, &self_p);
            if (src < 0) {
                fam_on_.store(false, std::memory_order_relaxed);
                sceKernelDeleteThread(th);   // un fil créé jamais démarré garde TCB + pile
                char m[112];
                std::snprintf(m, sizeof m, "famine: battement StartThread KO (rc=0x%08x) — filet INERTE", (unsigned)src);
                std::printf("  %s\n", m); progress(m);
            } else { fam_sce_th_ = th; fam_started_ = true; }
        }
#else
        int frc = pthread_create(&fam_th_, nullptr, fam_tramp, this);
        if (frc == 0) fam_started_ = true;
        else {
            fam_on_.store(false, std::memory_order_relaxed);
            char m[96];
            std::snprintf(m, sizeof m, "famine: battement pthread_create ECHEC (rc=%d) — filet INERTE", frc);
            std::printf("  %s\n", m); progress(m);
        }
#endif
    }
    GuestThread* t = main_;
    pin_runner(t, true);   // USER_0 + temoin « invite-main » + VM domain (le main traduit aussi)
    NT* n = nt(t);
    cpu_->thread_emu_bind(nullptr);                  // base emu
    t_cur = t;
    // Observabilité GIL (§19) : le main tourne sur le fil hôte appelant, jamais
    // passé par pthread_create. Il déclare sa pile comme les runners, sinon il
    // serait rapporté « hote ». Enregistré ICI et pas dans set_main() : run()
    // est la trame sous laquelle tout le reste du main s'exécute, et une SEULE
    // déclaration par fil est permise (deux intervalles qui se recouvrent
    // rendent l'étiquette ambiguë, donc muette — gil.h). Conséquence assumée :
    // les gil::Guard de rt_boot pris AVANT run() (bancs FAMINETEST/SELECTTEST)
    // tournent sur un main pas encore enregistré ; assert_held() y vérifie
    // alors seulement que le GIL est pris, et ne prétend rien de plus.
    // Pas de désenregistrement symétrique ici (contrairement à runner()) : le
    // fil main ne meurt pas, sa pile n'est donc jamais recyclée sous un autre
    // fil — le flanc que ferme unregister_stack() n'existe pas pour lui.
    { char ici; uintptr_t hi = (uintptr_t)&ici;
      gil::register_stack(hi - kMainStackDeclared, hi, t->id); }
    gil::lock();
    n->is_main = true;
    seed_first_run(t);
    t->state = S::Running;
    run_guest(t);
    // Main exited: D2 semantics = process teardown. Stop the workers, join
    // with a bounded wait, report stragglers.
    stop_reason_ = shutdown_ ? "shutdown requested" : "main exited";
    request_shutdown();
    // K4: snapshot the GuestThread* set UNDER the GIL before unlocking — the
    // vector cannot grow past shutdown_ (create_thread refuses), but iterating
    // the live vector unlocked would still race the last in-flight push_back's
    // reallocation. The cells themselves (unique_ptr heap objects) never move,
    // so raw pointers taken here are safe to walk unlocked below.
    std::vector<GuestThread*> snap;
    snap.reserve(threads_.size());
    for (auto& up : threads_) snap.push_back(up.get());
    gil::unlock();
    // Battement famine arrêté (drapeau + join) AVANT le join borné des
    // runners (§3.5) ; sommeil par tranches <= 50 ms => join court. Une
    // DERNIÈRE fenêtre peut se glisser entre gil::unlock() ci-dessus et le
    // store de fam_stop_ (le battement teste shutdown_ en tête de fenêtre,
    // mais la course reste possible) — conséquences nulles : les budgets sont
    // déjà à zéro (request_stop) et la couture teste shutdown_ AVANT la
    // sieste ; au pire une sieste d'1 ms d'un fil pas encore sorti.
    if (fam_started_) {
        fam_stop_.store(true, std::memory_order_relaxed);
#ifdef __vita__
        // Plan B : WaitThreadEnd BORNÉ (2 s, aligné sur le join borné des
        // runners ci-dessous) + DeleteThread (WaitThreadEnd ne libère ni TCB
        // ni pile — discipline SCHEDPROBE). Join court attendu : sommeil par
        // tranches <= 50 ms, battement épinglé USER_2 où tout le monde
        // bloque. La borne couvre le coin composé pin-KO + cœur captif +
        // runner wedgé (triple défaillance) : plutôt une fuite bornée dite
        // qu'un teardown infini muet (revue 2026-08-29). Sur expiration :
        // PAS de DeleteThread (fil non dormant => rc d'erreur — abandon
        // assumé, pas un ménage maquillé).
        SceUInt fam_to = 2 * 1000 * 1000;
        if (sceKernelWaitThreadEnd(fam_sce_th_, nullptr, &fam_to) < 0)
            progress("famine: battement ne sort pas (2 s) — fil abandonne (fuite bornee)");
        else
            sceKernelDeleteThread(fam_sce_th_);
        fam_sce_th_ = -1;
#else
        pthread_join(fam_th_, nullptr);
#endif
        fam_started_ = false;
    }
    uint64_t deadline = now_ms() + 2000;
    for (GuestThread* g : snap) {
        NT* gn = nt(g);
        if (gn->is_main || gn->joined || !gn->pth) continue;
        while (g->state != S::Finished && now_ms() < deadline) { sched_yield(); }
        if (g->state == S::Finished) { pthread_join(gn->pth, nullptr); gn->joined = true; }
        else std::printf("  [sched-native] thread %u did not stop (state=%d) — leaked\n",
                         g->id, (int)g->state);
    }
}

void NativeScheduler::request_shutdown() {
    gil::assert_held();
    shutdown_ = true;
    // The ONE legitimate request_stop broadcast: process shutdown. Monotonic
    // in native mode (see the cpu.h contract) — never call it per thread exit.
    if (cpu_) cpu_->request_stop();                  // broadcast: all emus' budgets zeroed
    for (auto& up : threads_) pthread_cond_broadcast(&nt(up.get())->cv);
}

// ---- introspection / diagnostics -------------------------------------------

void NativeScheduler::set_no_preempt(bool on) {
    if (on && !no_preempt_warned_) {
        no_preempt_warned_ = true;
        progress("sched-native: set_no_preempt no-op (dynarec cache is lock-protected; spec D7)");
    }
}

int NativeScheduler::live_count() {
    int c = 0; for (auto& up : threads_) if (up->state != S::Finished) ++c; return c;
}
int NativeScheduler::live_thread_count() { return live_count(); }
GuestThread* NativeScheduler::thread_by_id(uint32_t id) {
    gil::assert_held();
    for (auto& up : threads_) if (up->id == id) return up.get();
    return nullptr;
}
std::vector<uint32_t> NativeScheduler::live_thread_ids() {
    gil::assert_held();
    std::vector<uint32_t> v;
    for (auto& up : threads_) if (up->state != S::Finished) v.push_back(up->id);
    return v;
}

void NativeScheduler::dump_threads_to(void(*sink)(const char*)) {
    // WATCHDOG host thread. Try the GIL briefly; on failure dump the lock-free
    // flat snapshot anyway (stale-but-consistent — the C14 contract), flagged.
    bool locked = (pthread_mutex_trylock(gil::mutex()) == 0);
    if (!locked) sink("sched-native: GIL busy — racy snapshot follows");
    uint32_t fn = flat_n_.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < fn; ++i) {
        GuestThread* g = flat_[i];
        if (!g || g->state == S::Finished) continue;
        void* w = g->wait_obj;
        NT* gn = static_cast<NT*>(g->native);
        const uint32_t* ip = (g->state == S::Running && gn)
                             ? cpu_->thread_emu_ip(gn->emu) : nullptr;
        char m[128];
        std::snprintf(m, sizeof m, "thr %u st=%d wait=%s eip=%08x sl=%llu",
                      g->id, (int)g->state,
                      w ? static_cast<Waitable*>(w)->kind() : "-",
                      ip ? *ip : g->ctx.eip, (unsigned long long)g->slices);
        sink(m);
    }
    if (locked) pthread_mutex_unlock(gil::mutex());
}

NativeScheduler* g_native_sched = nullptr;           // runner_tramp backref

} // namespace d2rt
