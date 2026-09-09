/* dyn86_emitprof.h — PROFIL DU CODE EMIS par le traducteur x86 -> ARMv7.
 *
 * POURQUOI. Sur console, 96,8 % du temps d'une image est de l'execution du
 * code invite traduit (docs/perf/bilan_20260905.md) et les transitions de bloc
 * ont ete refutees avec preuve d'armement. Le temps est donc DANS le code emis.
 * La seule mesure existante etait un scalaire — « expansion=5,343x » — qui dit
 * COMBIEN le traducteur produit, jamais POUR QUOI. Ce module repond a « pour
 * quoi » : par famille d'instruction x86, combien d'octets ARM.
 *
 * DEUX CHIFFRES, JAMAIS CONFONDUS :
 *   - STATIQUE   : octets emis a la TRADUCTION (une fois par bloc traduit).
 *                  Designe le code VOLUMINEUX.
 *   - PONDERE    : les memes octets multiplies par le nombre d'EXECUTIONS du
 *                  bloc. Designe le code CHAUD. C'est celui qui compte.
 * Le compteur d'executions est EXACT (pas echantillonne) : le prologue de
 * chaque bloc incremente un compteur 64 bits qui lui est propre.
 *
 * COUT. Tout est sous -DD2_EMITPROF. Sans ce define, ce fichier ne compile
 * aucune donnee et aucune macro n'emet quoi que ce soit : le binaire livre ne
 * paie RIEN, pas meme une lecture de knob.
 *
 * BANC. qemu-arm uniquement. C'est du COMPTAGE, pas du temps : qemu ne
 * modelise pas les caches, aucune microseconde ni aucun img/s ne sort d'ici.
 */
#ifndef DYN86_EMITPROF_H
#define DYN86_EMITPROF_H

#ifdef D2_EMITPROF

#include <stdint.h>

/* Familles d'instructions x86. L'index D2EP_ORPHAN recueille tout octet emis
 * HORS de la fenetre d'une instruction (prologue de bloc, epilogue, purge de
 * barriere de fin) : c'est ce qui rend le controle croise exact —
 *   somme(fam_b[0..D2EP_NFAM]) == arm_size du bloc == dyn86_emit_arm_bytes. */
enum {
    D2EP_ALU32 = 0,   /* ADD/OR/ADC/SBB/AND/SUB/XOR/CMP/TEST/INC/DEC/NEG/MUL... 32 bits */
    D2EP_ALU8,        /* les memes en 8 bits, et en 16 bits (prefixe 0x66)   */
    D2EP_MOVLEA,      /* MOV, LEA, MOVZX/MOVSX, XCHG, BSWAP, CWDE/CDQ        */
    D2EP_BRANCH,      /* CALL / RET / JMP / Jcc / LOOP / INT                 */
    D2EP_X87,         /* D8..DF + FWAIT                                      */
    D2EP_SSE,         /* 0F : MMX / SSE / SSE2                               */
    D2EP_STRING,      /* MOVS/CMPS/STOS/LODS/SCAS (+ REP)                    */
    D2EP_STACK,       /* PUSH/POP/PUSHA/POPA/PUSHF/POPF/ENTER/LEAVE          */
    D2EP_FLAGS,       /* SETcc, CMOVcc, CLC/STC/CLD/STD/CMC, SAHF/LAHF       */
    D2EP_SHIFT,       /* SHL/SHR/SAR/ROL/ROR/RCL/RCR, SHLD/SHRD, BT*, BSF/BSR*/
    D2EP_OTHER,       /* NOP, CPUID, RDTSC, entrees/sorties, segments...     */
    D2EP_NFAM,
    D2EP_ORPHAN = D2EP_NFAM,
    D2EP_NFAM1        /* = D2EP_NFAM+1, taille des tableaux                  */
};

/* Sous-postes. Ils se RECOUVRENT entre eux et recouvrent les familles : ce
 * sont des annotations, pas une partition. FPUPUSHPOP est inclus dans HELPER,
 * UPDFLAGS aussi. Ne jamais les additionner. */
