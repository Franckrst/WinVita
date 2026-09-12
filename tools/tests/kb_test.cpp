// tools/tests/kb_test.cpp — ORACLE D'HOTE DU CLAVIER VIRTUEL.
//
// src/platform/vita_kb.h ne touche ni au VitaSDK ni a un appel systeme : tout
// ce qui suit se compile et tourne sur la machine de developpement, donc se
// PROUVE sans console. Ce qui reste a la console ne se prouve qu'a l'oeil : le
// clavier apparait-il bien PAR-DESSUS l'image, dans les deux chemins de
// presentation.
//
// POURQUOI CE FICHIER EST ICI ET PLUS SEULEMENT CHEZ UN PORTAGE
// ------------------------------------------------------------
// Le clavier est parti au moteur ; SON ORACLE EST RESTE chez le premier
// consommateur. C'est exactement le defaut que guest_sync.h avait deja montre
// une fois : l'objet demenage, la preuve reste derriere, et un SECOND
// consommateur herite d'un module sans filet — alors que le filet existe,
// tourne, et ne depend de rien d'autre que de l'en-tete deplace.
//
// Rejoue par tools/selftest.sh.
#include "platform/vita_kb.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, ...) do { ++g_checks; if (!(cond)) { ++g_fail; \
    std::printf("  FAIL %s:%d ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static const int W = 960, H = 544;
static std::vector<uint32_t> fb_new() { return std::vector<uint32_t>((size_t)W * H, 0xDEADBEEFu); }

static void t_ferme_ne_dessine_rien() {
    d2kb::State s; std::memset(&s, 0, sizeof s);
    auto fb = fb_new(); auto ref = fb;
    d2kb::draw(s, fb.data(), W, H);                 // open == 0
    CHECK(std::memcmp(fb.data(), ref.data(), fb.size() * 4) == 0, "clavier ferme : le tampon a change");
    d2kb::open_kb(s, 0);
    d2kb::draw(s, fb.data(), W, H);
    CHECK(std::memcmp(fb.data(), ref.data(), fb.size() * 4) != 0, "clavier ouvert : rien dessine");
    d2kb::close_kb(s);
    auto fb2 = fb_new(); auto ref2 = fb2;
    d2kb::draw(s, fb2.data(), W, H);
    CHECK(std::memcmp(fb2.data(), ref2.data(), fb2.size() * 4) == 0, "apres fermeture : le tampon a change");
}

static void t_dessin_borne_au_panneau() {
    d2kb::State s; std::memset(&s, 0, sizeof s); d2kb::open_kb(s, 0);
    auto fb = fb_new();
    d2kb::draw(s, fb.data(), W, H);
    const int y0 = d2kb::panel_y0(d2kb::LAY_FULL, H);
    CHECK(y0 > 0 && y0 < H, "y0 hors ecran (%d)", y0);
    int hors = 0;
    for (int y = 0; y < y0; ++y)
        for (int x = 0; x < W; ++x)
            if (fb[(size_t)y * W + x] != 0xDEADBEEFu) ++hors;
    CHECK(hors == 0, "%d pixels ecrits AU-DESSUS du panneau", hors);
}

// Couverture : les 95 caracteres ASCII imprimables sont atteignables.
static void t_couverture_ascii() {
    d2kb::State s; std::memset(&s, 0, sizeof s); d2kb::open_kb(s, 0);
    const d2kb::Layout& L = d2kb::LAY_FULL;
    bool vu[128] = { false };
    int dup = 0;
    for (int sh = 0; sh <= 1; ++sh)
        for (int r = 0; r < L.nrows; ++r)
            for (int c = 0; c < d2kb::cols(L, r); ++c) {
                const char f = d2kb::face(L, r, c, sh);
                CHECK(f >= 33 && f <= 126, "face (%d,%d,maj=%d) = 0x%02x hors ASCII imprimable", r, c, sh, (unsigned)f);
                if (f >= 33 && f <= 126) { if (vu[(int)f]) ++dup; vu[(int)f] = true; }
            }
    CHECK(dup == 0, "%d caracteres en double dans la disposition", dup);
    // l'espace a sa propre touche
    int ispace = -1;
    for (int i = 0; i < L.nfn; ++i) if (L.fnact[i] == d2kb::ACT_SPACE) ispace = i;
    CHECK(ispace >= 0, "aucune touche ESPACE");
    vu[32] = (ispace >= 0);
    int manquants = 0;
    for (int ch = 32; ch <= 126; ++ch)
        if (!vu[ch]) { ++manquants; std::printf("  manquant: 0x%02x '%c'\n", ch, ch); }
    CHECK(manquants == 0, "%d caracteres imprimables inatteignables", manquants);
    // et chacun a un glyphe NON VIDE dans la police (sinon touche muette)
    int vides = 0;
    for (int ch = 33; ch <= 126; ++ch) {
        const unsigned char* g = d2kb::glyph((char)ch);
        CHECK(g != 0, "pas de glyphe pour 0x%02x", ch);
        int on = 0; if (g) for (int i = 0; i < 16; ++i) on += (g[i] != 0);
        if (!on) { ++vides; std::printf("  glyphe vide: 0x%02x '%c'\n", ch, ch); }
    }
    CHECK(vides == 0, "%d glyphes vides", vides);
}

// L'emission : chaque cellule rend bien l'action et le caractere attendus.
static void t_activation() {
    d2kb::State s; std::memset(&s, 0, sizeof s); d2kb::open_kb(s, 0);
    const d2kb::Layout& L = d2kb::LAY_FULL;
    for (int r = 0; r < L.nrows; ++r)
        for (int c = 0; c < d2kb::cols(L, r); ++c) {
            s.shift = 0; char out = 0;
            d2kb::Act a = d2kb::activate(s, r, c, &out);
            CHECK(a == d2kb::ACT_CHAR && out == d2kb::face(L, r, c, 0), "(%d,%d) minuscule", r, c);
            s.shift = 2; out = 0;
            a = d2kb::activate(s, r, c, &out);
            CHECK(a == d2kb::ACT_CHAR && out == d2kb::face(L, r, c, 1), "(%d,%d) majuscule", r, c);
        }
    // hors table : jamais d'action, jamais d'ecriture
    char out = 1;
    CHECK(d2kb::activate(s, -1, 0, &out) == d2kb::ACT_NONE && out == 0, "activate(-1,0)");
    CHECK(d2kb::activate(s, 9, 0, &out) == d2kb::ACT_NONE, "activate(9,0)");
    CHECK(d2kb::activate(s, 0, 99, &out) == d2kb::ACT_NONE, "activate(0,99)");
}

static void t_majuscule_trois_etats() {
    d2kb::State s; std::memset(&s, 0, sizeof s); d2kb::open_kb(s, 0);
    CHECK(s.shift == 0, "MAJ non nulle a l'ouverture");
    d2kb::shift_cycle(s); CHECK(s.shift == 1, "1er appui MAJ");
    char out = 0;
    d2kb::activate(s, 1, 0, &out);                       // 'q' -> 'Q'
    CHECK(out == 'Q', "une seule touche : attendu Q, obtenu %c", out ? out : '?');
    CHECK(s.shift == 0, "MAJ une-seule-touche non retombee");
    d2kb::shift_cycle(s); d2kb::shift_cycle(s); CHECK(s.shift == 2, "verrouillage MAJ");
    d2kb::activate(s, 1, 0, &out); CHECK(out == 'Q', "verrouille : Q");
    CHECK(s.shift == 2, "le verrou est retombe");
    d2kb::shift_cycle(s); CHECK(s.shift == 0, "3e appui = MAJ relachee");
}

static void t_navigation() {
    for (int simple = 0; simple <= 1; ++simple) {
        d2kb::State s; std::memset(&s, 0, sizeof s); d2kb::open_kb(s, simple);
        const d2kb::Layout& L = d2kb::layout(s);
        const int nr = d2kb::nrows_total(L);
        // exhaustif : depuis chaque cellule, les 4 directions restent valides
        for (int r = 0; r < nr; ++r)
            for (int c = 0; c < d2kb::cols(L, r); ++c) {
                const int d[4][2] = { {-1,0},{1,0},{0,-1},{0,1} };
                for (int k = 0; k < 4; ++k) {
                    s.r = r; s.c = c;
                    d2kb::nav(s, d[k][0], d[k][1]);
                    CHECK(s.r >= 0 && s.r < nr, "nav: rangee %d hors [0,%d[", s.r, nr);
                    CHECK(s.c >= 0 && s.c < d2kb::cols(L, s.r),
                          "nav: colonne %d hors [0,%d[ (rangee %d)", s.c, d2kb::cols(L, s.r), s.r);
                }
            }
        // bouclage horizontal
        s.r = 1; s.c = 0; d2kb::nav(s, 0, -1);
        CHECK(s.c == d2kb::cols(L, 1) - 1, "pas de bouclage a gauche");
        d2kb::nav(s, 0, 1); CHECK(s.c == 0, "pas de bouclage a droite");
        // bouclage vertical
        s.r = 0; s.c = 0; d2kb::nav(s, -1, 0); CHECK(s.r == nr - 1, "pas de bouclage en haut");
        d2kb::nav(s, 1, 0); CHECK(s.r == 0, "pas de bouclage en bas");
    }
}

// Les touches dessinees et les touches touchables sont LES MEMES cellules.
static void t_tactile() {
    for (int simple = 0; simple <= 1; ++simple) {
        d2kb::State s; std::memset(&s, 0, sizeof s); d2kb::open_kb(s, simple);
        const d2kb::Layout& L = d2kb::layout(s);
        for (int r = 0; r < d2kb::nrows_total(L); ++r)
            for (int c = 0; c < d2kb::cols(L, r); ++c) {
                int x, y, w, h; d2kb::cell_rect(L, r, c, W, H, &x, &y, &w, &h);
                CHECK(x >= 0 && y >= 0 && x + w <= W && y + h <= H,
                      "cellule (%d,%d) hors ecran: %d,%d %dx%d", r, c, x, y, w, h);
                int rr = -1, cc = -1;
                CHECK(d2kb::hit(L, W, H, x + w / 2, y + h / 2, &rr, &cc) && rr == r && cc == c,
                      "centre de (%d,%d) touche (%d,%d)", r, c, rr, cc);
            }
        // au-dessus du panneau : aucune touche
        int rr, cc;
        CHECK(!d2kb::hit(L, W, H, W / 2, d2kb::panel_y0(L, H) - 4, &rr, &cc), "touche au-dessus du panneau");
    }
}

// MASQUE : le rendu ne doit dependre QUE de la longueur, jamais des lettres.
static void t_masque() {
    auto rendu = [](const char* txt, int mask) {
        d2kb::State s; std::memset(&s, 0, sizeof s); d2kb::open_kb(s, 0);
        s.mask = mask;
        for (int i = 0; txt[i]; ++i) d2kb::echo_push(s, txt[i]);
        auto fb = fb_new();
        d2kb::draw(s, fb.data(), W, H);
        return fb;
    };
    auto a = rendu("motdepasse", 1), b = rendu("ZZZZZZZZZZ", 1);
    CHECK(a == b, "mode masque : deux textes differents rendent des images differentes");
    auto c = rendu("motdepasse", 0);
    CHECK(!(a == c), "mode masque : identique au mode clair");
    auto d = rendu("motdepassX", 1);
    CHECK(a == d, "mode masque : la derniere lettre transparait");
    // longueur differente => image differente (le nombre de points se voit)
    auto e = rendu("motdepass", 1);
    CHECK(!(a == e), "mode masque : la longueur ne se voit pas");
}

static void t_echo_bornes() {
    d2kb::State s; std::memset(&s, 0, sizeof s); d2kb::open_kb(s, 0);
    for (int i = 0; i < 500; ++i) d2kb::echo_push(s, (char)('a' + (i % 26)));
    CHECK(s.echo_n == d2kb::ECHO_MAX, "echo_n=%d attendu %d", (int)s.echo_n, (int)d2kb::ECHO_MAX);
    CHECK(s.echo[d2kb::ECHO_MAX] == 0, "echo non termine par 0");
    for (int i = 0; i < 500; ++i) d2kb::echo_pop(s);
    CHECK(s.echo_n == 0, "echo_n=%d apres vidage", (int)s.echo_n);
    // etat corrompu (course d'affichage) : aucune ecriture hors du tampon
    for (int i = 0; i < 60; ++i) d2kb::echo_push(s, 'x');
    s.echo_n = 12345; d2kb::echo_push(s, 'y'); CHECK(s.echo_n >= 0 && s.echo_n <= d2kb::ECHO_MAX, "echo_n non borne apres corruption");
    s.echo_n = -7;    d2kb::echo_pop(s);       CHECK(s.echo_n == 0, "echo_n negatif non ramene a 0");
    // fermeture = effacement complet (le mot de passe ne reste pas en memoire)
    for (int i = 0; i < 20; ++i) d2kb::echo_push(s, 's');
    d2kb::close_kb(s);
    char zero[sizeof s.echo]; std::memset(zero, 0, sizeof zero);
    CHECK(std::memcmp((const void*)s.echo, zero, sizeof zero) == 0, "echo non efface a la fermeture");
    CHECK(s.mask == 0 && s.shift == 0, "modificateurs non remis a zero");
}

// Le dessin ne doit jamais lire hors du tampon quel que soit l'etat, meme
// incoherent (le fil de presentation lit un etat ecrit par le fil de jeu).
static void t_etat_incoherent() {
    d2kb::State s; std::memset(&s, 0, sizeof s); d2kb::open_kb(s, 0);
    auto fb = fb_new();
    const int rs[] = { -5, 0, 3, 4, 99 }, cs[] = { -3, 0, 11, 50 };
    for (int i = 0; i < 5; ++i) for (int j = 0; j < 4; ++j) {
        s.r = rs[i]; s.c = cs[j]; s.echo_n = (i * 7 + j) * 13 - 20; s.shift = i % 3; s.mask = j % 2;
        d2kb::draw(s, fb.data(), W, H);       // ne doit ni planter ni deborder (ASAN)
    }
    ++g_checks;
}

static void t_disposition_simple() {
    d2kb::State s; std::memset(&s, 0, sizeof s); d2kb::open_kb(s, 1);
    CHECK(std::strcmp(d2kb::layout(s).name, "simple") == 0, "la disposition SIMPLE demandee n'est pas rendue");
    const d2kb::Layout& L = d2kb::layout(s);
    // la disposition historique : 4x10 + 4 touches larges, majuscules seules
    CHECK(d2kb::cols(L, 0) == 10 && d2kb::cols(L, 4) == 4, "geometrie historique changee");
    char out = 0; d2kb::activate(s, 1, 0, &out); CHECK(out == 'A', "simple: (1,0) != A");
}

// --ppm <fichier> : rend une image du clavier (apercu visuel, pas un test).
static int dump_ppm(const char* path, int simple, int mask) {
    d2kb::State s; std::memset(&s, 0, sizeof s); d2kb::open_kb(s, simple);
    s.mask = mask; s.shift = 1; s.r = 2; s.c = 3;
    const char* demo = mask ? "motdepasse" : "Compte-D2 2026!";
    for (int i = 0; demo[i]; ++i) d2kb::echo_push(s, demo[i]);
    std::vector<uint32_t> fb((size_t)W * H, 0xFF203040u);
    d2kb::draw(s, fb.data(), W, H);
    FILE* f = std::fopen(path, "wb"); if (!f) return 1;
    std::fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (size_t i = 0; i < fb.size(); ++i) {
        const uint32_t v = fb[i];
        const unsigned char rgb[3] = { (unsigned char)(v & 0xFF), (unsigned char)((v >> 8) & 0xFF), (unsigned char)((v >> 16) & 0xFF) };
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f); return 0;
}

int main(int argc, char** argv) {
    if (argc >= 3 && !std::strcmp(argv[1], "--ppm"))
        return dump_ppm(argv[2], argc > 3 ? std::atoi(argv[3]) : 0, argc > 4 ? std::atoi(argv[4]) : 0);
    t_ferme_ne_dessine_rien();
    t_dessin_borne_au_panneau();
    t_couverture_ascii();
    t_activation();
    t_majuscule_trois_etats();
    t_navigation();
    t_tactile();
    t_masque();
    t_echo_bornes();
    t_etat_incoherent();
    t_disposition_simple();
    std::printf("%s: %d verifications, %d echecs\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
