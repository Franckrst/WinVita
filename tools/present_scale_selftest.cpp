// tools/present_scale_selftest.cpp — prouve que wx86::scale_blit rend OCTET
// POUR OCTET ce que rendait la boucle de presentation deja validee sur console.
//
// POURQUOI CET OUTIL EXISTE
// -------------------------
// La couche de presentation des portages vit sous `#ifdef __vita__` : elle
// n'est compilee ni sur bureau ni sous qemu, donc l'oracle d'identite d'image
// du projet ne peut PAS l'exercer hors console. Deplacer ce code sans filet
// reviendrait a modifier un chemin d'affichage critique en ne pouvant le
// verifier que sur materiel.
//
// Ce programme est ce filet. Il embarque une TRANSCRIPTION FIDELE de la boucle
// d'origine (ref_scale ci-dessous, plein ecran, telle qu'elle existe dans le
// portage) et la compare a la version generique du moteur sur des entrees
// pseudo-aleatoires deterministes. Une seule difference d'octet fait echouer.
//
// Ce qu'il prouve : l'extraction ne change pas un pixel, dans le cas plein
// ecran qui est celui du portage aujourd'hui.
// Ce qu'il NE prouve PAS : le comportement sur console (memoire d'affichage
// reelle, cadence, incrustations). Ca reste a valider sur materiel le jour ou
// le portage adoptera cette fonction.
#include "platform/present_scale.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

// ---- REFERENCE : les boucles D'ORIGINE DES PORTAGES, transcrites telles quelles
//
// Deux references, pas une. La premiere version de ce fichier ne portait que le
// cas PLEIN ECRAN du premier consommateur ; le sous-rectangle (les bandes
// noires laterales) n'etait verifie que par des invariants de bornes, jamais
// compare a une sortie de reference. Or c'est le cas par DEFAUT du second
// consommateur, et c'est celui ou un arrondi ou un pas de ligne faux se voit.
//
// ref_scale      — boucle plein ecran du premier consommateur (8 et 32 bits)
// ref_scale_rect — boucle a sous-rectangle du second (8, 16 et 32 bits), avec
//                  son cadrage proportionnel deja resolu par l'appelant.
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
            for (; x + 4 <= SCR_W; x += 4) {            // deroule 4:1
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

// Boucle du SECOND consommateur (src/platform/vita_present.cpp,
// do_scale_and_flip), transcrite au caractere pres sauf les noms d'ecran.
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
            for (; x + 4 <= dstW; x += 4) {             // deroule 4:1
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
                uint32_t bgr = r32[xacc >> 16];         // DIB 0x00RRGGBB (B,G,R,X en memoire)
                out[x] = 0xFF000000u | ((bgr & 0xFF) << 16) | (bgr & 0xFF00) | ((bgr >> 16) & 0xFF);
            }
        }
    }
}

// Generateur deterministe : un echec doit etre rejouable a l'identique.
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

    // Fond non nul des deux cotes : une difference dans les lignes dupliquees
    // se verrait, alors qu'un fond a zero pourrait la masquer.
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


// Cas compare a la boucle du SECOND consommateur. `stretch` choisit entre son
// mode plein ecran et son mode par defaut (cadrage proportionnel, bandes
// noires) — les deux passent par le meme corps, ce qui est exactement la
// propriete que fit_rect + scale_blit pretendent avoir.
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
    // 960x544 est l'ecran de la console ; les autres tailles verifient que rien
    // ne depend d'une dimension particuliere.
    const int screens[][2] = {{960, 544}, {640, 480}, {320, 200}, {1, 1}, {7, 3}};
    // Sources : les modes reels des jeux, plus des cas degeneres (source plus
    // grande que l'ecran, source minuscule, dimensions premieres).
    const int sources[][2] = {{640, 480}, {800, 600}, {320, 240}, {1024, 768},
                              {960, 544}, {13, 7}, {1, 1}, {1280, 1024}};

    int cases = 0;
    // (a) contre la boucle PLEIN ECRAN du premier consommateur. Elle ne connait
    //     que 8 et 32 bits : le 16 bits n'a pas de reference de ce cote-la, et
    //     l'y inventer serait ecrire soi-meme la reponse qu'on verifie.
    for (auto& s : screens)
        for (auto& g : sources)
            for (int bpp : {8, 32}) { one_case(s[0], s[1], g[0], g[1], bpp); ++cases; }

    // (b) contre la boucle du second consommateur, dans SES DEUX cadrages et
    //     sur SES TROIS formats. Le sous-rectangle n'etait jusqu'ici verifie
    //     que par des invariants de bornes — jamais compare a une sortie.
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

    // Bornes du cadrage : invariant, en plus de la comparaison ci-dessus.
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
