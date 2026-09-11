// src/runtime/guest_str.cpp — voir guest_str.h. Transcription fidele des corps
// que les DEUX portages hebergeaient (identiques a la comparaison, 2026-09-11).
#include "guest_str.h"
#include "runtime/cpu.h"
#include <cstdlib>
#include <cstring>

using namespace d2rt;

std::string wx86_gread_mb(Cpu& c, uint32_t p, int len) {
    std::string s;
    if (len < 0) {
        // Chemin rapide : la boucle octet par octet paie un c.read() virtuel PAR
        // CARACTERE (wsprintf lit son format ainsi, ~79 k appels par run).
        // strnlen sur la vue hote traverse exactement les memes octets.
        //
        // ATTENTION : hostptr() ne valide qu'UN octet. La boucle lente lit via
        // c.read, bornee, qui rend zero hors arene et s'arrete donc au bord ;
        // un strnlen nu sortirait dans la memoire hote. On borne le balayage a
        // l'etendue reellement mappee. Desactive sous le garde-memoire pour que
        // sa detection de lecture non mappee continue de fonctionner.
        static const bool mg = std::getenv("WX86_MEMGUARD") || std::getenv("D2_MEMGUARD");
        if (!mg) {
            uint32_t span = 0x10000; const char* hp = nullptr;
            while (span && !(hp = (const char*)c.hostptr(p, span))) span >>= 1;
            if (hp) {
                size_t n = strnlen(hp, span);
                if (n < span) { s.assign(hp, n); return s; }   // zero trouve dans l'etendue sure
            }
        }
        for (;;) { uint8_t b = 0; c.read(p++, &b, 1); if (!b) break; s.push_back((char)b); }
    } else {
        s.resize(len);
        if (len) c.read(p, &s[0], len);
    }
    return s;
}

std::vector<uint16_t> wx86_gread_wc(Cpu& c, uint32_t p, int len) {
    std::vector<uint16_t> s;
    if (len < 0) {
        for (;;) { uint16_t w = 0; c.read(p, &w, 2); p += 2; if (!w) break; s.push_back(w); }
    } else {
        s.resize(len);
        for (int i = 0; i < len; i++) { uint16_t w = 0; c.read(p + i * 2, &w, 2); s[i] = w; }
    }
    return s;
}

void wx86_gwrite_wc(Cpu& c, uint32_t p, uint16_t w) { c.write(p, &w, 2); }
