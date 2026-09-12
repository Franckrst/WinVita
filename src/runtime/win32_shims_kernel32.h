// src/runtime/win32_shims_kernel32.h — shims KERNEL32 generiques : aucun
// litteral, chemin ou comportement propre a un jeu donne. Inscrits via
// Bridge::register_shim, le meme mecanisme qu'un consommateur utilise pour
// en redefinir un pour son propre jeu (semantique d'ecrasement sur cle
// dupliquee, cf. bridge.h).
#pragma once
#include <cstdint>
namespace d2rt { class Bridge; }

// --- IDENTITE PROCESS / FILS, telle qu'un vrai Windows la distribue --------
// Windows alloue les PID et les TID par pas de 4 — ce sont des index dans une
// table du noyau — et ne rend JAMAIS 1 : le PID 1 n'existe pas. Un runtime qui
// rend 1 pour le process et 1, 2, 3... pour ses fils pose une contradiction
// gratuite sous les yeux du premier code qui regarde.
//
// Ces trois fonctions appartiennent au MOTEUR parce que la propriete qu'elles
// portent est la sienne : c'est lui qui possede la table effective, et c'est
// lui qui inscrit GetCurrentProcessId. L'embarqueur qui remplit un
// PROCESSENTRY32 ou un THREADENTRY32 lit ICI, il ne se refait pas sa propre
// numerotation — sinon les deux derivent, et la divergence n'est visible
// qu'a l'execution.
//
// Les identifiants INTERNES de l'ordonnanceur ne changent pas (1, 2, 3...) :
// journaux, bancs et profils les citent partout. Seule la vue INVITEE est
// convertie, dans les deux sens.
uint32_t wx86_win_pid();
uint32_t wx86_win_tid(uint32_t schedId);    // id d'ordonnanceur -> vue invitee
uint32_t wx86_sched_tid(uint32_t winTid);   // vue invitee -> id d'ordonnanceur (0 si invalide)

// BOUTON COUPANT. WX86_FID_AVANT=1 (alias D2_FID_AVANT) restaure les reponses
// D'AVANT cette campagne de fidelite : PID 1, tid 1/2/3...,
// IsProcessorFeaturePresent qui rend 0 pour tout. Le DEFAUT est le
// comportement FIDELE — patron « defaut = comportement fidele », jamais
// l'inverse. Ce knob existe pour UNE raison : montrer qu'un test de fidelite
// coupe vraiment. Un test qui passe dans les deux jambes ne prouve rien.
bool wx86_fid_avant();

void win32_shims_kernel32_install(d2rt::Bridge& br);
