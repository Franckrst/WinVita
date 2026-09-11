// src/render/render_null.cpp — le backend de COMPTAGE.
//
// Il ne dessine rien et compte tout. Deux usages, tous deux reels :
//   * sur bureau et sous qemu, ou il n'y a pas de GPU de console : le
//     constructeur de lots du portage tourne quand meme, et TOUS ses compteurs
//     restent mesurables — seule la soumission manque ;
//   * comme repli d'un backend dont l'initialisation echoue, pour qu'un GPU
//     indisponible donne une image sans acceleration plutot qu'un ecran noir.
//
// Les deux portages consommateurs du moteur avaient chacun ecrit le leur.
#include "render.h"

#include <cstdio>

namespace wx86 {
namespace render {

namespace {

NullStats g_stats{};

bool null_init(int, int) { return true; }
void null_shutdown() {}

bool null_texture_create(uint32_t, int, int, TexFormat) { ++g_stats.texCreates; return true; }

void null_texture_upload(uint32_t, int, int, int w, int h, const void*, int srcPitch) {
    // On compte les octets REELLEMENT presentes, pas la surface : un pas de
    // ligne plus large que le rectangle est un cas courant, et confondre les
    // deux fausserait toute comparaison de debit.
    if (w > 0 && h > 0 && srcPitch > 0)
        g_stats.texUploadBytes += (uint64_t)h * (uint64_t)srcPitch;
}

void null_palette_set(int, const uint32_t*) { ++g_stats.paletteSets; }

void null_draw(const DrawKey&, const Vertex*, uint32_t vertCount,
               const uint16_t*, uint32_t idxCount) {
    ++g_stats.draws;
    g_stats.verts += vertCount;
    g_stats.indices += idxCount;
}

void null_clear_color(uint32_t) { ++g_stats.clearColors; }
void null_clear_depth() { ++g_stats.clearDepths; }
void null_present(uint64_t) { ++g_stats.frames; }

// Rien n'est jamais en vol : le comptage est synchrone par construction.
uint64_t null_in_flight_from() { return ~(uint64_t)0; }
void null_drain() {}

int null_counters(char* out, unsigned n) {
    if (!out || n == 0) return 0;
    const int r = std::snprintf(out, n,
        "render(null): images=%llu lots=%llu sommets=%llu indices=%llu "
        "tex=%llu octets=%llu pal=%llu effC=%llu effZ=%llu",
        (unsigned long long)g_stats.frames,  (unsigned long long)g_stats.draws,
        (unsigned long long)g_stats.verts,   (unsigned long long)g_stats.indices,
        (unsigned long long)g_stats.texCreates, (unsigned long long)g_stats.texUploadBytes,
        (unsigned long long)g_stats.paletteSets,
        (unsigned long long)g_stats.clearColors, (unsigned long long)g_stats.clearDepths);
    if (r < 0) return 0;
    return (unsigned)r >= n ? (int)n - 1 : r;
}

const Backend kNull = {
    null_init, null_shutdown,
    null_texture_create, null_texture_upload, null_palette_set,
    null_draw,
    null_clear_color, null_clear_depth, null_present,
    null_in_flight_from, null_drain,
    null_counters,
};

}  // namespace

const Backend& null_backend() { return kNull; }
const NullStats& null_stats() { return g_stats; }
void null_stats_reset() { g_stats = NullStats{}; }

}  // namespace render
}  // namespace wx86
