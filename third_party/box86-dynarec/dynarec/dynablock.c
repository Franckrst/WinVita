#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>
#include <sys/mman.h>
#include <setjmp.h>

#include "debug.h"
#include "box86context.h"
#include "dynarec.h"
#include "emu/x86emu_private.h"
#include "tools/bridge_private.h"
#include "x86run.h"
#include "x86emu.h"
#include "box86stack.h"
#include "callback.h"
#include "emu/x86run_private.h"
#include "x86trace.h"
#include "dynablock.h"
#include "dynablock_private.h"
#include "dynarec_private.h"
#include "elfloader.h"
#include "bridge.h"
#ifdef ARM
#include "dynarec_arm.h"

extern int dyn86_link_direct(void);   // D2Vita scheduler quantum mode
#include "arm_lock_helper.h"
#else
#error Unsupported architecture!
#endif
#include "custommem.h"
#include "khash.h"

KHASH_MAP_INIT_INT(dynablocks, dynablock_t*)

/* --- D2Vita D2_JITPROFILE counters (contract + inclusive/exclusive rules in
 * src/dynarec86/dyn86.h). Every write below sits behind `dyn86_jitprof`, which
 * is armed once in dyn86_init(); with the knob absent this file behaves
 * exactly as before. Nothing here touches code generation or linking. */
extern int dyn86_jitprof;
extern uint64_t dyn86_jp_clock_bias_ns;
#define DYN86_JP_SAMPLE 64u
uint64_t dyn86_jp_lookup_calls = 0;
uint64_t dyn86_jp_lookup_hits  = 0;
uint64_t dyn86_jp_lookup_miss  = 0;
uint64_t dyn86_jp_created      = 0;
uint64_t dyn86_jp_recompiles   = 0;
uint64_t dyn86_jp_invalid      = 0;
uint64_t dyn86_jp_x86_insns    = 0;
uint64_t dyn86_jp_arm_bytes    = 0;
uint64_t dyn86_jp_lookup_ns    = 0;
uint64_t dyn86_jp_lookup_smp   = 0;
/* Set while DBGetBlock is re-translating a block it just invalidated, so the
 * publish site can tell a RE-compile from a genuinely new block. One guest
 * thread at a time runs under the cooperative scheduler, so a plain int is
 * enough (and it is diagnostics-only). */
static int dyn86_jp_recomp = 0;

uint32_t X31_hash_code(void* addr, int len)
{
    if(!len) return 0;
    uint8_t* p = (uint8_t*)addr;
	int32_t h = *p;
	for (--len, ++p; len; --len, ++p) h = (h << 5) - h + (int32_t)*p;
	return (uint32_t)h;
}

dynablock_t* InvalidDynablock(dynablock_t* db, int need_lock)
{
    if(db) {
        if(db->gone)
            return NULL; // already in the process of deletion!
        if(dyn86_jitprof) ++dyn86_jp_invalid;
        dynarec_log(LOG_DEBUG, "InvalidDynablock(%p), db->block=%p x86=%p:%p already gone=%d\n", db, db->block, db->x86_addr, db->x86_addr+db->x86_size-1, db->gone);
        if(need_lock)
            mutex_lock(&my_context->mutex_dyndump);
        // remove jumptable
        setJumpTableDefault(db->x86_addr);
        db->done = 0;
        db->gone = 1;
        if(need_lock)
            mutex_unlock(&my_context->mutex_dyndump);
    }
    return db;
}

void FreeInvalidDynablock(dynablock_t* db, int need_lock)
{
    if(db) {
        if(!db->gone)
            return; // already in the process of deletion!
        dynarec_log(LOG_DEBUG, "FreeInvalidDynablock(%p), db->block=%p x86=%p:%p already gone=%d\n", db, db->block, db->x86_addr, db->x86_addr+db->x86_size-1, db->gone);
        if(need_lock)
            mutex_lock(&my_context->mutex_dyndump);
        FreeDynarecMap((uintptr_t)db->actual_block);
        customFree(db);
        if(need_lock)
            mutex_unlock(&my_context->mutex_dyndump);
    }
}

void FreeDynablock(dynablock_t* db, int need_lock)
{
    if(db) {
        if(db->gone)
            return; // already in the process of deletion!
        if(dyn86_jitprof) ++dyn86_jp_invalid;
        dynarec_log(LOG_DEBUG, "FreeDynablock(%p), db->block=%p x86=%p:%p already gone=%d\n", db, db->block, db->x86_addr, db->x86_addr+db->x86_size-1, db->gone);
        if(need_lock)
            mutex_lock(&my_context->mutex_dyndump);
        // remove jumptable
        setJumpTableDefault(db->x86_addr);
        dynarec_log(LOG_DEBUG, " -- FreeDyrecMap(%p, %d)\n", db->actual_block, db->size);
        db->done = 0;
        db->gone = 1;
        if(db->previous)
            FreeInvalidDynablock(db->previous, 0);
        FreeDynarecMap((uintptr_t)db->actual_block);
        customFree(db);
        if(need_lock)
            mutex_unlock(&my_context->mutex_dyndump);
    }
}



/* D2Vita (audit T12 §12.4 entree 2) — AUTO-INTERBLOCAGE CORRIGE.
 * need_lock dit si FreeInvalidDynablock doit prendre mutex_dyndump lui-meme.
 * Il le FAUT depuis cleanDBFromAddressRange (qui tient mutex_prot, pas
 * dyndump) et il ne le faut SURTOUT PAS depuis internalDBGetBlock, qui tient
 * deja dyndump : ce mutex est initialise avec des attributs par defaut
 * (src/runtime/cpu_box86.cpp:322, pthread_mutex_init(..., nullptr)) donc
 * PTHREAD_MUTEX_NORMAL, donc non recursif — le reprendre depuis le meme fil
 * est un blocage definitif, pas une erreur rendue.
 * Avant ce correctif la valeur etait cablee a 1 pour les deux chemins. */
void MarkDynablock(dynablock_t* db, int need_lock)
{
    if(db) {
        dynarec_log(LOG_DEBUG, "MarkDynablock %p %p-%p\n", db, db->x86_addr, db->x86_addr+db->x86_size-1);
        if(!setJumpTableIfRef(db->x86_addr, db->jmpnext, db->block)) {
            dynablock_t* old = db;
            db = getDB((uintptr_t)old->x86_addr);
            if(!old->gone && db!=old) {
                printf_log(LOG_INFO, "Warning, couldn't mark block as dirty for %p, block=%p, current_block=%p\n", old->x86_addr, old, db);
                // the block is lost, need to invalidate it...
                old->gone = 1;
                old->done = 0;
                if(!db || db->previous)
                    FreeInvalidDynablock(old, need_lock);
                else
                    db->previous = old;
            }
        }
    }
}

static int IntervalIntersects(uintptr_t start1, uintptr_t end1, uintptr_t start2, uintptr_t end2)
{
    if(start1 > end2 || start2 > end1)
        return 0;
    return 1;
}

static int MarkedDynablock(dynablock_t* db)
{
    if(db) {
        if(getNeedTest((uintptr_t)db->x86_addr))
            return 1; // already done
    }
    return 0;
}

