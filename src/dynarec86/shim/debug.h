/* D2Vita shim replacing Box86's src/include/debug.h
 * Box86 is (c) ptitSeb, MIT license — see third_party/box86-dynarec/LICENSE
 * All logging is compiled out; all tunables are compile-time constants
 * chosen for a Cortex-A9 (PS Vita) / qemu-arm target.
 */
#ifndef __DEBUG_H_
#define __DEBUG_H_
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct box86context_s box86context_t;

/* ---- log levels ---- */
#define LOG_NONE 0
#define LOG_INFO 1
#define LOG_DEBUG 2
#define LOG_NEVER 3
#define LOG_VERBOSE 3

#define ftrace stderr

#define printf_log(L, ...)   do { } while(0)
#define printf_dump(L, ...)  do { } while(0)
#define printf_dlsym(L, ...) do { } while(0)
#define dynarec_log(L, ...)  do { } while(0)

#define EXPORT
#define EXPORTDYN

/* ---- fastmmu: single global guest->host delta -------------------------------
 * The PS Vita cannot map memory at chosen virtual addresses (memblock VAs are
 * kernel-assigned, all >= 0x80000000), while D2 requires its own layout
 * (reloc-stripped Game.exe at 0x400000, Fog pool validator rejects pointers
 * >= 0x80000000). So the guest keeps a compact D2-native address space
 * [0, span) and every dereference adds dyn86_membase (host block base).
 * Constraints: low 24 bits of dyn86_membase must be 0 (single-ADD encodable
 * as ARM imm8 ror 8); 0 means identity (emits nothing -> the validated
 * qemu/Linux path stays bit-identical). C-side derefs use DYN86_G2H. */
extern uintptr_t dyn86_membase;
#define DYN86_G2H(a)  ((uintptr_t)(a) + dyn86_membase)
/* Chemin inverse : d'une adresse HOTE vers l'adresse INVITEE. Sert au
 * diagnostic (nommer l'instruction x86 fautive depuis la table instsize). */
#define DYN86_H2G(a)  ((uint32_t)((uintptr_t)(a) - dyn86_membase))

/* ---- allocators: plain libc ---- */
#define box_malloc      malloc
#define box_realloc     realloc
#define box_calloc      calloc
#define box_free        free
#define box_memalign(a,s) aligned_alloc(a,s)
#define box_strdup      strdup

