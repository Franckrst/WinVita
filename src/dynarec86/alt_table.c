/* src/dynarec86/alt_table.c — LA TABLE DES ALTERNATES, et rien d'autre.
 *
 * Un « alternate » est la primitive d'extension numero 1 du dynarec : une
 * redirection INVITE -> INVITE que le traducteur applique aux cibles de
 * call/jmp, sans toucher un octet de la memoire invitee. C'est par la qu'un
 * portage fait tourner du code natif a la place d'une fonction du jeu.
 *
 * POURQUOI CE FICHIER EXISTE SEPAREMENT. La table vivait dans dyn86.c, au
 * milieu du dynarec, donc impossible a compiler sur bureau : son unique mode
 * de panne ne pouvait pas etre EXERCE par un oracle. Isolee, elle ne depend
 * que de la bibliotheque C, et tools/alt_table_selftest.cpp peut lui injecter
 * un echec d'allocation puis VERIFIER qu'elle le dit.
 *
 * CE QUI A CHANGE LE 2026-09-12. La table etait un tableau fixe — 24 places,
 * puis 52 — et le 53e enregistrement etait AVALE SANS UN MOT : un crochet
 * perdu, aucune trace, et un portage natif qui « ne tire pas » sans raison
 * visible. Une borne arbitraire sur la primitive d'extension numero 1 n'a pas
 * sa place dans un moteur generique. La table croit maintenant par doublement,
 * et le seul echec qui subsiste (allocation refusee) incremente un compteur
 * LISIBLE par l'embarqueur et ecrit une ligne dans le journal de la console.
 *
 * CONTRAT D'APPEL : dyn86_set_alternate s'appelle a l'INSTALLATION, avant que
 * le code traduit ne tourne. hasAlternate()/getAlternate() lisent la table sur
 * le chemin chaud (premiere ligne de internalDBGetBlock), sans verrou : la
 * taille `dyn86_alt_n` est donc PUBLIEE EN DERNIER, apres l'ecriture de
 * l'entree, et les anciens tableaux ne sont JAMAIS liberes — un lecteur peut
 * tenir l'ancien pointeur. Le gaspillage est borne par la somme des capacites
 * precedentes, quelques kilo-octets.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "bridge.h"   /* les declarations extern de la table (source unique) */

/* Journal de la console : lien FORT, no-op hors __vita__
 * (platform/vita_host.cpp). Jamais une reference FAIBLE au nom d'un
 * consommateur — cf. docs/migration.md, piege 9. */
#ifdef __cplusplus
extern "C" void wx86_vita_progress_c(const char* msg);
#else
void wx86_vita_progress_c(const char* msg);
#endif

uintptr_t *dyn86_alt_from = NULL;
uintptr_t *dyn86_alt_to   = NULL;
int dyn86_alt_n     = 0;      /* alternates POSES */
int dyn86_alt_refus = 0;      /* alternates PERDUS (allocation refusee) */
static int g_alt_cap = 0;

void dyn86_set_alternate(uintptr_t from, uintptr_t to) {
    int i;
    for (i = 0; i < dyn86_alt_n; i++)
        if (dyn86_alt_from[i] == from) { dyn86_alt_to[i] = to; return; }
    if (dyn86_alt_n == g_alt_cap) {
        const int cap = g_alt_cap ? g_alt_cap * 2 : 32;
        uintptr_t* nf = (uintptr_t*)malloc(sizeof(uintptr_t) * (size_t)cap);
        uintptr_t* nt = (uintptr_t*)malloc(sizeof(uintptr_t) * (size_t)cap);
        if (!nf || !nt) {
            char m[96];
            free(nf); free(nt);
            ++dyn86_alt_refus;
            snprintf(m, sizeof m,
                     "dyn86: set_alternate PERDU (%d poses, allocation refusee)", dyn86_alt_n);
            wx86_vita_progress_c(m);
            return;
        }
        if (dyn86_alt_n) {
            memcpy(nf, dyn86_alt_from, sizeof(uintptr_t) * (size_t)dyn86_alt_n);
            memcpy(nt, dyn86_alt_to,   sizeof(uintptr_t) * (size_t)dyn86_alt_n);
        }
        dyn86_alt_from = nf; dyn86_alt_to = nt; g_alt_cap = cap;
    }
    dyn86_alt_from[dyn86_alt_n] = from;
    dyn86_alt_to[dyn86_alt_n]   = to;
    dyn86_alt_n++;            /* PUBLIE EN DERNIER (voir le contrat ci-dessus) */
}

int dyn86_alt_count(void) { return dyn86_alt_n; }
int dyn86_alt_lost(void)  { return dyn86_alt_refus; }
