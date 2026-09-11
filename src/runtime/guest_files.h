// src/runtime/guest_files.h — les tables de poignees de FICHIER de l'invite.
//
// POURQUOI C'EST DU MOTEUR. Une poignee Win32 de fichier, la position logique
// qu'on lui associe, le chemin qu'elle porte et les vues de mapping sont de la
// semantique Win32, pas du jeu. Les deux portages qui utilisent ce moteur
// declaraient ces quatre tables avec les MEMES noms et les MEMES types, et
// FPos comme FMap y sont identiques au caractere pres, commentaire compris.
// C'etait donc de l'etat du moteur, duplique.
//
// CE QUI RESTE AU PORTAGE : la RESOLUTION de chemin. Ou vivent les donnees,
// quelles conventions de nommage a la plateforme, quelle racine — cela ne
// regarde que le consommateur. Le moteur tient la table, le portage dit ou
// trouver le fichier.
//
// FORME CHOISIE, ET SON PRIX. Le moteur expose les tables par REFERENCE plutot
// que par une famille d'accesseurs. C'est moins joli, et c'est deliberé : le
// but de cette etape est de transferer la PROPRIETE sans reecrire quarante
// sites d'appel sur un chemin d'entrees-sorties critique. Une seule table
// existe, celle-ci ; le portage n'en tient qu'une reference. Des accesseurs
// pourront la remplacer plus tard sans rien changer a la propriete.
#pragma once
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>

// Position logique suivie sans syscall (le fseek n'est fait qu'au moment utile).
struct WxFPos { long logical=0; long real=0; int dir=0; };  // dir: 0 aucun, 1 lecture, 2 ecriture
// Vue de mapping de fichier.
struct WxFMap { std::string path; uint32_t size; uint32_t view; };

std::map<uint32_t,FILE*>&       wx86_files();        // poignee -> flux hote
std::map<uint32_t,std::string>& wx86_file_paths();   // poignee -> chemin resolu
std::map<uint32_t,WxFPos>&      wx86_file_pos();     // poignee -> position logique
std::map<uint32_t,WxFMap>&      wx86_file_maps();    // poignee de mapping -> vue

// Compteur des poignees de mapping. Distinct de celui des objets attendables :
// ce sont deux espaces de numeros differents, et les melanger serait le meme
// accident que celui des handles noyau.
uint32_t wx86_fmap_next_id();
