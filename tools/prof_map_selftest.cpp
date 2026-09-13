// Oracle for src/runtime/prof_map.h.
//
// The profiler used to classify each sample with a hardcoded chain of six
// `else if` checks at Diablo II subsystem boundaries; that chain is now a
// table the consumer describes instead. The risk in such a conversion is an
// unfaithful transcription — a boundary that shifts by one address makes
// every profile compared against it incomparable.
//
// This oracle sweeps the entire .text range (0..0x300000, one point every
// 64 bytes, plus exact boundaries and their neighbors) and compares
// wx86_prof_family()'s output against a faithful transcription of the
// original chain (below).
//
// Negative control: the same comparison is replayed with a map where one
// boundary is off by one byte; it must diverge. An oracle that reports
// "identical" either way proves nothing.
#include <cstdio>
#include <cstdint>
#include "runtime/prof_map.h"

// --- the original chain, transcribed verbatim -------------------------------
static int famille_historique(uint32_t rva) {
    if      (rva >= 0x20b200 && rva < 0x20d040) return 0;   // Codec.cpp (DCC)
    else if (rva >= 0x1fe000 && rva < 0x204000) return 1;   // SpriteCache.cpp
    else if (rva >= 0x2094b0 && rva < 0x20ab00) return 2;   // Tilecmp.cpp
    else if (rva >= 0x242000 && rva < 0x280000) return 3;   // DRLG
    else if (rva >= 0x0f0000 && rva < 0x110000) return 4;   // Gfx / blit
    else if (rva <  0x030000)                   return 5;   // Storm
    else                                        return 6;   // remainder
}

// The map a D2 port installs (tools/rt_boot.cpp, d2_prof_map()).
static Wx86ProfMap carte_d2(bool faute) {
    Wx86ProfMap m;
    m.fam[0] = {0x20b200, 0x20d040};
    m.fam[1] = {0x1fe000, 0x204000};
    m.fam[2] = {0x2094b0, 0x20ab00};
    m.fam[3] = {0x242000, 0x280000};
    m.fam[4] = {0x0f0000, faute ? 0x110001u : 0x110000u};   // <- the injected fault
    m.fam[5] = {0x000000, 0x030000};
    m.nfam   = 6;
    m.zoom_4k_a = 0x0f0000; m.zoom_4k_b = 0x0d0000; m.zoom_256 = 0x0fa000;
    return m;
}

static long compare(const Wx86ProfMap& m, long* points) {
    long diff = 0, n = 0;
    // regular sweep
    for (uint32_t rva = 0; rva < 0x300000u; rva += 64) {
        ++n; if (wx86_prof_family(m, rva) != famille_historique(rva)) ++diff;
    }
    // exact boundaries and immediate neighbors
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

    // Empty map: the case of a port that hasn't described anything.
    // Everything must fall into the tail family — never into a family that
    // belongs to a different port's map.
    Wx86ProfMap vide;
    long tous = 0;
    for (uint32_t rva = 0; rva < 0x300000u; rva += 1024)
        if (wx86_prof_family(vide, rva) == kProfFamOther) ++tous;
    ok(tous == 0x300000 / 1024, "carte vide : tout tombe dans la famille de queue");

    // Detail windows: bounds and the disabled case.
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
