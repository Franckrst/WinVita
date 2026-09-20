#ifndef __X86EMU_PRIVATE_H_
#define __X86EMU_PRIVATE_H_

#include "regs.h"

typedef struct box86context_s box86context_t;
typedef struct i386_ucontext_s i386_ucontext_t;

#define ERR_UNIMPL  1
#define ERR_DIVBY0  2
#define ERR_ILLEGAL 4

#ifdef DYNAREC
#define CSTACK      32
#define CSTACKMASK  31
#endif

typedef struct forkpty_s {
    void*    amaster;
    void*   name;
    void*   termp;
    void*   winp;
    void*   f;  // forkpty function
} forkpty_t;

typedef struct x86emu_s x86emu_t;

typedef struct x86test_s {
    x86emu_t*   emu;
    uintptr_t   memaddr;
    int         memsize;
    int         test;
    int         clean;
    uint8_t     mem[16];
} x86test_t;

typedef struct emu_flags_s {
    uint32_t    need_jmpbuf:1;    // need a new jmpbuff for signal handling
    uint32_t    quitonlongjmp:2;  // quit if longjmp is called
    uint32_t    quitonexit:2;     // quit if exit/_exit is called
    uint32_t    longjmp:1;        // if quit because of longjmp
    uint32_t    jmpbuf_ready:1;   // the jmpbuf in the emu is ok and don't need refresh
} emu_flags_t;

#if defined(__vita__)
#include "dyn86_vita_compat.h"   /* newlib: jmp_buf + sigsetjmp->setjmp */
#define JUMPBUFF jmp_buf
#elif defined(ANDROID)
#include <setjmp.h>
#define JUMPBUFF sigjmp_buf
#else
#define JUMPBUFF struct __jmp_buf_tag
#endif

