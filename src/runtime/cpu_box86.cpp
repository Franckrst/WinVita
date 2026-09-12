// src/runtime/cpu_box86.cpp — Box86-dynarec-backed x86-32 CPU (ARM only).
//
// Implements the same Cpu contract as cpu_unicorn.cpp on top of the vendored
// Box86 dynarec core (third_party/box86-dynarec + src/dynarec86/shim).
// Memory model is IDENTITY: guest VA == host address (map() uses MAP_FIXED),
// which is exactly how Box86 itself runs x86 code.
//
// Trap mechanism: set_trap() maps the [lo,hi) window and fills every 16-byte
// slot (the Bridge allocates trap VAs 16 bytes apart, sentinel included) with
// Box86's "exit" bridge stub: CC 'S' 'C' + 4 zero bytes. The dynarec compiles
// that to emu->quit=1 + EIP=<slot>; our run() loop detects EIP inside the
// window, invokes the TrapFn (guest state is at a clean instruction boundary,
// ESP still pointing at the pushed return address — same contract as the
// Unicorn fetch-unmapped hook), then resumes at whatever EIP the handler set.
// This round-trips through the dynarec prolog/epilog on every native call.
// D2_INLINETRAP a supprime cet aller-retour (traduction du stub en appel natif
// EN LIGNE) ; MESURE ET REFUTE sur console le 05/09/2026 — 22,82 img/s contre
// un temoin encadrant a 22,83, avec preuve d'armement (100 % des prises
// passees par le chemin en ligne). Retire le meme jour.
//
// Preemption (was TODO 6a) is implemented via the block-entry budget: every
// translated block's prologue decrements emu->dyn86_budget (recharged per
// slice in run()) and exits DynaRun resumable at the block's start when it
// expires; request_stop() zeroes the budget (plus dyn86_request_stop's flag
// for the LinkNext seam, which covers not-yet-translated targets). Blocks
// stay direct-linked. Residual limitation: a loop contained in a SINGLE
// dynablock never re-enters a prologue and cannot be preempted this way (D2
// threads yield via import traps long before that matters).
//
// TODO (deferred to a later lot):
//  * SMC (6b): protectDB write-protects pages holding translated code; Box86
//    normally catches the SIGSEGV of a guest self-write and invalidates.
//    No segv handler is installed here yet; host-side Cpu::write() does call
//    unprotectDB() first, so bridge-side writes (IAT patches...) are safe.
//  * single instance: the Box86 core has one global my_context; only one
//    CpuBox86 may exist per process (asserted in make_cpu_box86).
//
// Box86 is (c) ptitSeb, MIT license — see third_party/box86-dynarec/LICENSE
#ifdef __arm__

#include <cstdint>
#include "runtime/guest_thread.h"   // X86Context (per-thread FPU blob)
#include "runtime/prof_map.h" // la carte des familles du profil (fournie par le portage)
#include "runtime/cpu.h"    // MUST be included before the Box86 headers:
                            // Box86's regs.h #defines R_EAX & friends.
#include "runtime/gil.h"    // GIL Guard at the trap dispatch (spec D3; inert coop)
#include "runtime/trapcnt.h"// compteur de prises par creneau: le chemin
                            // intrinseque court-circuite le Bridge, c'est ICI
                            // qu'il doit se compter (en-tete sans dependance)
#ifndef __vita__            // Vita delivers no POSIX signals; the SIGSEGV diag
#include <csignal>          // handler is a desktop/qemu debug aid only.
#include <ucontext.h>
#endif
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <ctime>      // coeur2 : chrono des prises contendues (D2_FILSTAT)
#include <pthread.h>
#include <cstdlib>
#include <atomic>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

// Box86 build-mode macros (the vendored headers branch on them).
#ifndef DYNAREC
#define DYNAREC
#endif
#ifndef ARM
#define ARM
#endif

extern "C" {
#include "debug.h"
#include "box86context.h"
#include "regs.h"
#include "emu/x86emu_private.h"
#include "emu/x86run_private.h"
#include "x86run.h"
#include "custommem.h"
#include "dynablock.h"
#include "dynarec/dynablock_private.h"
#include "dyn86.h"
#include "dyn86_memintrin.h"    // D2Vita : memcpy/memset natifs (D2_MEMINTRIN)

void dynarec86_setup_emu_helpers(x86emu_t* emu);    // shim_impl.c
}

// Box86's regs.h macros would shadow d2rt's enum Reg constants (the enum was
// already parsed inside cpu.h, so only *uses* below need the macros gone).
#undef R_EAX
#undef R_ECX
#undef R_EDX
#undef R_EBX
#undef R_ESP
#undef R_EBP
#undef R_ESI
#undef R_EDI
#undef R_EIP
#undef R_EFLAGS

// D2_EIPPROF sampling profiler (see run()). Buckets are C-visible so rt_boot
// can print them; base is the guest image base (set once the exe is mapped).
// DOIT rester au niveau FICHIER : dans le namespace anonyme de `d2rt`, GCC 15
// (VitaSDK) donne la priorité au lien interne et décore ces symboles malgré le
// `extern "C"` — le lien Vita échouait alors sur `undefined reference to
// d2rt_eipprof_*` alors que le GCC 13 du build qemu passait. (2026-08-26)
extern "C" {
    uint32_t d2rt_eipprof_on = 0;
    uint32_t d2rt_eipprof_base = 0;
    uint64_t d2rt_eipprof[8] = {0};
    uint64_t d2rt_eipprof_sub[32] = {0};    // 0x0f0000 + n*0x1000
    uint64_t d2rt_eipprof_sub2[32] = {0};   // 0x0d0000 + n*0x1000
    uint64_t d2rt_eipprof_fn[16] = {0};     // 0x0fa000 + n*0x100 (zoom fonction)
    uint64_t d2rt_eipprof_sub3[32] = {0};   // (réservé)
    uint64_t d2rt_eipprof_all[96] = {0};    // n*0x8000 sur tout le .text
    // Histogramme EXACT des EIP échantillonnés (= adresses de début de bloc).
    // Les cases de 32 KiB disent « où », pas « quoi » : pour porter une
    // fonction il faut son adresse, pas sa tranche. Table ouverte à sondage
    // linéaire ; ~10 k échantillons pour 8192 cases, la saturation ne se
    // produit pas — et si elle se produisait on PERD l'échantillon, on ne le
    // ré-attribue jamais à une autre adresse.
    uint32_t d2rt_eipprof_key[8192] = {0};
    uint64_t d2rt_eipprof_hit[8192] = {0};

    // ---- D2_TIMEPROF : profil en TEMPS du code invite --------------------
    // Contrat et raison d'etre : docs/perf/timeprof_20260907.md.
    // D2_EIPPROF echantillonne a l'expiration d'un budget de BLOCS : ses
    // pourcentages sont des parts d'ENTREES DE BLOC, pas de temps, et il
    // sur-represente d'un facteur ~10 les fonctions courtes tres appelees
    // (mesure du 07/09 : o1_intrin_console_20260907.md §3).
    // Ici le budget est ASSERVI pour que les intervalles soient egaux en TEMPS.
    //
    // ⚡ CE QUE CET INSTRUMENT NE FAIT PAS, ET QU'IL DEVAIT FAIRE.
    // L'asservissement egalise la DUREE des intervalles ; il ne change pas QUEL
    // bloc est tire a l'interieur d'un intervalle — le tirage se fait toujours
    // a un nombre de blocs fixe. L'adresse echantillonnee reste donc distribuee
    // proportionnellement a la FREQUENCE D'ENTREE, comme D2_EIPPROF.
    // Mesure du 07/09, meme fenetre console : 10dd60 = 14,11 % ici contre
    // 12,06 % la-bas, 075aa0 = 7,02 contre 6,53 — les deux profils CONCORDENT.
    // docs/perf/timeprof_20260907.md §3.
    //
    // CE QU'IL APPORTE QUAND MEME : 29x plus d'echantillons (116 486 contre
    // 3 994) et 3x plus d'adresses vues, donc un classement exploitable au-dela
    // de la 20e place ; et l'ALIASING est ecarte (echantillonner toutes les
    // ~8 ms dans une image de 18,5 ms groupe les releves sur certaines phases).
    // Que les deux profils concordent malgre cela ETABLIT que les chiffres
    // console de D2_EIPPROF n'etaient pas un artefact d'aliasing.
    //
    // COUT : -10,5 % sur l'image a 250 us de cible (budget ~708 blocs, soit
    // ~28x plus d'aller-retours epilogue/prologue). JAMAIS dans une jambe de
    // vitesse. Monter la cible a 1000 us divise le cout par ~4.
    //
    // Un VRAI profil en temps demanderait d'echantillonner le PC a des instants
    // choisis : impossible ici (l'EIP invite vit dans r14 et n'est en memoire
    // qu'a l'epilogue) sans un compteur de cycles (PMCCNTR), inaccessible
    // depuis l'userland Vita sans PMUSERENR pose en mode noyau.
    uint32_t d2rt_timeprof_on = 0;       // 0 = eteint ; sinon cible en us
    uint32_t d2rt_timeprof_base = 0;
    uint64_t d2rt_tp_key[8192] = {0};    // adresse invitee (0 = case libre)
    uint64_t d2rt_tp_hit[8192] = {0};
    uint64_t d2rt_tp_bucket[8] = {0};    // memes familles que d2rt_eipprof
    uint64_t d2rt_tp_samples = 0;        // echantillons RETENUS
    uint64_t d2rt_tp_susp = 0;           // intervalles rejetes (fil suspendu)
    uint64_t d2rt_tp_us = 0;             // temps couvert par les echantillons
    uint64_t d2rt_tp_blocks = 0;         // blocs couverts (pour le budget moyen)

    // ---- D2_LAGWATCH : anneau d'echantillons pour attribuer un GEL ---------
    // Le probleme des gels ponctuels n'est pas le meme que celui du debit : ce
    // sont des evenements RARES, pendant que l'utilisateur joue, et qu'on ne
    // peut pas rejouer. D2_FRAMEPROF sait deja QUAND ils arrivent et attribue
    // deja les causes HOTES (traduction JIT, synchro I-cache, lectures
    // fichier, decompressions, changements de fil). Ce qui manque est : QUELLE
    // FONCTION INVITEE tournait pendant le gel.
    // Cet anneau garde les derniers echantillons (horodate, EIP, fil). Quand
    // une image depasse le seuil, rt_boot y releve les echantillons tombes
    // DANS cette image et publie les adresses dominantes.
    // Cout : celui de l'echantillonneur seul (D2_TIMEPROF), donc regle par son
    // intervalle — 2000 us par defaut sous LAGWATCH, ~1 % au lieu des 10,5 %
    // mesures a 250 us.
    // ⚠️ LIMITE, a dire : une boucle invitee qui tient dans UN SEUL dynablock
    // ne re-entre jamais dans un prologue et n'est donc JAMAIS echantillonnee.
    // Un gel de cette nature apparaitra avec « ech=0 » — ce qui est lui-meme
    // une information, et non un silence.
    uint32_t d2rt_lag_on = 0;            // 1 = anneau alimente
    #define D2RT_LAG_RING 2048u
    uint64_t d2rt_lag_t[D2RT_LAG_RING]  = {0};   // horodate (us)
    uint32_t d2rt_lag_ip[D2RT_LAG_RING] = {0};   // EIP invite
    uint32_t d2rt_lag_w = 0;                     // curseur d'ecriture, monotone
}

