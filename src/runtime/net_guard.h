// src/runtime/net_guard.h — LE VERROU DE SORTIE : « rien ne part vers
// l'internet public ».
//
// Une unite FEUILLE, volontairement : elle ne depend de rien: ni du pont, ni
// du processeur, ni de la table de shims. C'est la condition pour que les
// DEUX etages qui posent la question puissent la lier, et pour qu'un banc
// puisse la lier aussi :
//   - la couche d'adresses (net_nonblock.cpp, wx86_net_resolve) — une requete
//     de resolution est deja un depart ;
//   - la couche socket (win32_shims_wsock32.cpp, connect/sendto/recvfrom) —
//     ce qui part reellement sur le fil.
//
// POURQUOI CE FICHIER EXISTE (2026-09-12). Le drapeau vivait dans
// win32_shims_wsock32.cpp, l'unite la plus lourde du moteur. Une feuille qui
// l'interrogeait tirait donc derriere elle Bridge::register_shim_ordinal,
// wx86_poll_gilfree et l'allocateur de brouillon invite. Cela avait deja
// coute DEUX contournements, et c'est la raison d'etre de ce decoupage :
//   1. tools/net_resolve_guard_selftest.cpp REDEFINISSAIT le predicat pour
//      pouvoir se batir — le filet du verrou testait donc une reecriture du
//      verrou, pas le verrou ;
//   2. le banc reseau du portage (d2-vita, tools/net_check.sh) ne se liait
//      tout simplement plus.
// Un symbole FAIBLE avec valeur par defaut aurait fait taire l'editeur de
// liens, et c'est precisement ce qu'il ne faut pas ici : une unite qui oublie
// de lier le vrai verrou hériterait en SILENCE d'un verrou desarme. Un verrou
// qui ment vaut moins que pas de verrou.
#pragma once
#include <cstdint>

// DEFAUT : DESARME. Le moteur n'a pas d'avis sur la politique reseau de son
// consommateur ; c'est l'embarqueur qui l'arme, et qui decide a quelles
// conditions il la leve.
void wx86_net_set_private_only(bool on);
bool wx86_net_private_only();

unsigned long long wx86_net_refused();   // destinations refusees depuis le demarrage
unsigned long long wx86_net_allowed();   // destinations laissees passer (temoin d'echelle)

// Predicat nu, expose pour qu'un embarqueur puisse poser la meme question
// ailleurs sans re-ecrire la table. ip en ordre RESEAU, tel qu'il est dans le
// sockaddr.
bool wx86_net_addr_is_private(uint32_t ip_be);

// LA DECISION, compteurs compris : « cette destination passe-t-elle ? ».
// Rend true si elle passe, et tient a jour les deux temoins. Ne journalise
// rien et ne touche aucune socket — l'etage appelant se charge de dire le
// refus comme il l'entend (code Winsock, observateur).
bool wx86_net_addr_allowed(uint32_t ip_be);