typedef struct x86emu_s {
    // cpu
	reg32_t     regs[8];
	x86flags_t  eflags;
    reg32_t     ip;
    uintptr_t   xSPSave;
    // fpu / mmx
	x87control_t cw;
	x87flags_t  sw;
	mmx87_regs_t x87[8];
	mmx87_regs_t mmx[8];
	uint32_t    top;        // top is part of sw, but it's faster to have it separatly
    int         fpu_stack;
	uint32_t    fpu_tags; // tags for the x87 regs, stacked, only on a 16bits anyway
    fpu_ld_t    fpu_ld[8]; // for long double emulation / 80bits fld fst
    fpu_ll_t    fpu_ll[8]; // for 64bits fild / fist sequence
    // sse
    sse_regs_t  xmm[8];
    mmxcontrol_t mxcsr;
    uintptr_t   old_ip;
    // defered flags
    defered_flags_t df;
    uint32_t    op1;
    uint32_t    op2;
    uint32_t    res;
    uint32_t    *x86emu_parity_tab; // helper
    #ifdef HAVE_TRACE
    uintptr_t   prev2_ip, prev_ip;
    #endif
    // segments
    uint16_t    segs[6];
    uint16_t    dummy_seg6, dummy_seg7; // to stay aligned
    uintptr_t   segs_offs[6];   // computed offset associate with segment
    uint32_t    segs_serial[6];  // are seg offset clean (not 0) or does they need to be re-computed (0)? For GS, serial need to be the same as context->sel_serial
    // emu control
    int         quit;
    int         error;
    int         fork;   // quit because need to fork
    int         exit;
    forkpty_t*  forkpty_info;
    emu_flags_t flags;
    x86test_t   test;       // used for dynarec testing
    // parent context
    box86context_t *context;
    // cpu helpers
    reg32_t     zero;
    reg32_t     *sbiidx[8];
    // scratch stack, used for alignement of double and 64bits ints on arm. 200 elements should be enough
    uint32_t    scratch[200];
    // local stack, do be deleted when emu is freed
    void*       stack2free; // this is the stack to free (can be NULL)
    void*       init_stack; // initial stack (owned or not)
    uint32_t    size_stack; // stack size (owned or not)
    JUMPBUFF*   jmpbuf;
    uintptr_t   old_savedsp;

    i386_ucontext_t *uc_link; // to handle setcontext

    int         type;       // EMUTYPE_xxx define

    // D2Vita: block-entry preemption budget. Every translated block's emitted
    // prologue decrements dyn86_budget; on expiry it stores ip = block start,
    // sets dyn86_bbreak=2 + quit=1 and exits via arm_epilog. This replaces the
    // LinkNext-funnel quantum (jump-table poisoning), letting blocks
    // DIRECT-LINK under the scheduler. Fields live at the struct tail so no
    // vendored offsets change.
    int         dyn86_budget;
    int         dyn86_bbreak;
    // D2Vita : vivacite PAR FIL (diagnostic famine/interblocage du backend
    // natif). dyn86_armed = valeur dont dyn86_budget a ete CHARGE au debut de
    // la tranche courante ; dyn86_blocks = total des blocs deja comptabilises
    // par les tranches precedentes. Le nombre de blocs executes depuis le boot
    // vaut donc dyn86_blocks + (dyn86_armed - dyn86_budget) tant que le budget
    // est > 0, et dyn86_blocks quand il est nul (fil poke par le filet
    // anti-famine). Ni le prologue de bloc ni aucun code genere n'y touchent :
    // cout nul sur le chemin chaud, une addition par TRANCHE.
    int         dyn86_armed;
    uint32_t    dyn86_blocks;
    // D2Vita (coeur2, 06/09/2026) : densite de traps et contention du GIL PAR
    // FIL. Ecrits UNIQUEMENT sous D2_FILSTAT=1, dans la fenetre de trap de
    // CpuBox86::run() ; aucun code genere n'y touche. Queue de structure :
    // aucun offset amont ne bouge (le prologue de bloc lit dyn86_budget a son
    // offset d'avant). Lus sans verrou par le battement famine (ligne fils:).
    uint32_t    dyn86_traps;        // entrees dans la fenetre de trap (= prises du GIL par ce fil)
    uint32_t    dyn86_gilcont;      // prises CONTENDUES (trylock echoue, puis lock bloquant)
    uint32_t    dyn86_gilwait_us;   // attente cumulee sur ces prises contendues (us, modulo 2^32)
    // D2Vita (lot eviction JIT) : PERIODE DE GRACE. Ces deux champs disent, a
    // un observateur SANS VERROU, si ce fil invite peut etre en train
    // d'executer du code traduit — la seule question qui autorise ou interdit
    // de rendre la memoire d'un bloc evince.
    //   dyn86_rundepth  > 0  <=>  ce fil est DANS une activation de DynaRun.
    //                   Compteur de PROFONDEUR et non bit de parite : DynaCall
    //                   reentre DynaRun (rappels invite depuis un shim), et un
    //                   schema pair/impair se serait inverse au premier
    //                   imbriquement — c'est-a-dire aurait declare « dehors »
    //                   un fil qui est dedans. Le pire bug possible ici.
    //   dyn86_rungen    incremente a chaque SORTIE vers la profondeur 0. Sert
    //                   a distinguer « toujours dans LA MEME activation
    //                   qu'au moment du retrait » de « est ressorti depuis ».
    // Ecrits UNIQUEMENT par le fil proprietaire, dans DynaRun (dynarec.c) :
    // une activation par trap (~30-100k/s), pas une par bloc. Aucun code
    // genere n'y touche. Queue de structure : aucun offset amont ne bouge.
    // L'ordre memoire est assure par mutex_dyndump et non par des barrieres
    // sur ce chemin chaud — l'argument complet est dans dynablock.c, au-dessus
    // de dyn86_grace_passed().
    uint32_t    dyn86_rundepth;
    uint32_t    dyn86_rungen;
    /* Sorties REPRENABLES consecutives accordees a ce fil parce que la
     * traduction manquait de memoire (dynarec.c). Remis a zero des qu'une
     * traduction aboutit. C'est la BORNE qui empeche l'echange d'un plantage
     * contre un gel : au-dela, le fil retombe sur la mort nommee. */
    uint32_t    dyn86_oomexit;
    /* Valeur de dyn86_ev_reclaimed vue a la derniere sortie OOM de ce fil :
     * c'est elle qui decide si dyn86_oomexit repart de zero (le mecanisme
     * AVANCE quelque part) ou continue de monter (rien ne bouge nulle part). */
    uint32_t    dyn86_oomwatch;
    /* « Une vague est retiree et c'est MOI qui retiens sa grace » : fait sortir
     * DBGetBlock de sa boucle de reessai tout de suite, pour aller chercher la
     * sortie reprenable de DynaRun qui, elle, debloquera la vague. */
    uint32_t    dyn86_evwait;
} x86emu_t;

#define EMUTYPE_NONE    0
#define EMUTYPE_MAIN    1
#define EMUTYPE_SIGNAL  2

// x86 #DE is not resumable: the faulting instruction writes nothing and the
// processor traps. Raising `error` alone left `quit` clear, and DynaRun loops
// on !quit — so the guest carried on past the fault, with the pre-division
// EAX/EDX still in place and the error only noticed at some later block
// boundary (or erased entirely by a budget preemption). Upstream's comment,
// "should rise a SIGFPE and not quit", describes a host-signal design this
// port does not have: the Vita delivers no POSIX signals, so `quit` is how a
// fault leaves the emulator here. Paired with CHECK_DIV0 in the dynarec, which
// is what actually leaves the translated block.
#define INTR_RAISE_DIV0(emu) {emu->error |= ERR_DIVBY0; emu->quit=1;}

void applyFlushTo0(x86emu_t* emu);

#endif //__X86EMU_PRIVATE_H_