/* ---- tunables, frozen as constants ---- */
static const int box86_log = 0;
static const int box86_dump = 0;
static const int box86_dynarec_log = 0;
static const int box86_dynarec = 1;
static const uintptr_t box86_pagesize = 4096;
static const uintptr_t box86_load_addr = 0;
static const int box86_showbt = 0;
static const int box86_maxcpu = 0;
static const int box86_maxcpu_immutable = 0;
static const int box86_dynarec_dump = 0;
static const int box86_dynarec_trace = 0;
static const int box86_dynarec_forced = 0;
static int box86_dynarec_largest __attribute__((unused)) = 0;
static const int box86_dynarec_bigblock = 2;   // build larger blocks across cond. branches (fewer transitions)
// D2_BUDGETTAIL=1 : sortir le chemin d'expiration du budget de blocs du
// PROLOGUE et le poser en QUEUE de bloc (voir dynarec_arm_pass.c). Meme code,
// autre placement. Defaut 0 = disposition historique.
// ⚡ CE KNOB N'A JAMAIS ETE MESURE SEUL SUR CONSOLE : il n'a couru que noye
// dans « les cinq ensemble » du lot dynarec (passe NALL, -3,7 %), dont les
// quatre autres membres sont desormais retires. Le profil du code emis lui
// donne une cible chiffree : 9,1 % des octets ARM emis sont ce chemin
// d'expiration, mort a chaque entree de bloc, que le BGT enjambe.
extern int box86_dynarec_budgettail;
// D2_FORWARD=<n> : taille maximale du TROU qu'un bloc peut enjamber vers
// l'avant pour continuer (dynarec_arm_pass.c, branche « forward extend »).
// Defaut 256 = valeur historique de box86, comportement INCHANGE.
// Comme box86_dynarec_callret, c'est une VRAIE variable definie dans
// src/dynarec86/dyn86.c et lue par le GENERATEUR au moment de la traduction :
// un binaire unique porte donc toutes les jambes de l'A/B (les blocs deja
// traduits gardent leur forme, ceux d'apres prennent la nouvelle).
// Plus grand = blocs plus longs, donc MOINS de transitions de bloc (chacune
// coute ~283 cycles sur la Vita) — mais aussi plus de code traduit, donc plus
// de pression sur l'I-cache et sur la table de saut. Le signe du resultat
// n'est PAS predictible depuis qemu : verdict console uniquement.
extern int box86_dynarec_forward;
static const int box86_dynarec_strongmem = 0;
static const int box86_dynarec_x87double = 0;
// safeflags : conservatisme des DRAPEAUX x86 aux RET/RETN, valeur amont de
// box86. NE PAS EN REFAIRE UN INTERRUPTEUR : le knob D2_SAFEFLAGS a existe et
// a ete retire le 05/09/2026 comme INERTE PAR CONSTRUCTION. Le corps de
// READFLAGS(A) est garde par `if(((A)!=X_PEND && ...) ...)` et X_PEND vaut
// 0x80 : avec A = X_PEND la condition est fausse A LA COMPILATION, donc les
// deux sites RET/RETN n'emettent RIEN, ni a 1 ni a 0. Les autres sites testent
// « > 1 » : inertes eux aussi au defaut. Les drapeaux differes de box86 sont
// deja paresseux aux retours ; il n'y a rien a prendre ici.
static const int box86_dynarec_safeflags = 1;
/* Expansion du traducteur : octets ARM emis, octets x86 traduits, blocs. */
extern unsigned long long dyn86_emit_arm_bytes, dyn86_emit_x86_bytes, dyn86_emit_blocks;
// D2_CALLRET=1 a la compilation : prediction de retour (le dynarec pousse la
// paire (adresse x86 de retour, cible ARM) sur la PILE ARM au CALL et la
// verifie au RET, ce qui evite la traversee de la table de saut).
// Historique : essaye puis retire, avec deux raisons ecrites. La seconde —
// « la pile de prediction vit dans l'emu, que nos fils cooperatifs PARTAGENT »
// — est PERIMEE : sous D2SCHED=native il y a un x86emu_t par fil
// (cpu_box86.cpp), et surtout arm_prolog/arm_epilog sauvent et restaurent
// xSPSave a CHAQUE entree/sortie de DynaRun, donc une sortie anticipee
// (budget de blocs, trap, couture) ne peut pas desynchroniser la pile.
// Defaut inchange = 0 : c'est un A/B a la compilation, pas une bascule.
// COMMUTABLE A CHAUD (D2_CALLRET=1 dans env.txt) : la valeur est lue par le
// GENERATEUR au moment ou il traduit un bloc, donc un binaire unique porte les
// deux jambes de l'A/B — le patron « defaut = ancien code » de la maison. Les
// blocs deja traduits gardent leur forme ; ceux d'apres prennent la nouvelle.
extern int box86_dynarec_callret;
// D2_SIGNTAG=1 : neutralise l'idiome Blizzard « pointeur complemente,
// discrimine par le BIT DE SIGNE » en discriminant sur le BIT 30. N'a de sens
// que sous D2LAYOUT=haut (memoire invitee au-dessus de 2 Gio) ; rt_boot refuse
// de l'armer ailleurs. Contrat et preuves :
// third_party/box86-dynarec/dynarec/dynarec_arm_signtag.h.
extern int dyn86_signtag;
extern unsigned long dyn86_signtag_seen;    // motifs reconnus (verdict pur)
extern unsigned long dyn86_signtag_done;    // motifs reecrits
// D2_MMUFOLD=1 / D2_MMUSTACK=1 : deux facons de retirer l'ADD de base fastmmu
// du CHEMIN CRITIQUE d'un acces memoire invite. Meme regle que callret :
// lus par le GENERATEUR a la traduction, defaut 0 = code d'avant.
// Contrat, recensement des formes et preuves de surete :
// third_party/box86-dynarec/dynarec/dynarec_arm_mmu.h.
extern int dyn86_mmufold;
extern int dyn86_mmustack;
extern unsigned long dyn86_mmu_folds;        // adressages absolus PLIES
extern unsigned long dyn86_mmu_sites;        // sites GETED* passes par geted
extern unsigned long dyn86_mmu_foldable;     // dont forme absolue = PLIABLES
extern unsigned long dyn86_mmu_st_heads;     // tetes de chaine PUSH/POP r32
extern unsigned long dyn86_mmu_st_links;     // maillons chaines
extern unsigned long dyn86_mmu_st_pop;       // dont POP  : 1 maillon de moins
extern unsigned long dyn86_mmu_st_push;      // dont PUSH : 1 instruction seule
extern unsigned long dyn86_mmu_st_rej_pos;   // refus : code emis entre deux
extern unsigned long dyn86_mmu_st_rej_pred;  // refus : cible de saut/barriere
extern unsigned long dyn86_mmu_st_rej_kind;  // refus : autre famille/registre
    // (uninitialized return-prediction stack in our minimal emu setup), and is
    // structurally wrong for D2Vita anyway — the prediction stack lives in the
    // emu, which our cooperative threads SHARE, so call/ret pairs from
    // different guest threads would interleave and mispredict permanently.
static const uintptr_t box86_nodynarec_start = 0;
static const uintptr_t box86_nodynarec_end = 0;
static const int box86_dynarec_fastnan = 1;
static const int box86_dynarec_fastround = 1;
static const int box86_dynarec_hotpage = 0;
static const int box86_dynarec_wait = 1;
static const int box86_dynarec_fastpage = 0;
static const int box86_dynarec_bleeding_edge = 1;
static const int box86_dynarec_missing = 0;
static const int box86_dynarec_test = 0;
static const int box86_dynarec_jvm = 0;
static const int box86_dynarec_tbb = 0;
/* ARM hw caps: Cortex-A9 = VFPv3-D32 + NEON, no idiv, no crypto */
static const int arm_vfp = 3;
static const int arm_swap = 1;
static const int arm_div = 0;
static const int arm_aes = 0;
static const int arm_pmull = 0;
static const int box86_libcef = 0;
static const int box86_sdl2_jguid = 0;
static const int dlsym_error = 0;
static const int cycle_log = 0;
static const int trace_xmm = 0;
static const int trace_emm = 0;
static const int box86_nosandbox = 0;
static const int box86_malloc_hack = 0;
static const int box86_sse_flushto0 = 0;
static const int box86_x87_no80bits = 0;
static const int allow_missing_libs = 0;
static const int box86_prefer_wrapped = 0;
static const int box86_prefer_emulated = 0;
static const int box86_steam = 0;
static const int box86_wine = 0;
static const int box86_musl = 0;
static const int box86_showsegv = 0;
static const int box86_mutex_aligned = 0;
static const uintptr_t trace_start = 0, trace_end = 0;
static char* const trace_func = 0;
static const uint64_t start_cnt = 0;
static const uintptr_t fmod_smc_start = 0, fmod_smc_end = 0;
static const uint32_t default_fs = 0;
static const int jit_gdb = 0;
static int box86_mapclean __attribute__((unused)) = 0;

#endif /* __DEBUG_H_ */