void MarkRangeDynablock(dynablock_t* db, uintptr_t addr, uintptr_t size, int need_lock)
{
    // Mark will try to find *any* blocks that intersect the range to mark
    if(!db)
        return;
    dynarec_log(LOG_DEBUG, "MarkRangeDynablock %p-%p .. startdb=%p, sizedb=%p\n", (void*)addr, (void*)addr+size-1, (void*)db->x86_addr, (void*)db->x86_size);
    if(IntervalIntersects((uintptr_t)db->x86_addr, (uintptr_t)db->x86_addr+db->x86_size-1, addr, addr+size+1))
        MarkDynablock(db, need_lock);
}

int FreeRangeDynablock(dynablock_t* db, uintptr_t addr, uintptr_t size)
{
    if(!db)
        return 1;

    int need_lock = my_context?1:0;
    if(IntervalIntersects((uintptr_t)db->x86_addr, (uintptr_t)db->x86_addr+db->x86_size-1, addr, addr+size+1)) {
        FreeDynablock(db, need_lock);
        return 0;
    }
    return 1;
}

dynablock_t *AddNewDynablock(uintptr_t addr)
{
    dynablock_t* block;
    #if 0
    // check if memory as the correct flags
    int prot = getProtection(addr);
    if(!(prot&(PROT_EXEC|PROT_DYNAREC|PROT_DYNAREC_R))) {
        dynarec_log(LOG_VERBOSE, "Block asked on a memory with no execution flags 0x%02X\n", prot);
        return NULL;
    }
    
    #endif
    // create and add new block
    dynarec_log(LOG_VERBOSE, "Ask for DynaRec Block creation @%p\n", (void*)addr);
    block = (dynablock_t*)customCalloc(1, sizeof(dynablock_t));
    return block;
}

//TODO: move this to dynrec_arm.c and track allocated structure to avoid memory leak
#if defined(__vita__)
#define JUMPBUFF jmp_buf         /* newlib: array type, same shape as ANDROID */
#elif defined(ANDROID)
#define JUMPBUFF sigjmp_buf
#else
#define JUMPBUFF struct __jmp_buf_tag
#endif
static __thread JUMPBUFF dynarec_jmpbuf;
#if defined(ANDROID) || defined(__vita__)
#define DYN_JMPBUF dynarec_jmpbuf
#else
#define DYN_JMPBUF &dynarec_jmpbuf
#endif

void cancelFillBlock()
{
    longjmp(DYN_JMPBUF, 1);
}

/* 
    return NULL if block is not found / cannot be created. 
    Don't create if create==0
*/
// Translation-time attribution (freeze diagnosis): cumulative wall time spent
// translating x86 blocks to ARM, sampled by the Vita watchdog.
uint64_t dyn86_fill_ns = 0; uint32_t dyn86_fill_count = 0;
uint32_t dyn86_fill_fail = 0;   // fills that produced no cached block (e.g. JIT map OOM)
// D2_EIPTRAP diag: armed with a guest EIP (hex env); the first chain to that
// address dumps the SOURCE block (who jumped there) — catches wild jumps
// through corrupted function pointers at the moment they happen.
uintptr_t dyn86_eiptrap = 0;
uintptr_t dyn86_eiptrap2 = 0;   // D2Vita: second trapped address (D2_EIPTRAP2), shared ring tagged A/B
uint32_t dyn86_diag_regs = 0;   // arms the arm_next.S reg-sync STM (set at boot when an eiptrap/xfertrace knob is present)

#include <time.h>
static inline uint64_t dyn86_prof_now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Journal du moteur (src/platform/vita_host.h) : reference FORTE, meme
 * bibliotheque — meme motif que dynarec.c. INERTE hors console (definition
 * vide, vita_host.cpp), d'ou le stderr double dans dyn86_ev_say : sans lui la
 * validation de niveau 1 sous qemu n'aurait rien a lire. */
void wx86_vita_progress_c(const char* msg);

/* ===================================================================== */
/* D2Vita — EVICTION DU CACHE DE TRADUCTION (lot 2)                      */
/* ===================================================================== */
/*
 * CE QUE CE BLOC RESOUT. Quand l'arene JIT ne peut plus grandir,
 * AllocDynarecMap rend 0, FillBlock rend NULL, et le fil invite meurt :
 * ce build n'a pas d'interpreteur (shim/shim_impl.c, Run() pose quit=1 et
 * ERR_UNIMPL). Ici on rend la memoire d'anciens blocs et on retraduit. Un
 * a-coup de retraduction vaut infiniment mieux qu'un processus mort.
 *
 * LE SEUL VRAI DANGER, ET COMMENT IL EST ECARTE. Le code traduit tourne
 * SANS GIL : au moment ou on veut rendre la memoire d'un bloc, un autre fil
 * invite peut avoir son PC dedans, ou detenir un dynablock_t* obtenu juste
 * avant. Liberer sous ses pieds serait pire que le defaut d'origine. D'ou
 * une separation stricte en deux temps :
 *
 *   RETRAIT  = InvalidDynablock(db, 0) : setJumpTableDefault + gone=1.
 *              Ne rend AUCUNE memoire. Sur a tout instant, parce que TOUTES
 *              les entrees dans un bloc passent par la table de sauts —
 *              c'est deja ce sur quoi box86 fonde sa propre invalidation, et
 *              les chainages sont des branchements INDIRECTS sur adresse
 *              32 bits complete, jamais des B/BL a portee limitee. Un fil
 *              deja dans le bloc le finit dans une memoire intacte, puis
 *              rate la table de sauts et retraduit.
 *
 *   RECUPERATION = FreeDynarecMap + customFree, uniquement APRES une
 *              PERIODE DE GRACE.
 *
 * LA GRACE, ENONCEE. « Tout emu invite vivant est sorti de DynaRun au moins
 * une fois depuis le retrait, ou n'y est pas en ce moment. »
 * Elle couvre les DEUX dangers d'un coup, ce qui est la raison de l'avoir
 * choisie ainsi :
 *   - les PC ARM, puisque executer du code traduit c'est etre dans DynaRun ;
 *   - les dynablock_t* detenus par du code C, puisque DBGetBlock lit
 *     db->done/db->block APRES avoir relache mutex_dyndump (dynablock.c, la
 *     boucle plus bas) — mais tous ses appelants sont dans DynaRun.
 *
 * ORDRE MEMOIRE, SANS BARRIERE SUR LE CHEMIN CHAUD. dyn86_rundepth est ecrit
 * par le fil proprietaire sans barriere (cf. dynarec.c). Le recupereur
 * pourrait donc, en theorie, lire un 0 perime. C'est sur, et voici pourquoi :
 * pour detenir un pointeur vers un bloc, ou pour executer du code traduit, un
 * fil a NECESSAIREMENT traverse DBGetBlock, qui prend et relache
 * mutex_dyndump. Son ecriture de rundepth precede ce relachement, qui precede
 * la prise de mutex_dyndump par le recupereur (qui la tient pendant tout ce
 * qui suit). Le seul fil dont on pourrait lire un rundepth perime est un fil
 * qui vient d'entrer dans DynaRun sans avoir encore fait la moindre
 * recherche — donc qui ne detient rien, et qui ne peut pas atteindre un bloc
 * RETIRE puisqu'il n'est plus dans la table de sauts.
 * Meme argument pour le registre d'emus : un emu qui a pu executer un bloc
 * s'est enregistre avant sa premiere recherche, donc avant un relachement de
 * mutex_dyndump.
 * Dans l'autre sens, une lecture perimee de rungen ne peut que faire
 * AJOURNER : conservateur, jamais dangereux.
 *
 * CE QU'ELLE NE COUVRE PAS, ET QUI EST BORNE. Une boucle entierement contenue
 * dans UN SEUL dynablock ne repasse jamais par un prologue et ne peut pas
 * etre preemptee (limitation documentee, src/dynarec86/dyn86.h). Un tel fil
 * ne sort jamais de DynaRun et bloque la grace pour toujours. D'ou la BORNE :
 * apres DYN86_EV_ROUNDS tours sans rien pouvoir recuperer, on abandonne et on
 * retombe exactement sur le comportement d'aujourd'hui — la mort nommee. On
 * ne troque jamais un plantage contre un gel.
 */

