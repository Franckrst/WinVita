// src/runtime/win32_shims_locale.h — shims KERNEL32 de LOCALE, de CHAINES et
// de CONSOLE. Vague 2 de l'extraction du groupe KERNEL32.
//
// POURQUOI CES TROIS SOUS-GROUPES ENSEMBLE. Ils partagent la meme dependance,
// et c'est elle qui les bloquait : lire ou ecrire une chaine a une adresse
// INVITEE (guest_str.h), et parfois la ranger dans le brouillon invite
// (guest_scratch.h). Tant que ces deux primitifs vivaient chez le
// consommateur, aucun de ces corps ne pouvait partir. Ils sont dans le moteur
// depuis winx86 b124f1f ; ce fichier encaisse ce deblocage.
//
// GENERICITE : constatee, pas deduite. Les 36 corps sont IDENTIQUES au
// caractere pres (hors commentaires et espaces) a ceux du second consommateur
// du moteur, qui porte un tout autre jeu — verifie un par un, pas par
// echantillon. Les six qui « different » ne different que par l'accesseur du
// LCID (wx86_locale_lcid() ici, une globale la-bas) : meme semantique, le
// second portage n'a simplement pas encore migre.
//
// CE QUI N'EST PAS ICI. GetPrivateProfileStringA/IntA, qui vivent au milieu de
// la meme zone du portage, restent chez lui : leur corps lit un VRAI fichier
// .ini par ini_lookup(), un service que le portage possede. Ce n'est pas de la
// prudence, c'est une dependance nommee.
#pragma once
namespace d2rt { class Bridge; }

void win32_shims_locale_install(d2rt::Bridge& br);
