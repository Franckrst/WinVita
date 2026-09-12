// tools/prof_map_selftest.cpp — l'oracle de src/runtime/prof_map.h.
//
// CE QU'IL PROUVE. Jusqu'au 2026-09-12 le profileur du moteur classait chaque
// echantillon dans une chaine de six `else if` ecrite EN DUR, aux bornes des
// sous-systemes de Diablo II. Cette chaine est devenue une table que le
// consommateur decrit. Le risque de la conversion est une transcription
// infidele — un seau qui bouge d'une adresse et tous les profils dates
// deviennent incomparables.
//
// L'oracle compare donc, sur un BALAYAGE de tout le .text (0..0x300000, un
// point tous les 64 octets, plus les bornes exactes et leurs voisins), la
// sortie de wx86_prof_family() a une transcription fidele de la chaine
// d'origine, recopiee ici depuis l'historique de cpu_box86.cpp.
//
// ET QU'IL COUPE : la meme comparaison est rejouee avec une carte dont UNE
// borne est decalee d'un octet ; elle doit produire des divergences. Un oracle
// qui rend « identique » dans les deux jambes ne prouve rien.
#include <cstdio>
#include <cstdint>
#include "runtime/prof_map.h"

// --- la chaine d'origine, transcrite mot pour mot ---------------------------
static int famille_historique(uint32_t rva) {
    if      (rva >= 0x20b200 && rva < 0x20d040) return 0;   // Codec.cpp (DCC)
    else if (rva >= 0x1fe000 && rva < 0x204000) return 1;   // SpriteCache.cpp
    else if (rva >= 0x2094b0 && rva < 0x20ab00) return 2;   // Tilecmp.cpp
    else if (rva >= 0x242000 && rva < 0x280000) return 3;   // DRLG
    else if (rva >= 0x0f0000 && rva < 0x110000) return 4;   // Gfx / blit
    else if (rva <  0x030000)                   return 5;   // Storm
    else                                        return 6;   // reste
}

// La carte que le portage D2 installe (tools/rt_boot.cpp, d2_prof_map()).
static Wx86ProfMap carte_d2(bool faute) {
    Wx86ProfMap m;
    m.fam[0] = {0x20b200, 0x20d040};
    m.fam[1] = {0x1fe000, 0x204000};
    m.fam[2] = {0x2094b0, 0x20ab00};
    m.fam[3] = {0x242000, 0x280000};
    m.fam[4] = {0x0f0000, faute ? 0x110001u : 0x110000u};   // <- la faute
    m.fam[5] = {0x000000, 0x030000};
    m.nfam   = 6;
    m.zoom_4k_a = 0x0f0000; m.zoom_4k_b = 0x0d0000; m.zoom_256 = 0x0fa000;
    return m;
}

static long compare(const Wx86ProfMap& m, long* points) {
    long diff = 0, n = 0;
    // balayage regulier
    for (uint32_t rva = 0; rva < 0x300000u; rva += 64) {
        ++n; if (wx86_prof_family(m, rva) != famille_historique(rva)) ++diff;
    }
    // bornes exactes et voisins immediats
    static const uint32_t bornes[] = {
        0x20b200,0x20d040,0x1fe000,0x204000,0x2094b0,0x20ab00,
        0x242000,0x280000,0x0f0000,0x110000,0x030000,0x0d0000,0x0fa000,0x0fb000 };
    for (unsigned i = 0; i < sizeof bornes / sizeof *bornes; ++i)
        for (int d = -2; d <= 2; ++d) {
            const uint32_t rva = bornes[i] + (uint32_t)d;
            ++n; if (wx86_prof_family(m, rva) != famille_historique(rva)) ++diff;
        }
    *points = n;
    return diff;
}

static int erreurs = 0, verifs = 0;
static void ok(bool c, const char* quoi) {
    ++verifs; if (!c) { std::printf("  ECHEC: %s\n", quoi); ++erreurs; }
}

int main() {
    long n = 0;
    const long d_sain = compare(carte_d2(false), &n);
    std::printf("prof_map: %ld points compares a la chaine d'origine\n", n);
    ok(d_sain == 0, "la carte reproduit la chaine d'origine, point pour point");

    long n2 = 0;
    const long d_faute = compare(carte_d2(true), &n2);
    ok(d_faute > 0, "CONTROLE NEGATIF : une borne decalee d'un octet est VUE");

    // Carte VIDE : le cas d'un portage qui n'a rien decrit. Tout doit tomber
    // dans la famille de queue — jamais dans une famille nommee par un autre jeu.
    Wx86ProfMap vide;
    long tous = 0;
    for (uint32_t rva = 0; rva < 0x300000u; rva += 1024)
        if (wx86_prof_family(vide, rva) == kProfFamOther) ++tous;
    ok(tous == 0x300000 / 1024, "carte vide : tout tombe dans la famille de queue");

    // Fenetres de detail : bornes et extinction.
    ok(wx86_prof_zoom(0x0f0000, 12, 32, 0x0effff) == -1, "zoom : sous la base -> hors fenetre");
    ok(wx86_prof_zoom(0x0f0000, 12, 32, 0x0f0000) ==  0, "zoom : premiere case");
    ok(wx86_prof_zoom(0x0f0000, 12, 32, 0x10ffff) == 31, "zoom : derniere case");
    ok(wx86_prof_zoom(0x0f0000, 12, 32, 0x110000) == -1, "zoom : au-dela -> hors fenetre");
    ok(wx86_prof_zoom(0x0fa000,  8, 16, 0x0fa0ff) ==  0, "zoom fin : case 0 sur 256 octets");
    ok(wx86_prof_zoom(0x0fa000,  8, 16, 0x0fa100) ==  1, "zoom fin : case 1");
    ok(wx86_prof_zoom(0x0fa000,  8, 16, 0x0fb000) == -1, "zoom fin : au-dela -> hors fenetre");
    ok(wx86_prof_zoom(0,        12, 32, 0x000100) == -1, "base 0 = fenetre eteinte");

    std::printf("prof_map: %d verifications, %d echec(s)\n", verifs, erreurs);
    return erreurs ? 1 : 0;
}