/* Registre d'emus : tableau fixe, en AJOUT SEUL, et volontairement distinct
 * du vecteur emus_ de CpuBox86 — celui-la exige le GIL (un push_back peut
 * reallouer) alors qu'on est ici sous mutex_dyndump et sans GIL. Un emu n'est
 * jamais retire : un fil invite fini garde son emu, avec rundepth a 0, donc
 * il passe la grace sans rien couter. */
#define DYN86_MAXEMU    64
#define DYN86_RETIRE_MAX 512    /* blocs retires par vague */
#define DYN86_EV_ROUNDS  8      /* tours d'eviction avant d'abandonner */

static x86emu_t* g_emureg[DYN86_MAXEMU];
static volatile int g_emureg_n = 0;
static int g_emureg_overflow = 0;

/* Appele par CpuBox86 a la creation de CHAQUE emu (emu de base inclus),
 * toujours AVANT que le fil correspondant n'execute quoi que ce soit. */
void dyn86_emu_register(x86emu_t* e)
{
    if(!e) return;
    for(int i=0; i<g_emureg_n; ++i) if(g_emureg[i]==e) return;
    if(g_emureg_n >= DYN86_MAXEMU) {
        /* Plus d'emus que le registre n'en tient : on ne peut plus PROUVER la
         * grace, donc on desarme l'eviction plutot que de deviner. Une mort
         * nommee vaut mieux qu'un usage-apres-liberation. */
        if(!g_emureg_overflow) {
            g_emureg_overflow = 1;
            fprintf(stderr, "[jit] registre d'emus plein (%d) — EVICTION DESARMEE\n", DYN86_MAXEMU);
            wx86_vita_progress_c("JIT: registre d'emus plein — eviction desarmee (securite)");
        }
        return;
    }
    g_emureg[g_emureg_n] = e;
    g_emureg_n = g_emureg_n + 1;   /* le slot est ecrit AVANT le compteur */
}

/* Compteurs. Exportes (non statiques) pour que le portage puisse les afficher
 * dans sa ligne de battement sans que le moteur ait a connaitre son format. */
uint32_t dyn86_ev_rounds    = 0;  /* tours d'eviction entames                */
uint32_t dyn86_ev_retired   = 0;  /* blocs retires (desinscrits)             */
uint32_t dyn86_ev_reclaimed = 0;  /* blocs reellement liberes                */
uint32_t dyn86_ev_deferred  = 0;  /* recuperations AJOURNEES : un fil etait
                                   * encore dedans. C'est LA preuve que la
                                   * grace sert a quelque chose ; a zero
                                   * permanent, elle n'a jamais eu a mordre. */
uint32_t dyn86_ev_refills   = 0;  /* traductions sauvees par une eviction    */
/* Refus du TAS DE METADONNEES (custommem.c), distinct de dyn86_jit_allocfail
 * qui compte les refus d'ARENE. Deux penuries differentes, deux compteurs :
 * les confondre ferait passer un manque de RAM utilisateur pour une arene
 * JIT trop petite, et enverrait le reglage dans la mauvaise direction. */
uint32_t dyn86_meta_allocfail = 0;
uint32_t dyn86_ev_handback  = 0;  /* borne atteinte SANS avoir pu liberer :
                                   * rendu a la boucle de reessai de
                                   * DBGetBlock, qui cede HORS VERROU. Ce
                                   * n'est PAS une mort — la mort, c'est
                                   * dyn86_ev_fatal, compte dans dynarec.c au
                                   * seul endroit qui la provoque vraiment. */
uint32_t dyn86_ev_fatal     = 0;  /* fils invites reellement tues par la
                                   * saturation (dynarec.c, repli Run())     */
uint32_t dyn86_ev_oomexit   = 0;  /* sorties REPRENABLES accordees a un fil
                                   * qui ne pouvait pas traduire : voir
                                   * dynarec.c, c'est ce qui debloque la grace */
uint32_t dyn86_ev_corrupt   = 0;  /* blocs dont le pointeur `self` ne se
                                   * relit pas au moment de les rendre :
                                   * detecteur d'usage-apres-liberation      */
uint64_t dyn86_ev_bytes     = 0;  /* octets rendus a l'arene                 */

static void dyn86_ev_say(const char* what);
/* Declaree ICI et pas au point de definition : dyn86_ev_atexit l'appelle plus
 * haut dans le fichier. Le build ARM de l'oracle ne le signalait qu'en
 * avertissement (declaration implicite, C89) ; la chaine Vita en fait une
 * ERREUR — la raison pour laquelle on lit le journal et pas le code de retour. */
void dyn86_ev_dump(const char* what);

static int dyn86_ev_poison(void)
{
    static int on = -1;
    if(on < 0) {
        const char* e = getenv("WX86_JITEVICT_POISON"); if(!e) e = getenv("D2_JITEVICT_POISON");
        on = (e && *e=='0') ? 0 : 1;   /* absent => arme */
    }
    return on;
}

/* Vague en attente de recuperation. UNE SEULE a la fois : on ne retire une
 * nouvelle vague que quand la precedente est rendue. Ca borne la memoire de
 * service, ca borne le nombre de blocs desinscrits mais non rendus (donc la
 * retraduction inutile), et ca rend l'etat trivial a raisonner. */
static dynablock_t* g_retire[DYN86_RETIRE_MAX];
static int      g_retire_n = 0;
/* « L'arene a refuse au moins une allocation et rien n'a ete rendu depuis. »
 * Sert a une seule chose, mais elle est decisive : ne pas TENTER une
 * traduction dont on sait qu'elle echouera, tant qu'une vague attend sa
 * grace. Un FillBlock rate coute deux passes completes du traducteur, ET il
 * les paye EN TENANT mutex_dyndump. Avec cinq fils invites qui echouent tous,
 * le verrou n'est jamais libre assez longtemps pour que le fil dont la grace
 * attend la sortie puisse l'obtenir — les mutex pthread n'etant pas FIFO, il
 * starve. Journal a l'appui : « retient=0(prof=1 gen=13124/13124) », l'emu
 * principal fige alors qu'il executait 1,4 million de blocs par ailleurs. */
static int      g_arena_full = 0;
static uint32_t g_retire_gen[DYN86_MAXEMU];
static int      g_retire_emus = 0;

