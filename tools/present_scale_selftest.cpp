// Proves that wx86::scale_blit renders byte-for-byte what the presentation
// loop already validated on console renders.
//
// Why this tool exists: the presentation layer lives under `#ifdef __vita__`
// and is compiled neither on desktop nor under qemu, so the project's image-
// identity oracle cannot exercise it off console. Moving this code without a
// safety net would mean changing a critical display path that could only be
// verified on hardware.
//
// This program is that safety net. It embeds a faithful transcription of the
// original loop (ref_scale below, full-screen, as it exists in the port) and
// compares it against the engine's generic version on deterministic
// pseudo-random inputs. A single byte difference fails the test.
//
// What it proves: extraction doesn't change a pixel, for the full-screen
// case (the port's case today).
// What it does not prove: behavior on console (real display memory, frame
// cadence, overlays). That still needs hardware validation whenever a port
// adopts this function.
#include "platform/present_scale.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

// ---- Reference: the ports' original loops, transcribed as-is ----------------
//
// Two references, not one: a full-screen loop and a sub-rect (letterboxed)
// loop, since one port defaults to letterboxing, and that's exactly the case
// where a rounding or pitch mistake would show up.
//
// ref_scale      — full-screen loop (8 and 32 bit)
// ref_scale_rect — sub-rect loop (8, 16 and 32 bit), with its aspect-fit
//                  rectangle already resolved by the caller.
void ref_scale(uint32_t* dst, int SCR_W, int SCR_H,
               const uint8_t* pixels, int w, int h, int bpp, const uint8_t* pal) {
    uint32_t pal32[256];
    if (bpp == 8)
        for (int i = 0; i < 256; ++i) {
            const uint8_t* p = pal + i * 4;             // B,G,R,0
            pal32[i] = 0xFF000000u | (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
        }
    const uint32_t sx = (uint32_t)((w << 16) / SCR_W);
    const uint32_t sy = (uint32_t)((h << 16) / SCR_H);
    int prev_srcy = -1;
    for (int y = 0; y < SCR_H; ++y) {
        int srcy = (int)((y * sy) >> 16);
        uint32_t* out = dst + (size_t)y * SCR_W;
        if (srcy == prev_srcy) { std::memcpy(out, out - SCR_W, SCR_W * 4); continue; }
        prev_srcy = srcy;
        const uint8_t* row = pixels + (size_t)srcy * w * (bpp / 8);
        uint32_t xacc = 0;
        if (bpp == 8) {
            int x = 0;
            for (; x + 4 <= SCR_W; x += 4) {            // 4:1 unroll
                out[x + 0] = pal32[row[xacc >> 16]]; xacc += sx;
                out[x + 1] = pal32[row[xacc >> 16]]; xacc += sx;
                out[x + 2] = pal32[row[xacc >> 16]]; xacc += sx;
                out[x + 3] = pal32[row[xacc >> 16]]; xacc += sx;
            }
            for (; x < SCR_W; ++x, xacc += sx) out[x] = pal32[row[xacc >> 16]];
        } else if (bpp == 32) {
            const uint32_t* r32 = (const uint32_t*)row;
            for (int x = 0; x < SCR_W; ++x, xacc += sx) {
                uint32_t bgr = r32[xacc >> 16];
                out[x] = 0xFF000000u | ((bgr & 0xFF) << 16) | (bgr & 0xFF00) | ((bgr >> 16) & 0xFF);
            }
        }
    }
}

// Transcribed character-for-character from src/platform/vita_present.cpp
// (do_scale_and_flip), except for the screen variable names.
void ref_scale_rect(uint32_t* dst, int SCR_W, int /*SCR_H*/,
                    const uint8_t* pixels, int srcW, int srcH, int bpp,
                    const uint8_t* pal, int ox, int oy, int dstW, int dstH) {
    uint32_t pal32[256];
    if (bpp == 8)
        for (int i = 0; i < 256; ++i) {
            const uint8_t* p = pal + i * 4;             // B,G,R,0
            pal32[i] = 0xFF000000u | (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
        }
    const uint32_t sx = (uint32_t)(((uint32_t)srcW << 16) / (uint32_t)dstW);
    const uint32_t sy = (uint32_t)(((uint32_t)srcH << 16) / (uint32_t)dstH);
    const int srcPitch = srcW * (bpp / 8);
    int prev_srcy = -1;
    for (int y = 0; y < dstH; ++y) {
        int srcy = (int)((y * sy) >> 16);
        uint32_t* out = dst + (size_t)(oy + y) * SCR_W + ox;
        if (srcy == prev_srcy) { std::memcpy(out, out - SCR_W, (size_t)dstW * 4); continue; }
        prev_srcy = srcy;
        const uint8_t* row = pixels + (size_t)srcy * srcPitch;
        uint32_t xacc = 0;
        if (bpp == 8) {
            int x = 0;
            for (; x + 4 <= dstW; x += 4) {             // 4:1 unroll
                out[x+0] = pal32[row[xacc >> 16]]; xacc += sx;
                out[x+1] = pal32[row[xacc >> 16]]; xacc += sx;
                out[x+2] = pal32[row[xacc >> 16]]; xacc += sx;
                out[x+3] = pal32[row[xacc >> 16]]; xacc += sx;
            }
            for (; x < dstW; ++x, xacc += sx) out[x] = pal32[row[xacc >> 16]];
        } else if (bpp == 16) {
            const uint16_t* r16 = (const uint16_t*)row;
            for (int x = 0; x < dstW; ++x, xacc += sx) {
                uint32_t p = r16[xacc >> 16];
                uint32_t r = (p >> 10) & 31, g = (p >> 5) & 31, b = p & 31;
                out[x] = 0xFF000000u
                       | (((b << 3) | (b >> 2)) << 16)
                       | (((g << 3) | (g >> 2)) << 8)
                       |  ((r << 3) | (r >> 2));
            }
        } else if (bpp == 32) {
            const uint32_t* r32 = (const uint32_t*)row;
            for (int x = 0; x < dstW; ++x, xacc += sx) {
                uint32_t bgr = r32[xacc >> 16];         // DIB 0x00RRGGBB (B,G,R,X in memory)
                out[x] = 0xFF000000u | ((bgr & 0xFF) << 16) | (bgr & 0xFF00) | ((bgr >> 16) & 0xFF);
            }
        }
    }
}

// Deterministic generator: a failure must be reproducible exactly.
uint32_t rng_state = 0x12345678u;
uint32_t rnd() {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return rng_state;
}

int failures = 0;

wx86::SrcFormat fmt_of(int bpp) {
    return bpp == 8  ? wx86::SrcFormat::Pal8
         : bpp == 16 ? wx86::SrcFormat::Rgb555
                     : wx86::SrcFormat::Bgra32;
}


void one_case(int scrW, int scrH, int w, int h, int bpp) {
    const int bytesPerPx = bpp / 8;
    std::vector<uint8_t> src((size_t)w * h * bytesPerPx);
    for (auto& b : src) b = (uint8_t)(rnd() & 0xFF);
    std::vector<uint8_t> pal(1024);
    for (auto& b : pal) b = (uint8_t)(rnd() & 0xFF);

    // Nonzero background on both sides: a difference in duplicated rows would
    // show up, whereas a zeroed background could mask it.
    std::vector<uint32_t> a((size_t)scrW * scrH, 0xDEADBEEFu);
    std::vector<uint32_t> b((size_t)scrW * scrH, 0xDEADBEEFu);

    ref_scale(a.data(), scrW, scrH, src.data(), w, h, bpp, pal.data());

    const wx86::DstRect full{0, 0, scrW, scrH};
    wx86::scale_blit(b.data(), scrW, full, src.data(), w, h, w * bytesPerPx,
                     fmt_of(bpp), pal.data());

    if (std::memcmp(a.data(), b.data(), a.size() * 4) != 0) {
        size_t firstDiff = 0;
        while (firstDiff < a.size() && a[firstDiff] == b[firstDiff]) ++firstDiff;
        std::printf("  ECHEC ecran=%dx%d source=%dx%d bpp=%d — 1er ecart au pixel %zu "
                    "(attendu 0x%08X, obtenu 0x%08X)\n",
                    scrW, scrH, w, h, bpp, firstDiff,
                    (unsigned)a[firstDiff], (unsigned)b[firstDiff]);
        ++failures;
    }
}


// Case compared against the sub-rect reference loop. `stretch` chooses
// between full-screen mode and the default aspect-fit mode (letterboxing) —
// both go through the same body, which is exactly the property fit_rect +
// scale_blit are meant to have.
void one_case_rect(int scrW, int scrH, int w, int h, int bpp, bool stretch) {
    const int bytesPerPx = bpp / 8;
    std::vector<uint8_t> src((size_t)w * h * bytesPerPx);
    for (auto& b : src) b = (uint8_t)(rnd() & 0xFF);
    std::vector<uint8_t> pal(1024);
    for (auto& b : pal) b = (uint8_t)(rnd() & 0xFF);

    const wx86::DstRect r = wx86::fit_rect(w, h, scrW, scrH, stretch);

    std::vector<uint32_t> a((size_t)scrW * scrH, 0xDEADBEEFu);
    std::vector<uint32_t> b((size_t)scrW * scrH, 0xDEADBEEFu);

    ref_scale_rect(a.data(), scrW, scrH, src.data(), w, h, bpp, pal.data(),
                   r.x, r.y, r.w, r.h);
    wx86::scale_blit(b.data(), scrW, r, src.data(), w, h, w * bytesPerPx,
                     fmt_of(bpp), pal.data());

    if (std::memcmp(a.data(), b.data(), a.size() * 4) != 0) {
        size_t firstDiff = 0;
        while (firstDiff < a.size() && a[firstDiff] == b[firstDiff]) ++firstDiff;
        std::printf("  ECHEC %s ecran=%dx%d source=%dx%d bpp=%d rect=%d,%d %dx%d — "
                    "1er ecart au pixel %zu (attendu 0x%08X, obtenu 0x%08X)\n",
                    stretch ? "plein ecran" : "cadre",
                    scrW, scrH, w, h, bpp, r.x, r.y, r.w, r.h, firstDiff,
                    (unsigned)a[firstDiff], (unsigned)b[firstDiff]);
        ++failures;
    }
}

}  // namespace

int main() {
    // 960x544 is the console's screen; the other sizes verify nothing
    // depends on a specific dimension.
    const int screens[][2] = {{960, 544}, {640, 480}, {320, 200}, {1, 1}, {7, 3}};
    // Sources: real game modes, plus degenerate cases (source larger than
    // the screen, a tiny source, prime dimensions).
    const int sources[][2] = {{640, 480}, {800, 600}, {320, 240}, {1024, 768},
                              {960, 544}, {13, 7}, {1, 1}, {1280, 1024}};

    int cases = 0;
    // (a) against the full-screen reference loop. It only knows 8 and 32
    //     bit: there is no 16-bit reference on this side, and inventing one
    //     would mean writing the answer we're trying to verify.
    for (auto& s : screens)
        for (auto& g : sources)
            for (int bpp : {8, 32}) { one_case(s[0], s[1], g[0], g[1], bpp); ++cases; }

    // (b) against the sub-rect reference loop, in both its letterbox modes
    //     and across all three formats.
    for (auto& s : screens)
        for (auto& g : sources)
            for (int bpp : {8, 16, 32}) {
                one_case_rect(s[0], s[1], g[0], g[1], bpp, true);  ++cases;
                one_case_rect(s[0], s[1], g[0], g[1], bpp, false); ++cases;
            }

    if (failures) {
        std::printf("present_scale: %d ECHEC(S) sur %d cas\n", failures, cases);
        return 1;
    }
    std::printf("present_scale: %d cas (plein ecran ET sous-rectangle, 8/16/32 bits),"
                " sortie identique octet pour octet\n", cases);

    // Aspect-fit bounds: an invariant, on top of the comparison above.
    int bad = 0;
    for (auto& s : screens)
        for (auto& g : sources) {
            const wx86::DstRect r = wx86::fit_rect(g[0], g[1], s[0], s[1], false);
            if (r.w < 1 || r.h < 1 || r.w > s[0] || r.h > s[1] ||
                r.x < 0 || r.y < 0 || r.x + r.w > s[0] || r.y + r.h > s[1]) {
                std::printf("  ECHEC cadrage source=%dx%d ecran=%dx%d -> %d,%d %dx%d\n",
                            g[0], g[1], s[0], s[1], r.x, r.y, r.w, r.h);
                ++bad;
            }
        }
    if (bad) { std::printf("present_scale: %d cadrage(s) hors ecran\n", bad); return 1; }
    std::printf("present_scale: cadrage proportionnel dans l'ecran dans tous les cas\n");
    return 0;
}
