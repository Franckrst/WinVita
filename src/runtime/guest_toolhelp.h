// src/runtime/guest_toolhelp.h — l'etat des instantanes Toolhelp32.
//
// CreateToolhelp32Snapshot fige une liste de fils et de processus, que
// Process32First/Next et Thread32First/Next parcourent ensuite. Le curseur de
// parcours et la liste figee sont de la semantique Win32 : un instantane est
// une notion du systeme, pas du jeu. Les DEUX portages declaraient cette meme
// structure et cette meme table, a l'identique.
//
// Ce qui n'est PAS ici : la liste de MODULES que Module32First/Next parcourt.
// Elle se derive de la table de modules chargee par le pont, et chaque portage
// la construit a sa facon — elle reste donc chez lui.
#pragma once
#include <cstdint>
#include <cstddef>
#include <map>
#include <vector>

struct WxSnapState {
    std::vector<uint32_t> tids; size_t tcur=0;
    std::vector<uint32_t> pids; size_t pcur=0;
};

// Table des instantanes. Exposee par reference, meme raison et meme prix que
// les tables de fichiers : transferer la PROPRIETE sans reecrire les sites
// d'appel. Une seule table existe, celle-ci.
std::map<uint32_t,WxSnapState>& wx86_snapshots();