/* Vrai si CHAQUE emu de l'instantane est sorti de DynaRun depuis le retrait,
 * ou n'y est pas en ce moment. Lit rundepth AVANT rungen : voir dynarec.c.
 *
 * AUCUN emu n'est exclu — PAS MEME CELUI DU FIL QUI RECUPERE. C'est le point
 * le plus couteux de ce lot, et il a ete paye :
 *
 * La premiere version excluait `self`, sur l'argument que le fil evinceur est
 * dans du C, que le chainage de blocs est un saut TERMINAL et que arm_next.S
 * repart vers la CIBLE (bx r3), jamais vers le bloc source. Cet argument est
 * juste, et il est incomplet. Il ignore D2_CALLRET — la prediction d'adresse
 * de retour, armee par defaut dans ce portage (tools/rt_boot.cpp) : a chaque
 * CALL invite, le traducteur EMPILE SUR LA PILE ARM le couple (adresse de
 * retour x86, ADRESSE NATIVE DE RETOUR), et le RET fait un `BXcond(cEQ, x3)`
 * dessus — il retourne DIRECTEMENT au milieu d'un bloc, sans consulter la
 * table de sauts (dynarec_arm_helper.c, ret_to_epilog/retn_to_epilog).
 * Desinscrire un bloc de la table ne met donc PAS ce bloc hors d'atteinte : le
 * fil evinceur garde, sur sa propre pile ARM, des adresses natives vers tous
 * les blocs qu'il a traverses depuis son entree dans DynaRun, et il y
 * reviendra en remontant ses RET.
 *
 * Mesure : avec l'exclusion, 3 plantages sur 4 sous arene serree, signal 5 a
 * hostoff=+0x14c DANS un bloc — un retour en plein milieu, exactement la
 * signature de la prediction. Sans l'exclusion : aucun.
 *
 * Ce qui rend le progres possible malgre tout, c'est que la pile de prediction
 * ne survit PAS a une sortie de DynaRun : arm_prolog/arm_epilog sauvent et
 * restaurent xSPSave a chaque entree/sortie (cf. shim/debug.h). « Etre sorti
 * de DynaRun au moins une fois » signifie donc AUSSI « n'a plus aucune adresse
 * native en reserve » — la grace couvre la prediction par la meme phrase qui
 * couvre le PC. Et le fil evinceur obtient cette sortie par le chemin
 * reprenable de dynarec.c : il retire une vague, ne peut rien rendre lui-meme,
 * sort proprement, revient, et c'est a ce moment-la que sa generation a
 * avance et que la vague devient recuperable. */
static int      g_grace_blocker = -1;      /* dernier emu ayant retenu la grace */
static uint32_t g_grace_bd = 0, g_grace_bg = 0;

static int dyn86_grace_passed(x86emu_t* self)
{
    (void)self;   /* aucune exclusion : voir ci-dessus */
    /* D2_JITEVICT_NOGRACE=1 — INSTRUMENT DE BANC, jamais une configuration de
     * jeu : supprime la periode de grace, c'est-a-dire rend la memoire d'un
     * bloc sans verifier que personne n'est dedans. Sa raison d'etre est de
     * PROUVER QUE LE DETECTEUR D'USAGE-APRES-LIBERATION MARCHE. Un detecteur
     * qui n'a jamais rien detecte ne demontre rien : il peut etre correct, ou
     * simplement mort. Avec ce bouton on fabrique la faute a la demande et on
     * verifie que l'empoisonnement (instruction ARM indefinie) la voit. */
    static int nograce = -1;
    if(nograce < 0) {
        const char* e = getenv("WX86_JITEVICT_NOGRACE"); if(!e) e = getenv("D2_JITEVICT_NOGRACE");
        nograce = (e && *e=='1') ? 1 : 0;
        if(nograce) fprintf(stderr, "[jit] BANC: PERIODE DE GRACE SUPPRIMEE (D2_JITEVICT_NOGRACE=1) — usage-apres-liberation ATTENDU\n");
    }
    if(nograce) { g_grace_blocker = -1; return 1; }
    for(int i=0; i<g_retire_emus; ++i) {
        x86emu_t* e = g_emureg[i];
        if(!e) continue;
        const volatile uint32_t* pd = &e->dyn86_rundepth;
        const volatile uint32_t* pg = &e->dyn86_rungen;
        uint32_t d = *pd;
        if(d == 0) continue;                 /* dehors : sur */
        uint32_t g = *pg;
        if(g != g_retire_gen[i]) continue;   /* est ressorti depuis : sur */
        /* Qui retient, et dans quel etat. Sans ce nom, un ajournement
         * permanent n'est qu'un compteur qui monte : impossible de dire si la
         * grace est trop stricte ou si un fil est reellement fige. */
        g_grace_blocker = i; g_grace_bd = d; g_grace_bg = g;
        return 0;                            /* toujours dans la meme activation */
    }
    g_grace_blocker = -1; g_grace_bd = 0; g_grace_bg = 0;
    return 1;
}

/* Pousse les AUTRES emus vers une frontiere de bloc : le prologue de chaque
 * bloc traduit decremente dyn86_budget, et a zero il sort de DynaRun de facon
 * exactement REPRENABLE. Sans ca, un fil dans une longue sequence traduite
 * sans trap ferait ajourner la recuperation jusqu'a la borne. Implemente cote
 * CpuBox86 (dyn86_emu_nudge) pour ne pas dupliquer ici l'ordre porteur
 * desarmement/accumulation de la vivacite par fil. */
void dyn86_emu_nudge(void* emu);   /* cpu_box86.cpp */

static void dyn86_nudge_others(x86emu_t* self)
{
    for(int i=0; i<g_emureg_n; ++i)
        if(g_emureg[i] && g_emureg[i]!=self)
            dyn86_emu_nudge(g_emureg[i]);
}

/* Rend la memoire de la vague si la grace est passee. Rend les octets rendus
 * (0 = rien, soit parce qu'il n'y a pas de vague, soit parce qu'elle est
 * ajournee). Appele sous mutex_dyndump. */
