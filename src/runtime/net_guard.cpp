// src/runtime/net_guard.cpp — voir net_guard.h. Corps deplaces depuis
// win32_shims_wsock32.cpp le 2026-09-12, sans changement de comportement :
// memes valeurs par defaut, meme table d'adresses privees, memes compteurs
// incrementes aux memes endroits.
#include "runtime/net_guard.h"

static bool               g_privOnly = false;
static unsigned long long g_refused = 0, g_allowed = 0;

void wx86_net_set_private_only(bool on) { g_privOnly = on; }
bool wx86_net_private_only() { return g_privOnly; }
unsigned long long wx86_net_refused() { return g_refused; }
unsigned long long wx86_net_allowed() { return g_allowed; }

// « Cette adresse peut-elle atteindre l'internet public ? » — une question de
// la couche socket, sans un mot sur le protocole ou le produit qui tourne
// dessus. ip est en ordre RESEAU, tel qu'il est dans le sockaddr.
bool wx86_net_addr_is_private(uint32_t ip_be) {
    const uint8_t a = (uint8_t)(ip_be & 0xff), b = (uint8_t)((ip_be >> 8) & 0xff);
    if (a == 127) return true;                       // boucle locale
    if (ip_be == 0) return true;                     // non specifie
    if (ip_be == 0xFFFFFFFFu) return true;           // diffusion LAN
    if (a == 10) return true;                        // 10/8
    if (a == 172 && b >= 16 && b <= 31) return true; // 172.16/12
    if (a == 192 && b == 168) return true;           // 192.168/16
    if (a == 169 && b == 254) return true;           // 169.254/16 lien-local
    if (a >= 224 && a <= 239) return true;           // multicast
    return false;
}

bool wx86_net_addr_allowed(uint32_t ip_be) {
    if (!g_privOnly || wx86_net_addr_is_private(ip_be)) { g_allowed++; return true; }
    g_refused++;
    return false;
}
