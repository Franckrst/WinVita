// src/runtime/sched_native.h — the native backend of ThreadScheduler
// (spec docs/superpowers/specs/2026-08-28-native-scheduler-etape1-design.md).
//
// One pthread per guest thread (pte on Vita => real sceKernel threads), all
// pinned to ONE core in etape 1, OS preemption. The GIL (gil.h) is the
// scheduler lock: every method here runs with it held (methods are called
// from shims, which run under the trap-dispatch gil::Guard). wait() blocks
// with pthread_cond_wait on the GIL mutex. Wake-up is a full re-poll of every
// blocked thread in id order — the EXACT mirror of the cooperative
// pick_ready(), so Waitable semantics (auto-reset consumption, IOCP guest
// out-param writes, wake ordering) carry over verbatim.
//
// CONTRACT: under native, notify() after ANY Waitable mutation is MANDATORY —
// there is no polling fallback (coop's pick_ready re-polled every switch and
// forgave a forgotten notify; native sleeps forever). Audited 2026-08-28: all
// current shims notify.
#pragma once
#include "runtime/guest_thread.h"
#include "runtime/cpu.h"
#include "runtime/bridge.h"
#include "runtime/gil.h"

#include <memory>
#include <vector>
#include <atomic>
#include <pthread.h>

namespace d2rt {

// Filet anti-famine (spec docs/superpowers/specs/2026-08-29-filets-anti-famine-
// design.md §3.4) — constantes du mécanisme (Tâche 2) et du détecteur
// (Tâche 3). Les knobs env correspondants (D2_FAMINE, D2_FAMINE_WINDOW_MS,
// D2_FAMINE_NAP_US) sont parsés par la branche native de rt_boot, qui appelle
// arm_famine() AVANT run().
constexpr uint32_t kFamineNapUsDefault    = 1000; // sieste = plancher sched_yield pte vérifié (1 ms, T12 §1.3)
constexpr uint32_t kFamineLogEvery        = 8;    // par FLUX (détections, siestes) : 1re journalisée, puis 1 sur 8 (§3.6)
constexpr uint32_t kFamineWindowMsDefault = 200;  // fenêtre de détection — borne aussi la latence de désaffamage (§3.4)
constexpr uint32_t kFamineExpiredMarginMs = 50;   // marge avant de compter une deadline « expirée » (§3.2)
// Observabilité GIL (audit §19) : au-delà de cette ancienneté de prise, une
// fenêtre sautée sur trylock échoué n'est plus « un shim qui progresse » mais
// un détenteur qui ne rend pas la main — le battement le NOMME (une ligne, et
// STRICTEMENT rien d'autre : aucun réveil, aucun poke, aucune sieste de plus).
constexpr uint32_t kGilStuckMs = 2000;
// Cadence MAXIMALE du résumé « fenêtre sautée (GIL occupé) » (§3.6 amendé).
// Borne en TEMPS et non en modulo : le nombre de sauts par seconde dépend de
// la fenêtre de détection (5/s à 200 ms, 50/s au banc 20 ms), donc un modulo
// donnerait un débit de journal différent selon le knob. 5 s = au plus 12
// lignes par minute quelle que soit la fenêtre, sous la cadence du chien de
// garde (10 s) : le journal ne peut plus rester muet plus de 5 s pendant que
// le filet renonce, et il ne peut pas noyer boot_progress non plus.
constexpr uint32_t kFamineSkipReportMs = 5000;
// Nombre MAXIMAL de runners détaillés dans le champ « run= » (le reste est
// résumé par « +k » : tronquer en silence serait un champ menteur). 10 tient
// dans la ligne alive: et couvre les ~8 fils invités d'une partie en ligne.
constexpr uint32_t kVivacityMaxThreads = 10;

class NativeScheduler : public ThreadScheduler {
public:
    NativeScheduler(Cpu* cpu, Bridge* br,
                    uint32_t stack_region, uint32_t stack_each, uint32_t tib_region);