static size_t dyn86_reclaim_wave(x86emu_t* self)
{
    if(!g_retire_n)
        return 0;
    if(!dyn86_grace_passed(self)) {
        ++dyn86_ev_deferred;
        /* RE-POUSSER A CHAQUE AJOURNEMENT, et pas seulement au retrait.
         * Mesure avant ce correctif : 511 ajournements pour 2 recuperations —
         * la grace ne convergeait pas. La cause est structurelle et vaut d'etre
         * ecrite : les fils qui retiennent la grace sont, le plus souvent,
         * ceux qui sont BLOQUES SUR LE MUTEX QUE L'EVICTEUR TIENT, dans
         * LinkNext. Ils ont rundepth>0, ne progressent pas, et leur generation
         * est figee tant que l'evicteur ne rend pas la main. Une seule poussee
         * au moment du retrait se perd d'ailleurs : CpuBox86::run() recharge
         * le budget a chaque debut de tranche. Repousser ici, a chaque
         * ajournement, fait qu'au moment ou l'evicteur rend la main et ou le
         * fil bloque repart, celui-ci sort de DynaRun au PROCHAIN prologue de
         * bloc — et le tour suivant trouve la vague mure. */
        dyn86_nudge_others(self);
        return 0;
    }
    size_t freed = 0;
    g_arena_full = 0;                 /* on rend de la memoire : l'arene respire */
    for(int i=0; i<g_retire_n; ++i) {
        dynablock_t* db = g_retire[i];
        if(!db) continue;
        /* `previous` d'abord : FreeInvalidDynablock ne suit pas la chaine, et
         * le maillon precedent est lui aussi dans l'arene. L'oublier ferait
         * fuir exactement ce qu'on essaie de recuperer. */
        if(db->previous) { FreeInvalidDynablock(db->previous, 0); db->previous = NULL; }
        /* DETECTEUR D'USAGE-APRES-LIBERATION, deux moitiees.
         *
         * (1) Relecture du pointeur `self` que FillBlock a ecrit en tete du
         *     bloc. S'il ne se relit pas, quelqu'un a deja ecrit la-dedans et
         *     l'arene est corrompue AVANT nous : on le dit au lieu de rendre
         *     un bloc dont on ne sait plus rien.
         * (2) Empoisonnement du code ARM rendu, avec l'instruction ARM
         *     PERMANENTE INDEFINIE 0xE7F001F0 (UDF, celle que Linux et gdb
         *     utilisent). C'est ce qui transforme « un fil est revenu dans un
         *     bloc libere » — indetectable, corruption silencieuse, la faute
         *     que ce lot doit absolument ne pas introduire — en un
         *     deroutement immediat et nomme. La memoire rendue etant vite
         *     reutilisee par une autre traduction, la fenetre est courte,
         *     mais c'est precisement la fenetre ou une grace fautive se
         *     manifesterait.
         *     Cout : un memset par bloc RENDU, jamais par bloc execute.
         *     D2_JITEVICT_POISON=0 le desarme si jamais il coute trop. */
        if(*(dynablock_t**)db->actual_block != db) {
            ++dyn86_ev_corrupt;
            if(dyn86_ev_corrupt==1)
                dyn86_ev_say("INTEGRITE: pointeur self illisible avant liberation");
        } else if(dyn86_ev_poison()) {
            uint32_t* w = (uint32_t*)db->actual_block;
            size_t n = (size_t)db->size / sizeof(uint32_t);
            for(size_t k=0; k<n; ++k) w[k] = 0xE7F001F0u;   /* ARM UDF */
        }
        freed += (size_t)db->size;
        FreeDynarecMap((uintptr_t)db->actual_block);
        customFree(db);
        ++dyn86_ev_reclaimed;
    }
    g_retire_n = 0; g_retire_emus = 0;
    dyn86_ev_bytes += (uint64_t)freed;
    return freed;
}

/* Retire une vague : desinscrit des blocs et prend l'instantane de grace.
 * Ne rend rien tout de suite — c'est le point. */
static void dyn86_retire_wave(x86emu_t* self, int want)
{
    if(g_retire_n || g_emureg_overflow)
        return;                      /* une vague a la fois */
    if(want > DYN86_RETIRE_MAX) want = DYN86_RETIRE_MAX;
    int n = dyn86_jit_collect_victims(g_retire, want);
    if(n <= 0) return;
    /* DEDOUBLONNAGE PAR CONSTRUCTION. InvalidDynablock rend NULL quand le bloc
     * etait DEJA `gone` : c'est donc lui, et non une confiance accordee a
     * l'enumerateur, qui garantit qu'un bloc n'entre qu'une fois dans la
     * vague. Un doublon signifierait une double liberation — la faute meme
     * que ce lot doit eviter — et le filet doit etre ici, au point ou la
     * vague se constitue, pas dans la marche qui l'alimente : l'enumerateur
     * est une heuristique de choix, celui-ci est un invariant. */
    int k = 0;
    for(int i=0; i<n; ++i)
        if(InvalidDynablock(g_retire[i], 0))   /* on tient deja mutex_dyndump */
            g_retire[k++] = g_retire[i];
    if(k <= 0) return;
    g_retire_n = k;
    dyn86_ev_retired += (uint32_t)k;
    /* Instantane APRES le retrait : un fil qui entre dans DynaRun ensuite ne
     * peut plus atteindre ces blocs (ils ne sont plus dans la table de
     * sauts), donc seuls comptent ceux deja dedans. */
    g_retire_emus = g_emureg_n;
    if(g_retire_emus > DYN86_MAXEMU) g_retire_emus = DYN86_MAXEMU;
    for(int i=0; i<g_retire_emus; ++i) {
        x86emu_t* e = g_emureg[i];
        const volatile uint32_t* pg = e ? &e->dyn86_rungen : NULL;
        g_retire_gen[i] = pg ? *pg : 0;
    }
    dyn86_nudge_others(self);
}

/* Un tour d'eviction, appele quand FillBlock a echoue FAUTE DE MEMOIRE.
 * Rend non nul si de la memoire a ete rendue (donc si une retraduction vaut
 * la peine d'etre retentee tout de suite). Sous mutex_dyndump. */
/* Interrupteur d'arret (D2_JITEVICT=0). Sa raison d'etre est double : c'est le
 * repli si l'eviction se revele fautive sur une console, et c'est le bras
 * « avant » de l'A/B — le protocole du projet veut les deux bras dans LE MEME
 * binaire, un seul bouton entre eux, sinon on compare deux compilations.
 * Teste au site d'appel et non dans le tour : desarme, PAS UN SEUL compteur ne
 * bouge et pas une ligne n'est ecrite, si bien que le bras « avant » est
 * exactement le comportement d'avant ce lot. Absent => arme (convention
 * D2_FAMINE : l'absence arme, seul un '0' explicite desarme). */
int dyn86_evict_armed(void)
{
    static int armed = -1;
    if(armed < 0) {
        const char* e = getenv("WX86_JITEVICT"); if(!e) e = getenv("D2_JITEVICT");
        armed = (e && *e=='0') ? 0 : 1;
        if(!armed) fprintf(stderr, "[jit] EVICTION DESARMEE (D2_JITEVICT=0)\n");
    }
    return armed && !g_emureg_overflow;
}

/* Taille de vague ADAPTATIVE. 64 blocs (~40 Kio) suffisent largement quand
 * l'arene a juste besoin d'un peu d'air — c'est le cas nominal, « evincer un
 * peu, rarement ». Sous pression reelle, en revanche, chaque recuperation qui
 * aboutit doit rapporter assez pour servir plusieurs traductions, sinon on
 * repaie la periode de grace pour 40 Kio a chaque fois. La vague double donc a
 * chaque tour qui n'a rien pu rendre, et retombe a 64 des qu'une traduction
 * est sauvee : elle suit la demande au lieu de la deviner. */
static int g_wave = 64;

static void dyn86_ev_atexit(void) { dyn86_ev_dump("BILAN DE FIN DE RUN"); }

