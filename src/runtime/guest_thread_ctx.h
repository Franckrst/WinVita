// src/runtime/guest_thread_ctx.h — le TIB du fil COURANT, et la derniere
// erreur Win32 qui y vit.
//
// POURQUOI CE POINT D'EXTENSION EXISTE. Une foule de shims KERNEL32 doivent
// ecrire la derniere erreur (SetLastError, et toute fonction qui echoue
// proprement). Cela veut dire ecrire a TIB+0x34 du fil courant. Or :
//   - la LOGIQUE est generique : « le TIB du fil courant, ou celui du fil
//     principal si l'ordonnanceur n'est pas encore la », et l'offset 0x34 est
//     celui du champ LastErrorValue du TEB Win32, pas une invention locale ;
//   - mais la DONNEE est propre au portage : quel ordonnanceur est vivant, et
//     ou son plan memoire a place le TIB principal.
//
// Le moteur porte donc la logique, le consommateur fournit les deux donnees.
// C'est ce partage qui permet aux deux portages existants de s'en servir sans
// que le moteur connaisse le moindre detail de l'un ou de l'autre : tous deux
// hebergeaient jusqu'ici la meme fonction cur_tib(), mot pour mot.
#pragma once
#include <cstdint>
namespace d2rt { class Cpu; class ThreadScheduler; }

// A appeler par le consommateur des que son plan memoire est fige.
void wx86_set_main_tib(uint32_t tib);
// A appeler par le consommateur des que son ordonnanceur existe (et a nouveau
// s'il en change). nullptr est valide : avant l'ordonnanceur, le fil principal.
void wx86_set_scheduler(d2rt::ThreadScheduler* s);

// TIB du fil courant, ou le TIB principal si aucun fil n'est ordonnance.
uint32_t wx86_cur_tib();
// L'ordonnanceur vivant, tel que le consommateur l'a declare. Les shims de
// synchronisation en ont besoin pour endormir et reveiller ; le declarer une
// fois evite que chaque portage garde SON pointeur a cote de celui du moteur —
// c'est exactement le jumeau d'etat qui a coute trois incidents cette semaine.
d2rt::ThreadScheduler* wx86_sched();

// Derniere erreur Win32 (TEB+0x34) du fil courant.
void     wx86_set_lasterr(d2rt::Cpu& c, uint32_t v);
uint32_t wx86_get_lasterr(d2rt::Cpu& c);
