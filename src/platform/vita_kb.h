// src/platform/vita_kb.h — CLAVIER VIRTUEL COMPLET (dessine dans le tampon
// d'image). En-tete autonome : aucune dependance au VitaSDK, aucun appel
// systeme, aucun verrou, aucune allocation. C'est ce qui permet de le
// compiler et de le PROUVER sur l'hote (tools/tests/kb_test.cpp) alors que le
// chemin d'affichage, lui, ne tourne que sur console.
//
// POURQUOI PAS LE CLAVIER SYSTEME (SceIme / sceCommonDialogUpdate) :
// voir docs/perf/clavier_20260906.md. En deux lignes : sceCommonDialogUpdate
// exige un sceGxm initialise, une SURFACE DE COULEUR sceGxm et un
// SceGxmSyncObject reellement fait avancer par sceGxmDisplayQueueAddEntry. Le
// chemin d'affichage par DEFAUT (vita_present.cpp) n'a NI sceGxm NI file
// d'affichage — il pose un memblock CDRAM avec sceDisplaySetFrameBuf — et
// c'est precisement le chemin du menu ou l'on tape un compte et un mot de
// passe. Le dialogue systeme y est donc structurellement impossible, et
// l'installer sur le seul mode ou son contrat tient (D2_GXMASYNC=1) ferait
// ecrire le GPU du dialogue et le CPU des incrustations dans LE MEME tampon
// sans ordre garanti.
//
// Modele : le tick d'ENTREE (fil de jeu) ecrit l'etat ; le fil de PRESENTATION
// le lit pour dessiner. Memes courses benignes que le compteur d'images
// (affichage seulement, jamais l'etat du jeu).
//
// SECURITE : l'echo local (ce que l'on vient de taper) sert a se relire ; en
// mode MASQUE il s'affiche en points. Il n'est JAMAIS passe a une fonction de
// journal, et il est remis a zero a la fermeture.
#ifndef D2VITA_VITA_KB_H
#define D2VITA_VITA_KB_H

#include <stdint.h>
#include <string.h>

