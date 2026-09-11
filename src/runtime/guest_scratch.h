// src/runtime/guest_scratch.h — allocateur de brouillon INVITE.
//
// POURQUOI C'EST UN PRIMITIF DU MOTEUR, ET PAS DU CONSOMMATEUR.
// Une bonne partie de l'API Win32 rend un POINTEUR vers de la memoire que
// l'appelant ne libere pas : GetCommandLineA, GetEnvironmentStrings,
// inet_ntoa, gethostbyname, le bloc RTL_CRITICAL_SECTION_DEBUG... Sur une
// machine reelle cette memoire appartient a la DLL systeme. Ici les DLL
// systeme n'existent pas : c'est le moteur qui les incarne, donc c'est au
// moteur de posseder l'endroit ou ces valeurs de retour vivent. Et ce
// pointeur doit etre une adresse INVITEE (x86 32 bits, lisible par le jeu),
// pas une adresse hote — un new/malloc de l'hote ne convient pas.
// Tout portage vers ce moteur a exactement le meme besoin ; le laisser chez
// le consommateur obligeait chaque portage a le reecrire.
//
// PARTAGE DES ROLES. Le MOTEUR possede l'allocateur (allocation, bornage,
// signalement d'epuisement). Le CONSOMMATEUR declare la plage qu'il concede
// (wx86_scratch_init) : le plan memoire, lui, est bien specifique au projet
// — l'arene compacte de d2vita n'a rien d'universel.
//
// CONTRAT, ecrit noir sur blanc parce qu'il a deja coute une enquete :
// l'allocation est DEFINITIVE. Rien n'est jamais libere, il n'y a pas de
// free et il n'y en aura pas. Cet allocateur est reserve aux valeurs de
// retour a duree de vie PROCESSUS — typiquement une chaine constante ou une
// structure allouee UNE FOIS et mise en cache par l'appelant. Allouer ici a
// CHAQUE appel sur un chemin repete est un defaut, pas un usage : c'est
// ainsi qu'on epuise la plage en session longue. Pour de la memoire a duree
// de vie bornee, l'embarqueur a ses propres tas invites.
#pragma once
#include <cstdint>

namespace d2rt { class Cpu; }

// Declare la plage invitee concedee a l'allocateur : base est l'adresse
// INVITEE du premier octet utilisable, size la taille en octets. Le pointeur
// repart de base a chaque appel — un embarqueur qui bascule de plan memoire
// en a besoin (le plan compact de d2vita est choisi apres coup).
//
// size == 0 signifie EXPRESSEMENT « plage non bornee » : on alloue sans
// verifier la borne. C'est le comportement d'avant le correctif C6, garde ici
// parce que l'embarqueur connait souvent son point de depart bien avant de
// connaitre sa taille (chez d2vita, la taille ne se sait qu'une fois la
// region invitee cartographiee). Armer la borne ensuite, quand elle est
// connue, se fait par wx86_scratch_set_limit — SANS toucher au pointeur,
// donc sans perdre ce qui a deja ete alloue entre-temps.
void wx86_scratch_init(uint32_t base, uint32_t size);

// Arme (ou deplace) la borne haute, pointeur courant INCHANGE. limit est
// l'adresse invitee du premier octet HORS plage ; 0 la retire.
void wx86_scratch_set_limit(uint32_t limit);

// Alloue n octets (arrondis a 8) et rend l'adresse INVITEE, ou 0 si la plage
// bornee est epuisee. Un 0 est bien plus supportable pour les appelants qu'un
// debordement silencieux dans la region voisine : c'est exactement ce qui
// menacait ici, l'allocateur n'etant borne que depuis le correctif C6 (une
// session longue marchait dans le trou d'arene puis dans les reserves VA,
// avec pour seul symptome une faute hote sous qemu).
uint32_t wx86_scratch_alloc(uint32_t n);

// Ecrit la chaine s (terminateur compris) dans le brouillon et rend son
// adresse invitee, ou 0 si l'allocation echoue. Le raccourci dont chaque
// shim qui rend un char* a besoin.
uint32_t wx86_scratch_put_cstr(d2rt::Cpu& c, const char* s);

// Etat, pour le diagnostic de l'embarqueur (occupation, bilan de fin de
// session). used = octets consommes depuis base ; size = plage concedee.
uint32_t wx86_scratch_used();
uint32_t wx86_scratch_size();
uint32_t wx86_scratch_base();

// Signalement d'EPUISEMENT. Le moteur est muet par construction (il ne
// connait ni le journal, ni la console, ni le format de l'embarqueur) : il
// appelle ce rappel AU PLUS UNE FOIS, a la premiere allocation refusee, avec
// l'adresse courante, la taille demandee et la borne. A l'embarqueur d'en
// faire une ligne de journal. Non arme = epuisement silencieux, l'allocation
// rend 0 comme toujours.
typedef void (*Wx86ScratchOomFn)(uint32_t at, uint32_t want, uint32_t limit);
void wx86_scratch_set_oom_handler(Wx86ScratchOomFn cb);