static int dyn86_evict_round(x86emu_t* emu, size_t want)
{
    (void)want;
    g_arena_full = 1;
    ++dyn86_ev_rounds;
    /* BILAN DE FIN DE RUN, arme au premier tour seulement. Les lignes
     * intermediaires sont limitees en debit (sinon elles noieraient le
     * journal), si bien que la DERNIERE ligne visible n'est pas l'etat final —
     * piege dans lequel cette analyse est tombee une fois, en lisant
     * « rendus=0 » sur une ligne emise au premier ajournement alors que des
     * milliers de blocs avaient ete rendus ensuite. Un compteur qu'on ne peut
     * lire qu'a un instant arbitraire ne mesure rien.
     * Arme ICI et pas au boot : sans eviction, aucune ligne — ce qui rend
     * l'assertion « l'eviction n'a jamais tire » verifiable par l'absence. */
    /* Une ligne au TOUT PREMIER tour, toujours. C'est ce qui rend l'assertion
     * « l'eviction n'a jamais tire » verifiable par l'ABSENCE de ligne : sans
     * elle, un journal muet voudrait dire soit « zero eviction » soit « des
     * evictions qui n'ont jamais atteint un palier d'affichage », et le banc
     * anti-emballement ne prouverait rien. */
    if(dyn86_ev_rounds == 1) { atexit(dyn86_ev_atexit); dyn86_ev_say("premier tour"); }
    size_t freed = dyn86_reclaim_wave(emu);  /* vague precedente, si mure */
    if(freed) { g_wave = 64; return 1; }
    dyn86_retire_wave(emu, g_wave);          /* sinon, en preparer une */
    freed = dyn86_reclaim_wave(emu);         /* souvent mure tout de suite */
    if(freed) { g_wave = 64; return 1; }
    if(g_wave < DYN86_RETIRE_MAX) g_wave <<= 1;
    return 0;
}

/* Une ligne de journal a chaque changement d'etat notable, jamais par bloc.
 * Sur console elle atterrit dans boot_progress.txt ; sous qemu,
 * wx86_vita_progress_c est inerte, d'ou le stderr en plus — sans quoi la
 * validation de niveau 1 n'aurait rien a lire. */
static void dyn86_ev_say(const char* what) { dyn86_ev_dump(what); }

/* Publie l'etat complet des compteurs. Exporte, parce que le seul endroit qui
 * sait qu'un fil invite vient REELLEMENT de mourir est dynarec.c (le repli
 * vers Run(), talon sans interpreteur), et qu'une mort qui ne s'accompagne pas
 * du bilan de l'eviction laisse le lecteur incapable de dire si le mecanisme
 * a echoue ou n'a simplement jamais ete sollicite. */
void dyn86_ev_dump(const char* what)
{
    char m[208];
    snprintf(m, sizeof m,
        "JIT eviction: %s — tours=%u retires=%u rendus=%u ajournes=%u octets=%lluKo sauvees=%u rendues-au-reessai=%u sorties=%u morts=%u integrite=%u emus=%d retient=%d(prof=%u gen=%u/%u)",
        what, dyn86_ev_rounds, dyn86_ev_retired, dyn86_ev_reclaimed,
        dyn86_ev_deferred, (unsigned long long)(dyn86_ev_bytes>>10),
        dyn86_ev_refills, dyn86_ev_handback, dyn86_ev_oomexit, dyn86_ev_fatal, dyn86_ev_corrupt,
        g_emureg_n, g_grace_blocker, g_grace_bd, g_grace_bg,
        (g_grace_blocker>=0 && g_grace_blocker<DYN86_MAXEMU) ? g_retire_gen[g_grace_blocker] : 0u);
    fprintf(stderr, "[jit] %s\n", m);
    wx86_vita_progress_c(m);
}