namespace d2kb {

// --- police 8x16, ASCII 32..126 --------------------------------------------
// GENEREE par tools/kb_font_gen.py (DejaVu Sans Mono rendue puis seuillee,
// plus quelques retouches listees dans le generateur). Ne pas editer ici :
// regenerer.
#include "vita_kb_font.h"

inline const unsigned char* glyph(char ch) {
    const unsigned c = (unsigned char)ch;
    if (c < 32u || c > 126u) return 0;
    return g_kb_font95[c - 32u];
}

// --- actions ----------------------------------------------------------------
enum Act { ACT_NONE = 0, ACT_CHAR, ACT_SPACE, ACT_BACK, ACT_ENTER,
           ACT_SHIFT, ACT_MASK, ACT_CLOSE };

// --- disposition ------------------------------------------------------------
// Les quatre rangees de caracteres couvrent l'ASCII IMPRIMABLE ENTIER
// (32..126) : 94 signes en 47 cellules x 2 (minuscule / majuscule), plus
// l'espace qui a sa propre touche. Le test d'hote le verifie caractere par
// caractere — c'est la propriete qui manquait a l'ancien clavier (39 signes).
struct Layout {
    const char*   lo[4];      // face des touches, MAJ relachee
    const char*   up[4];      // face des touches, MAJ engagee
    int           nrows;      // rangees de caracteres
    const char*   fn[8];      // libelles de la rangee de fonctions
    unsigned char fnact[8];   // action de chaque touche de fonction
    int           nfn;
    int           echo;       // 1 = ligne d'echo local
    const char*   name;
};

static const Layout LAY_FULL = {
    { "1234567890-=", "qwertyuiop[]", "asdfghjkl;'\\", "zxcvbnm,./`" },
    { "!@#$%^&*()_+", "QWERTYUIOP{}", "ASDFGHJKL:\"|", "ZXCVBNM<>?~" },
    4,
    { "maj", "espace", "effacer", "entree", "masque", "fermer" },
    { ACT_SHIFT, ACT_SPACE, ACT_BACK, ACT_ENTER, ACT_MASK, ACT_CLOSE },
    6, 1, "complet"
};

// Disposition HISTORIQUE (celle d'avant le 06/09), gardee sous D2_KBSIMPLE=1 :
// c'est le retour en arriere SANS changer de VPK. Meme moteur, meme police.
static const Layout LAY_SIMPLE = {
    { "1234567890", "ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ-_. " },
    { "1234567890", "ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ-_. " },
    4,
    { "ESPACE", "EFFACER", "ENTREE", "FERMER" },
    { ACT_SPACE, ACT_BACK, ACT_ENTER, ACT_CLOSE },
    4, 0, "simple"
};

// --- geometrie --------------------------------------------------------------
enum { ROW_H = 40, CELL_PAD = 3, ECHO_H = 34, PAD = 6, ECHO_MAX = 40 };

struct State {
    volatile int open;
    volatile int r, c;
    volatile int shift;      // 0 = aucune, 1 = une seule touche, 2 = verrouillee
    volatile int mask;       // 1 = echo en points (mot de passe)
    volatile int simple;     // 1 = disposition historique
    volatile int echo_n;
    char         echo[ECHO_MAX + 1];   // ECHO LOCAL — JAMAIS JOURNALISE
};

inline const Layout& layout(const State& s) { return s.simple ? LAY_SIMPLE : LAY_FULL; }

inline int cols(const Layout& L, int r) {
    if (r < 0 || r > L.nrows) return 0;
    return (r < L.nrows) ? (int)strlen(L.lo[r]) : L.nfn;
}
inline int nrows_total(const Layout& L) { return L.nrows + 1; }

inline int panel_h(const Layout& L) {
    return PAD + (L.echo ? (ECHO_H + PAD) : 0) + nrows_total(L) * ROW_H + PAD;
}
inline int panel_y0(const Layout& L, int scr_h) { return scr_h - panel_h(L); }

inline void cell_rect(const Layout& L, int r, int c, int scr_w, int scr_h,
                      int* x, int* y, int* w, int* h) {
    const int n = cols(L, r) > 0 ? cols(L, r) : 1;
    const int cw = scr_w / n;
    *x = c * cw + CELL_PAD;
    *w = cw - 2 * CELL_PAD;
    *y = panel_y0(L, scr_h) + PAD + (L.echo ? (ECHO_H + PAD) : 0) + r * ROW_H;
    *h = ROW_H - 4;
}

// Face affichee (et emise) par une cellule de caractere ; 0 pour la rangee de
// fonctions ou une cellule hors table.
inline char face(const Layout& L, int r, int c, int shift) {
    if (r < 0 || r >= L.nrows) return 0;
    const char* row = shift ? L.up[r] : L.lo[r];
    const int n = (int)strlen(L.lo[r]);
    if (c < 0 || c >= n) return 0;
    if ((int)strlen(row) != n) return 0;      // tables desaccordees : rien plutot que faux
    return row[c];
}

// --- echo local -------------------------------------------------------------
inline void echo_clear(State& s) { memset(s.echo, 0, sizeof s.echo); s.echo_n = 0; }
inline void echo_push(State& s, char ch) {
    int n = s.echo_n;
    if (n < 0 || n > ECHO_MAX) n = 0;
    if (n == ECHO_MAX) {                        // fenetre glissante des 40 derniers
        memmove(s.echo, s.echo + 1, ECHO_MAX - 1);
        n = ECHO_MAX - 1;
    }
    s.echo[n] = ch; s.echo[n + 1] = 0; s.echo_n = n + 1;
}
inline void echo_pop(State& s) {
    int n = s.echo_n;
    if (n <= 0 || n > ECHO_MAX) { echo_clear(s); return; }
    s.echo[n - 1] = 0; s.echo_n = n - 1;
}

// --- ouverture / fermeture --------------------------------------------------
inline void open_kb(State& s, int simple) {
    s.simple = simple ? 1 : 0;
    s.r = 1; s.c = 0; s.shift = 0; s.mask = 0;
    echo_clear(s);
    s.open = 1;
}
inline void close_kb(State& s) {
    s.open = 0;
    echo_clear(s);                 // le mot de passe ne survit pas a la fermeture
    s.shift = 0; s.mask = 0;
}

// --- navigation -------------------------------------------------------------
// Le changement de rangee REPROJETTE la colonne : les rangees n'ont pas le
// meme nombre de touches (12, 11, 6) et un simple clamp collerait la selection
// a droite. Proportionnel = la touche sous le pouce reste sous le pouce.
inline void nav(State& s, int dr, int dc) {
    const Layout& L = layout(s);
    int r = s.r, c = s.c;
    const int nr = nrows_total(L);
    if (r < 0 || r >= nr) r = 0;
    if (dr) {
        const int oldn = cols(L, r) > 0 ? cols(L, r) : 1;
        const int nr2 = ((r + dr) % nr + nr) % nr;
        const int newn = cols(L, nr2) > 0 ? cols(L, nr2) : 1;
        c = (c * newn + oldn / 2) / oldn;
        if (c >= newn) c = newn - 1;
        if (c < 0) c = 0;
        r = nr2;
    }
    if (dc) {
        const int n = cols(L, r) > 0 ? cols(L, r) : 1;
        c = ((c + dc) % n + n) % n;
    }
    s.r = r; s.c = c;
}

// --- activation d'une cellule ----------------------------------------------
// PURE au sens ou elle ne fait qu'ecrire l'etat du clavier et RENDRE l'action :
// c'est l'appelant (vita_present.cpp) qui injecte dans le jeu. Le fil de jeu
// ne bloque donc jamais ici.
inline Act activate(State& s, int r, int c, char* out_ch) {
    const Layout& L = layout(s);
    if (out_ch) *out_ch = 0;
    if (r < 0 || r >= nrows_total(L)) return ACT_NONE;
    if (c < 0 || c >= cols(L, r)) return ACT_NONE;
    if (r < L.nrows) {
        const char ch = face(L, r, c, s.shift != 0);
        if (!ch || ch == ' ') return ACT_NONE;
        if (s.shift == 1) s.shift = 0;          // MAJ « une seule touche »
        echo_push(s, ch);
        if (out_ch) *out_ch = ch;
        return ACT_CHAR;
    }
    switch (L.fnact[c]) {
        case ACT_SHIFT: s.shift = (s.shift + 1) % 3; return ACT_SHIFT;
        case ACT_MASK:  s.mask  = !s.mask;           return ACT_MASK;
        case ACT_SPACE: echo_push(s, ' '); if (out_ch) *out_ch = ' '; return ACT_SPACE;
        case ACT_BACK:  echo_pop(s);                 return ACT_BACK;
        case ACT_ENTER: echo_clear(s);               return ACT_ENTER;
        case ACT_CLOSE: close_kb(s);                 return ACT_CLOSE;
        default: return ACT_NONE;
    }
}

// Bascule directe de la majuscule (bouton Carre), meme machine a trois etats.
inline void shift_cycle(State& s) { s.shift = (s.shift + 1) % 3; }
inline void mask_toggle(State& s) { s.mask = !s.mask; }

// --- test de contact --------------------------------------------------------
// Coordonnees ECRAN (960x544). Rend 1 et remplit (r,c) si le point tombe dans
// une touche.
inline int hit(const Layout& L, int scr_w, int scr_h, int px, int py, int* r, int* c) {
    for (int rr = 0; rr < nrows_total(L); ++rr)
        for (int cc = 0; cc < cols(L, rr); ++cc) {
            int x, y, w, h; cell_rect(L, rr, cc, scr_w, scr_h, &x, &y, &w, &h);
            if (px >= x && px < x + w && py >= y && py < y + h) {
                if (r) *r = rr;
                if (c) *c = cc;
                return 1;
            }
        }
    return 0;
}

// --- dessin -----------------------------------------------------------------
namespace draw_detail {
// ⚠️ Le tampon est en A8B8G8R8 : l'octet de POIDS FAIBLE est le ROUGE. Ecrire
// 0xFF2060A0 en pensant « bleu » donne de l'orange a l'ecran (c'etait le cas de
// l'ancien clavier). D'ou ce constructeur explicite.
inline constexpr uint32_t rgb(uint32_t r, uint32_t g, uint32_t b) {
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}
inline void px(uint32_t* fb, int W, int H, int x, int y, uint32_t v) {
    if (x >= 0 && x < W && y >= 0 && y < H) fb[(size_t)y * W + x] = v;
}
inline void rect(uint32_t* fb, int W, int H, int x, int y, int w, int h, uint32_t v) {
    for (int yy = y; yy < y + h; ++yy)
        for (int xx = x; xx < x + w; ++xx) px(fb, W, H, xx, yy, v);
}
inline void frame(uint32_t* fb, int W, int H, int x, int y, int w, int h, uint32_t v) {
    for (int xx = x; xx < x + w; ++xx) { px(fb, W, H, xx, y, v); px(fb, W, H, xx, y + h - 1, v); }
    for (int yy = y; yy < y + h; ++yy) { px(fb, W, H, x, yy, v); px(fb, W, H, x + w - 1, yy, v); }
}
inline void ch(uint32_t* fb, int W, int H, char c, int x, int y, int sc, uint32_t v) {
    const unsigned char* g = glyph(c);
    if (!g) return;
    for (int gy = 0; gy < 16; ++gy)
        for (int gx = 0; gx < 8; ++gx)
            if (g[gy] & (0x80 >> gx))
                for (int sy = 0; sy < sc; ++sy)
                    for (int sx = 0; sx < sc; ++sx)
                        px(fb, W, H, x + gx * sc + sx, y + gy * sc + sy, v);
}
inline void text(uint32_t* fb, int W, int H, const char* t, int x, int y, int sc, uint32_t v) {
    for (int i = 0; t[i]; ++i) ch(fb, W, H, t[i], x + i * 8 * sc, y, sc, v);
}
inline void dot(uint32_t* fb, int W, int H, int x, int y, uint32_t v) {   // point du mode masque
    static const unsigned char d[8] = { 0x3c, 0x7e, 0xff, 0xff, 0xff, 0xff, 0x7e, 0x3c };
    for (int gy = 0; gy < 8; ++gy)
        for (int gx = 0; gx < 8; ++gx)
            if (d[gy] & (0x80 >> gx))
                for (int sy = 0; sy < 2; ++sy)
                    for (int sx = 0; sx < 2; ++sx)
                        px(fb, W, H, x + gx * 2 + sx, y + gy * 2 + sy, v);
}
} // namespace draw_detail

// Ne dessine RIEN si le clavier est ferme — la condition est ici ET chez
// l'appelant : un clavier ferme qui laisse une trace a l'ecran serait un
// defaut d'affichage permanent.
inline void draw(const State& s, uint32_t* fb, int scr_w, int scr_h) {
    using namespace draw_detail;
    if (!fb || !s.open) return;
    const Layout& L = layout(s);
    const int y0 = panel_y0(L, scr_h);
    if (y0 < 0) return;
    rect(fb, scr_w, scr_h, 0, y0, scr_w, scr_h - y0, rgb(0x18,0x18,0x18));

    if (L.echo) {
        const int ex = 8, ey = y0 + PAD, ew = scr_w - 16, eh = ECHO_H;
        rect(fb, scr_w, scr_h, ex, ey, ew, eh, rgb(0x0C,0x0C,0x0C));
        frame(fb, scr_w, scr_h, ex, ey, ew, eh, rgb(0x50,0x50,0x50));
        int n = s.echo_n; if (n < 0) n = 0; if (n > ECHO_MAX) n = ECHO_MAX;
        for (int i = 0; i < n; ++i) {
            const int cx = ex + 6 + i * 16, cy = ey + (eh - 32) / 2;
            if (s.mask) dot(fb, scr_w, scr_h, cx, cy + 8, rgb(0xC0,0xC0,0xC0));
            else        ch(fb, scr_w, scr_h, s.echo[i], cx, cy, 2, rgb(0xE0,0xE0,0xE0));
        }
        // caret
        rect(fb, scr_w, scr_h, ex + 6 + n * 16, ey + 6, 2, eh - 12, rgb(0xFF,0xC0,0x40));
    }

    for (int r = 0; r < nrows_total(L); ++r) {
        const int nc = cols(L, r);
        for (int c = 0; c < nc; ++c) {
            int x, y, w, h; cell_rect(L, r, c, scr_w, scr_h, &x, &y, &w, &h);
            const bool sel = (r == s.r && c == s.c);
            const bool fnrow = (r >= L.nrows);
            bool armed = false;
            if (fnrow) {
                if (L.fnact[c] == ACT_SHIFT && s.shift) armed = true;
                if (L.fnact[c] == ACT_MASK  && s.mask)  armed = true;
            }
            uint32_t fill = fnrow ? rgb(0x26,0x26,0x26) : rgb(0x30,0x30,0x30);
            if (armed) fill = rgb(0xC0,0x80,0x10);          // modificateur engage : ambre
            if (sel)   fill = rgb(0x20,0x70,0xC0);          // selection : bleu
            rect(fb, scr_w, scr_h, x, y, w, h, fill);
            if (sel) frame(fb, scr_w, scr_h, x, y, w, h, rgb(0xFF,0xFF,0xFF));
            if (!fnrow) {
                const char f = face(L, r, c, s.shift != 0);
                if (f && f != ' ') ch(fb, scr_w, scr_h, f, x + (w - 16) / 2, y + (h - 32) / 2, 2, rgb(0xFF,0xFF,0xFF));
            } else {
                const char* t = L.fn[c];
                const int n = (int)strlen(t);
                text(fb, scr_w, scr_h, t, x + (w - n * 8) / 2, y + (h - 16) / 2, 1, rgb(0xFF,0xFF,0xFF));
                if (L.fnact[c] == ACT_SHIFT && s.shift == 2)     // MAJ verrouillee : barre
                    rect(fb, scr_w, scr_h, x + 6, y + h - 6, w - 12, 3, rgb(0xFF,0xFF,0xFF));
            }
        }
    }
}

} // namespace d2kb
#endif // D2VITA_VITA_KB_H
