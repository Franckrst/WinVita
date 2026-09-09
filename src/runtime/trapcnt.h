// src/runtime/trapcnt.h — compteur de PRISES par créneau de trap.
//
// POURQUOI CE FICHIER. Bridge::dump_prof() donne un TEMPS par créneau, mais
// jamais un COMPTE, et il est compilé DEHORS du binaire livré (PROF_COUNTERS).
// Surtout, il rate totalement les intrinsèques : le chemin rapide
// (cpu_box86.cpp try_intrinsic) rend la main AVANT d'entrer dans le Bridge, si
// bien qu'un créneau servi en intrinsèque n'apparaît nulle part. Un
// recensement bâti sur dump_prof sous-compte donc les créneaux les plus
// chauds — exactement ceux qu'on veut chiffrer.
//
// Ce compteur-ci est TOUJOURS armé (il est le dénominateur de tout prix
// unitaire à venir), et il coûte : un chargement, deux comparaisons, une
// addition 64 bits. Il est incrémenté sur des chemins DÉJÀ sous le GIL
// (gil::Guard de la répartition de trap dans CpuBox86::run, et le corps de
// Bridge::trap_handler qui court sous ce même Guard), donc aucune atomique et
// aucun verrou supplémentaire.
//
// L'index est EXACTEMENT celui de trap_handler : (va - trap_base)/16, la même
// division que celle qui trouve le slot. Aucun deuxième schéma d'adressage à
// tenir cohérent.
#pragma once
#include <cstdint>

namespace d2rt {
namespace trapcnt {

// Bridge::alloc_trap espace les créneaux de 16 octets et slots_ en réserve
// 2048 ; 4096 entrées (32 Ko de BSS) couvrent donc le double de la réserve.
// Au-delà, bump() ignore silencieusement : un compteur tronqué vaut mieux
// qu'une écriture hors tableau, et dump_trap_counts() le dit.
inline constexpr uint32_t kMax = 4096;

inline uint32_t base = 0;          // = Bridge::trap_base_, posé par commit()
inline uint64_t hits[kMax] = {};   // prises cumulées par créneau

inline void bump(uint32_t va) {
    const uint32_t b = base;
    if (!b || va < b) return;
    const uint32_t i = (va - b) >> 4;   // /16 : le pas d'alloc_trap
    if (i < kMax) ++hits[i];
}

}  // namespace trapcnt
}  // namespace d2rt