static dynablock_t* internalDBGetBlock(x86emu_t* emu, uintptr_t addr, uintptr_t filladdr, int create, int need_lock)
{
    if(hasAlternate((void*)addr))
        return NULL;
    dynablock_t* block = getDB(addr);
    if(block || !create) {
        // HIT (definition): the jump table already points at a live block for
        // this guest address, so the lookup returns without translating.
        if(dyn86_jitprof && block) ++dyn86_jp_lookup_hits;
        return block;
    }

    if(need_lock) {
        if(box86_dynarec_wait) {
            mutex_lock(&my_context->mutex_dyndump);
        } else {
            if(mutex_trylock(&my_context->mutex_dyndump))   // FillBlock not available for now
                return NULL;
        }
        block = getDB(addr);    // just in case
        if(block) {
            if(dyn86_jitprof) ++dyn86_jp_lookup_hits;   // raced, still a hit
            mutex_unlock(&my_context->mutex_dyndump);
            return block;
        }
        /* CHEMIN RAPIDE DE PENURIE (voir g_arena_full). Une vague attend sa
         * grace et l'arene est pleine : tenter la traduction serait payer deux
         * passes du traducteur, sous le verrou, pour un echec certain. On
         * essaie de rendre la vague ; si elle n'est pas mure, on rend la main
         * TOUT DE SUITE — c'est ce qui laisse les autres fils sortir de
         * DynaRun, et c'est leur sortie, pas notre acharnement, qui debloque
         * la situation. */
        if(g_arena_full && g_retire_n) {
            if(!dyn86_reclaim_wave(emu)) {
                dyn86_nudge_others(emu);
                emu->dyn86_evwait = 1;
                mutex_unlock(&my_context->mutex_dyndump);
                return NULL;
            }
            /* La vague etait mure : la traduction qui suit ne tient QUE grace a
             * la memoire qu'on vient de rendre. C'est donc bien une traduction
             * sauvee, au meme titre que celles du chemin lent — ne pas la
             * compter ici laisserait « sauvees=0 » sur des runs ou l'eviction
             * a tout porte, et le compteur mentirait par omission. */
            ++dyn86_ev_refills;
        }
    }

    // MISS (definition): no live block for this address -> this lookup pays a
    // full translation (a NEW block, or a re-translation of one just
    // invalidated by the hash test in DBGetBlock).
    if(dyn86_jitprof) ++dyn86_jp_lookup_miss;
    block = AddNewDynablock(addr);

    /* D2Vita — REFUS DU TAS DE METADONNEES. AddNewDynablock passe par
     * customCalloc, donc par les blocs mmap de 64 Kio de custommem.c : le
     * SEUL poste memoire que ce port demande encore au noyau en pleine
     * partie. Ce refus existait deja sur console (RAM utilisateur engagee a
     * 100% au boot) mais n'etait pas un refus : customMalloc ecrivait a
     * travers MAP_FAILED et tuait le processus (signature
     * hfault_sys|SceLibKernel|0x120, 0.1.7 et 0.1.9, cinq consoles).
     * Maintenant que le refus remonte, il se traite comme le refus d'arene
     * JIT juste en dessous, avec la MEME machinerie : une vague d'eviction
     * rend les dynablock_t par customFree (cf. le commentaire l.287), donc
     * elle rend exactement la memoire qui manque ici.
     * Le NULL final n'est pas une mort : c'est la boucle de reessai de
     * DBGetBlock qui cede HORS VERROU, seule fenetre de grace legitime. */
    if(!block && dyn86_evict_armed()) {
        for(int ev=0; ev<DYN86_EV_ROUNDS && !block; ++ev) {
            if(!dyn86_evict_round(emu, sizeof(dynablock_t)))
                break;
            block = AddNewDynablock(addr);
        }
        if(block) ++dyn86_ev_refills;
    }
    if(!block) {
        ++dyn86_meta_allocfail;
        if(dyn86_meta_allocfail == 1 || !(dyn86_meta_allocfail & 0xFF))
            dyn86_ev_say("refus du tas de metadonnees");
        if(g_retire_n) emu->dyn86_evwait = 1;
        if(need_lock)
            mutex_unlock(&my_context->mutex_dyndump);
        return NULL;
    }

    // fill the block
    block->x86_addr = (void*)addr;
    if(sigsetjmp(DYN_JMPBUF, 1)) {
        printf_log(LOG_INFO, "FillBlock at %p triggered a segfault, cancelling\n", (void*)addr);
        FreeDynablock(block, 0);
        if(need_lock)
            mutex_unlock(&my_context->mutex_dyndump);
        return NULL;
    }
    uint64_t dyn86_fb_t0 = dyn86_prof_now_ns();
    uint32_t dyn86_af0 = dyn86_jit_allocfail;
    void* ret = FillBlock(block, filladdr);
    dyn86_fill_ns += dyn86_prof_now_ns() - dyn86_fb_t0; dyn86_fill_count++;
    /* D2Vita (lot eviction JIT) — LE SITE. On est ici sous mutex_dyndump dans
     * les DEUX cas (need_lock=1 : pris plus haut ; need_lock=0 : l'appelant le
     * tient, cf. le commentaire de l'auto-interblocage plus bas), et aucun
     * autre verrou n'est detenu : liberer d'ici emprunte exactement l'arete
     * dyndump -> dynarec -> g_blk_mx que FreeDynablock utilise depuis
     * toujours. Aucun ordre de verrou nouveau n'est introduit par l'eviction.
     *
     * La condition porte sur dyn86_jit_allocfail, pas sur `!ret` : FillBlock
     * rend aussi NULL pour un opcode non implemente ou un abandon de passe,
     * et evincer du code chaud n'y changerait rien. Seul un refus de
     * l'ALLOCATEUR justifie de rendre de la memoire.
     *
     * La borne est ici, pas dans une boucle d'attente : DYN86_EV_ROUNDS
     * tentatives, chacune une retraduction immediate si de la memoire a ete
     * rendue. Si la grace n'est pas passee, on ne PATIENTE PAS en tenant le
     * verrou — ce serait l'interblocage certain contre un fil bloque sur
     * mutex_dyndump dans LinkNext (box86_dynarec_wait=1). On rend NULL, et
     * c'est la boucle de reessai de DBGetBlock qui cede HORS VERROU : c'est
     * elle, la fenetre de grace, et elle ne coute rien de plus. */
    if(!ret && dyn86_jit_allocfail != dyn86_af0 && dyn86_evict_armed()) {
        for(int ev=0; ev<DYN86_EV_ROUNDS && !ret; ++ev) {
            if(!dyn86_evict_round(emu, (size_t)block->x86_size))
                break;                       /* rien rendu : la grace n'est pas passee */
            dyn86_fb_t0 = dyn86_prof_now_ns();
            ret = FillBlock(block, filladdr);
            dyn86_fill_ns += dyn86_prof_now_ns() - dyn86_fb_t0; dyn86_fill_count++;
        }
        /* Rien n'a pu etre rendu : il ne sert a RIEN de retenter ici. Le fil
         * courant est dans DynaRun, donc il retient lui-meme la grace de la
         * vague qu'il vient de retirer ; seule sa SORTIE la debloquera. On le
         * dit a DBGetBlock, qui abandonne sa boucle de reessai sur-le-champ au
         * lieu de refaire 64 traductions vouees a echouer. */
        if(!ret && g_retire_n) emu->dyn86_evwait = 1;
        if(ret) {
            ++dyn86_ev_refills;
            if(dyn86_ev_refills==1 || !(dyn86_ev_refills & 0x3F)) dyn86_ev_say("traduction sauvee");
        } else {
            /* Borne atteinte sans avoir pu liberer. On rend la main a la
             * boucle de reessai de DBGetBlock, qui cede HORS VERROU : c'est
             * elle, la fenetre de grace, et le tour suivant trouve tres
             * souvent la vague mure. Ce n'est donc PAS la mort du fil, et le
             * compteur ne doit pas pretendre le contraire. */
            ++dyn86_ev_handback;
            if(dyn86_ev_handback==1 || !(dyn86_ev_handback & 0xFF)) dyn86_ev_say("rendu a la boucle de reessai");
        }
    }
    if(!ret) {
        dyn86_fill_fail++;
        dynarec_log(LOG_DEBUG, "Fillblock of block %p for %p returned an error\n", block, (void*)addr);
        customFree(block);
        block = NULL;
    }
    // check size
    if(block) {
        int blocksz = block->x86_size;
        if(blocksz>my_context->max_db_size)
            my_context->max_db_size = blocksz;
        // fill-in jumptable
        // D2Vita quantum mode: publish the jmpnext stub instead of the direct
        // block so every chain funnels through LinkNext (getDB still works:
        // the dynablock_t* sits right before jmpnext too).
        if(!addJumpTableIfDefault(block->x86_addr, (block->dirty||!dyn86_link_direct())?block->jmpnext:block->block)) {
            FreeDynablock(block, 0);
            block = getDB(addr);
            // need_lock=0 : on tient DEJA mutex_dyndump ici, dans les DEUX cas.
            // Si le parametre need_lock de cette fonction valait 1 on l'a pris
            // plus haut ; s'il valait 0 c'est que l'appelant le tenait (le
            // trylock de DBGetBlock rend 0 en cas de SUCCES et cette valeur est
            // passee telle quelle). Il n'existe aucun chemin qui atteigne cette
            // ligne sans le mutex. Passer 1 ici etait l'auto-interblocage.
            MarkDynablock(block, 0);   // just in case...
        } else {
            if(block->x86_size)
                block->done = 1;    // don't validate the block if the size is null, but keep the block
            // Published. A re-translation is counted as a recompile at the
            // hash-mismatch site, not here, so "created" stays "blocks that
            // did not exist before".
            if(dyn86_jitprof && !dyn86_jp_recomp) ++dyn86_jp_created;
        }
    }
    if(need_lock)
        mutex_unlock(&my_context->mutex_dyndump);

    dynarec_log(LOG_DEBUG, "%04d| --- DynaRec Block created @%p:%p (%p, 0x%x bytes)\n", GetTID(), (void*)addr, (void*)(addr+((block)?block->x86_size:1)-1), (block)?block->block:0, (block)?block->size:0);

    return block;
}

