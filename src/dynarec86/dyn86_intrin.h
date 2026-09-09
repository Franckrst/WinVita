/* src/dynarec86/dyn86_intrin.h — D2Vita : INTRINSEQUES NATIVES reconnues
 * A LA TRADUCTION. Generalisation du mecanisme prouve par D2_MEMINTRIN.
 * ------------------------------------------------------------------------
 *
 * POURQUOI CE MODULE EXISTE
 *
 * Le profil EIP du chemin Glide, mesure sur CONSOLE le 07/09 et reproduit sur
 * trois passes (docs/perf/profil_glide_console_20260907.md), designe une
 * fonction unique :
 *
 *     Game+0x10dd60 (VA 0x50dd60) = 11,8-12,6 % de tout le temps invite
 *     projection monde -> ecran, 173 octets d'entiers PURS, aucun appel,
 *     3 700-5 300 appels par image.
 *
 * Cette cible avait deja ete reperee le 06/09 (dessin_glide_20260906.md:143)
 * et REFUSEE, avec un motif exact :
 *
 *     « non — 409 o d'entiers purs, mais 4-5 k traps par image ; le corps
 *       invite est de l'ordre de 2-3 blocs traduits, LE TRAP COUTE PLUS »
 *
 * Le refus portait sur le mecanisme de CROCHET (alternate + trap), qui coute
 * ~1,3 us console par aller-retour. Il ne portait pas sur l'idee de porter la
 * fonction. Or D2_MEMINTRIN a prouve sur console le 07/09 qu'un appel natif
 * DIRECT emis a la traduction — pas de trap, pas de repartiteur — fonctionne a
 * l'echelle : 514 214 appels servis, 0 repli, 0 rejet.
 *
 * Ce module extrait ce mecanisme de memintrin et le rend TABULAIRE, pour que
 * la question « le trap coute plus que le corps » ne se pose plus jamais.
 *
 * CE QUE LE TRADUCTEUR EMET, quand un bloc COMMENCE a une VA enregistree :
 *
 *     mov r1, xESP                  ; ESP invite (les arguments de pile)
 *     bl  <helper>                  ; r0 = xEmu, contrat int fn(emu, esp)
 *     cmp r0, #0
 *     beq <corps traduit d'origine> ; REPLI FIDELE : rien n'a ete fait
 *     [ldr xREG, [xEmu, regs[REG]]] ; pour chaque registre de `regmask`
 *     <ret_to_epilog | retn_to_epilog(retn)>
 *
 * Aucun trap, aucun passage par le repartiteur : un appel natif direct depuis
 * le code traduit, comme les helpers div32/imul8 de box86.
 *
 * ⚡ CE QUI REND LA RECONNAISSANCE PAR DEBUT DE BLOC LEGITIME
 *
 * Elle ne capture une fonction que si TOUTE entree cree un bloc qui COMMENCE a
 * son premier octet. Il faut donc, pour chaque cible, verifier au
 * desassemblage qu'il n'existe :
 *   - aucun `jmp` direct vers l'entree (ce serait une queue d'appel : le bloc
 *     ne recommencerait pas) — un `call` direct, lui, va tres bien ;
 *   - aucun saut ENTRANT dans le CORPS (au-dela du premier octet).
 * Les references en DONNEE (creneau de vtable) ne sont pas un probleme : un
 * appel indirect cree lui aussi un bloc a l'entree.
 *
 * Preuve faite pour 0x50dd60 (tools/verif_intrin_capture.py) : 1 reference en
 * donnee (vtable VA 0x72f1dc), 16 `call rel32` DIRECTS, 0 `jmp` entrant,
 * 0 saut dans le corps.
 *
 * SURETE. Le helper rend 0 des qu'il n'est pas SUR — l'invite fait alors le
 * travail avec son propre code traduit, intact. Le repli n'est pas un chemin
 * d'erreur, c'est le contrat : tout ce que le natif ne sait pas reproduire a
 * l'identique doit etre refuse.
 *
 * ⚡ ENTREES : `inmask` N'EST PAS FACULTATIF.
 * Les 8 registres x86 vivent en PERMANENCE dans r4-r11 et ne sont ranges dans
 * `emu` QU'A L'EPILOGUE du bloc (arm_epilog.S : `stm r0,{r4-r12,r14}`). Un
 * helper qui lit `emu->regs[...]` sans que le bit correspondant soit dans
 * `inmask` lit donc une valeur PERIMEE — celle du dernier retour au
 * repartiteur, pas celle de l'appel en cours.
 * memintrin n'avait pas ce probleme : il ne lit que la PILE invitee, via `esp`.
 * Le premier client de ce module (proj 0x50dd60) prend deux de ses arguments
 * en REGISTRE, et l'oubli s'est vu tout de suite : 10 912 330 divergences sur
 * 10 919 963 comparaisons a l'oracle croise. C'est pour cela que `inmask` est
 * un champ obligatoire et non une option.
 *
 * REGISTRES ET DRAPEAUX. Le natif n'ecrit QUE les registres declares dans
 * `regmask` (recharges depuis `emu` par le code emis). Les autres gardent la
 * valeur qu'ils avaient a l'entree — ce qui est CONSERVATEUR quand le corps
 * invite les salissait, et FAUX si un appelant lisait un registre que le corps
 * invite ecrasait de facon observable. C'est pourquoi `regmask` doit lister
 * tout registre que le corps invite modifie ET qu'un appelant peut lire, y
 * compris les registres « volatils » de la convention : c'est le
 * desassemblage qui tranche, pas la convention.
 * Les DRAPEAUX sont laisses intacts (le differe de box86 reste valide) la ou
 * l'invite les detruisait : plus conservateur, pas moins.
 *
 * Box86 is (c) ptitSeb, MIT license — see third_party/box86-dynarec/LICENSE
 */
