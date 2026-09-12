/* D2Vita shim replacing Box86's src/include/bridge.h */
#ifndef __BRIDGE_H_
#define __BRIDGE_H_
#include <stdint.h>
typedef struct bridge_s bridge_t;
typedef struct x86emu_s x86emu_t;
typedef void (*wrapper_t)(x86emu_t* emu, uintptr_t fnc);
static inline void* GetNativeFnc(uintptr_t fnc) { (void)fnc; return 0; }
static inline void* GetNativeFncOrFnc(uintptr_t fnc) { return (void*)fnc; }
/* Translation-time redirect table (D2Vita "guest pristine" mode): when the
 * dynarec translates a call/jmp whose GUEST target is a registered "from", it
 * emits the branch to "to" instead — WITHOUT touching guest memory. Used to run
 * the native-blit shim from Game.exe's genuine, unpatched code. Tiny (linear
 * scan sur 24 places au plus).
 * CORRECTION 2026-08-31 : ce commentaire affirmait « uniquement à la TRADUCTION
 * — jamais à l'exécution ». C'est FAUX. hasAlternate() est la PREMIERE ligne de
 * internalDBGetBlock (third_party/box86-dynarec/dynarec/dynablock.c:258), donc
 * il s'exécute à CHAQUE recherche de dynablock, pas seulement quand on traduit.
 * L'effet reste petit (au plus 24 comparaisons sur un pointeur deja chaud), mais
 * un commentaire qui dit « jamais a l'execution » fait ecarter a tort ce site
 * quand on cherche ou part le temps — et DBGetBlock pese 4,8 % du budget.
 * Set via dyn86_set_alternate (src/dynarec86/alt_table.c, qui porte le contrat
 * complet et son oracle).
 *
 * PLUS AUCUNE BORNE (2026-09-12). La table a vecu bornee a 24 puis 52 places,
 * et tout enregistrement au-dela etait AVALE SANS UN MOT. Elle croit par
 * doublement ; le seul echec restant (allocation refusee) se compte dans
 * dyn86_alt_refus et s'ecrit dans le journal. */
extern uintptr_t *dyn86_alt_from;
extern uintptr_t *dyn86_alt_to;
extern int dyn86_alt_n;       /* alternates POSES ; publie APRES l'entree */
extern int dyn86_alt_refus;   /* alternates PERDUS (allocation refusee) */
static inline int hasAlternate(void* addr) {
    for(int i=0;i<dyn86_alt_n;i++) if(dyn86_alt_from[i]==(uintptr_t)addr) return 1;
    return 0;
}
static inline void* getAlternate(void* addr) {
    for(int i=0;i<dyn86_alt_n;i++) if(dyn86_alt_from[i]==(uintptr_t)addr) return (void*)dyn86_alt_to[i];
    return addr;
}
static inline uintptr_t CheckBridged(bridge_t* bridge, void* fnc) { (void)bridge; (void)fnc; return 0; }
#endif