    GuestThread* set_main(uint32_t entry, uint32_t stack_top, uint32_t tib) override;
    GuestThread* create_thread(uint32_t entry, uint32_t param,
                               uint32_t stack_size, bool suspended) override;
    void resume(GuestThread* t) override;
    GuestThread* current() override;
    void yield() override;             // brief GIL release + sched_yield
    uint32_t wait(Waitable* w, uint32_t timeout_ms) override;
    void wait_noresult(Waitable* w, uint32_t timeout_ms) override;
    uint32_t wait_timeout_result(Waitable* w, uint32_t timeout_ms,
                                 uint32_t eax_on_timeout) override;
    void notify(Waitable* w) override;
    void exit_current(uint32_t code) override;
    void run() override;               // runs MAIN on the calling host thread
    int live_count() override;

    void set_no_preempt(bool) override;        // no-op + one-shot log (spec D7)
    void request_shutdown() override;
    GuestThread* thread_by_id(uint32_t id) override;
    std::vector<uint32_t> live_thread_ids() override;
    int live_thread_count() override;
    void dump_threads_to(void(*sink)(const char*)) override;   // watchdog: trylock
    void set_fault_dispatcher(FaultFn f) override { fault_disp_ = std::move(f); }
    // Stored but UNUSED in etape 1: the sink is the cooperative backend's
    // mechanism for refreshing the guest-inlined GetTickCount page when the
    // VIRTUAL clock advances. Native has no virtual clock — the tick page is
    // served real-time by tick_real through the shims (rt_boot disables the
    // guest-inline tick under native, Task 9).
    void set_time_sink(std::function<void(uint64_t)> f) override { time_sink_ = std::move(f); }
    const char* stop_reason() const override { return stop_reason_; }

    // ---- coeur2 : QUEL fil invite a le droit de partir sur un autre coeur ---
    // Le moteur ne connait AUCUN nom de module et ne sait pas comment
    // s'appelle le fil « serveur » du jeu : il ne sait qu'une chose, l'adresse
    // d'ENTREE invitee de ce fil, que le consommateur lui donne a
    // l'installation. 0 (defaut) = personne, donc topologie d'avant a l'octet
    // pres. Table configuree a l'installation, jamais une question posee au
    // portage : le moteur compare `t->entry` et se tait.
    //
    // Avant le 2026-09-12 cette adresse etait calculee ICI, par
    // `br_->module_base("Game.exe") + kCoeur2ServerRva` — le nom du module du
    // PREMIER consommateur, en dur dans l'ordonnanceur generique. Un portage
    // dont l'executable ne s'appelle pas ainsi obtenait base=0, donc un
    // WX86_COEUR_SERVEUR inerte SANS UN MOT.
    void set_server_thread_entry(uint32_t guest_entry_va) { server_entry_ = guest_entry_va; }
    uint32_t server_thread_entry() const { return server_entry_; }

