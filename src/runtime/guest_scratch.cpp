// src/runtime/guest_scratch.cpp — voir guest_scratch.h.
//
// Corps repris de misc()/put_cstr de tools/rt_boot.cpp (d2vita), deplace ici
// le 2026-09-11 : l'allocateur est un primitif du moteur, pas du jeu. La
// logique d'allocation et de bornage est identique a l'octet pres ; seules
// changent la provenance des bornes (posees par l'embarqueur via
// wx86_scratch_init au lieu d'etre lues dans les globales du plan memoire de
// d2vita) et la sortie du diagnostic d'epuisement (rappel de l'embarqueur au
// lieu d'un appel direct a d2_crashlog, que le moteur ne connait pas).
#include "runtime/guest_scratch.h"
#include "runtime/cpu.h"
#include <cstring>

namespace {
uint32_t g_base = 0;
uint32_t g_cur  = 0;
uint32_t g_limit = 0;          // 0 = desarme
bool     g_warned = false;
Wx86ScratchOomFn g_oom = nullptr;
}

void wx86_scratch_init(uint32_t base, uint32_t size) {
    g_base = base;
    g_cur = base;
    g_limit = size ? base + size : 0;             // 0 = non borne (cf. en-tete)
    g_warned = false;
}

void wx86_scratch_set_limit(uint32_t limit) { g_limit = limit; }

void wx86_scratch_set_oom_handler(Wx86ScratchOomFn cb) { g_oom = cb; }

uint32_t wx86_scratch_alloc(uint32_t n) {
    const uint32_t sz = (n + 7) & ~7u;
    const uint32_t a = g_cur;
    // Le test de debordement (a+sz < a) compte autant que celui de la borne :
    // une demande absurde enroulerait l'addition et passerait la borne par le
    // bas. Borne absente (g_limit == 0) : on alloue sans verifier, exactement
    // comme le faisait misc() avant que sa borne ne soit posee.
    if (g_limit && (a + sz > g_limit || a + sz < a)) {
        if (!g_warned) {
            g_warned = true;
            if (g_oom) g_oom(a, sz, g_limit);
        }
        return 0;
    }
    g_cur += sz;
    return a;
}

uint32_t wx86_scratch_put_cstr(d2rt::Cpu& c, const char* s) {
    const uint32_t n = (uint32_t)std::strlen(s) + 1;
    const uint32_t a = wx86_scratch_alloc(n);
    if (!a) return 0;
    c.write(a, s, n);
    return a;
}

uint32_t wx86_scratch_used() { return g_cur - g_base; }
uint32_t wx86_scratch_size() { return g_limit ? g_limit - g_base : 0; }
uint32_t wx86_scratch_base() { return g_base; }