dynablock_t* DBGetBlock(x86emu_t* emu, uintptr_t addr, int create)
{
    if(isInHotPage(addr))
        return NULL;
    // D2_JITPROFILE: time 1 lookup in DYN86_JP_SAMPLE. A single lookup costs
    // far less than the clock's resolution, so this is a SAMPLED ESTIMATE, not
    // a measurement: the per-call FillBlock time is subtracted (translation is
    // reported separately) and so is the calibrated cost of one clock read.
    // Timing every call would cost more than the path being measured.
    uint64_t jp_t0 = 0, jp_fill0 = 0; int jp_sample = 0;
    if(dyn86_jitprof) {
        ++dyn86_jp_lookup_calls;
        if(!(dyn86_jp_lookup_calls & (uint64_t)(DYN86_JP_SAMPLE-1))) {
            jp_sample = 1; jp_fill0 = dyn86_fill_ns; jp_t0 = dyn86_prof_now_ns();
        }
    }
    // D2Vita concurrency retry (native scheduler, torture mt/smc): a lookup can
    // observe a block whose done was zeroed by a CONCURRENT invalidator — the
    // SMC write barrier / FlushInstructionCache path (unprotectDB(mark=1) ->
    // cleanDBFromAddressRange -> MarkDynablock) runs under mutex_prot only,
    // NEVER mutex_dyndump, and translated-code dispatch holds no GIL. When the
    // lost-mark fallback (MarkDynablock: old->done=0) or FreeDynablock lands
    // between internalDBGetBlock's return and the db->done gate below, the
    // revalidation is skipped and a not-done block reaches DynaRun. Upstream
    // box86 shrugs: Run(emu,1) interprets one step and re-looks-up — a de
    // facto retry. D2Vita has NO interpreter (Run() is an aborting stub), so
    // the faithful minimal equivalent is to RETRY the lookup here, bounded:
    // the transient window closes as soon as the invalidator's step completes
    // (next lookup either creates a fresh block under mutex_dyndump or finds
    // the newly published one). A PERMANENT failure (FillBlock unimplemented
    // opcode, fill segfault-cancel) stays NULL through every retry and still
    // reaches the honest controlled abort, just DYN86_DBGET_RETRY yields
    // later. create==0 callers keep the immediate NULL (means "not translated
    // yet", not a race). DYN86_DBGET_RETRY lives in dynablock.h (shared with
    // DBAlternateBlock and DynaRun's downstream re-lookup, dynarec.c).
    dynablock_t *db;
    int dyn86_retry = 0;
    for(;;) {
        db = internalDBGetBlock(emu, addr, addr, create, 1);
        if(db && db->done && db->block && getNeedTest(addr)) {
            if(db->always_test)
                sched_yield();  // just calm down...
            uint32_t hash = X31_hash_code((void*)DYN86_G2H(db->x86_addr), db->x86_size);
            int need_lock = mutex_trylock(&my_context->mutex_dyndump);
            if(hash!=db->hash) {
                if(dyn86_jitprof) { ++dyn86_jp_recompiles; dyn86_jp_recomp = 1; }
                db->done = 0;   // invalidating the block
                dynarec_log(LOG_DEBUG, "Invalidating block %p from %p:%p (hash:%X/%X, always_test:%d) for %p\n", db, db->x86_addr, db->x86_addr+db->x86_size-1, hash, db->hash, db->always_test, (void*)addr);
                // Free db, it's now invalid!
                dynablock_t* old = InvalidDynablock(db, need_lock);
                // start again... (will create a new block)
                db = internalDBGetBlock(emu, addr, addr, create, need_lock);
                if(db) {
                    if(db->previous)
                        FreeInvalidDynablock(db->previous, need_lock);
                    db->previous = old;
                } else
                    FreeInvalidDynablock(old, need_lock);
                dyn86_jp_recomp = 0;
            } else {
                dynarec_log(LOG_DEBUG, "Validating block %p from %p:%p (hash:%X, always_test:%d) for %p\n", db, db->x86_addr, db->x86_addr+db->x86_size-1, db->hash, db->always_test, (void*)addr);
                protectDB((uintptr_t)db->x86_addr, db->x86_size);
                // fill back jumptable (unless D2Vita quantum mode: stay funneled)
                if(isprotectedDB((uintptr_t)db->x86_addr, db->x86_size) && !db->always_test && dyn86_link_direct()) {
                    setJumpTableIfRef(db->x86_addr, db->block, db->jmpnext);
                }
            }
            if(!need_lock)
                mutex_unlock(&my_context->mutex_dyndump);
        }
        if(db && db->block && db->done)
            break;                                   // valid block: done
        if(!create || ++dyn86_retry > DYN86_DBGET_RETRY)
            break;                                   // permanent: honest abort path
        if(emu->dyn86_evwait) { emu->dyn86_evwait = 0; sched_yield(); break; }  // ceder, puis sortir de DynaRun
        sched_yield();                               // let the invalidator finish its step
    }
    if(!db || !db->block || !db->done)
        emu->test.test = 0;
    if(jp_sample) {
        uint64_t dt = dyn86_prof_now_ns() - jp_t0;
        uint64_t fdt = dyn86_fill_ns - jp_fill0;         // translation: reported apart
        dt = (dt > fdt) ? dt - fdt : 0;
        dt = (dt > dyn86_jp_clock_bias_ns) ? dt - dyn86_jp_clock_bias_ns : 0;
        dyn86_jp_lookup_ns += dt; ++dyn86_jp_lookup_smp;
    }
    return db;
}

dynablock_t* DBAlternateBlock(x86emu_t* emu, uintptr_t addr, uintptr_t filladdr)
{
    dynarec_log(LOG_DEBUG, "Creating AlternateBlock at %p for %p\n", (void*)addr, (void*)filladdr);
    int create = 1;
    // D2Vita concurrency retry: same invalidation dance as DBGetBlock, same
    // window (see the comment there — flagged in 427d51f's own commit body).
    // create is always 1 here, so a permanent failure (NULL or never-done)
    // exhausts the bound and returns as before: the caller's honest path.
    dynablock_t *db;
    int dyn86_retry = 0;
    for(;;) {
        db = internalDBGetBlock(emu, addr, filladdr, create, 1);
        if(db && db->done && db->block && getNeedTest(filladdr)) {
            if(db->always_test)
                sched_yield();  // just calm down...
            int need_lock = mutex_trylock(&my_context->mutex_dyndump);
            uint32_t hash = X31_hash_code((void*)DYN86_G2H(db->x86_addr), db->x86_size);
            if(hash!=db->hash) {
                if(dyn86_jitprof) { ++dyn86_jp_recompiles; dyn86_jp_recomp = 1; }
                db->done = 0;   // invalidating the block
                dynarec_log(LOG_DEBUG, "Invalidating alt block %p from %p:%p (hash:%X/%X) for %p\n", db, db->x86_addr, db->x86_addr+db->x86_size, hash, db->hash, (void*)addr);
                // Free db, it's now invalid!
                dynablock_t* old = InvalidDynablock(db, need_lock);
                // start again... (will create a new block)
                db = internalDBGetBlock(emu, addr, filladdr, create, need_lock);
                if(db) {
                    if(db->previous)
                        FreeInvalidDynablock(db->previous, need_lock);
                    db->previous = old;
                } else
                    FreeInvalidDynablock(old, need_lock);
                dyn86_jp_recomp = 0;
            } else {
                protectDB((uintptr_t)db->x86_addr, db->x86_size);
                // fill back jumptable (unless D2Vita quantum mode: stay funneled)
                if(isprotectedDB((uintptr_t)db->x86_addr, db->x86_size) && !db->always_test && dyn86_link_direct()) {
                    setJumpTableIfRef(db->x86_addr, db->block, db->jmpnext);
                }
            }
            if(!need_lock)
                mutex_unlock(&my_context->mutex_dyndump);
        }
        if(db && db->block && db->done)
            break;                                   // valid block: done
        if(++dyn86_retry > DYN86_DBGET_RETRY)
            break;                                   // permanent: honest abort path
        if(emu->dyn86_evwait) { emu->dyn86_evwait = 0; sched_yield(); break; }  // sortir de DynaRun d'abord
        sched_yield();                               // let the invalidator finish its step
    }
    if(!db || !db->block || !db->done)
        emu->test.test = 0;
    return db;
}
