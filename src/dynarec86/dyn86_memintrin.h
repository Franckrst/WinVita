/* src/dynarec86/dyn86_memintrin.h — D2Vita : memcpy/memset du CRT de D2
 * servis SANS TRAP, par reconnaissance A LA TRADUCTION.
 * ------------------------------------------------------------------------
 * LE CONSTAT (profil par fil, -DPROF_COUNTERS, docs/perf/storm_io_20260906.md)
 *   memcpy CRT `Game+0x2829c0` = 7,78 % de TOUS les blocs traduits du jeu
 *   (80,8 M executions de bloc pour 25,6 M appels sur le run de patrouille).
 *   Et ce chiffre SOUS-ESTIME le temps : `rep movsd` est traduit par box86 en
 *   une boucle de 4 instructions ARM PAR MOT (dynarec_arm_00.c case 0xA5) qui
 *   vit ENTIEREMENT dans un seul bloc — le compteur de blocs ne voit qu'une
 *   entree, quelle que soit la longueur copiee. Idem `rep stosd` pour memset
 *   `Game+0x281ef0`.
 *
 * POURQUOI PAS UN CROCHET (alternate + trap) : un aller-retour de trap coute
 * ~3,6 us sous qemu / ~1,3 us console, contre ~3 blocs traduits par appel.
 * Pour les petites copies le trap couterait PLUS que le corps. Le crochet est
 * donc exclu par la mesure, pas par gout.
 *
 * CE QUE FAIT CE MODULE : le traducteur, quand on lui demande de traduire le
 * BLOC QUI COMMENCE a l'entree de memcpy/memset, emet a la place :
 *     mov r1, xESP                  ; ESP invite
 *     bl  dyn86_mi_copy/dyn86_mi_set ; helper C (r0 = emu)
 *     cmp r0, #0
 *     beq <corps traduit d'origine> ; REPLI FIDELE : rien n'a ete fait
 *     ldr xEAX, [xEmu, #regs[_AX]]  ; EAX = dst, ecrit par le helper
 *     <ret_to_epilog>               ; le RET normal du dynarec
 * Aucun trap, aucun passage par le repartiteur : c'est un appel natif direct
 * depuis le code traduit, comme les helpers div32/imul8 de box86.
 *
 * SURETE / REPLI. Le helper REFUSE (rend 0, l'invite fait le travail) :
 *   - si le knob n'est pas arme ou l'adresse n'a pas ete publiee ;
 *   - taille > dyn86_mi_maxn (16 Mio) ;
 *   - source ou destination hors ARENE ([0, dyn86_mi_span)), quand l'arene est
 *     connue — meme garde que arena_check() cote shim ;
 *   - mode PROFIL (2) : il mesure et rend toujours 0.
 * Le recouvrement N'EST PAS un cas de refus : le corps du CRT de D2 est le
 * corps COMMUN memcpy/memmove (branche arriere en 0x682b84 quand
 * dst > src && dst < src+n), c'est-a-dire exactement la semantique de
 * memmove() — c'est memmove() qui est appele cote natif.
 *
 * CE QUE LE NATIF NE REPRODUIT PAS : les drapeaux et ECX/EDX/ESI/EDI que le
 * corps invite salit. cdecl les declare volatils et les 186 (memcpy) / 1135
 * (memset) sites d'appel sont tous du code genere par le compilateur. Les
 * drapeaux sont LAISSES INTACTS (le differe de box86 reste valide) la ou
 * l'invite les detruisait : c'est plus conservateur, pas moins.
 *
 * CAPTURE COMPLETE DES APPELS : au desassemblage de Game.exe 1.14d, memcpy
 * 0x6829c0 a 186 sites `call rel32` DIRECTS, memset 0x681ef0 en a 1135, et
 * NI L'UN NI L'AUTRE n'a de `jmp` entrant ni de reference en donnee (aucun
 * dword absolu egal a leur VA dans tout le fichier). Tout appel cree donc un
 * bloc qui COMMENCE a l'entree de la fonction : la reconnaissance par adresse
 * de debut de bloc les prend tous.
 *
 * Box86 is (c) ptitSeb, MIT license — see third_party/box86-dynarec/LICENSE
 */