// ⚡ CES DEFINITIONS DOIVENT RESTER AU SCOPE GLOBAL. Elles vivaient dans le
// namespace ANONYME de ce fichier : `extern "C"` n'y donne PAS une liaison
// externe — le symbole garde sa decoration
// (_ZN4d2rt12_GLOBAL__N_113d2rt_b5_callsE) et rt_boot ne le trouve plus.
// Le piege est silencieux cote qemu (g++ 13 acceptait) et n'a echoue qu'a
// l'edition de liens VITA (arm-vita-eabi 15.2) : deux chaines d'outils, deux
// verdicts, sur le meme source.
#if defined(D2_TLSCOUNT) || defined(D2_B5CENSUS)
extern "C" { unsigned long long d2_tls_hits = 0; }
#endif
extern "C" {
    unsigned long long d2rt_b5_calls  = 0;   // entrees dans try_intrinsic
    unsigned long long d2rt_b5_loads  = 0;   // lectures de la table
    unsigned long long d2rt_b5_hits   = 0;   // intrinseques servies
    unsigned long long d2rt_b5_emutls = 0;   // chaines emutls dans le corps servi
    unsigned int       d2rt_b5_index_on = 0; // jambe armee : 1 = index direct
    unsigned int       d2rt_b5_direct_n = 0; // creneaux couverts par l'index direct
    unsigned int       d2rt_b5_direct_lo = 0;// VA du creneau d'indice 0
}

// ---- LA CARTE DES FAMILLES du profil d'adresses (runtime/prof_map.h) -------
// Au SCOPE GLOBAL, pour la meme raison que les compteurs ci-dessus : ces deux
// accesseurs sont des symboles du moteur, et les aides en ligne doivent etre
// visibles depuis le namespace anonyme plus bas.
static Wx86ProfMap g_profMap;
void wx86_prof_set_map(const Wx86ProfMap& m) { g_profMap = m; }
const Wx86ProfMap& wx86_prof_map() { return g_profMap; }

// La CLASSIFICATION elle-meme vit dans prof_map.h (fonctions en ligne), pour
// qu'un oracle de bureau puisse l'exercer : cette unite-ci ne se compile que
// pour ARM/Vita. Ici, seules les trois fenetres de detail sont cablees a leurs
// compteurs. Elles ne dependent PLUS de la famille trouvee : le code d'avant
// testait `b == 4`, puis `b == 6 && rva dans [0x0d0000,0x0f0000)`, ce qui etait
// exactement « rva dans la fenetre » pour la carte d'alors — deux fenetres qui
// ne se recouvrent pas donnent les memes comptes qu'un if/else-if.
static inline void wx86_prof_zooms(uint32_t rva) {
    const Wx86ProfMap& m = g_profMap;
    int k;
    if ((k = wx86_prof_zoom(m.zoom_4k_a, 12, 32, rva)) >= 0) ++d2rt_eipprof_sub[k];
    if ((k = wx86_prof_zoom(m.zoom_4k_b, 12, 32, rva)) >= 0) ++d2rt_eipprof_sub2[k];
    if ((k = wx86_prof_zoom(m.zoom_256,   8, 16, rva)) >= 0) ++d2rt_eipprof_fn[k];
}

