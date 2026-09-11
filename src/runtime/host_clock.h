// src/runtime/host_clock.h — l'horloge MONOTONE de l'hote.
//
// A ne pas confondre avec l'horloge INVITEE (le GetTickCount que le jeu lit,
// qui peut etre virtuelle, mise a l'echelle ou figee par un banc). Celle-ci
// est le temps qui passe vraiment, du point de vue de la machine : elle sert
// a mesurer des durees hote — un grain audio, une attente, un budget.
//
// C'est un primitif de moteur pour une raison bete mais suffisante : sa
// realisation depend de la PLATEFORME, pas du jeu. Sur la console c'est
// l'horloge du noyau Sony, ailleurs c'est le CLOCK_MONOTONIC de la libc. Un
// portage n'a rien a dire la-dessus, et pourtant les deux portages existants
// en avaient chacun une copie (rt_now_us chez l'un, d2vita_now_us chez
// l'autre — jusqu'au nom qui trahit le fork).
//
// Monotone : jamais en arriere, insensible aux changements d'heure. Aucune
// garantie sur l'origine, seules les DIFFERENCES ont un sens.
#pragma once
#include <cstdint>

// Microsecondes depuis une origine arbitraire.
uint64_t wx86_now_us();
// Millisecondes depuis la meme origine (wx86_now_us()/1000).
uint64_t wx86_now_ms();
