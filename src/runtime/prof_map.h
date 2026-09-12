// src/runtime/prof_map.h — LA CARTE DES FAMILLES du profil d'adresses.
//
// Les echantillonneurs d'adresse du moteur (D2_EIPPROF, echantillonnage par
// blocs ; D2_TIMEPROF, echantillonnage uniforme en temps) rangent chaque
// echantillon dans une FAMILLE. Jusqu'au 2026-09-12 ces familles etaient six
// plages d'adresses ecrites EN DUR dans cpu_box86.cpp, commentees « Codec.cpp
// (DCC) », « SpriteCache.cpp », « Tilecmp.cpp », « DRLG »... : des noms de
// sous-systemes de Diablo II, dans un profileur generique.
//
// Ce n'etait pas une supposition. Le SECOND consommateur imprime deja ces
// libelles pour un binaire qui n'a ni Codec, ni DCC, ni DRLG : ses echantillons
// tombaient dans les plages du premier jeu par simple coincidence d'adresses,
// et la ligne « familles: » mentait sans qu'aucun test puisse le voir.
//
// Le moteur ne connait donc plus aucun sous-systeme. Il compte des plages de
// RVA que le CONSOMMATEUR decrit, et c'est le consommateur qui les nomme a
// l'impression. SANS carte installee — le cas d'un portage qui n'a pas decrit
// son binaire — tout tombe dans la famille de queue (kProfFamOther), qui est
// le seul profil honnete possible : « je ne sais pas ou c'est ».
//
// Forme : TABLE CONFIGUREE A L'INSTALLATION, comme wx86_net_set_observer et
// wx86_sync_set_observer. Le moteur expose et raconte, il ne demande jamais.
#pragma once
#include <cstdint>

// Une famille = une plage de RVA [lo, hi). Les plages sont essayees DANS
// L'ORDRE : la premiere qui contient l'adresse gagne, exactement comme la
// chaine de `else if` qu'elle remplace. Elles peuvent donc se recouvrir.
struct Wx86ProfRange { uint32_t lo = 0, hi = 0; };

// Nombre maximal de familles NOMMEES, et index de la famille de queue. Ces
// deux nombres sont la forme des compteurs que le moteur exporte deja
// (d2rt_eipprof[8], d2rt_tp_bucket[8]) et des sept libelles que les portages
// impriment : ils ne peuvent pas bouger d'un cote sans bouger de l'autre.
enum { kProfFamMax = 6, kProfFamOther = 6 };

struct Wx86ProfMap {
    Wx86ProfRange fam[kProfFamMax] = {};
    int           nfam = 0;        // familles reellement decrites (0 = aucune)
    // Trois fenetres de DETAIL, chacune remplissant un compteur deja exporte.
    // 0 = fenetre eteinte (une RVA nulle est l'en-tete PE, jamais du code).
    uint32_t zoom_4k_a = 0;        // 32 cases de 4 KiB  -> d2rt_eipprof_sub
    uint32_t zoom_4k_b = 0;        // 32 cases de 4 KiB  -> d2rt_eipprof_sub2
    uint32_t zoom_256  = 0;        // 16 cases de 256 o  -> d2rt_eipprof_fn
};

// A appeler avant d'armer un profil. Sans appel, la carte reste vide.
void wx86_prof_set_map(const Wx86ProfMap& m);
const Wx86ProfMap& wx86_prof_map();

// ---- LA CLASSIFICATION, en en-tete ----------------------------------------
// Elle vit ici et pas dans cpu_box86.cpp pour UNE raison : cpu_box86.cpp ne se
// compile que pour ARM/Vita, donc son classement n'a jamais pu etre exerce par
// un oracle. Ici, tools/prof_map_selftest.cpp le compare a une transcription
// fidele de la chaine de else-if d'origine, sur un balayage de tout le .text
// — et prouve que l'oracle coupe en injectant une faute d'une borne.

// Famille d'une RVA : la PREMIERE plage qui la contient, sinon kProfFamOther.
inline int wx86_prof_family(const Wx86ProfMap& m, uint32_t rva) {
    for (int i = 0; i < m.nfam; ++i)
        if (rva >= m.fam[i].lo && rva < m.fam[i].hi) return i;
    return kProfFamOther;
}

// Case d'une fenetre de detail de `slots` cases de 2^shift octets a partir de
// `base`, ou -1 si l'adresse est hors fenetre (base = 0 : fenetre eteinte).
inline int wx86_prof_zoom(uint32_t base, unsigned shift, int slots, uint32_t rva) {
    if (!base || rva < base) return -1;
    const uint32_t off = rva - base;
    if ((off >> shift) >= (uint32_t)slots) return -1;
    return (int)(off >> shift);
}