namespace d2rt {
namespace {

// Diagnostic SIGSEGV/SIGBUS handler: with the identity memory model a guest
// wild access is a host fault — print the guest state (EIP & friends) before
// dying so the crash is attributable. Async-signal-unsafe printf is fine for
// a terminal diagnostic. This is also the seed of the future SMC handler.
extern "C" x86emu_t* dyn86_diag_emu = nullptr;
// Crash-log fd (rt_boot opens $D2WRITE/crash.log and sets this). diag_segv mirrors
// a concise one-line summary here with async-signal-safe write() BEFORE the risky
// guest-stack scans below — so a host SIGSEGV leaves a durable post-mortem line
// even if the scan itself re-faults. -1 = no crash log (write() is skipped).
extern "C" int dyn86_crash_fd = -1;
static uintptr_t g_mb;   // guest->host membase (defined once; H(va)=va+g_mb). Fwd for diag_segv.
#ifndef __vita__
static void diag_segv(int sig, siginfo_t* si, void* uctx) {
    x86emu_t* e = dyn86_diag_emu;
    uintptr_t pc = 0, lr = 0, sp = 0, r0 = 0, r1 = 0, r2 = 0;
    uintptr_t live[8] = {0}; bool live_ok = false;
#if defined(__arm__)
    if (uctx) {
        auto& mc = ((ucontext_t*)uctx)->uc_mcontext;
        pc = mc.arm_pc; lr = mc.arm_lr; sp = mc.arm_sp;
        r0 = mc.arm_r0; r1 = mc.arm_r1; r2 = mc.arm_r2;
        // Convention box86/ARM : r4..r11 SONT les huit registres x86 VIVANTS
        // (EAX ECX EDX EBX ESP EBP ESI EDI). emu->ip/emu->regs ne sont
        // synchronises qu'aux frontieres de bloc : au milieu d'un dynablock ils
        // sont PERIMES, et c'est ce qui a fait perdre du temps le 05/09. Ces
        // huit-la sont exacts a l'instruction fautive.
        live[0]=mc.arm_r4; live[1]=mc.arm_r5; live[2]=mc.arm_r6; live[3]=mc.arm_r7;
        live[4]=mc.arm_r8; live[5]=mc.arm_r9; live[6]=mc.arm_r10; live[7]=mc.arm_fp;
        live_ok = true;
    }
#endif
    if (live_ok)
        fprintf(stderr, "\n[cpu_box86] x86 VIVANTS (r4..r11) EAX=%08x ECX=%08x EDX=%08x EBX=%08x ESP=%08x EBP=%08x ESI=%08x EDI=%08x",
                (unsigned)live[0],(unsigned)live[1],(unsigned)live[2],(unsigned)live[3],
                (unsigned)live[4],(unsigned)live[5],(unsigned)live[6],(unsigned)live[7]);
    // Durable crash line FIRST (before the guest-stack scans, which may re-fault).
    if (dyn86_crash_fd >= 0) {
        char cb[224];
        int n = snprintf(cb, sizeof cb,
            "CRASH host-sig=%d fault-addr=%p host-pc=%p guest-EIP=%08x EAX=%08x ECX=%08x EDX=%08x EBX=%08x ESP=%08x EBP=%08x ESI=%08x EDI=%08x\n",
            sig, si ? si->si_addr : nullptr, (void*)pc,
            e?e->ip.dword[0]:0, e?e->regs[0].dword[0]:0, e?e->regs[1].dword[0]:0, e?e->regs[2].dword[0]:0,
            e?e->regs[3].dword[0]:0, e?e->regs[4].dword[0]:0, e?e->regs[5].dword[0]:0,
            e?e->regs[6].dword[0]:0, e?e->regs[7].dword[0]:0);
        if (n > 0) { ssize_t w = write(dyn86_crash_fd, cb, (size_t)n); (void)w; fsync(dyn86_crash_fd); }
    }
    fprintf(stderr, "\n[cpu_box86] host signal %d at addr=%p host-pc=%p lr=%p sp=%p r0=%p r1=%p r2=%p",
            sig, si ? si->si_addr : nullptr, (void*)pc, (void*)lr, (void*)sp, (void*)r0, (void*)r1, (void*)r2);
    // which guest block was executing? (host-pc inside a dynarec chunk)
    if (dynablock_t* db = FindDynablockFromNativeAddress((void*)pc)) {
        fprintf(stderr, " | dynablock x86=%p size=%d hostoff=+0x%x",
                db->x86_addr, db->x86_size, (unsigned)(pc - (uintptr_t)db->block));
        // ADRESSE x86 EXACTE de l'instruction fautive. Reclamee par la
        // relecture profonde du 2026-08-25 (« remonter du PC hote a l'adresse
        // x86 exacte via la table instsize du dynablock plutot qu'imprimer
        // emu->ip », qui n'est synchronise qu'aux frontieres de bloc). La
        // table instsize donne, pour chaque instruction x86 traduite, sa
        // taille x86 et sa taille native en mots : on marche les deux jusqu'a
        // encadrer le PC hote. Une entree a 15 signifie « la suivante
        // continue » (encodage 4 bits de box86).
        if (db->instsize && db->x86_addr) {
            uintptr_t x86a = (uintptr_t)db->x86_addr, arma = (uintptr_t)db->block;
            int i = 0;
            while (db->instsize[i].x86 || db->instsize[i].nat) {
                int xs = 0, as = 0;
                do { xs += db->instsize[i].x86; as += db->instsize[i].nat * 4; ++i; }
                while (db->instsize[i-1].x86 == 15 || db->instsize[i-1].nat == 15);
                if (pc >= arma && pc < arma + (uintptr_t)as) {
                    fprintf(stderr, " x86-insn=0x%08x", (unsigned)DYN86_H2G(x86a));
                    if (dyn86_crash_fd >= 0) { char xb[64];
                        int xn = snprintf(xb, sizeof xb, "CRASH x86-insn=0x%08x\n", (unsigned)DYN86_H2G(x86a));
                        if (xn > 0) { ssize_t w = write(dyn86_crash_fd, xb, (size_t)xn); (void)w; } }
                    break;
                }
                arma += as; x86a += xs;
            }
        }
    }
    // a few return-address candidates off the host stack (caller of the faulting fn)
    if (sp) {
        fprintf(stderr, "\n[cpu_box86] host stack:");
        for (int i = 0; i < 24; ++i) {
            uintptr_t v = ((uintptr_t*)sp)[i];
            if (v >= 0x76000000 && v < 0x76400000) fprintf(stderr, " [%d]=%p", i, (void*)v);
        }
    }
    // NOTE: emu->ip is only synced at block boundaries/STM points — treat as approximate.
    if (e)
        fprintf(stderr, " | guest EIP=%08x EAX=%08x ECX=%08x EDX=%08x EBX=%08x ESP=%08x EBP=%08x ESI=%08x EDI=%08x",
                e->ip.dword[0], e->regs[0].dword[0], e->regs[1].dword[0], e->regs[2].dword[0],
                e->regs[3].dword[0], e->regs[4].dword[0], e->regs[5].dword[0],
                e->regs[6].dword[0], e->regs[7].dword[0]);
    // Guest call chain: scan the guest stack for return addresses (values in a
    // loaded-module code range). H(va)=va+g_mb, so the guest stack is directly
    // readable in the host address space. Env D2_CODELO/HI override the range.
    if (e) {   // g_mb may be 0 (identity mmap) — gesp+g_mb is still the host ptr
        uint32_t gesp = e->regs[4].dword[0];
        uint32_t lo = 0x01900000, hi = 0x02100000;   // compact-layout module window (forced-reloc Game.exe, 2026-08-25 squeeze); override via D2_CODELO/HI
        if (const char* s = getenv("WX86_CODELO") ? getenv("WX86_CODELO") : getenv("D2_CODELO")) lo = (uint32_t)strtoul(s, nullptr, 16);
        if (const char* s = getenv("WX86_CODEHI") ? getenv("WX86_CODEHI") : getenv("D2_CODEHI")) hi = (uint32_t)strtoul(s, nullptr, 16);
        const uint32_t* gs = (const uint32_t*)(uintptr_t)(gesp + g_mb);
        // Raw stack words: the faulting function's args (a -1 %s pointer + the
        // adjacent format-string pointer are visible here). ASCII-annotate any
        // word that points at a printable guest string.
        fprintf(stderr, "\n[cpu_box86] guest stack @%08x:", gesp);
        for (int i = 0; i < 16; ++i) {
            uint32_t v = gs[i];
            fprintf(stderr, " %08x", v);
            const char* p = (const char*)(uintptr_t)(v + g_mb);
            if (v > 0x1000 && v < 0x60000000) {
                char buf[20]; int n = 0; bool printable = true;
                for (; n < 16; ++n) { char c = p[n]; if (c == 0) break;
                    if (c < 0x20 || c > 0x7e) { printable = false; break; } buf[n] = c; }
                if (printable && n >= 3) { buf[n] = 0; fprintf(stderr, "(\"%s\")", buf); }
            }
        }
        // Return-address chain (text range only).
        fprintf(stderr, "\n[cpu_box86] guest call chain:");
        for (int i = 0, shown = 0; i < 256 && shown < 12; ++i) {
            uint32_t v = gs[i];
            if (v >= lo && v < hi) { fprintf(stderr, " %08x", v); ++shown; }
        }
    }
    fprintf(stderr, "\n");
    _exit(139);
}
#endif // !__vita__


// fastmmu: guest->host delta (env WX86_MEMBASE, repli D2MEMBASE; hex, low 24 bits zero; 0 =
// identity). The guest keeps its validated D2 layout; host pages live at
// va+g_mb — the exact Vita memory model (memblock VAs are kernel-assigned).
// g_mb defined above (forward-declared before diag_segv); initialized here.
// JOURNAL DU MOTEUR. Une sortie fatale precoce DOIT laisser une ligne : sur
// materiel il n'y a pas de stderr, et un _exit silencieux se lit
// « l'application se ferme au lancement » (rapport HW 2026-08-24 : mort pile a
// l'allocation de l'arene compacte).
//
// Le service appartient au moteur (platform/vita_host.h) : sur console il ecrit
// la ligne durable, hors console il ne fait rien. L'appel est DIRECT et en lien
// FORT — plus de reference faible a tester.
//
// Ce qu'il y avait avant, et pourquoi c'etait faux : une reference FAIBLE vers
// `d2vita_progress_c`, le nom du PREMIER consommateur. Un portage dont les
// symboles ne portent pas ce prefixe obtenait un journal muet, sans la moindre
// erreur de lien pour l'en avertir. Un moteur generique ne connait pas le nom
// de ses consommateurs.
#include "platform/vita_host.h"
static inline void* H(uint32_t va) { return (void*)((uintptr_t)va + g_mb); }
// C11 (deep review 2026-08-25): evaluated LAZILY, not as a pre-main static —
// on Vita the knob arrives via env.txt which platform_init applies AFTER static
// init, so a pre-main getenv could never see it. -1 = not yet checked.
static int g_memGuardV = -1;
static inline bool mem_guard(){ if(g_memGuardV<0) g_memGuardV = (std::getenv("WX86_MEMGUARD")||std::getenv("D2_MEMGUARD"))?1:0; return g_memGuardV!=0; }
// Single-arena mode (the exact Vita model): ONE host block covers the whole
// guest span [0, arena_size); membase = block base (kernel/host-assigned, not
// chosen); map()/set_trap() no longer host-mmap per region — the block already
// backs them. On Vita this block is one sceKernelAllocMemBlockForVM; here it is
// one lazy mmap (overcommit), proving the model without needing the compressed
// hardware layout yet. Enabled by WX86_ARENA=<hex bytes> (repli D2ARENA).
static bool     g_arena = false;
// (compteurs D2_EIPPROF : définis AU NIVEAU FICHIER, voir avant `namespace d2rt`)
#define g_eipProf        d2rt_eipprof_on
#define g_eipProfBase    d2rt_eipprof_base
#define g_eipProfBuckets d2rt_eipprof
// C3: usable guest span behind the single block ([0, g_arena_span)). 0 = unknown
// (sparse/identity layouts) -> the check below is skipped, exactly as before.
static uint32_t g_arena_span = 0;
static uint64_t g_arenaViol = 0;
// One cheap compare per shim-side deref. A guest VA past the block would alias
// host memory with no fault; name it (durably, once per 64) and refuse instead.
static inline bool arena_check(uint32_t va, uint32_t n, const char* what) {
    if (!g_arena_span) return true;                       // unknown span: legacy behaviour
    if ((uint64_t)va + n <= (uint64_t)g_arena_span) return true;
    if (++g_arenaViol <= 64) {
        fprintf(stderr, "[cpu_box86] C3 OUT-OF-ARENA %s va=0x%08x n=%u span=0x%08x\n", what, va, n, g_arena_span);
        { char m[128];
            snprintf(m, sizeof m, "C3: acces hors arene %s va=%08x n=%u (span=%08x)", what, va, n, g_arena_span);
            wx86_vita_progress_c(m); }
    }
    return false;
}

class CpuBox86;
static CpuBox86* g_self = nullptr;     // single instance (asserted in make_cpu_box86)

// Native backend: per-HOST-thread current emu. Zero (=> base emu_) on the
// main thread and under the cooperative backend — the historical single-emu
// behaviour is the TLS default, not a mode test.
#if defined(D2_TLSCOUNT) || defined(D2_B5CENSUS)
#define D2_TLSCOUNT_HIT() (++d2_tls_hits)
// RECENSEMENT B3 — compteur d'APPELS a E(), incremente AVANT le raccourci
// g_multi_emu. Les deux compteurs disent deux choses differentes et il faut
// les deux :
//   * d2_tls_hits = chaines emutls REELLEMENT payees dans CE run. Sous coop le
//     raccourci les met a zero : le chiffre est donc muet sur le regime livre.
//   * d2_e_calls  = appels a E(), c'est-a-dire le nombre de chaines que le
//     regime LIVRE (D2SCHED=native, ou g_multi_emu est vrai par construction)
//     paierait pour le meme travail invite. C'est ce compteur qui sert de
//     recensement, parce qu'il est INDEPENDANT DE L'ORDONNANCEUR : le banc
//     deterministe (coop, D2_VIRTCLOCK) le rend identique au banc natif pour
//     le meme scenario, alors que d2_tls_hits ne le peut pas.
// Un prix unitaire n'est PAS deduit ici : ce compteur est un COMPTE, et le
// prix de la chaine se mesure sur console, pas sous qemu.
extern "C" { unsigned long long d2_e_calls = 0; }
#define D2_ECALL() (++d2_e_calls)
#else
#define D2_TLSCOUNT_HIT() ((void)0)
#define D2_ECALL()        ((void)0)
#endif
// ---- RECENSEMENT B5 (build de MESURE : -DD2_B5CENSUS, jamais livre) --------
// Ce que chaque compteur mesure, EXACTEMENT :
//   appels  = entrees dans try_intrinsic (= une par trap invite->hote servi ou
//             non par le chemin rapide) ;
//   lectures= acces a la TABLE des intrinseques (une par iteration de sondage
//             cote heritage ; zero cote index direct quand le creneau est hors
//             de la fenetre) ;
//   servis  = appels ou l'intrinseque a rendu vrai (donc le trap n'est jamais
//             entre dans le pont) ;
//   emutls  = resolutions d'etat par fil (E()) FACTUREES pendant le corps de
//             l'intrinseque. C'est un COMPTE, pas une estimation : E() est le
//             seul site qui devienne __emutls_get_address sur Vita, et il est
//             instrumente a la source. Il vaut 0 sous coop PAR CONSTRUCTION
//             (le raccourci g_multi_emu rend emu_ sans toucher au TLS) : le
//             recensement de ce poste ne se fait que sous D2SCHED=native.
// Toujours DEFINIS (pour que rt_boot les imprime sans #ifdef), incrementes
// UNIQUEMENT sous D2_B5CENSUS : un ++ inconditionnel sur ce chemin serait la
// faute PROF_COUNTERS (4,4 % mesures console).
// (definitions remontees au scope global — voir plus haut : dans un
// namespace ANONYME, extern "C" ne suffit pas a donner une liaison
// externe, et les symboles restaient decores.)
// D2_B5INDEX=1 (ou D2_B5=1) : index DIRECT dans la repartition des
// intrinseques, a la place du sondage lineaire. DEFAUT = ANCIEN CHEMIN.
static bool g_b5index = false;
#ifdef D2_B5CENSUS
// TEST D'ECHELLE du recensement (build de MESURE uniquement — ce code n'existe
// pas dans le binaire livre). D2_B5SCALE=N refait, par intrinseque servie,
// N-1 fois EXACTEMENT le travail que les deux compteurs pretendent mesurer :
// une lecture de la table des cles, et une resolution d'etat par fil (E()).
// Les compteurs doivent alors monter de (N-1) par service — un compteur qui
// ne bouge pas ne mesure pas ce qu'il annonce.
// AUCUN EFFET DE BORD : la lecture de ikey_[] et celle de regs[R_ESP] sont
// pures, et le resultat part dans un puits global jamais relu.
static uint32_t g_b5scale = 1;
extern "C" { unsigned long long d2rt_b5_sink = 0; }
#endif
// ⚡ ROLLBACK DES OPTIMISATIONS EMUTLS — D2_NOEMUOPT=1.
// Sous natif, la SEULE optimisation emutls encore active dans la boucle chaude
// est le HISSAGE : E() resolu une fois par tranche au lieu d'a chaque acces
// (le raccourci g_multi_emu, lui, ne sert que sous coop — sous natif le
// drapeau est vrai et l'ancien chemin reprend a l'identique). Ce drapeau le
// demonte, et retablit l'ecriture de t_fault_addr par trap, retiree pour la
// meme raison. But : un A/B sur console SANS quitter D2SCHED=native.
// Lu UNE fois : un getenv par tranche couterait plus que ce qu'on mesure.
static bool g_noEmuOpt = false;
// coeur2 (06/09/2026) — D2_FILSTAT=1 : densite de traps et contention du GIL
// PAR FIL (x86emu_t::dyn86_traps / dyn86_gilcont / dyn86_gilwait_us). Lu UNE
// fois. Defaut ETEINT : le chemin de trap garde son gil::lock() nu, plus un
// test de booleen global (le patron de tous les knobs de ce fichier).
// Arme aussi par D2_COEUR_SERVEUR (l'experience porte toujours sa mesure).
static bool g_filstat = false;
static __thread x86emu_t* t_emu = nullptr;
// D2_TIMEPROF : etat de l'asservissement, PAR FIL. Au niveau fichier et non
// membre static de la classe : un membre `static __thread` exige une definition
// hors classe, et le namespace anonyme de `d2rt` decore alors le symbole (meme
// piege que le pave d'avertissement en tete de ce fichier).
static __thread uint64_t tp_last_us = 0;
static __thread int      tp_budget  = 0;
// Faux tant qu'aucun emu PAR FIL n'existe (donc tant que t_emu est nul partout,
// cf. la demonstration au-dessus de E()). Pose une seule fois, avant tout
// pthread_create de runner : le happens-before du pthread_create suffit, le
// relaxed est ici de l'hygiene, pas une precaution utile.
static std::atomic<bool> g_multi_emu{false};
// Fault/preempt state was shared members; with N host threads running run()
// concurrently they must be per-thread (a worker's fault must not clobber the
// main thread's pending one).
static __thread int         t_preempt = 0;
static __thread const char* t_fault = nullptr;
static __thread uint32_t    t_fault_addr = 0;
static __thread uint32_t    t_fault_err = 0;

// coeur2 : prise du GIL a la fenetre de trap, avec COMPTAGE PAR FIL sous
// D2_FILSTAT=1. Sans le knob : exactement gil::Guard (lock/unlock nus). Avec :
// trylock d'abord ; s'il echoue la prise est CONTENDUE — on la compte et on
// chronometre le lock bloquant (clock_gettime SEULEMENT sur ce chemin rare,
// jamais sur une prise libre). Les compteurs vivent dans l'emu du fil : c'est
// ce que la ligne fils: du battement famine lit (thread_emu_filstat).
static inline uint32_t filstat_now_us() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull);
}
struct TrapGuard {
    bool a;
    explicit TrapGuard(x86emu_t& e) {
        a = gil::active(); if (!a) return;
        if (!g_filstat) { gil::lock(); return; }
        ++e.dyn86_traps;
        if (gil::try_lock()) return;
        ++e.dyn86_gilcont;
        const uint32_t t0 = filstat_now_us();
        gil::lock();
        e.dyn86_gilwait_us += filstat_now_us() - t0;
    }
    ~TrapGuard() { if (a) gil::unlock(); }
    TrapGuard(const TrapGuard&) = delete; TrapGuard& operator=(const TrapGuard&) = delete;
};

