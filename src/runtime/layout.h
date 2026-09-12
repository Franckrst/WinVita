// src/runtime/layout.h — decalage global du plan memoire invite.
//
// POURQUOI. Sur Vita la memoire invitee n'est PAS a l'identite : le noyau ne
// rend a l'application que des blocs >= 0x80000000, alors que le jeu vit en bas
// (plan compact 0x00500000..0x11900000). D'ou l'arene unique et
// `membase = hote - invite` : CHAQUE acces memoire x86 devient « ADD
// adresse+membase » PUIS « LDR/STR », et cet ADD est sur le chemin critique de
// chaque chargement (12,6 % du code emis des blocs chauds, 21,4 % du total).
// Le recensement des formes d'adressage (docs/perf/fastmmu_20260905.md) dit que
// 61,5 % de ces ADD ne sont supprimables par AUCUNE astuce d'adressage ARM :
// la seule facon de tous les retirer est membase = 0, c'est-a-dire le MODELE A
// L'IDENTITE, ou l'adresse invitee EST l'adresse hote. Sur Vita cela impose de
// faire vivre TOUT l'invite au-dessus de 0x80000000.
//
// L'obstacle suppose etait le commentaire en tete des « guest allocators » de
// tools/rt_boot.cpp : « Fog's pointer validator rejects addresses >=
// 0x80000000 ». D2LAYOUT=haut existe pour trancher cette question SOUS QEMU :
// il rejoue le plan compact translate, avec membase=0 (pas de D2ARENA), donc
// exactement le modele a l'identite.
// VERDICT (docs/perf/identite_20260905.md) : le commentaire est FAUX tel qu'il
// est ecrit — aucun validateur ne refuse quoi que ce soit. Mais le plan haut
// plante quand meme : la bibliotheque de conteneurs de Blizzard range ses
// pointeurs COMPLEMENTES et les distingue des deplacements PAR LE BIT DE
// SIGNE. Au-dessus de 2 Gio, `~p` devient positif et l'encodage se trompe en
// silence. D2HI sous 2 Gio (le defaut de l'oracle : 0x01000000) donne en
// revanche 4000 images PIXEL-IDENTIQUES a la reference console.
//
// CONTRAT. D2LAYOUT absent, ou toute autre valeur que « haut », rend 0 :
// comportement d'avant, a l'octet pres. « compact » garde son sens exact
// (translation nulle). Le decalage est lu PARESSEUSEMENT (statique locale) :
// sur Vita env.txt n'est applique qu'apres l'initialisation statique, un
// getenv pre-main ne verrait jamais le knob.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace d2rt {

// Decalage applique a TOUTES les regions invitees. 0 = aucun (defaut).
// WX86_HI=<hex> (repli D2HI) change la base (defaut 0x81000000, la forme des
// blocs Vita). Les noms WX86_* sont les PRIMAIRES : les anciens noms au prefixe
// du premier consommateur restent acceptes pour ne casser aucune recette.
// Aligne sur 1 Mio : le plan compact l'est deja, et un ADD de membase encodable
// exige des bits bas nuls du cote arene — ici membase vaut 0, mais on garde
// l'alignement pour que les adresses restent lisibles dans les journaux.
inline uint32_t layout_hi() {
    static const uint32_t hi = [] () -> uint32_t {
        const char* l = std::getenv("WX86_LAYOUT");
        if (!l) l = std::getenv("D2LAYOUT");
        if (!l || std::strcmp(l, "haut")) return 0u;
        const char* h = std::getenv("WX86_HI");
        if (!h) h = std::getenv("D2HI");
        uint32_t v = h ? (uint32_t)std::strtoul(h, nullptr, 16) : 0x81000000u;
        return v & ~0xFFFFFu;
    }();
    return hi;
}

// Vrai pour les deux plans TASSES (compact et haut) : ils partagent le meme
// pack, seul le decalage change.
inline bool layout_packed() {
    const char* l = std::getenv("WX86_LAYOUT");
    if (!l) l = std::getenv("D2LAYOUT");
    return l && (!std::strcmp(l, "compact") || !std::strcmp(l, "haut"));
}

// Bornes de « l'espace utilisateur » annonce au jeu (GetSystemInfo) et utilise
// par l'introspection VirtualQuery. En plan haut, l'espace utilisateur EST le
// bloc : [HI, HI + 0x12000000). Sans decalage, les valeurs Win32 d'avant.
inline uint32_t layout_user_lo() { uint32_t h = layout_hi(); return h ? h : 0x00010000u; }
inline uint32_t layout_user_hi() { uint32_t h = layout_hi(); return h ? (h + 0x12000000u) : 0x7FFF0000u; }

} // namespace d2rt