    // Watchdog heartbeat: total wake deliveries (the native "switches").
    const uint64_t* wakes_ptr() const { return &wakes_total_; }
    // ---- Filet anti-famine : armement + observabilité (Tâche 3, §3.2/§3.4) --
    // Appelé AVANT run() par la branche native de rt_boot. Mémorise la config
    // et arme fam_on_ ; le pthread du battement est démarré par run() et
    // arrêté (drapeau + join) AVANT le join borné des runners (§3.5). frames =
    // compteur d'images du jeu, fourni par l'appelant, lu volatile. 0 =>
    // défauts. (L'ancienne rédaction nommait `g_frame de rt_boot` et
    // `d2vita_watchdog_start` : deux objets d'un portage, dans le contrat d'une
    // méthode publique du moteur.)
    void arm_famine(const volatile int* frames, uint32_t window_ms, uint32_t nap_us);
    bool     fam_armed()      const { return fam_on_.load(std::memory_order_relaxed); }
    uint32_t fam_detections() const { return fam_detections_.load(std::memory_order_relaxed); }
    uint32_t fam_naps()       const { return fam_naps_.load(std::memory_order_relaxed); }
    uint32_t fam_gil_skips()  const { return fam_gil_skips_.load(std::memory_order_relaxed); }
    // Pointeurs pour la ligne alive: du watchdog Vita (champ fam=<det>/<siestes>,
    // NATIF SEULEMENT — le coop passe null et le champ est absent, pas un zéro
    // menteur, §3.6). Lecture 32 bits sans déchirure sur ARM32 — même régime
    // heartbeat que wakes_ptr().
    const uint32_t* fam_detections_ptr() const {
        static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t),
                      "atomic<u32> doit être lisible comme un u32 nu (watchdog)");
        return reinterpret_cast<const uint32_t*>(&fam_detections_);
    }
    const uint32_t* fam_naps_ptr() const { return reinterpret_cast<const uint32_t*>(&fam_naps_); }
    // ---- Observabilité GIL (audit §19) — lecture SANS verrou ---------------
    // L'id de fil invité du détenteur vient maintenant de gil::Probe::owner_tag,
    // résolu par gil::probe() depuis la MARQUE DE PILE publiée par lock() (voir
    // gil.h : plus aucun pthread_self() sur le chemin de chaque trap). Les fils
    // qui peuvent prendre le GIL déclarent leur pile ici : le main dans run(),
    // les runners dans runner(). Étiquette 0 = fil non enregistré ou ambigu,
    // imprimé « hote » — exactement ce que rendait l'ancien -1.
    // Fenêtre de pile DÉCLARÉE, sous le point d'enregistrement. Toujours un
    // SOUS-ENSEMBLE STRICT de la pile réelle (gil.h) :
    //   * runner : pile de 256 KiB (pthread_attr_setstacksize dans
    //     create_thread) et point d'enregistrement à quelques centaines
    //     d'octets du sommet — 240 KiB laissent 16 KiB de marge ;
    //   * main : 4 MiB sur Vita (sceUserMainThreadStackSize, vita_present.cpp),
    //     8 MiB par défaut sur l'hôte, et run() est appelé peu profond.
    static constexpr uintptr_t kRunnerStackDeclared = 240 * 1024;
    static constexpr uintptr_t kMainStackDeclared   = 512 * 1024;
    // Formate le champ « gil=… » de la ligne alive: du chien de garde. Rend le
    // nombre d'octets écrits, 0 quand le GIL est inactif (coop : champ ABSENT,
    // pas un zéro menteur — précédent du champ fam=, §3.6). Appelé par le SEUL
    // fil chien de garde : ses statiques (borne inférieure de durée à la
    // cadence de ce fil) n'ont pas d'autre écrivain.
    int gil_field(char* out, unsigned n);
    // ---- Vivacité PAR RUNNER (§ « famine vs interblocage ») ---------------
    // Formate le champ « run=<id>:<état>:<blocs>[,…] » de la ligne alive: (et
    // des lignes du battement famine, seule fenêtre disponible sur qemu).
    // <blocs> = blocs traduits exécutés depuis le boot (Cpu::thread_emu_blocks),
    // MONOTONE : il bouge dès que le fil exécute du code traduit, shim ou pas.
    // <état> = R Running, B Blocked, P Ready, N New.
    // LECTURE, entre DEUX relevés successifs, pendant que frames= est figé :
    //   * un compteur bouge                   => FAMINE (quelqu'un tourne, les
    //                                            autres n'avancent pas) ;
    //   * aucun ne bouge, états B partout     => tout le monde ATTEND. Sain si
    //                                            quelque chose doit arriver
    //                                            (réseau), INTERBLOCAGE si
    //                                            personne ne peut plus signaler ;
    //   * aucun ne bouge, un R                => le fil R est arrêté DANS un
    //                                            shim (voir gil=/dans=).
    // Ces cas appellent des correctifs opposés, d'où le champ. Il ne prétend
    // PAS distinguer seul attente saine et interblocage : c'est l'état publié
    // à côté du compteur qui donne la moitié manquante.
    // Rend le nombre d'octets écrits, 0 = rien à dire (coop, ou backend sans
    // compteur de blocs) — champ ABSENT plutôt que zéro menteur, comme
    // fam=/gil= ; « run=? » quand la place manque (jamais un champ qui
    // disparaît en silence).
    // Sans verrou (cliché plat flat_), appelable par le chien de garde ET par
    // le battement famine.
    int vivacity_field(char* out, unsigned n);
    // ---- Recensement des blocs PAR FIL (etape 2, verrou d'Amdahl) ---------
    // Meme compteur que vivacity_field, mais CUMULE et sur TOUS les fils, les
    // Finished COMPRIS. C'est la difference qui compte : vivacity_field saute
    // les fils termines parce qu'il repond a « qui avance MAINTENANT » ; ici la
    // question est « qui a fait le travail DEPUIS LE BOOT », et un fil qui
    // aurait tout fait puis serait sorti doit apparaitre, sinon la mesure ment
    // par omission au moment precis ou elle deciderait d'un chantier.
    //
    // CE QU'IL SERT A TRANCHER : le multi-coeur ne rend quelque chose que si
    // plusieurs fils invites executent vraiment du code traduit. Si un seul fil
    // porte l'essentiel des blocs, repartir les fils sur trois coeurs ne change
    // rien -- le fil chaud reste seul sur le sien. Le champ « max=NN% » est
    // donc le chiffre de decision, et le critere est pose dans la spec
    // 2026-08-31-native-scheduler-etape2-design.md AVANT la mesure.
    //
    // Appele au DEMONTAGE, depuis le fil main, quand plus aucun runner ne
    // court : pas de verrou, et les compteurs ne bougent plus.
    // Rend le nombre d'octets ecrits ; 0 si le backend n'a pas de compteur de
    // blocs (champ ABSENT plutot qu'un zero menteur, comme fam=/gil=/run=).
    int blocks_census(char* out, unsigned n);
    // Polls GIL-free (rt_boot d2rt_poll_gilfree, spec D4 "timeout court
    // re-teste") : re-test du shutdown entre deux tranches de 250 ms — sans
    // lui, un fil parque 15 s dans un ::poll survit au join borne (2 s) du
    // teardown et re-entre dans les shims APRES les destructeurs statiques
    // du main (UAF). Lecture volatile sans GIL, voulue (mono-coeur etape 1).
    bool shutdown_requested() const { return shutdown_; }
    uint64_t wake_signal_ = 0, wake_timeout_ = 0;   // parity with coop's counters