#ifndef DYN86_MEMINTRIN_H_
#define DYN86_MEMINTRIN_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 0 = eteint (defaut, code d'avant a l'instruction pres)
 * 1 = servir en natif
 * 2 = PROFIL seul : mesure la distribution puis rend 0 (l'invite fait tout) */
extern int dyn86_memintrin;

/* VA invitees des deux entrees, publiees par rt_boot APRES le chargement du PE
 * (Game.exe peut etre relocalise : ce ne sont pas des constantes). 0 = pas de
 * reconnaissance pour cette fonction. */
extern uintptr_t dyn86_mi_cpy_va, dyn86_mi_set_va;

/* Bornes du garde-fou : etendue de l'arene invitee (0 = inconnue, garde
 * desactivee, comportement historique) et taille maximale servie. */
extern uint32_t dyn86_mi_span;
extern uint32_t dyn86_mi_maxn;

/* Compteurs (rapport final). servis/replis = APPELS, pas blocs. */
extern unsigned long long dyn86_mi_cpy_served, dyn86_mi_cpy_fb, dyn86_mi_cpy_bytes;
extern unsigned long long dyn86_mi_set_served, dyn86_mi_set_fb, dyn86_mi_set_bytes;
extern unsigned long long dyn86_mi_cpy_calls,  dyn86_mi_set_calls;
/* Raisons de repli (diagnostic) : 0 = hors arene, 1 = taille, 2 = profil. */
extern unsigned long long dyn86_mi_rej[4];

/* Distribution mesuree (mode 2 ET mode 1 : le comptage coute deux additions).
 * hist[k] = appels dont la taille est dans [2^(k-1), 2^k) ; hist[0] = n==0.
 * align[a] = appels dont (dst|src) & 3 == a (0 = les deux alignes mot). */
#define DYN86_MI_HBITS 26
extern unsigned long long dyn86_mi_cpy_hist[DYN86_MI_HBITS], dyn86_mi_set_hist[DYN86_MI_HBITS];
extern unsigned long long dyn86_mi_cpy_align[4];
extern unsigned long long dyn86_mi_cpy_overlap;   /* dst/src qui se chevauchent */
extern unsigned long long dyn86_mi_cpy_small;     /* n < 16 */
extern unsigned long long dyn86_mi_cpy_maxseen;   /* plus grande taille vue    */
/* Combien d'appels (et d'octets) l'INVITE traite deja par sa branche SSE2 —
 * `_VEC_memzero` 0x68a797 pour memset, la copie movdqa 0x68c9c0 pour memcpy.
 * my_cpuid (shim_impl.c) annonce SSE2, donc le CRT arme son drapeau
 * ds:0x994c88 et ces branches sont VIVANTES : box86 traduit leurs `movdqa` en
 * NEON, 16 octets par instruction. C'est le chiffre qui dit si un portage
 * natif a encore quelque chose a gagner sur les GROS transferts. [0]=appels,
 * [1]=octets. Compte en mode PROFIL seulement. */
extern uint32_t dyn86_mi_sse2_va;                 /* VA de ds:0x994c88 (0 = inconnu) */
extern unsigned long long dyn86_mi_cpy_sse[2], dyn86_mi_set_sse[2];

/* --- ORACLE CROISE D2_MEMVERIFY (patron D2_DCCVERIFY / D2_RLEVERIFY) ------
 * Arme, le helper NE SERT PAS : il photographie la source (ou la valeur de
 * remplissage) dans un tampon HOTE, remplace l'adresse de retour sur la pile
 * INVITEE par un trap, et rend 0 — l'invite fait donc la copie lui-meme, avec
 * son propre code traduit. Au trap, dyn86_mi_verify_ret() compare octet a
 * octet ce que l'invite a ecrit avec ce que le natif aurait ecrit, puis rend
 * l'adresse de retour d'origine (le shim fait redirect_next dessus).
 * Un seul appel en vol a la fois (les autres sont comptes « sautes »). */
extern int      dyn86_mi_verify;        /* D2_MEMVERIFY=1 */
extern uint32_t dyn86_mi_verify_trap;   /* VA du trap de retour, posee par rt_boot */
extern unsigned long long dyn86_mi_ver_n, dyn86_mi_ver_bad, dyn86_mi_ver_skip;
uint32_t dyn86_mi_verify_ret(void);     /* compare puis rend l'adresse de retour */

/* Publication depuis rt_boot / CpuBox86. */
void dyn86_mi_arm(int mode, uintptr_t cpy_va, uintptr_t set_va);
void dyn86_mi_set_sse2va(uint32_t va);
void dyn86_mi_set_span(uint32_t span);
void dyn86_mi_arm_verify(uint32_t trap_va);

/* Les deux helpers appeles PAR LE CODE EMIS. r0 = emu, r1 = ESP invite.
 * Rendent 1 = servi (EAX ecrit dans emu->regs[0]), 0 = repli. */
int dyn86_mi_copy(void* emu, uint32_t esp);
int dyn86_mi_set (void* emu, uint32_t esp);

#ifdef __cplusplus
}
#endif
#endif /* DYN86_MEMINTRIN_H_ */