class CpuBox86 : public Cpu {
public:
    CpuBox86() {
        // Rollback des optimisations emutls, lu UNE fois (env.txt est deja
        // charge a la construction : les lignes « env.txt: » sortent a 0,00 s,
        // l'arene est sondee a 1,00 s).
        g_noEmuOpt = getenv("WX86_NOEMUOPT") || getenv("D2_NOEMUOPT");
        {   const char* fs = getenv("WX86_FILSTAT"); if (!fs) fs = getenv("D2_FILSTAT");
            const char* cs = getenv("WX86_COEUR_SERVEUR"); if (!cs) cs = getenv("D2_COEUR_SERVEUR");
            g_filstat = (fs && fs[0] && !(fs[0]=='0' && !fs[1]))
                     || (cs && cs[0] && !(cs[0]=='0' && !cs[1])); }
        // B5 : lu UNE fois (un getenv par trap couterait plus que ce qu'on
        // mesure). « 0 » vaut explicitement ETEINT, comme INLINEHOT.
        {   auto on = [](const char* n){ const char* v = getenv(n);
                                        return v && v[0] && !(v[0]=='0' && !v[1]); };
            g_b5index = on("WX86_B5INDEX") || on("D2_B5INDEX") || on("WX86_B5") || on("D2_B5");
            d2rt_b5_index_on = g_b5index ? 1u : 0u; }
#ifdef D2_B5CENSUS
        if (const char* sc = getenv("WX86_B5SCALE") ? getenv("WX86_B5SCALE") : getenv("D2_B5SCALE")) {
            unsigned long v = strtoul(sc, nullptr, 10);
            if (v >= 1 && v <= 1024) g_b5scale = (uint32_t)v;
        }
#endif
        if (g_noEmuOpt)
            wx86_vita_progress_c("emutls: OPTIMISATIONS DESACTIVEES (D2_NOEMUOPT=1) — "
                                 "E() resolu a chaque acces, t_fault_addr reecrit par trap");
        const char* as = getenv("WX86_ARENA"); if (!as) as = getenv("D2ARENA");
        if (as) {
            // ⚡ 12/09 : VOIE 5.2 (jit_budget_20260908.md §5.2) TENTEE ET
            // REFUTEE. L'idee — reserver la piscine JIT avant l'arene,
            // agrandie du rabiot d'alignement mesure (16+13=29 Mo) — supposait
            // qu'un bloc ForVM pouvait depasser 16 Mio. FAUX : verifie sur
            // console, 17 Mo ET 29 Mo sont TOUS DEUX refuses
            // (sce=0x80024B0B, SCE_KERNEL_ERROR_MEMBLOCK_OVERFLOW) — 16 Mio
            // (0x1000000) est un PLAFOND NOYAU par bloc VM, pas un choix de ce
            // projet. Effet revele en route, PAS introduit par cette tentative :
            // jitpool_reserve() (mman_vita.c) marque g_jitpool_tried=1 AVANT de
            // connaitre le resultat de l'allocation — deja le cas dans le
            // chemin paresseux a 16 Mio, avant ce chantier. Un refus GARANTI a
            // 29 Mio l'a juste rendu visible ; le meme bug latent reste dans le
            // chemin a 16 Mio aujourd'hui (un refus, quelle qu'en soit la
            // cause, y desarmerait la piscine pour le reste de la session,
            // sans retentative, seule trace : la ligne REFUSEE). Non corrige —
            // change un comportement, pas seulement un diagnostic. Voir
            // docs/audit/repartition_ram_20260912.md pour la mesure complete.
            // Le rabiot d'alignement de l'arene (13 Mio ce soir) reste donc
            // un gachis REEL mais SANS solution a cout nul : le recuperer
            // demanderait de rogner une autre marge deja calibree (voie 5.1).
            uint64_t sz = strtoull(as, nullptr, 16);
            // Page-granular size: only the guest->host DELTA needs 16 MiB
            // alignment, and D2ARENA already carries the 16 MiB slack for that.
            // Rounding the SIZE up to 16 MiB too asked the kernel for 7 MB more
            // than needed — the difference between booting and failing on real
            // hardware (2026-08-24: free user = 294 MB vs 304 MB requested).
            sz = (sz + 0xFFFull) & ~0xFFFull;
            // ⚡ 08/09 : NE RESERVER QUE CE QU'IL FAUT. D2ARENA porte 16 Mio
            // de marge dont l'unique role est de pouvoir arrondir membase au
            // multiple de 16 Mio (contrainte du ADD imm8-ror-8). On payait ces
            // 16 Mio A TOUS LES COUPS. Mesure sur la console :
            //     base=0x83d00000 membase=0x84000000 span=294 Mo reserve=297 Mo
            //     perdu-alignement=3072 Ko
            // soit 3 Mio reellement perdus, 281 Mio necessaires, et 13 Mio de
            // queue jamais touchee — alors que la marge libre totale est de
            // 11 Mio et que le jeu meurt en std::bad_alloc apres quelques
            // minutes.
            //
            // On essaie donc des marges CROISSANTES et on garde la premiere qui
            // laisse l'etendue requise apres alignement. La derniere echelle
            // vaut l'ancien comportement : le pire cas est inchange, le cas
            // courant rend une dizaine de Mio.
            // ⚡ 08/09, 2e passe : SONDER LA BASE PLUTOT QUE DE TIRER AU SORT.
            // L'echelle de marges croissantes rendait 285 Mo un lancement et
            // 297 le suivant — selon la base que le noyau veut bien donner, qui
            // depend elle-meme des allocations precedentes. Or a 297 il ne
            // reste que 5 Mo, le JIT ne peut plus grandir, et comme il n'y a
            // PAS d'interpreteur (shim_impl.c), tout bloc refuse tue le fil
            // invite. La taille de l'arene n'est donc pas un reglage de
            // confort : elle decide si le jeu vit.
            //
            // On sonde donc l'allocateur avec un petit bloc, on lit la base
            // qu'il propose, on en deduit la marge d'alignement EXACTE, puis on
            // demande need+cette-marge. Le repli sur need+16 Mio (l'ancien
            // calcul) reste en dernier ressort.
            const uint64_t need = (sz > 0x1000000ull) ? sz - 0x1000000ull : sz;
            void* blk = MAP_FAILED;
            {
                uint64_t slack = 0x1000000ull;          // defaut = ancien calcul
                void* probe = mmap(nullptr, 0x100000, PROT_READ|PROT_WRITE,
                                   MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
                if (probe != MAP_FAILED) {
                    const uintptr_t pb = (uintptr_t)probe;
                    slack = (uint64_t)((((pb + 0xFFFFFFu) & ~(uintptr_t)0xFFFFFFu) - pb));
                    munmap(probe, 0x100000);
                }
                for (int att = 0; att < 2 && blk == MAP_FAILED; ++att) {
                    const uint64_t try_sz = need + slack;
                    void* t = mmap(nullptr, try_sz, PROT_READ|PROT_WRITE,
                                   MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
                    if (t == MAP_FAILED) { slack = 0x1000000ull; continue; }
                    const uintptr_t tb = (uintptr_t)t;
                    const uintptr_t mb = (tb + 0xFFFFFFu) & ~(uintptr_t)0xFFFFFFu;
                    if ((uint64_t)(tb + try_sz - mb) >= need) { blk = t; sz = try_sz; }
                    else { munmap(t, try_sz); slack = 0x1000000ull; }
                }
            }
            if (blk == MAP_FAILED)      // dernier recours : exactement l'ancien calcul
                blk = mmap(nullptr, sz, PROT_READ|PROT_WRITE,
                           MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
            if (blk == MAP_FAILED) { fprintf(stderr, "[cpu_box86] D2ARENA mmap %llx failed\n",
                                             (unsigned long long)sz);
                { char m[112];
                    snprintf(m, sizeof m, "FATAL: arena alloc failed (%llu MB) — see the mmap FAIL line above",
                             (unsigned long long)(sz >> 20));
                    wx86_vita_progress_c(m); }
                _exit(2); }
            // membase must have zero low 24 bits (single-ADD imm8-ror-8). mmap is
            // page-aligned; round the *guest→host delta* up to 16 MiB and rely on
            // NORESERVE slack (the arena is oversized by one granule for this).
            uintptr_t base = (uintptr_t)blk;
            g_mb = (base + 0xFFFFFFu) & ~(uintptr_t)0xFFFFFFu;
            g_arena = true;
            // C3 (deep review): with a single block, H(va)=va+membase NEVER
            // faults — a wild guest pointer silently aliases whatever host
            // object sits there (the JIT cache, the newlib heap), so corruption
            // surfaces far from its cause. Record the span so shim-side derefs
            // can be range-checked (see g_arena_span / arena_check below).
            g_arena_span = (uint32_t)((base + sz > g_mb) ? (base + sz - g_mb) : 0);
            // Rendre le gachis d'alignement VISIBLE : c'est lui qu'on cherche a
            // supprimer, et sans ce releve on ne saurait pas s'il a coute 0 ou
            // 16 Mio sur ce lancement.
            { char m[152];
                snprintf(m, sizeof m, "arene: base=%p membase=%p span=%u Mo reserve=%llu Mo perdu-alignement=%u Ko",
                         (void*)base, (void*)g_mb, (unsigned)(g_arena_span>>20),
                         (unsigned long long)(sz>>20), (unsigned)((g_mb-base)>>10));
                wx86_vita_progress_c(m); }
            dyn86_set_membase(g_mb);
            // Meme etendue pour le garde-fou des intrinseques memcpy/memset
            // (dyn86_memintrin.h) : le helper natif ecrit en memoire invitee
            // sans passer par write()/arena_check, il doit donc porter la
            // MEME borne, sinon un pointeur sauvage aliaserait le tas hote.
            dyn86_mi_set_span(g_arena_span);
            fprintf(stderr, "[cpu_box86] arena: block=%p size=0x%llx membase=0x%lx (Vita single-block model)\n",
                    blk, (unsigned long long)sz, (unsigned long)g_mb);
        } else if (const char* mbs = getenv("WX86_MEMBASE") ? getenv("WX86_MEMBASE")
                                                            : getenv("D2MEMBASE")) {
            g_mb = strtoul(mbs, nullptr, 16);
            if (g_mb & 0xFFFFFFu) { fprintf(stderr, "[cpu_box86] D2MEMBASE low 24 bits must be 0\n"); _exit(2); }
            dyn86_set_membase(g_mb);
            fprintf(stderr, "[cpu_box86] fastmmu membase=0x%08x\n", (unsigned)g_mb);
        }
        g_self = this;
        std::memset(&ctx_, 0, sizeof ctx_);
        pthread_mutex_init(&ctx_.mutex_dyndump, nullptr);
        pthread_mutex_init(&ctx_.mutex_lock, nullptr);
        my_context = &ctx_;
        init_custommem_helper(&ctx_);
        dyn86_init();                  // capture the pristine jump-table default
        std::memset(&emu_, 0, sizeof emu_);
        emu_.context = &ctx_;
        dynarec86_setup_emu_helpers(&emu_);
        emu_.df = d_none;
        dyn86_diag_emu = &emu_;
#ifndef __vita__
        struct sigaction sa;
        std::memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = diag_segv;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, nullptr);
        sigaction(SIGBUS, &sa, nullptr);
#endif
    }

    // Current-emu accessor: the TLS binding if a native runner bound one,
    // else the base emu — every register/run/save path below goes through it.
    // D2_TLSCOUNT : build de MESURE uniquement (jamais livre, jamais par
    // defaut). Compte les resolutions de t_emu, c'est-a-dire EXACTEMENT ce
    // qui devient un __emutls_get_address sur Vita (toolchain vitasdk
    // --disable-tls). Le desassemblage de l'eboot dit qu'un appel a reg() ou
    // set_reg() en vaut UN (les 3 sites statiques de reg() sont des chemins
    // ALTERNATIFS : i<=7, i==8, i==9), donc ce compteur est un compte d'appels
    // emutls, pas une estimation. C'est une BORNE INFERIEURE du total par
    // trap : les t_* de bridge.cpp (t_redirect_eip, t_yield_pending) et
    // t_cur de sched_native s'y ajoutent, non comptes ici.
    // ---- Le raccourci qui supprime la chaine emutls sous coop ---------------
    //
    // MESURE QUI MOTIVE CECI (audit t12 §27.5) : E() est appele 16,81 fois par
    // trap Bridge sous coop — le defaut LIVRE — et chaque appel resout t_emu.
    // Sur Vita la chaine du toolchain vitasdk (--disable-tls) est
    // __emutls_get_address -> pthread_getspecific -> pte_osTlsGetValue ->
    // sceKernelGetTLSAddr, un appel INTER-MODULE facture 1,29 us au banc
    // console du 2026-08-30. Dix-sept par trap, ~178 traps par image.
    //
    // L'INVARIANT QUI REND LE RACCOURCI SUR, et il se prouve en trois pas :
    //   1. t_emu n'est jamais rendu non nul que par thread_emu_bind(emu != 0) ;
    //   2. le SEUL appelant avec un emu non nul est NativeScheduler::runner
    //      (sched_native.cpp:501) — le main se lie a nullptr (:902) ;
    //   3. un emu par fil ne peut venir que de thread_emu_create(), et le seul
    //      site hors NativeScheduler est la sonde de rt_boot.cpp:2744, qui est
    //      A L'INTERIEUR de la branche D2SCHED=native.
    // Donc : tant que thread_emu_create() n'a jamais ete appele, t_emu vaut
    // nullptr sur TOUS les fils, et « t_emu ? *t_emu : emu_ » vaut emu_ par
    // construction. Le raccourci ne choisit pas une valeur differente : il
    // evite de DEMANDER une valeur dont la reponse est deja connue.
    //
    // COUT DU RACCOURCI : un chargement global relaxed et un branchement,
    // contre ~570 cycles. Sous natif, rien ne change — le drapeau est vrai et
    // l'ancien chemin reprend a l'identique.
    //
    // L'invariant n'est pas seulement affirme : thread_emu_bind le VERIFIE
    // (plus bas), sur un chemin froid, et le crie s'il tombe.
    x86emu_t& E() {
        D2_ECALL();
        if (!g_multi_emu.load(std::memory_order_relaxed)) return emu_;
        D2_TLSCOUNT_HIT(); return t_emu ? *t_emu : emu_;
    }
    const x86emu_t& E() const {
        D2_ECALL();
        if (!g_multi_emu.load(std::memory_order_relaxed)) return emu_;
        D2_TLSCOUNT_HIT(); return t_emu ? *t_emu : emu_;
    }

    bool map(uint32_t va, uint32_t size, const void* init, int prot) override {
        uint32_t a = va & ~0xFFFu;
        uint32_t end = (va + size + 0xFFFu) & ~0xFFFu;
        // Arena mode: the single block already backs [0, arena) — no per-region
        // mmap (exactly how a Vita VM memblock works: one allocation, addressed
        // by offset). Otherwise host-mmap this region RWX at H(a).
        if (!g_arena) {
            void* p = mmap(H(a), end - a, PROT_READ|PROT_WRITE|PROT_EXEC,
                           MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE, -1, 0);
            if (p == MAP_FAILED || p != H(a)) {
                fprintf(stderr, "[DEBUG map] va=0x%x size=%u a=0x%x end=0x%x H(a)=%p errno=%d(%s) p=%p\n",
                        va, size, a, end, H(a), errno, strerror(errno), p);
                return false;
            }
        }
        if (init) std::memcpy(H(va), init, size);
        setProtection(a, end - a, host_prot(prot));    // custommem bookkeeping
        return true;
    }
    bool protect(uint32_t va, uint32_t size, int prot) override {
        updateProtection(va, size, host_prot(prot));   // keeps PROT_DYNAREC bits
        return true;
    }
    bool read(uint32_t va, void* dst, uint32_t n) override {
        // D2_MEMGUARD: a shim handed a bad guest pointer would fault __memcpy_neon
        // deep in the host (uncatchable box86 SIGSEGV). Under the guard, verify the
        // page is mapped first; if not, log the address and hand back zeros so the
        // run survives and the culprit address is visible instead of a core dump.
        // custommem's protection map is keyed by GUEST address (setProtection in
        // map(), and the dynarec's getProtection/protectDB all use guest VAs), so
        // query `va` — NOT H(va). Under membase!=0 (Vita) the old H(va) key found
        // nothing and made every guarded read return zeros. (C11, deep review.)
        if (mem_guard() && getProtection((uintptr_t)va) == 0) {
            static int rn = 0;
            if (rn++ < 64) std::fprintf(stderr,
                "  [box86 guard] shim read of UNMAPPED guest 0x%08x (n=%u) -> zeros\n", va, n);
            std::memset(dst, 0, n);
            return false;
        }
        if (!arena_check(va, n, "read")) { std::memset(dst, 0, n); return false; }   // C3
        std::memcpy(dst, (const void*)H(va), n);
        return true;
    }
    bool write(uint32_t va, const void* src, uint32_t n) override {
        // A translated page is host-write-protected (protectDB); lift it and
        // mark the affected dynablocks dirty before the host-side write.
        if (!arena_check(va, n, "write")) return false;                              // C3
        if (isprotectedDB(va, n)) unprotectDB(va, n, 1);
        std::memcpy(H(va), src, n);
        return true;
    }
    // Honest dynarec invalidation for FlushInstructionCache / VirtualProtect:
    // mark any translated block in the range needtest, exactly like the write()
    // barrier does. Re-hash on next entry -> re-translate if the bytes changed.
    void invalidate_code(uint32_t va, uint32_t size) override {
        if (size == 0) size = 1;
        if (isprotectedDB(va, size)) unprotectDB(va, size, 1);
    }
    // Voir runtime/cpu.h : DESTRUCTION inconditionnelle, pas un marquage
    // conditionne a isprotectedDB. cleanDBFromAddressRange est cle par ADRESSE
    // INVITEE (cf. dynarec/dynarec_arm_functions.c:399).
    void discard_code(uint32_t va, uint32_t size) override {
        if (size == 0) return;
        cleanDBFromAddressRange((uintptr_t)va, (size_t)size, 1 /* destroy */);
    }
    // C3: callers use hostptr for bulk shim I/O (ReadFile, gzero, present) — a
    // wild VA there would memcpy straight into a host object. Null makes every
    // caller fall back to read()/write(), which are themselves range-checked.
    void* hostptr(uint32_t va, uint32_t n = 1) override {
        if (!arena_check(va, n, "hostptr")) return nullptr;
        return H(va); }
    // custommem tracks every map()ed region via setProtection — zero means no
    // guest region covers this address (a raw garbage pointer from the guest).
    bool mapped(uint32_t va) override { return getProtection((uintptr_t)va) != 0; }   // guest-keyed (C11)
    const uint32_t* ip_ptr() const override { return &E().ip.dword[0]; }

    uint32_t reg(int r) override {
        if (r >= R_EAX && r <= R_EDI) return E().regs[r].dword[0];
        if (r == R_EIP) return E().ip.dword[0];
        if (r == R_EFLAGS) {
            UpdateFlags(&E());         // materialize Box86's deferred flags
            return E().eflags.x32;
        }
        return 0;
    }
    void set_reg(int r, uint32_t v) override {
        if (r >= R_EAX && r <= R_EDI) { E().regs[r].dword[0] = v; return; }
        if (r == R_EIP) { E().ip.dword[0] = v; return; }
        if (r == R_EFLAGS) {
            E().eflags.x32 = v;
            E().df = d_none;           // the value is now authoritative
        }
    }

    // Surcharges groupees (cf. cpu.h) : UNE resolution d'emu au lieu de cinq.
    // R_ESP est dans la plage des registres generaux (cpu.h l. 21), donc
    // e.regs[R_ESP] est bien le meme emplacement que celui qu'ecrivait
    // set_reg(R_ESP, ...). read_u32 ne resout rien : il passe par read()/hostptr.
    // Lecture groupee des huit generaux (cf. cpu.h) : UNE resolution d'emu.
    void regs_gp(uint32_t* out) override {
        const x86emu_t& e = E();
        for (int r = R_EAX; r <= R_EDI; ++r) out[r] = e.regs[r].dword[0];
    }
    uint32_t trap_retaddr() override {
        return read_u32(E().regs[R_ESP].dword[0]);
    }
    void trap_epilogue(uint32_t eax, uint32_t esp_add, uint32_t eip) override {
        x86emu_t& e = E();
        e.regs[R_EAX].dword[0]  = eax;
        e.regs[R_ESP].dword[0] += esp_add;
        e.ip.dword[0]           = eip;
    }
    // B5 : trap stdcall argc=0 — UNE resolution d'emu pour la lecture de
    // l'adresse de retour ET les trois ecritures, au lieu de deux (une par
    // trap_retaddr(), une par trap_epilogue()). read_u32 ne resout rien : il
    // passe par read()/hostptr, jamais par E().
    void trap_ret0(uint32_t eax) override {
        x86emu_t& e = E();
        const uint32_t esp = e.regs[R_ESP].dword[0];
        const uint32_t ret = read_u32(esp);
        e.regs[R_EAX].dword[0] = eax;
        e.regs[R_ESP].dword[0] = esp + 4;
        e.ip.dword[0]          = ret;
    }

    void set_fs_base(uint32_t base) override {
        E().segs_offs[_FS] = base;
        E().segs_serial[_FS] = 1;      // emitted FS path: serial!=0 -> use offs
    }
    bool fs_base_is_direct() const override { return true; }

    // Scheduler slice budget. Unicorn counts native instructions; here the
    // unit is BLOCK ENTRIES — every translated block's emitted prologue
    // decrements emu->dyn86_budget (recharged in run()) and exits resumable
    // when it expires. Same ~8 guest insns/block scale as the old LinkNext
    // chains, but blocks stay DIRECT-LINKED (no arm_next round-trip per
    // transition). Residual (unchanged): a loop inside ONE dynablock never
    // re-enters the prologue and cannot be preempted this way.
    void set_run_limit(uint64_t n) override {
        limit_blocks_ = n ? (uint32_t)((n / 8) ? (n / 8) : 1) : 0;
    }
    // True for both break causes (chain-quantum expiry AND request_stop) —
    // deliberately mirroring CpuUnicorn, which also reports limit_hit_ for a
    // request_stop() that lands while a budget is set. The scheduler treats
    // both identically: slice over, thread still Ready.
    bool take_limit_hit() override { int p = t_preempt; t_preempt = 0; return p != 0; }

    // Warden-safe per-slot fast path (docs/perf, Tier 1). Ce test s'execute sur
    // TOUTES les traversees invite->hote (3400 a 4500 par image), pas seulement
    // sur les quelques-unes qui touchent : le balayage lineaire d'avant faisait
    // donc payer la table entiere a chaque trap qui ne trouve rien.
    // Remplace par une table de hachage a ADRESSAGE OUVERT (sondage lineaire),
    // figee apres l'amorcage : une seule lecture dans le cas courant.
    //   * cle = la VA du slot ; 0 = case libre. Une VA de trap n'est JAMAIS 0
    //     (le pont les alloue depuis trap_base_, toujours > 0), donc 0 est un
    //     sentinel sur.
    //   * dispersion = va >> 4 : le pont espace les slots de 16 octets, cette
    //     division rend donc des indices CONSECUTIFS — la meilleure repartition
    //     possible, aucun agregat.
    //   * la table est dimensionnee a 4x le nombre max d'intrinseques (16), donc
    //     au plus 25 % pleine : le sondage s'arrete sur une case libre en une ou
    //     deux etapes, et NE PEUT PAS boucler (il reste toujours des vides).
    // Semantique inchangee : meme ensemble de slots, meme fonction appelee.
    static constexpr uint32_t kIntrinMax  = 16;
    static constexpr uint32_t kIntrinMask = 63;   // table de 64 cases (>= 4x kIntrinMax)
    // ---- B5 : l'INDEX DIRECT (D2_B5INDEX=1), et pourquoi il enleve du travail
    // Le sondage lineaire ci-dessus paie, sur CHAQUE trap invite->hote (3400 a
    // 4500 par image), une lecture dans ikey_[] — donc une ligne de cache — au
    // seul but de decouvrir que le creneau n'est PAS un intrinseque, ce qui est
    // le cas de l'ecrasante majorite d'entre eux.
    // Le pont alloue les creneaux tous les 16 octets a partir de trap_base_ :
    // l'ensemble des intrinseques enregistres occupe donc une FENETRE CONTIGUE
    // de VA, etroite (deux creneaux aujourd'hui). Une soustraction non signee
    // et une comparaison la testent SANS AUCUN ACCES MEMOIRE — un slot en
    // dessous de idir_lo_ enroule vers un entier enorme, donc rejete par la
    // meme comparaison. Dans la fenetre, l'indice est exact : plus de sondage,
    // plus de comparaison de cle, et UNE SEULE table au lieu de deux (ikey_ et
    // ifn_ etaient deux lignes de cache distinctes sur un succes).
    // Si la fenetre depassait kDirectMax creneaux, l'index n'est PAS arme et le
    // sondage reprend : idir_n_ == 0 rejette tout (voir rebuild_direct).
    static constexpr uint32_t kDirectMax = 256;   // 256 creneaux = 4 Ko de VA

    inline bool try_intrinsic(uint32_t slot) {
        if (no_intrinsics_) return false;
#ifdef D2_B5CENSUS
        ++d2rt_b5_calls;
        const unsigned long long tls0 = d2_tls_hits;
#endif
        bool served = false;
        if (g_b5index) {
            const uint32_t d = (slot - idir_lo_) >> 4;   // non signe : sous lo => enorme
            if (d < idir_n_) {
#ifdef D2_B5CENSUS
                ++d2rt_b5_loads;
#endif
                if (IntrinsicFn fn = idir_[d]) {
                    // MEME COMPTABILITE QUE LA JAMBE HERITAGE (ne pas retirer) :
                    // un creneau servi ici ne repasse jamais par
                    // Bridge::trap_handler, c'est donc le seul endroit ou il
                    // puisse etre compte ; et si fn rend faux, le trap part
                    // dans le pont, qui comptera lui-meme.
                    served = fn(*this, slot);
                    if (served) d2rt::trapcnt::bump(slot);
                }
            }
        } else {
            uint32_t i = (slot >> 4) & kIntrinMask;
            for (;;) {
#ifdef D2_B5CENSUS
                ++d2rt_b5_loads;
#endif
                uint32_t k = ikey_[i];
                if (k == slot) {
                    // Comptabiliser SEULEMENT quand l'intrinseque a reellement
                    // servi : un creneau servi ici ne repasse jamais par
                    // Bridge::trap_handler, c'est donc le seul endroit ou il peut
                    // etre compte ; et si fn rend faux, le trap part dans le pont,
                    // qui comptera lui-meme (sinon on compterait deux fois).
                    served = ifn_[i](*this, slot);
                    if (served) d2rt::trapcnt::bump(slot);
                    break;
                }
                if (!k) break;                     // case libre => absent
                i = (i + 1) & kIntrinMask;
            }
        }
#ifdef D2_B5CENSUS
        if (served) {
            for (uint32_t x = 1; x < g_b5scale; ++x) {     // test d'echelle
                ++d2rt_b5_loads;
                d2rt_b5_sink += ikey_[(slot >> 4) & kIntrinMask];
                d2rt_b5_sink += E().regs[R_ESP].dword[0];  // UNE resolution par tour
            }
            ++d2rt_b5_hits; d2rt_b5_emutls += d2_tls_hits - tls0;
        }
#endif
        return served;
    }
    void set_intrinsic(uint32_t va, IntrinsicFn fn) override {
        if (!va) return;                           // 0 est le sentinel « case libre »
        uint32_t i = (va >> 4) & kIntrinMask;
        for (uint32_t probe = 0; probe <= kIntrinMask; ++probe, i = (i + 1) & kIntrinMask) {
            if (ikey_[i] == va) { ifn_[i] = fn; rebuild_direct(); return; }   // remplacement
            if (!ikey_[i]) {
                if (intrin_n_ >= (int)kIntrinMax) return;        // meme plafond qu'avant
                ikey_[i] = va; ifn_[i] = fn; ++intrin_n_; rebuild_direct(); return;
            }
        }
    }
    // Reconstruit l'index direct depuis la table de hachage — chemin FROID
    // (deux appels au demarrage). La table de hachage reste la source de
    // verite : l'index n'en est qu'une projection, donc les deux jambes
    // servent EXACTEMENT le meme ensemble de creneaux et la meme fonction.
    void rebuild_direct() {
        uint32_t lo = 0xFFFFFFFFu, hi = 0;
        for (uint32_t i = 0; i <= kIntrinMask; ++i)
            if (ikey_[i]) { if (ikey_[i] < lo) lo = ikey_[i]; if (ikey_[i] > hi) hi = ikey_[i]; }
        idir_n_ = 0; d2rt_b5_direct_n = 0;
        if (!hi) return;
        const uint32_t span = ((hi - lo) >> 4) + 1;
        if (span > kDirectMax) return;             // fenetre trop large : index NON arme
        idir_lo_ = lo;
        for (uint32_t d = 0; d < span; ++d) idir_[d] = nullptr;
        for (uint32_t i = 0; i <= kIntrinMask; ++i)
            if (ikey_[i]) idir_[(ikey_[i] - lo) >> 4] = ifn_[i];
        idir_n_ = span;
        d2rt_b5_direct_n = span; d2rt_b5_direct_lo = lo;
    }
    void set_intrinsics_enabled(bool on) override { no_intrinsics_ = !on; }

    void set_alternate(uint32_t from, uint32_t to) override { dyn86_set_alternate(from, to); }
    void set_trap(uint32_t lo, uint32_t hi, TrapFn fn) override {
        trap_lo_ = lo; trap_hi_ = hi; trap_fn_ = std::move(fn);
        if (!trap_mapped_) {
            if (!g_arena) {
                void* p = mmap(H(lo), hi - lo, PROT_READ|PROT_WRITE|PROT_EXEC,
                               MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE, -1, 0);
                if (p == MAP_FAILED || p != H(lo)) return;
            }
            // one Box86 exit stub (CC 'S' 'C' 00 00 00 00) per 16-byte slot —
            // the Bridge allocates trap VAs 16 bytes apart (sentinel included)
            for (uint32_t s = lo; s < hi; s += 16) {
                uint8_t* b = (uint8_t*)H(s);
                b[0] = 0xCC; b[1] = 'S'; b[2] = 'C';
            }
            setProtection(lo, hi - lo, PROT_READ|PROT_EXEC);
            trap_mapped_ = true;
        }
    }

    void request_stop() override {
        // Zero EVERY emu's budget: the NEXT block entry's prologue exits
        // resumable at that block's start. dyn86_request_stop keeps the flag
        // for LinkNext (not-yet-translated targets). Native shutdown must
        // reach all runners AND the base/main emu, whichever host thread
        // calls this; `quit` also covers "not currently running".
        // Remaining limitation: a loop inside ONE dynablock can't be stopped.
        dyn86_request_stop();
        // Comptabiliser la tranche AVANT de casser le budget — meme discipline
        // que nudge_thread_budget ci-dessous, et pour la meme raison, mais ici
        // elle manquait. CE QUE CA CORRIGE : le budget zerote rend le garde
        // `b > 0` de thread_emu_blocks faux, donc tout lecteur POSTERIEUR au
        // shutdown ne recoit plus que dyn86_blocks, sans le delta de la tranche
        // en cours. Sous coop c'etait sans effet visible (les tranches sont
        // courtes et deja accumulees) ; sous NATIF le budget vaut 0x7FFFFFFF et
        // n'expire jamais, donc la tranche en cours EST tout le travail du fil
        // depuis son dernier reveil. Un recensement de fin de partie lisait
        // ainsi ~24 000 blocs la ou le jeu en avait execute des millions, et
        // seize fois plus d'images ne bougeaient ce chiffre que de 0,8 % — la
        // signature exacte d'un compteur fige, pas d'une mesure.
        // Famille « diagnostic qui ment » : le chiffre existait, il etait faux,
        // et rien dans la ligne ne le disait.
        save_slice(&emu_);
        emu_.dyn86_budget = 0; emu_.quit = 1;
        for (x86emu_t* e : emus_) { save_slice(e); e->dyn86_budget = 0; e->quit = 1; }
    }

    // Sauve la tranche en cours dans le total cumule, et DESARME pour qu'elle
    // ne soit pas comptee deux fois. Extrait de nudge_thread_budget, ou
    // l'ordre a ete etabli et commente ; request_stop l'a longtemps omis.
    // ORDRE LOAD-BEARING : (1) desarmer, ce qui annule le delta lu par un
    // observateur, PUIS (2) accumuler. Un releve tombe entre les deux
    // SOUS-estime d'une tranche ; l'ordre inverse ferait apparaitre un bond,
    // lu « ce fil galope » — le sens FAUX. `volatile` : cet ordre doit
    // survivre a l'optimiseur.
    static void save_slice(x86emu_t* e) {
        volatile int*      pa = &e->dyn86_armed;
        volatile uint32_t* pn = &e->dyn86_blocks;
        const int b = __atomic_load_n(&e->dyn86_budget, __ATOMIC_RELAXED);
        const int a = *pa;
        *pa = 0;                                                     // (1)
        if (b > 0 && a >= b) *pn = *pn + (uint32_t)(a - b);          // (2)
    }

    // Filet anti-famine (spec 2026-08-29 §3.3) : décalque du zérotage de
    // budget de request_stop ci-dessus, SANS quit ni g_stop (monotones,
    // shutdown-only — contrat cpu.h). Store atomique relaxed : le prologue
    // de bloc fait load-décrément-store sur dyn86_budget ; un zéro écrit
    // entre son load et son store est perdu — bénin, rattrapé à la fenêtre
    // de détection suivante. Appelant sous GIL (registre emus_ append-only).
    void nudge_thread_budget(void* emu) override {
        x86emu_t* e = emu ? (x86emu_t*)emu : &emu_;   // nullptr = emu de base (main)
        // Comptabilise la tranche AVANT de la casser (vivacité par fil, cpu.h) :
        // le poke détruit l'information « combien de blocs ce fil a exécuté »,
        // donc c'est ici, et nulle part ailleurs, qu'elle peut être sauvée.
        // ORDRE : (1) armed <- budget courant, ce qui annule le delta lu par un
        // observateur, (2) accumulation, (3) le poke. Un relevé tombé entre (1)
        // et (2) SOUS-estime d'une tranche ; aucun ordre ne fait apparaître un
        // bond de +2^31, ce qui serait lu comme « ce fil galope » — le sens FAUX.
        // (mêmes accès `volatile`, même raison qu'en (2) de run() : l'ordre
        // désarmer -> accumuler -> poker doit survivre à l'optimiseur.)
        save_slice(e);                                               // (1) desarmer + (2) sauver
        __atomic_store_n(&e->dyn86_budget, 0, __ATOMIC_RELAXED);     // (3) le poke lui-meme
    }

    // ---- Native-scheduler support: one x86emu_t per guest thread --------
    void* thread_emu_create() override {
        // ARME le chemin TLS de E() (demonstration complete au-dessus de E()).
        // Pose ICI et pas dans thread_emu_bind : la creation precede
        // NECESSAIREMENT la liaison, donc le drapeau est vrai avant qu'un seul
        // fil puisse observer un t_emu non nul. Sous coop cette fonction n'est
        // jamais appelee — la sonde de rt_boot.cpp:2744 est dans la branche
        // D2SCHED=native — et le drapeau reste faux pour toute la partie.
        g_multi_emu.store(true, std::memory_order_relaxed);
        x86emu_t* e = new x86emu_t();
        std::memset(e, 0, sizeof *e);
        e->context = &ctx_;
        dynarec86_setup_emu_helpers(e);
        e->df = d_none;
        // FPU: inherit the BASE emu's current state — the long-validated
        // "inherit" semantics of the cooperative first slice (see
        // load_context's C12 field-regression note). Never a zero blob.
        X87Blob b; blob_from_emu(b, emu_); blob_to_emu(*e, b);
        emus_.push_back(e);            // caller holds the GIL (native ctor path)
        return e;
    }
    void thread_emu_bind(void* e) override {
        // GARDE DE L'INVARIANT de E() : lier un emu NON NUL alors que le
        // drapeau est faux voudrait dire qu'un emu par fil est apparu sans
        // passer par thread_emu_create — et E() rendrait alors l'emu de BASE a
        // un fil qui a le sien, c'est-a-dire une corruption silencieuse et
        // totale. Chemin FROID (une fois par fil invite), donc la garde est
        // gratuite ; et elle CRIE au lieu de deriver.
        if (e && !g_multi_emu.load(std::memory_order_relaxed)) {
            std::fprintf(stderr, "FATAL: thread_emu_bind(non nul) sans thread_emu_create — invariant E() rompu\n");
            wx86_vita_progress_c("FATAL: invariant E() rompu (bind sans create)");
            std::abort();
        }
        t_emu = (x86emu_t*)e;
    }
    const uint32_t* thread_emu_ip(void* e) override {
        return e ? &((x86emu_t*)e)->ip.dword[0] : &emu_.ip.dword[0];
    }
    // Vivacité par fil (cpu.h) : blocs traduits exécutés depuis le boot,
    // MONOTONE. Dérivé du budget de blocs (décrémenté par le prologue de chaque
    // bloc traduit) : coût nul sur le chemin chaud, et il bouge même quand le
    // fil ne franchit aucun trap — c'est ce qui distingue un tourniquet en code
    // traduit d'un fil réellement bloqué. nullptr = emu de base (main).
    // `volatile` : les écrivains sont du code généré et un store atomique
    // relaxed ; la course formelle disparaît pour zéro instruction de plus.
    bool thread_emu_filstat(void* e, uint32_t* traps, uint32_t* cont, uint32_t* wait_us) override {
        if (!g_filstat) return false;                 // champ ABSENT sans le knob
        const x86emu_t* em = e ? (const x86emu_t*)e : &emu_;
        const volatile uint32_t* pt = &em->dyn86_traps;
        const volatile uint32_t* pc = &em->dyn86_gilcont;
        const volatile uint32_t* pw = &em->dyn86_gilwait_us;
        if (traps) *traps = *pt; if (cont) *cont = *pc; if (wait_us) *wait_us = *pw;
        return true;
    }
    bool thread_emu_blocks(void* e, uint32_t* out) override {
        x86emu_t* em = e ? (x86emu_t*)e : &emu_;
        const volatile uint32_t* pn = &em->dyn86_blocks;
        const volatile int*      pa = &em->dyn86_armed;
        const volatile int*      pb = &em->dyn86_budget;
        // ORDRE DE LECTURE LOAD-BEARING (cpu.h) : total, PUIS armement, PUIS
        // budget. Couplé à « désarmer avant d'accumuler » côté écrivain, il
        // interdit de compter deux fois une tranche, donc tout bond en avant.
        uint32_t n = *pn; int a = *pa; int b = *pb;
        // budget <= 0 : tranche épuisée ou fil poké — le delta est déjà dans n.
        if (out) *out = (b > 0 && a >= b) ? n + (uint32_t)(a - b) : n;
        return true;
    }

    // NOTE natif : appelé HORS gil::Guard et compteurs mono-écrivain — sûr en
    // coop seulement ; inatteignable en natif (budget armé infini, bbreak ne
    // tire jamais). Ne pas activer D2_EIPPROF+native sans repenser ceci.
    // D2_EIPPROF: échantillonneur de temps CPU. IMPORTANT — on n'échantillonne
    // QUE sur expiration du budget de blocs, c'est-à-dire quand le thread a
    // réellement consommé sa tranche à CALCULER. Échantillonner à la reprise
    // (première version) sur-représentait massivement le code qui CÈDE souvent
    // la main : 86 % des relevés tombaient dans la boucle de messages 0x4fa590
    // (PeekMessage/Sleep), qui ne brûle pourtant aucun CPU.
    void eipprof_sample(uint32_t ip) {
        uint32_t rva = ip - g_eipProfBase;
        ++g_eipProfBuckets[wx86_prof_family(g_profMap, rva)];
        wx86_prof_zooms(rva);
        // Histogramme PLEINE PORTÉE : 96 cases de 32 KiB couvrant tout le .text
        // (0..0x300000). Les zones nommées ci-dessus ne couvraient que 8 % du
        // temps réel — il faut chercher sans a priori.
        if ((rva >> 15) < 96) ++d2rt_eipprof_all[rva >> 15];
        { uint32_t h = (ip * 2654435761u) >> 19;           // 13 bits -> 8192 cases
          for (int p = 0; p < 24; ++p) {
              uint32_t s = (h + p) & 8191u;
              if (!d2rt_eipprof_hit[s]) { d2rt_eipprof_key[s] = ip; d2rt_eipprof_hit[s] = 1; break; }
              if (d2rt_eipprof_key[s] == ip) { ++d2rt_eipprof_hit[s]; break; }
          } }
    }

    // ---- D2_TIMEPROF : echantillonnage UNIFORME EN TEMPS ------------------
    // Appele au meme endroit que eipprof_sample (expiration du budget), seul
    // moment ou emu->ip est a jour : entre deux epilogues, EIP vit dans r14 et
    // la memoire est perimee.
    //
    // ASSERVISSEMENT. On mesure l'intervalle reel et on corrige le budget du
    // prochain pas pour viser `d2rt_timeprof_on` microsecondes. Un facteur
    // borne a [1/4, 4] par pas : la correction converge en quelques pas sans
    // osciller quand une scene change brutalement de cout par bloc.
    //
    // INTERVALLES SUSPENDUS. Sous D2SCHED=native le fil peut etre depossede du
    // coeur entre deux expirations : l'intervalle mesure contient alors du
    // temps ou ce fil ne calculait PAS, et l'attribuer a l'adresse echantillonnee
    // serait un mensonge. Au-dela de 8x la cible, l'echantillon est REJETE et
    // compte a part (`suspendus=`). Un instrument qui cache ce qu'il a jete ne
    // prouve rien.
    void timeprof_sample(uint32_t ip, int consumed) {
        const uint64_t now = dyn86_jp_now_us();
        const uint64_t target = d2rt_timeprof_on;
        if (!tp_last_us) { tp_last_us = now; tp_budget = 2048; return; }
        const uint64_t dt = now - tp_last_us;
        tp_last_us = now;
        if (!dt) return;                      // sous la resolution de l'horloge

        // Correction du budget : viser `target` us par intervalle.
        {   int nb = tp_budget;
            if (dt * 4 < target)      nb = tp_budget * 4;
            else if (dt * 2 < target) nb = tp_budget * 2;
            else if (dt > target * 4) nb = tp_budget / 4;
            else if (dt > target * 2) nb = tp_budget / 2;
            if (nb < 32)      nb = 32;
            if (nb > 1000000) nb = 1000000;
            tp_budget = nb; }

        if (dt > target * 8) { ++d2rt_tp_susp; return; }   // fil suspendu

        ++d2rt_tp_samples;
        d2rt_tp_us += dt;
        d2rt_tp_blocks += (uint64_t)(consumed > 0 ? consumed : 0);

        const uint32_t rva = ip - d2rt_timeprof_base;
        ++d2rt_tp_bucket[wx86_prof_family(g_profMap, rva)];
        if(d2rt_lag_on) {                 // anneau d'attribution des gels
            const uint32_t w = d2rt_lag_w;
            d2rt_lag_t[w % D2RT_LAG_RING] = now;
            d2rt_lag_ip[w % D2RT_LAG_RING] = ip;
            d2rt_lag_w = w + 1;
        }
        { uint32_t h = (ip * 2654435761u) >> 19;
          for (int p = 0; p < 24; ++p) {
              uint32_t s = (h + p) & 8191u;
              if (!d2rt_tp_hit[s]) { d2rt_tp_key[s] = ip; d2rt_tp_hit[s] = 1; break; }
              if (d2rt_tp_key[s] == ip) { ++d2rt_tp_hit[s]; break; }
          } }
    }

    bool run(uint32_t eip, const char** fault) override {
        dyn86_slice_begin();           // clear stop flag + break marker (native
                                       // mode: dyn86_set_native keeps the stop)
        // ---- L'emu du fil, resolu UNE fois pour toute la tranche ------------
        // POURQUOI : sous natif, chaque acces via l'accesseur est un
        // __emutls_get_address -> pthread_getspecific -> pte_osTlsGetValue ->
        // sceKernelGetTLSAddr, un appel INTER-MODULE facture 0,43-0,58 us au
        // banc console (audit t12 §26.7 ; NE PAS confondre avec les 1,29 us de
        // §26.6, qui mesurent sceKernelGetProcessTimeWide -- lecture d'horloge
        // en plus -- et qui MAJORE). Le desassemblage du binaire livre en
        // comptait QUATORZE dans le seul corps de run(), dont SIX par tour de
        // boucle : donc six par prise de GIL, puisque le Guard plus bas est
        // pris DANS la boucle.
        //
        // POURQUOI C'EST CORRECT, et pas un cache : t_emu n'est ecrit que par
        // thread_emu_bind, qui a exactement DEUX appelants (sched_native.cpp
        // l. 501 et l. 902), tous deux AVANT run_guest -- donc avant tout run()
        // sur ce fil hote. Sous coop l'accesseur rend emu_, un membre. Dans les
        // deux cas il designe le MEME objet pendant toute l'activation de
        // run() : on ne memorise pas une valeur, on nomme une reference.
        //
        // CE QUE LA REFERENCE NE CHANGE PAS : e.quit et e.dyn86_bbreak sont
        // ecrits par d'AUTRES fils (request_stop). Une reference ne retient que
        // l'ADRESSE, jamais le contenu : chaque lecture retourne en memoire,
        // exactement comme avant. Et DynaRun reste un appel externe opaque que
        // le compilateur ne peut pas traverser (pas de -flto sur ce fichier,
        // build_rt_boot_vpk.sh l. 94). Le jour ou -flto arriverait, relire ceci.
        //
        // DETTE A SURVEILLER : si thread_emu_bind devenait un jour appelable
        // depuis un corps de shim, le hissage deviendrait faux ET MUET. Le
        // garde-fou n'est pas un assert (il ne vivrait que sous -DD2_TLSCOUNT,
        // jamais dans le binaire joue) : c'est le NOMBRE D'APPELANTS, a
        // reverifier si l'on touche a thread_emu_bind.
        // Pointeur EPINGLE pour la tranche, ou nullptr si le rollback est
        // demande — chaque acces repasse alors par E(), donc par la chaine
        // emutls complete, exactement comme avant l'optimisation.
        x86emu_t* const pin_ = g_noEmuOpt ? nullptr : &E();
        auto EMU = [&]() -> x86emu_t& { return pin_ ? *pin_ : E(); };
        EMU().ip.dword[0] = eip;
        EMU().error = 0;                   // per-slice reset, per-EMU state: this
                                       // runner's fault must not ghost into a
                                       // later slice on the same emu
        // Recharge the block-entry preemption budget for this slice.
        // Vivacité par fil (cpu.h) : la tranche qui s'achève est comptabilisée
        // ici, et `armed` mémorise la valeur rechargée — le lecteur n'a donc
        // AUCUNE constante 0x7FFFFFFF à supposer (elle deviendrait fausse le
        // jour où le natif armerait une limite via set_run_limit).
        // armed == 0 <=> tranche déjà comptabilisée par nudge_thread_budget (ou
        // toute première entrée) ; budget <= 0 <=> tranche consommée jusqu'au
        // bout (préemption par expiration du budget) : elle compte en entier.
        //
        // ORDRE DES ÉCRITURES — c'est ce qui rend le champ honnête. Le lecteur
        // (thread_emu_blocks) lit blocks, PUIS armed, PUIS budget, sans verrou.
        // On DÉSARME donc AVANT d'accumuler : tant que armed vaut 0, aucun
        // delta n'est ajouté au total lu, donc aucun lecteur ne peut compter
        // DEUX FOIS la tranche qui vient de s'achever. Un lecteur tombé dans
        // la fenêtre sous-estime d'une tranche ; il ne voit JAMAIS un bond en
        // avant — un bond serait lu « ce fil galope », c'est-à-dire le sens
        // FAUX (famine annoncée là où il y a arrêt). L'ordre inverse rendait
        // ce bond possible, sur quelques instructions par tranche.
        // Accès `volatile` : interdit au compilateur de réordonner ces quatre
        // écritures entre elles. Coût nul (une fois par TRANCHE), et sous
        // natif tous les fils sont sur un seul cœur (USER_0) : pas de
        // réordonnancement matériel à couvrir en plus.
        {   volatile int*      pa = &EMU().dyn86_armed;
            volatile uint32_t* pn = &EMU().dyn86_blocks;
            volatile int*      pb = &EMU().dyn86_budget;
            const int a = *pa, b = *pb;
            *pa = 0;                                                    // (1) plus aucun delta à lire
            if (a > 0) *pn = *pn + (uint32_t)(a - (b > 0 ? b : 0));     // (2) la tranche est sauvée
            // D2_TIMEPROF : le budget est celui que l'asservissement a calcule
            // pour le fil courant, et non la tranche d'ordonnancement.
            const int fresh = d2rt_timeprof_on ? (tp_budget ? tp_budget : 2048)
                            : (limit_blocks_ ? (int)limit_blocks_ : 0x7FFFFFFF);
            *pb = fresh;                                                // (3) budget de la tranche
            *pa = fresh;                                                // (4) les deltas reprennent
        }
        EMU().dyn86_bbreak = 0;
        for (;;) {
            EMU().quit = 0;
            const uint32_t start = EMU().ip.dword[0];
            DynaRun(&EMU());
            uint32_t ip = EMU().ip.dword[0];
            // Block-budget expiry: preempted, exactly resumable at EIP =
            // the start of the block that was about to run.
            if (EMU().dyn86_bbreak) { EMU().dyn86_bbreak = 0; t_preempt = 2;
                // ⚡ CE N'EST PAS UN PROFIL DE TEMPS. Le budget se decremente a
                // chaque ENTREE DE BLOC (prologue emis), donc l'echantillonnage
                // est uniforme EN ENTREES DE BLOC. Une fonction courte et tres
                // appelee est massivement sur-representee ; une boucle longue
                // tenant dans un seul bloc est sous-comptee (biais inverse deja
                // consigne dans profil_code_emis_20260905.md §0).
                // Mesure du 07/09 : Game+0x10dd60 pesait 12,55 % de ce profil ;
                // remplacer entierement son corps n'a deplace l'image que de
                // 0,265 ms sur 18,55 (et dans le mauvais sens), la ou 12,55 %
                // en vaudrait 2,3 — facteur ~10.
                // docs/perf/o1_intrin_console_20260907.md §3.
                if (g_eipProf) eipprof_sample(ip);   // profil par ENTREES DE BLOC
                if (d2rt_timeprof_on)                // profil en TEMPS
                    timeprof_sample(ip, tp_budget ? tp_budget : 2048);
                return true; }
            // LinkNext seam break (request_stop seen at a not-yet-linked seam):
            // preempted but resumable at EIP = the pending chain target.
            if (int b = dyn86_take_break()) { t_preempt = b; return true; }
            if (EMU().error) {
                t_fault_addr = ip;
                t_fault_err = EMU().error;     // ERR_UNIMPL=1 / ERR_DIVBY0=2 / ERR_ILLEGAL=4
                t_fault = "box86 dynarec fault (unimplemented/illegal/div0)";
                if (fault) *fault = t_fault;
                return false;
            }
            uint32_t slot = ip & ~0xFu;    // exit stub leaves EIP inside the slot
            if (slot >= trap_lo_ && slot < trap_hi_ && trap_fn_) {
                TrapGuard gg(EMU());       // native: serialize shims/intrinsics (spec D3) ; = gil::Guard sans D2_FILSTAT
                // PLUS de « t_fault_addr = slot » ici. C'etait une ecriture dans
                // une variable __thread, donc UNE chaine emutls complete
                // (__emutls_get_address -> pthread_getspecific -> pte_osTlsGetValue
                // -> sceKernelGetTLSAddr, inter-module) A CHAQUE TRAP — sur les
                // 9,39 chaines par prise mesurees par D2VPK_TLSWRAP.
                // ELLE NE PERD RIEN : tous les lecteurs de fault_addr() sont sur
                // un chemin de FAUTE (sched_native.cpp:430/435/442/453,
                // sched_cooperative.cpp:310/314, rt_boot.cpp:7194), et l'unique
                // « return false » de run() est la branche de faute plus haut,
                // qui pose t_fault_addr = ip elle-meme. Si une faute survient
                // DANS un shim, la ligne suivante a deja publie le slot dans
                // e.ip, donc cpu_->reg(R_EIP) — imprime cote a cote avec
                // faultAddr sur CHACUN de ces sites — porte la meme information.
                if (g_noEmuOpt) t_fault_addr = slot;   // ecriture par trap RETABLIE
                EMU().ip.dword[0] = slot;      // consistent state, like CpuUnicorn
                if (try_intrinsic(slot)) continue;   // fast path: never enters the Bridge
                bool resume = trap_fn_(*this, slot);
                if (!resume) return true;  // sentinel: clean finish
                continue;                  // resume at handler-set EIP
            }
            // quit without error outside the trap window: clean stop
            return true;
        }
    }
    uint32_t fault_addr() const override { return t_fault_addr; }
    // Precise Windows exception code for the SEH dispatcher (audit p8). UNIMPL is
    // an emulator gap, NOT a guest fault -> return 0 so the dispatcher skips it
    // (a __except "succeeding" on an emulator gap would mask it and diverge from
    // real hardware). NOTE: some genuine guest AVs arrive tagged ERR_ILLEGAL via
    // emit_signal(SIGSEGV) in generated code, so they surface as ILLEGAL_INSTRUCTION.
    uint32_t fault_code() const override {
        if (t_fault_err & 2) return 0xC0000094u;   // ERR_DIVBY0  -> INTEGER_DIVIDE_BY_ZERO
        if (t_fault_err & 4) return 0xC000001Du;   // ERR_ILLEGAL -> ILLEGAL_INSTRUCTION
        return 0;                                  // ERR_UNIMPL / unknown: terminate as before
    }

    // Per-thread FPU/SIMD context. One x86emu_t serves every guest thread, so
    // x87/MMX/XMM state must ride in the thread's X86Context across switches:
    // quantum preemption stops threads mid-computation where that state is
    // live (cooperative waits sit at Win32 call boundaries where the x87
    // stack is empty by ABI — the leak only bites on preemption, which is why
    // the virtual clock's fixed interleavings never tripped it and realclock
    // gameplay corrupted sprite decodes / switch dispatches).
    struct X87Blob {
        x87control_t cw; x87flags_t sw;
        mmx87_regs_t x87[8], mmx[8];
        uint32_t top; int fpu_stack; uint32_t fpu_tags;
        fpu_ld_t fpu_ld[8]; fpu_ll_t fpu_ll[8];
        sse_regs_t xmm[8]; mmxcontrol_t mxcsr;
    };
    static void blob_from_emu(X87Blob& b, const x86emu_t& e) {
        b.cw=e.cw; b.sw=e.sw;
        std::memcpy(b.x87,e.x87,sizeof b.x87); std::memcpy(b.mmx,e.mmx,sizeof b.mmx);
        b.top=e.top; b.fpu_stack=e.fpu_stack; b.fpu_tags=e.fpu_tags;
        std::memcpy(b.fpu_ld,e.fpu_ld,sizeof b.fpu_ld); std::memcpy(b.fpu_ll,e.fpu_ll,sizeof b.fpu_ll);
        std::memcpy(b.xmm,e.xmm,sizeof b.xmm); b.mxcsr=e.mxcsr;
    }
    static void blob_to_emu(x86emu_t& e, const X87Blob& b) {
        e.cw=b.cw; e.sw=b.sw;
        std::memcpy(e.x87,b.x87,sizeof b.x87); std::memcpy(e.mmx,b.mmx,sizeof b.mmx);
        e.top=b.top; e.fpu_stack=b.fpu_stack; e.fpu_tags=b.fpu_tags;
        std::memcpy(e.fpu_ld,b.fpu_ld,sizeof b.fpu_ld); std::memcpy(e.fpu_ll,b.fpu_ll,sizeof b.fpu_ll);
        std::memcpy(e.xmm,b.xmm,sizeof b.xmm); e.mxcsr=b.mxcsr;
    }
public:
    void save_context(X86Context& c) override {
        Cpu::save_context(c);
        static_assert(sizeof(X87Blob) <= sizeof(c.fpu), "X86Context::fpu too small");
        X87Blob b; blob_from_emu(b, E());
        std::memcpy(c.fpu, &b, sizeof b); c.fpu_valid = true;
    }
    void load_context(const X86Context& c) override {
        Cpu::load_context(c);
        // First slice of a never-run thread (fpu_valid=false): INHERIT the
        // current emu FPU state — the long-validated semantics. The C12 attempt
        // to load an all-zero "pristine" blob here was a FIELD REGRESSION
        // (2026-08-25, real Vita): cw=0 means x87 precision control = single
        // and tags != TAGS_EMPTY, so the DCC sprite decode (x87-heavy) went
        // subtly wrong on every thread's first slice — including MAIN, whose
        // pre-scheduler CRT state (cw=0x27F) got clobbered — ending in the
        // CelDataHash.cpp:1420 Halt at game load. A truly correct power-on
        // blob would be reset_fpu() semantics (cw=0x37F, tags=TAGS_EMPTY,
        // mxcsr=0x1F80) applied to WORKERS only, and must be validated on
        // hardware before it ships; until then, inherit.
        if (c.fpu_valid) {
            X87Blob b; std::memcpy(&b, c.fpu, sizeof b);
            blob_to_emu(E(), b);
        }
    }

private:
    static int host_prot(int p) {
        int hp = 0;
        if (p & P_R) hp |= PROT_READ;
        if (p & P_W) hp |= PROT_WRITE;
        if (p & P_X) hp |= PROT_EXEC;
        return hp;
    }

    box86context_t ctx_{};
    x86emu_t emu_{};
    // Per-guest-thread emus created by thread_emu_create (native backend only;
    // empty under coop). Append-only; request_stop broadcasts to all of them.
    std::vector<x86emu_t*> emus_;
    uint32_t limit_blocks_ = 0;        // per-slice block budget (0 = unlimited)
    uint32_t trap_lo_ = 0, trap_hi_ = 0;
    bool trap_mapped_ = false;
    TrapFn trap_fn_;
    // Per-slot Warden-safe intrinsics (docs/perf, Tier 1): a tiny fixed table of
    // {trap VA -> inline handler}. Only a few ultra-hot trivial imports qualify.
    // Adressage ouvert (cf. try_intrinsic) : ikey_[i]==0 => case libre.
    uint32_t    ikey_[kIntrinMask + 1] = {};
    IntrinsicFn ifn_ [kIntrinMask + 1] = {};
    int    intrin_n_ = 0;                          // occupation (plafond kIntrinMax)
    // B5 : projection en INDEX DIRECT de la table ci-dessus (cf. rebuild_direct).
    // idir_n_ == 0 => index non arme : la comparaison `d < idir_n_` rejette
    // tout, y compris avant tout enregistrement.
    uint32_t    idir_lo_ = 0;
    uint32_t    idir_n_  = 0;
    IntrinsicFn idir_[kDirectMax] = {};
    bool   no_intrinsics_ = std::getenv("WX86_DISABLE_INTRINSICS") || std::getenv("D2_DISABLE_INTRINSICS");
    // Fault/preempt state lives in the t_* __thread vars (top of file): with N
    // native runners it is per-HOST-thread, never shared members.
};

CpuBox86* g_instance = nullptr;

} // namespace

Cpu* make_cpu_box86() {
    if (g_instance) return nullptr;    // Box86 core is single-instance (global my_context)
    g_instance = new CpuBox86();
    return g_instance;
}

} // namespace d2rt

#endif // __arm__