#ifndef DYN86_INTRIN_H_
#define DYN86_INTRIN_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Contrat d'un helper : rend 1 s'il a SERVI (l'appel est fini, le code emis
 * execute le RET), 0 pour REPLIER sur le corps traduit d'origine.
 *   emu = x86emu_t*  (r0, invariant du dynarec)
 *   esp = ESP invite AU MOMENT DE L'ENTREE : [esp] = adresse de retour,
 *         [esp+4] = 1er argument de pile, etc. */
typedef int (*dyn86_intrin_fn)(void* emu, uint32_t esp);

/* Indices de registres pour `regmask` — memes valeurs que regs.h (_AX..._DI).
 * Un bit par registre a RECHARGER depuis emu apres l'appel. */
#define DYN86_IR_AX (1u<<0)
#define DYN86_IR_CX (1u<<1)
#define DYN86_IR_DX (1u<<2)
#define DYN86_IR_BX (1u<<3)
#define DYN86_IR_SP (1u<<4)
#define DYN86_IR_BP (1u<<5)
#define DYN86_IR_SI (1u<<6)
#define DYN86_IR_DI (1u<<7)

typedef struct dyn86_intrin_s {
    uintptr_t         va;       /* VA invitee de l'ENTREE (0 = creneau libre) */
    dyn86_intrin_fn   fn;
    uint16_t          retn;     /* octets depiles par le RET (stdcall) ; 0 = RET nu */
    uint16_t          inmask;   /* registres RANGES dans emu AVANT l'appel      */
    uint16_t          regmask;  /* registres recharges depuis emu APRES l'appel */
    const char*       name;     /* pour la ligne de preuve d'armement */
    unsigned long long calls;   /* entrees dans le helper                      */
    unsigned long long served;  /* dont servies (calls - served = replis)      */
} dyn86_intrin_t;

#define DYN86_INTRIN_MAX 16

/* 0 = rien n'est emis (defaut, code d'avant a l'instruction pres)
 * 1 = SERVIR en natif
 * 2 = PLOMBERIE SEULE : le helper est appele et rend TOUJOURS 0, donc l'invite
 *     execute son corps traduit. Tout client DOIT honorer ce mode.
 *     C'est le seul moyen de separer les deux couts d'un portage :
 *        mode 2 - temoin          = ce que coute la PLOMBERIE
 *        mode 1 - mode 2          = ce que rapporte le CORPS natif
 *     Sans lui, une jambe qui regresse ne dit pas si le natif est lent ou si
 *     c'est l'aller-retour qui mange le gain. */
extern int dyn86_intrin_on;
extern int dyn86_intrin_n;
extern dyn86_intrin_t dyn86_intrin_tbl[DYN86_INTRIN_MAX];

/* Enregistre une cible. A appeler APRES le chargement du PE (les VA ne sont
 * pas des constantes : Game.exe peut etre relocalise) et AVANT la premiere
 * traduction. Rend 1 si l'entree est posee, 0 si la table est pleine ou si la
 * VA est deja enregistree. N'arme RIEN par lui-meme : voir dyn86_intrin_on. */
int dyn86_intrin_add(uintptr_t va, dyn86_intrin_fn fn, uint16_t retn,
                     uint16_t inmask, uint16_t regmask, const char* name);

/* Consultee A LA TRADUCTION, une fois par debut de bloc. Rend NULL si la VA
 * n'est pas une cible ou si le mecanisme est eteint. La table est figee avant
 * la premiere traduction, donc ce predicat est PUR : passes 2 et 3 emettent
 * exactement la meme chose (exigence du detecteur de divergence de box86). */
const dyn86_intrin_t* dyn86_intrin_find(uintptr_t va);

/* Ligne de preuve d'armement, publiee par la fenetre de 10 s. Rend le nombre
 * d'octets ecrits. Ecrit « intrin: eteint » si rien n'est arme — un compteur
 * muet ne prouve rien (lecon du 07/09 : la preuve doit sortir dans la fenetre
 * PERIODIQUE, pas au rapport final que la console n'atteint jamais). */
int dyn86_intrin_report(char* out, unsigned cap);

#ifdef __cplusplus
}
#endif

#endif /* DYN86_INTRIN_H_ */
