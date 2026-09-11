// src/platform/vita_host.h — les services de la CONSOLE, pas ceux du jeu.
//
// POURQUOI CE FICHIER EXISTE
// --------------------------
// Le tri « generique / specifique au jeu » manque une troisieme categorie :
// specifique a la CONSOLE. Elle appartient au moteur, parce que le moteur cible
// la Vita — pas au portage, qui ne fait que consommer le materiel. Confondue
// avec « specifique au jeu », elle reste du mauvais cote, et elle y est restee.
//
// La preuve n'est pas theorique. Les deux portages consommateurs ont ecrit ces
// blocs chacun de son cote, et la comparaison ligne a ligne donne :
//
//   journal de progression   corps IDENTIQUE au caractere pres (seul le chemin
//                            du fichier differe : c'est la seule donnee du
//                            portage, elle est donc injectee)
//   repartition des coeurs   76 lignes, ZERO ligne differente
//   horloge monotone         identique
//
// Ce n'est donc pas une abstraction inventee a l'avance : c'est la forme commune
// constatee apres coup, sur du code qui existait deja en double.
//
// CE QUE LE PORTAGE GARDE
// -----------------------
// Le CHEMIN de son journal (chaque portage ecrit dans son propre dossier), et
// la politique qu'il veut appliquer. Tout le reste est ici.
//
// COUT
// ----
// Aucune indirection n'est ajoutee sur le chemin par image : ces services sont
// appeles au demarrage, a la creation d'un fil, ou une fois par fenetre de
// 10 s. Le journal ouvre et ferme le fichier a chaque ligne — c'est deliberé,
// c'est ce qui le rend durable a travers un plantage.
#pragma once
#include <stdint.h>

// ---- Journal de progression durable ---------------------------------------
// Sur console, un printf ne prouve rien : il n'y a pas de sortie standard a
// lire. Ce journal est la SEULE fenetre sur un demarrage sans ecran. Il ouvre,
// ecrit et ferme a chaque ligne pour que la ligne survive a un plantage dur.
//
// Le verrou n'est pas un ornement : sur carn-vita, le 2026-09-06, un demarrage
// sur sept mourait sous Vita3K (`Invalid read at 0x30302e30` — les octets ASCII
// « 0.00 » d'un horodatage ecrits PAR-DESSUS un pointeur du tas newlib) parce
// que deux fils faisaient fopen/fprintf/fclose sur ce meme fichier. Un journal
// de demarrage qui peut tuer le demarrage qu'il observe est le pire defaut
// possible.

// LE CHEMIN EST FOURNI PAR LE PORTAGE, et c'est la seule chose qu'il fournit.
// Il est DEFINI, pas pose par un setter : une ligne de journal peut partir
// avant n'importe quel point d'initialisation qu'on choisirait, et une
// initialisation paresseuse rameuterait __cxa_guard_acquire — la famille de
// pieges que la garde `nm` des scripts de build existe pour attraper. Un
// initialiseur constant n'a ni ordre ni garde.
//
// Le portage ecrit, une fois, a portee de fichier :
//     extern "C" const char* const wx86_vita_progress_path = "ux0:data/…/x.txt";
extern "C" const char* const wx86_vita_progress_path;

void wx86_vita_progress(const char* msg);
extern "C" void wx86_vita_progress_c(const char* msg);

// ---- Sommeil reel ----------------------------------------------------------
// L'HORLOGE monotone n'est PAS ici : elle vit dans runtime/host_clock.h
// (wx86_now_us / wx86_now_ms), parce qu'elle a un sens hors console aussi.
// Seul le sommeil reste ici — il passe par l'ordonnanceur Sony.
void wx86_vita_sleep_ms(uint32_t ms);

// ---- Repartition des fils hotes sur les coeurs user ------------------------
// WX86_COEURS (repli : D2_COEURS) — trois chiffres 0..3, un par role :
//   position 0 = presentation, 1 = chien de garde, 2 = battement anti-famine.
// Defaut « 222 ».
//
// LE QUATRIEME COEUR. Le SDK ne definit que USER_0/1/2 (0x10000/0x20000/
// 0x40000), mais la sonde a relu `activeCpuMask=0x000f0000` sur CONSOLE, soit
// QUATRE bits de coeur user actifs. Le chiffre `3` le demande ; s'il est
// refuse, le rc le dit et rien ne bouge.
#define WX86_CPU_MASK_USER_3  0x00080000

int  wx86_vita_core_mask(int who);

// AUTO-EPINGLAGE A L'ENTREE DU FIL — pas au moment de la creation. Constat
// console : un masque pose PAR LE CREATEUR avant sceKernelStartThread rend
// rc=0 mais se RELIT 0 une fois le fil parti, et les fils migrent (deux
// ouvriers finissent sur le MEME coeur, -15 %). Chaque fil hote doit donc
// refaire l'epinglage sur lui-meme en premiere instruction.
extern "C" int wx86_vita_pin_self(int mask, unsigned* relu);

// Inscrit un fil au tableau publie par wx86_vita_core_window_line().
void wx86_vita_core_register(const char* nom, int uid, unsigned wanted, int pin_rc);

// Ligne « coeurs: » de la fenetre de 10 s, publiee dans le journal. Joue la
// sonde du 4e coeur a la premiere invocation.
void wx86_vita_core_window_line();