private:
    // Per-thread native extension, owned via GuestThread::native.
    struct NT {
        // Fil hôte du runner. RESTE 0 pour le main (qui n'est pas passé par
        // pthread_create) : plus personne ne le lit pour l'observabilité depuis
        // que l'identité du détenteur du GIL est une marque de pile (gil.h) —
        // ne pas y remettre un pthread_self(). Le join borné du teardown saute
        // le main sur is_main, avant même de regarder ce champ.
        pthread_t       pth{};
        pthread_cond_t  cv;
        void*           emu = nullptr;      // Cpu::thread_emu_create handle
        bool            woken = false;      // wake_check_all delivered
        uint64_t        t_signal_us = 0;    // D2_WAKEPROF : instant du pthread_cond_signal
        uint32_t        delivered_eax = 0;
        bool            is_main = false;
        bool            joined = false;
        // ---- Filet anti-famine (spec 2026-08-29 §3.2/§3.3) ----
        // SceUID du thread noyau sous-jacent (int32_t : les en-têtes psp2 ne
        // sont pas visibles ici). Posé dans runner() sur Vita
        // (sceKernelGetThreadId), 0 ailleurs et pour le main. Diagnostics ;
        // le repli P4 (boost de priorité, approche A) en aurait aussi besoin.
        int32_t         sce_uid = 0;
        // Échéance d'un wait TEMPORISÉ en cours, base now_ms()/CLOCK_MONOTONIC
        // (PAS l'abs REALTIME du cond_timedwait — le détecteur T3 compare à
        // now_ms()). Posée par wait_common avant le blocage, remise à 0 après ;
        // deux écritures par wait, sous GIL. 0 = pas d'attente temporisée.
        uint64_t        deadline_ms = 0;
        // Dernière époque famine acquittée par CE fil (0 = jamais). Écrite et
        // lue par le runner lui-même uniquement, hors GIL — mono-écrivain.
        uint32_t        fam_ack = 0;
        // Ce fil a-t-il déjà journalisé UNE sieste ? La 1re sieste de CHAQUE
        // fil est journalisée en plus du modulo global kFamineLogEvery — sans
        // quoi un fil pourrait rester masqué par le modulo (revue M2, §3.6).
        // Même régime mono-écrivain que fam_ack (le runner lui-même, hors GIL).
        bool            fam_napped = false;
    };
    static void* runner_tramp(void* guest_thread);
    void runner(GuestThread* t);            // worker body (GIL taken inside)
    // ---- coeur2 (06/09/2026) : le fil SERVEUR du jeu (un fil invite comme un
    // autre) sur un AUTRE coeur — WX86_COEUR_SERVEUR=<c> (1..3 = USER_c ;
    // absent ou 0 = topologie d'avant, tout sur USER_0). Identification du
    // fil : par ADRESSE D'ENTREE invitee, que le CONSOMMATEUR fournit via
    // set_server_thread_entry() (le moteur ne connait aucun module), ou par id
    // invite (WX86_COEUR_SERVEUR_ID=<n>, prioritaire s'il est donne).
    // Sous qemu il n'y a pas de coeurs USER : la meme decision
    // devient une affinite HOTE quand WX86_QEMU_MONOCOEUR=<cpu> emule la
    // topologie console (tous les runners + main sur <cpu>, le serveur sur
    // <cpu>+c). Sans ces knobs, AUCUN appel d'affinite n'est ajoute.
    bool is_server_thread(GuestThread* t);
    void pin_runner(GuestThread* t, bool is_main);   // appele PAR LE FIL LUI-MEME, a l'entree
    void run_guest(GuestThread* t);         // seed + cpu run + finish (GIL held at entry/exit)
    void seed_first_run(GuestThread* t);
    void finish_thread(GuestThread* t, bool ok, const char* fault);
    void wake_check_all();                  // pick_ready() mirror; GIL held
    uint32_t wait_common(Waitable* w, uint32_t timeout_ms, uint32_t eax_on_timeout);
    NT* nt(GuestThread* t) { return static_cast<NT*>(t->native); }
    uint64_t now_ms() const;

    Cpu* cpu_; Bridge* br_;
    uint32_t server_entry_ = 0;             // coeur2 : entree invitee du fil serveur (0 = aucun)
    uint32_t stack_region_, stack_each_, stack_next_;
    uint32_t tib_region_, tib_next_;
    std::vector<std::unique_ptr<GuestThread>> threads_;   // mutated under GIL only
    // Flat snapshot for the lock-free watchdog reader (C14 pattern, same as coop).
    static constexpr uint32_t kFlatMax = 128;
    GuestThread* flat_[kFlatMax] = {nullptr};
    std::atomic<uint32_t> flat_n_{0};
    GuestThread* main_ = nullptr;
    uint32_t next_id_ = 1;
    FaultFn fault_disp_;
    std::function<void(uint64_t)> time_sink_;
    // volatile is sufficient in etape 1: every runner is pinned on ONE core
    // (no multi-core visibility to guarantee); move to atomics in etape 2.
    volatile bool shutdown_ = false;
    // Watchdog reads this lock-free via wakes_ptr() from another core (ARM32:
    // a 64-bit load can tear) — heartbeat semantics, torn reads tolerated.
    uint64_t wakes_total_ = 0;
    const char* stop_reason_ = "";
    bool no_preempt_warned_ = false;
    bool stack_overrun_warned_ = false;     // one-shot TIB-overrun warning
    uint32_t thread_cap_ = 64;              // fail CreateThread cleanly past this

    // ---- Filet anti-famine (Tâche 2 : mécanisme seul — poke + sieste à la
    // couture). Le détecteur/battement (Tâche 3) écrit fam_epoch_/fam_on_/
    // fam_detections_/fam_gil_skips_. Tant que fam_on_ est faux et fam_epoch_
    // à 0, TOUT est inerte : l'expiration spontanée du budget 0x7FFFFFFF
    // garde le comportement « spurious resume » d'aujourd'hui, sans sieste
    // (époque déjà acquittée par construction). Atomiques : les runners lisent
    // hors GIL et le battement (T3) court sur un autre cœur sur Vita.
    std::atomic<uint32_t> fam_epoch_{0};      // époque famine, 0 = jamais ; ++ par le battement sous trylock GIL (T3)
    // ORDRE CONTRACTUEL (T3) : le battement DOIT publier fam_epoch_ AVANT de
    // poker les budgets — la couture lit l'ÉPOQUE, pas le budget ; si on poke
    // puis publie, un fil en transit de couture peut perdre les deux (budget
    // rechargé par la tranche + époque pas encore vue), auto-guéri seulement
    // à la fenêtre suivante.
    std::atomic<bool>     fam_on_{false};     // armé par le battement (T3, D2_FAMINE) — défaut : filet coupé
    std::atomic<uint32_t> fam_detections_{0}; // détections du battement (T3)
    std::atomic<uint32_t> fam_naps_{0};       // siestes prises (incrémenté hors GIL par fam_note_nap)
    std::atomic<uint32_t> fam_gil_skips_{0};  // fenêtres sautées sur trylock GIL échoué (T3)
    std::atomic<uint32_t> fam_gil_stuck_{0};  // fenêtres sautées AVEC un détenteur figé (§19) — flux de journal propre
    // Résumé borné de la branche « trylock GIL échoué » : instant du dernier
    // résumé publié et compte des sauts déjà résumés. Écrits/lus par le SEUL
    // fil du battement — pas d'atomique nécessaire.
    uint64_t fam_skip_last_ms_ = 0;
    uint32_t fam_skip_last_n_  = 0;
    uint32_t fam_nap_us_ = kFamineNapUsDefault;  // durée de sieste (knob D2_FAMINE_NAP_US en T3)
    void fam_nap();                           // sceKernelDelayThread / nanosleep — appeler HORS GIL
    void fam_note_nap(GuestThread* t, uint32_t eip);  // compteur + journal limité (§3.6)

    // ---- Battement famine (Tâche 3 : détecteur, §3.2). Démarré par run() si
    // fam_on_, arrêté (drapeau + join) avant le join borné des runners (§3.5).
    // Hôte (qemu) : pthread (fam_tramp/fam_th_). Vita : thread Sce BRUT —
    // plan B spec §3.5, ouvert par l'audit §10.3 (captif dès la naissance) et
    // expliqué §10.6 (pte crée à la priorité 191, la MOINS urgente, et JETTE
    // le rc du StartThread). Priorité explicite 0x10000100, épinglé USER_2
    // par le CRÉATEUR avant le start, rc create/pin/start tous testés (run()).
    static void* fam_tramp(void* self);
    void fam_beat();                          // boucle de détection (§3.2) — publique aux deux chemins de création
    pthread_t fam_th_{};                      // hôte seulement
#ifdef __vita__
    int32_t fam_sce_th_ = -1;                 // SceUID du battement Sce brut (plan B §3.5)
#endif
    bool fam_started_ = false;                // battement lancé par run()
    std::atomic<bool> fam_stop_{false};       // drapeau d'arrêt (teardown)
    const volatile int* fam_frame_ = nullptr; // compteur d'images (g_frame), lu volatile
    uint32_t fam_window_ms_ = kFamineWindowMsDefault;
};

// Backref for runner_tramp (a plain pthread start_routine has one void* and it
// carries the GuestThread*). Defined in sched_native.cpp; assigned by
// make_scheduler (Task 9) right after `new NativeScheduler(...)`, BEFORE any
// create_thread.
extern NativeScheduler* g_native_sched;

} // namespace d2rt