enum {
    D2EP_S_PROLOG = 0,  /* prologue de bloc (budget de preemption)           */
    D2EP_S_HELPER,      /* tout appel a une fonction C (call_c/_d/_dr/_ddr)  */
    D2EP_S_UPDFLAGS,    /* materialisation des drapeaux (READFLAGS complet)  */
    D2EP_S_FPUPURGE,    /* fpu_purgecache : vidage x87/MMX/SSE vers l'emu    */
    D2EP_S_FPUPUSHPOP,  /* fpu_pushcache/popcache autour d'un appel C        */
    D2EP_S_BLOCKEND,    /* jump_to_next / jump_to_epilog                     */
    D2EP_S_PROBE,       /* LA SONDE ELLE-MEME (8 insns/bloc) — a soustraire   */
    D2EP_S_DEFERFLAGS,  /* archivage op1/op2/res/df pour drapeaux differes    */
    D2EP_S_CMPJCC,      /* paires « ALU qui pose les drapeaux » + Jcc collee  */
    D2EP_S_NSUB
};

/* Correspondance x86 -> offset ARM, une entree par instruction traduite.
 * C'est ce qui rend le desassemblage du code emis LISIBLE : sans elle, un
 * bloc n'est qu'un ruban de mots ARM ou l'on ne sait pas quelle instruction
 * x86 a produit quoi. Arene commune, allouee par bump ; un debordement est
 * signale, jamais silencieux. */
typedef struct { uint32_t x86; uint32_t off; } d2ep_map_t;
#define D2EP_MAPCAP 1200000

typedef struct d2ep_block_s {
    uint64_t exec;                    /* incremente par le PROLOGUE EMIS     */
    uint32_t arm_start;               /* adresse HOTE du code emis           */
    uint32_t map_i, map_n;
    uint32_t x86_addr;
    uint32_t arm_bytes;
    uint32_t x86_bytes;
    uint32_t ninsts;
    uint32_t valid;                   /* 1 = bloc reellement livre           */
    uint32_t fam_b[D2EP_NFAM1];
    uint32_t fam_n[D2EP_NFAM1];
    uint32_t fam_x[D2EP_NFAM1];       /* octets x86 consommes                */
    uint32_t mem_b[D2EP_NFAM1];       /* dont : operande MEMOIRE (mod != 3)  */
    uint32_t mem_n[D2EP_NFAM1];
    uint32_t sub_b[D2EP_S_NSUB];
    uint32_t sub_n[D2EP_S_NSUB];
} d2ep_block_t;

#define D2EP_MAXBLOCKS 32768

extern d2ep_block_t d2ep_blocks[D2EP_MAXBLOCKS];
extern int          d2ep_nblocks;
extern int          d2ep_overflow;
extern d2ep_block_t* d2ep_cur;        /* bloc en cours de traduction (pass2) */
extern int          d2ep_last_pos;    /* arm_size au dernier changement      */
extern int          d2ep_cur_fam;
/* compteurs d'EXECUTION des helpers C (exacts, incrementes dans le C) */
extern d2ep_map_t   d2ep_map[D2EP_MAPCAP];
extern int          d2ep_map_n;
extern int          d2ep_map_overflow;
extern uint64_t     d2ep_run_updateflags;      /* appels a UpdateFlags       */
extern uint64_t     d2ep_run_updateflags_work; /* dont df != d_none          */

int  d2ep_alloc(uintptr_t x86_addr);            /* -1 si sature              */
void d2ep_begin(int idx);
void d2ep_switch(int fam, int pos);             /* cloture la fenetre en cours */
void d2ep_openinst(const uint8_t* p, int pos, uintptr_t x86);
void d2ep_closeinst(int pos, int x86len);
void d2ep_sub(int k, int bytes, int n);
void d2ep_end(int idx, int pos, int x86_bytes, int ninsts);
int  d2ep_classify(const uint8_t* p, int* hasmem);
void d2ep_report(void (*out)(const char*));
void d2ep_setarm(int idx, uintptr_t arm_start);
void d2ep_dump(const char* path, int topn);
void d2ep_csv(const char* path);

#endif /* D2_EMITPROF */
#endif
