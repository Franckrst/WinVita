// src/runtime/win32_shims_wsock32.cpp — see win32_shims_wsock32.h.
//
// First pass (2026-09-10) moved only argument-pure, stateless ordinals here
// (byte-order, WSAStartup/WSACleanup, gethostname) after an audit found the
// rest "entangled" — but entangled turned out to mean "the socket table and
// a couple of generic host-syscall wrappers happened to live inside
// d2vita's tools/rt_boot.cpp", not "genuinely D2/BNCS-specific". Pushed
// further (2026-09-11, explicit direction: the network stack itself is
// generic, only the BNCS application protocol on top of it is D2-specific):
// the socket-handle table, the networking-enabled gate, and every ordinal
// that is a real Winsock->POSIX passthrough with NO protocol literal now
// live here too — accept, bind, closesocket, getpeername, getsockname,
// getsockopt, ioctlsocket (both DLLs' ordinal), listen, setsockopt,
// shutdown, socket — plus wx86_connect_wait/wx86_recv_blocking/
// wx86_send_simple, generic helper bodies that d2vita's connect/recv/send
// shims (kept d2vita-side, see below) now call into instead of duplicating.
//
// Troisieme passe (2026-09-11) — la couche est maintenant generique de bout
// en bout pour connect/recv/send. Ce qui restait cote d2vita n'etait pas la
// mecanique reseau mais la POLITIQUE posee autour : ports du protocole
// applicatif, redirection de mise au point, bascules d'horloge propres au
// consommateur, instrumentation. Deux points d'extension
// generiques suffisent a la rendre au consommateur :
//   - wx86_net_set_observer() : UN observateur passif qui recoit connect /
//     connect-done / send / recv. Le moteur raconte, il ne demande jamais
//     d'avis et ne change rien selon la reponse. Le decodage de protocole et
//     toute politique applicative vivent chez le consommateur.
//   - wx86_net_set_redirect() : une route generique de connect, l'equivalent
//     d'une entree de fichier hosts ou d'un mandataire sortant. Elle ne sait
//     rien du protocole qui passe dessus.
// Note pour qui branche un client sur un serveur prive : cette route est un
// FILET, pas la bonne facon de faire. La facon fidele au PC est de configurer
// la liste de serveurs du client lui-meme (ses cles de registre / son .ini)
// pour qu'il demande votre hote d'entree de jeu. Mesure le 2026-09-11 sur le
// consommateur de reference : liste de serveurs du registre ramenee a une
// entree locale, AUCUNE redirection activee -> le client se connecte tout seul
// au serveur local et son transfert de fichier se deroule normalement.
//
// Cinquieme passe (2026-09-12) — sendto (#20) et recvfrom (#17) rejoignent la
// table : ils n'avaient JAMAIS ete inscrits, ni ici ni chez le consommateur, et
// tombaient donc sur le shim par defaut (arret controle + derive ESP). Et avec
// eux le VERROU DE SORTIE (wx86_net_set_private_only) : « cette adresse peut-
// elle atteindre l'internet public ? » est une question de la couche socket,
// au meme titre que la route ci-dessus, et pas une question de protocole. Le
// moteur fournit le mecanisme et reste MUET ; l'embarqueur l'arme, decide a
// quelles conditions il le leve, et journalise les refus via WX86_NET_REFUSED.
//
// Ce qui reste cote consommateur, avec une vraie raison a chaque fois :
//   - select (WSOCK32.dll!#18) : son coeur (traduction fd_set <-> pollfd) est
//     generique, mais c'est aussi le site exact d'une famine reseau reelle
//     deja corrigee (2026-08-30, D2_SELECTBLOCK) dont la signature de
//     regression ne se voit que sous charge reseau reelle. Non touche.
// Quatrieme passe (2026-09-11) — inet_addr/inet_ntoa/gethostbyname rejoignent
// le moteur. Ce qui les retenait n'etait pas leur contenu (aucun des trois ne
// connait D2) mais le fait qu'ils ecrivent dans la memoire INVITEE, via un
// allocateur de brouillon qui vivait chez le consommateur. Cet allocateur est
// devenu un primitif du moteur (guest_scratch.h) et la raison est tombee.
// Deux defauts reels corriges au passage, pas seulement deplaces :
//   - inet_ntoa rendait une chaine "127.0.0.1" ecrite en dur sur l'ordinal
//     WSOCK32, quel que soit l'argument (un bouchon, pas une implementation) ;
//   - gethostbyname faisait cinq allocations DEFINITIVES a chaque appel, ce
//     qui menait tout droit a l'epuisement de la plage en session longue.
//     Les deux sont maintenant caches par cle.
//
// Trace-log tradeoff, disclosed rather than hidden: d2vita's W() helper
// wraps every WSOCK32/WS2_32 ordinal it registers with a uniform
// g_netwatch_on() call trace. Ordinals moved here register directly via
// this file's own REGORD and no longer go through that wrapper, so their
// entries disappear from that specific trace (accept/bind/closesocket/
// getpeername/getsockname/getsockopt/setsockopt/shutdown/socket/
// ioctlsocket). Functional behavior is unchanged; only that one verbose
// diagnostic loses coverage for these ordinals. connect/recv/send/select
// (unchanged, still W()-registered) keep full tracing.
#include "win32_shims_wsock32.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/poll_gil.h"
#include "runtime/net_nonblock.h"
#include "runtime/guest_scratch.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <map>
#include <vector>
#include <cerrno>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0   // absent from the VitaSDK newlib; harmless without SIGPIPE
#endif
using namespace d2rt;

// --- socket-handle table --------------------------------------------------
namespace {
std::map<uint32_t, WsockHandle> g_wsockTable;
uint32_t g_wsockNext = 1;
bool g_netEnabled = false;
uint32_t g_wsaLastErr = 0;
}

uint32_t wx86_sock_alloc(int fd) {
    uint32_t h = g_wsockNext++;
    g_wsockTable[h] = WsockHandle{fd, false, 0};
    return h;
}
bool wx86_sock_get(uint32_t h, WsockHandle& out) {
    auto it = g_wsockTable.find(h);
    if (it == g_wsockTable.end()) return false;
    out = it->second; return true;
}
int wx86_sock_fd(uint32_t h) {
    auto it = g_wsockTable.find(h);
    return it == g_wsockTable.end() ? -1 : it->second.fd;
}
uint16_t wx86_sock_port(uint32_t h) {
    auto it = g_wsockTable.find(h);
    return it == g_wsockTable.end() ? 0 : it->second.port;
}
void wx86_sock_set_port(uint32_t h, uint16_t port) {
    auto it = g_wsockTable.find(h);
    if (it != g_wsockTable.end()) it->second.port = port;
}
bool wx86_sock_nonblock(uint32_t h) {
    auto it = g_wsockTable.find(h);
    return it == g_wsockTable.end() ? false : it->second.nonblock;
}
void wx86_sock_set_nonblock(uint32_t h, bool nb) {
    auto it = g_wsockTable.find(h);
    if (it != g_wsockTable.end()) it->second.nonblock = nb;
}
void wx86_sock_erase(uint32_t h) { g_wsockTable.erase(h); }

bool wx86_net_enabled() { return g_netEnabled; }
void wx86_net_set_enabled(bool on) { g_netEnabled = on; }

static uint32_t        g_routeIp = 0;
static WsockObserverFn g_observer = nullptr;
static bool            g_privOnly = false;
static unsigned long long g_refused = 0, g_allowed = 0;

void wx86_net_set_redirect(uint32_t ip) { g_routeIp = ip; }
uint32_t wx86_net_redirect() { return g_routeIp; }
void wx86_net_set_observer(WsockObserverFn cb) { g_observer = cb; }

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

// Lecture d'une chaine C invitee, octet par octet — les shims d'adresse
// recoivent un char* invite. Bornee (1 Kio) : un pointeur errant ne doit pas
// faire boucler le moteur sur toute la memoire invitee a la recherche d'un
// zero qui n'existe pas.
static std::string wx86_guest_cstr(Cpu& c, uint32_t p) {
    std::string s;
    if (!p) return s;
    for (uint32_t i = 0; i < 1024; i++) {
        uint8_t ch = 0;
        c.read(p + i, &ch, 1);
        if (!ch) break;
        s.push_back((char)ch);
    }
    return s;
}

// Single dispatch point. Zero cost when nobody observes.
static void wx86_net_notify(int kind, Cpu* c, uint32_t handle, int fd,
                            uint32_t ip, uint32_t routeIp, uint16_t port,
                            int result, uint32_t wsaErr,
                            const uint8_t* data, int len) {
    if (!g_observer) return;
    WsockEvent e{kind, c, handle, fd, ip, routeIp, port, result, wsaErr, data, len};
    g_observer(e);
}

// LE VERROU DE SORTIE, applique EN AVAL de la route : il ne voit que ce qui
// partirait REELLEMENT sur le fil. Rend true si la destination est autorisee.
// Un refus ne touche AUCUNE socket hote : pas de poignee TCP entamee, pas un
// octet emis. Le moteur ne journalise rien lui-meme (il est muet) — il le DIT
// par WX86_NET_REFUSED, et l'embarqueur ecrit ou il veut.
static bool wx86_net_allow(Cpu* c, uint32_t handle, int fd,
                           uint32_t ip_be, uint16_t port, int kindRefus) {
    if (!g_privOnly || wx86_net_addr_is_private(ip_be)) { g_allowed++; return true; }
    g_refused++;
    wx86_net_set_last_error(10013);   // WSAEACCES — le code Winsock d'un envoi interdit
    wx86_net_notify(WX86_NET_REFUSED, c, handle, fd, ip_be, ip_be, port,
                    kindRefus, 10013, nullptr, 0);
    return false;
}

uint32_t wx86_net_last_error() { return g_wsaLastErr; }
void wx86_net_set_last_error(uint32_t code) { g_wsaLastErr = code; }

uint32_t wx86_wsa_from_errno(int e) {
    switch (e) {
        case EWOULDBLOCK: return 10035; case EINPROGRESS: return 10036;
        case EALREADY: return 10037; case EISCONN: return 10056; case ECONNREFUSED: return 10061;
        case ETIMEDOUT: return 10060; case EHOSTUNREACH: return 10065; case ENETUNREACH: return 10051;
        default: return 10054; } // WSAECONNRESET as a catch-all
}

int wx86_connect_wait(int fd, int timeout_ms, uint32_t* outWsaErr) {
    pollfd pf{fd, POLLOUT, 0};
    int pr = wx86_poll_gilfree(&pf, timeout_ms);
    if (pr > 0) {
        int se = 0; socklen_t sl = sizeof se;
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &se, &sl);
        if (se == 0) return 0;
        uint32_t e = wx86_wsa_from_errno(se);
        wx86_net_set_last_error(e);
        if (outWsaErr) *outWsaErr = e;
        return -1;
    }
    wx86_net_set_last_error(10060);   // WSAETIMEDOUT
    if (outWsaErr) *outWsaErr = 10060;
    return -1;
}

int wx86_recv_blocking(int fd, void* buf, uint32_t len, bool blocking, int wait_ms, uint32_t* outWsaErr) {
    ssize_t n = ::recv(fd, buf, len, 0);
    if (n < 0 && blocking && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        pollfd pf{fd, POLLIN, 0};
        wx86_poll_gilfree(&pf, wait_ms);
        n = ::recv(fd, buf, len, 0);
    }
    if (n >= 0) return (int)n;
    { uint32_t e = wx86_wsa_from_errno(errno); wx86_net_set_last_error(e); if (outWsaErr) *outWsaErr = e; }
    return -1;
}

int wx86_send_simple(int fd, const void* buf, uint32_t len, uint32_t* outWsaErr) {
    ssize_t n = ::send(fd, buf, len, MSG_NOSIGNAL);
    if (n >= 0) return (int)n;
    { uint32_t e = wx86_wsa_from_errno(errno); wx86_net_set_last_error(e); if (outWsaErr) *outWsaErr = e; }
    return -1;
}

void win32_shims_wsock32_install(Bridge& br) {
    // NOTE for whoever edits this next: register EACH (dll, ordinal) pair via
    // its OWN direct `br.register_shim_ordinal(...)` call inside this
    // function's body. Do not factor the "same ordinal on both DLL names"
    // pattern into a second-level helper that itself calls a helper —
    // tools/shim_seq.py's static parser only resolves ONE level of local
    // lambda helper (it looks for a direct br.register_shim(_ordinal) call
    // inside a helper's own body); a helper that calls another helper
    // resolves to zero targets and every key it registers vanishes
    // SILENTLY from the safety-net table. Hit exactly this bug once while
    // writing this file (a `BOTH(ord,...)` wrapping `REGORD(dll,ord,...)`
    // twice) — caught only because shim_seq.sh's ENSEMBLE check is run
    // before every commit, not because the mistake was otherwise visible.
    auto REGORD = [&](const char* dll, uint32_t ord, uint32_t ac, std::function<uint32_t(Cpu&)> fn) {
        Shim s;
        s.argc = ac;
        s.stdcall_cleanup = true;
        s.tag = std::string(dll) + "!#" + std::to_string(ord);
        s.fn = std::move(fn);
        br.register_shim_ordinal(dll, ord, s);
    };

    // Byte order — pure bit manipulation, zero state.
    auto htons_fn = [](Cpu& c) { uint32_t v = c.arg(0) & 0xffff; return ((v >> 8) | (v << 8)) & 0xffff; };
    REGORD("WSOCK32.dll", 9, 1, htons_fn);
    REGORD("WS2_32.dll", 9, 1, htons_fn);
    auto ntohs_fn = htons_fn;  // same bit-swap, different ordinal (BSD historically shares the body)
    REGORD("WSOCK32.dll", 15, 1, ntohs_fn);
    REGORD("WS2_32.dll", 15, 1, ntohs_fn);
    auto htonl_fn = [](Cpu& c) { return __builtin_bswap32(c.arg(0)); };
    REGORD("WSOCK32.dll", 8, 1, htonl_fn);
    REGORD("WS2_32.dll", 8, 1, htonl_fn);
    auto ntohl_fn = htonl_fn;
    REGORD("WSOCK32.dll", 14, 1, ntohl_fn);
    REGORD("WS2_32.dll", 14, 1, ntohl_fn);

    // WSAStartup(wVersionRequested, WSADATA*) / WSACleanup(): fill/no-op on a
    // caller-supplied buffer. Wrong argc here once sent Fog's init executing
    // .rdata (a stdcall ESP-balance bug, not a networking bug) — keep argc
    // exact if this is ever touched again.
    auto wsastartup_fn = [](Cpu& c) {
        uint32_t p = c.arg(1);
        if (p) {
            std::vector<uint8_t> z(400, 0);
            c.write(p, z.data(), 400);
            uint16_t v = (uint16_t)(c.arg(0) & 0xffff);
            c.write(p, &v, 2);
            c.write(p + 2, &v, 2);
        }
        return 0u;
    };
    REGORD("WSOCK32.dll", 115, 2, wsastartup_fn);
    REGORD("WS2_32.dll", 115, 2, wsastartup_fn);
    auto wsacleanup_fn = [](Cpu&) { return 0u; };
    REGORD("WSOCK32.dll", 116, 0, wsacleanup_fn);
    REGORD("WS2_32.dll", 116, 0, wsacleanup_fn);

    auto wsaasyncselect_fn = [](Cpu&) { return 0u; };  // WSAAsyncSelect(s,hWnd,wMsg,lEvent) — stub
    REGORD("WSOCK32.dll", 101, 4, wsaasyncselect_fn);
    REGORD("WS2_32.dll", 101, 4, wsaasyncselect_fn);
    auto wsaisblocking_fn = [](Cpu&) { return 0u; };  // WSAIsBlocking -> FALSE
    REGORD("WSOCK32.dll", 112, 0, wsaisblocking_fn);
    REGORD("WS2_32.dll", 112, 0, wsaisblocking_fn);

    // gethostname: "vita" is the winx86 execution PLATFORM (Vita), not a D2
    // literal — same category as vita_kb.h/vita_audio.cpp already living here.
    auto gethostname_fn = [](Cpu& c) {
        uint32_t b = c.arg(0);
        if (b) c.write(b, "vita", 5);
        return 0u;
    };
    REGORD("WSOCK32.dll", 57, 2, gethostname_fn);
    REGORD("WS2_32.dll", 57, 2, gethostname_fn);

    // --- real generic socket primitives (2026-09-11) ------------------
    auto closesocket_fn = [](Cpu& c) -> uint32_t {
        if (!wx86_net_enabled()) return 0u;
        int fd = wx86_sock_fd(c.arg(0));
        if (fd >= 0) { ::close(fd); wx86_sock_erase(c.arg(0)); }
        return 0u;
    };
    REGORD("WSOCK32.dll", 3, 1, closesocket_fn);
    REGORD("WS2_32.dll", 3, 1, closesocket_fn);

    auto getpeername_fn = [](Cpu& c) -> uint32_t {
        if (!wx86_net_enabled()) return 0xFFFFFFFFu;
        int fd = wx86_sock_fd(c.arg(0));
        if (fd < 0) { wx86_net_set_last_error(10038); return 0xFFFFFFFFu; }
        uint8_t sa[16] = {0}; socklen_t sl = 16;
        if (::getpeername(fd, (sockaddr*)sa, &sl) < 0) { wx86_net_set_last_error(wx86_wsa_from_errno(errno)); return 0xFFFFFFFFu; }
        if (c.arg(1)) c.write(c.arg(1), sa, 16);
        if (c.arg(2)) c.write_u32(c.arg(2), 16);
        return 0u;
    };
    REGORD("WSOCK32.dll", 5, 3, getpeername_fn);
    REGORD("WS2_32.dll", 5, 3, getpeername_fn);

    auto getsockname_fn = [](Cpu& c) -> uint32_t {
        if (!wx86_net_enabled()) return 0xFFFFFFFFu;
        int fd = wx86_sock_fd(c.arg(0));
        if (fd < 0) { wx86_net_set_last_error(10038); return 0xFFFFFFFFu; }
        uint8_t sa[16] = {0}; socklen_t sl = 16;
        if (::getsockname(fd, (sockaddr*)sa, &sl) < 0) { wx86_net_set_last_error(wx86_wsa_from_errno(errno)); return 0xFFFFFFFFu; }
        if (c.arg(1)) c.write(c.arg(1), sa, 16);
        if (c.arg(2)) c.write_u32(c.arg(2), 16);
        return 0u;
    };
    REGORD("WSOCK32.dll", 6, 3, getsockname_fn);
    REGORD("WS2_32.dll", 6, 3, getsockname_fn);

    // getsockopt(s,level,optname,optval*,optlen*): the generic passthrough.
    // D2 polls SO_ERROR after a non-blocking connect signals writable —
    // that USE is d2vita's connect flow, but the shim body itself is a
    // plain Winsock->POSIX option translation, nothing D2-specific in it.
    auto getsockopt_fn = [](Cpu& c) -> uint32_t {
        if (!wx86_net_enabled()) return 0xFFFFFFFFu;
        int fd = wx86_sock_fd(c.arg(0));
        if (fd < 0) { wx86_net_set_last_error(10038); return 0xFFFFFFFFu; }
        int level = (int)c.arg(1) == 0xffff ? SOL_SOCKET : (int)c.arg(1);
        int opt = (int)c.arg(2);
        if (c.arg(1) == 0xffff && opt == 0x1007) opt = SO_ERROR;
        else if (c.arg(1) == 0xffff && opt == 0x1008) opt = SO_TYPE;
        uint8_t buf[64] = {0}; socklen_t bl = c.arg(4) ? c.read_u32(c.arg(4)) : 4; if (bl > 64) bl = 64;
        if (::getsockopt(fd, level, opt, buf, &bl) < 0) { wx86_net_set_last_error(wx86_wsa_from_errno(errno)); return 0xFFFFFFFFu; }
        if (c.arg(1) == 0xffff && (int)c.arg(2) == 0x1007 && bl >= 4) {
            int he; std::memcpy(&he, buf, 4);
            uint32_t w = he ? wx86_wsa_from_errno(he) : 0u;
            std::memcpy(buf, &w, 4);
        }
        if (c.arg(3)) c.write(c.arg(3), buf, bl);
        if (c.arg(4)) c.write_u32(c.arg(4), bl);
        return 0u;
    };
    REGORD("WSOCK32.dll", 7, 5, getsockopt_fn);
    REGORD("WS2_32.dll", 7, 5, getsockopt_fn);

    auto setsockopt_fn = [](Cpu& c) -> uint32_t {
        if (!wx86_net_enabled()) return 0xFFFFFFFFu;
        int fd = wx86_sock_fd(c.arg(0));
        if (fd < 0) { wx86_net_set_last_error(10038); return 0xFFFFFFFFu; }
        int lvl = (int)c.arg(1), opt = (int)c.arg(2);
        uint32_t vp = c.arg(3), vl = c.arg(4);
        std::vector<uint8_t> v(vl ? vl : 4, 0);
        if (vp && vl) c.read(vp, v.data(), vl);
        if (lvl == 0xffff && opt == 0x0008) {   // SO_KEEPALIVE
            int on = v.size() >= 4 ? *(int*)v.data() : 1;
            ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);
        } else if (lvl == 6 && opt == 1) {      // TCP_NODELAY
            int on = v.size() >= 4 ? *(int*)v.data() : 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
        }
        return 0u;
    };
    REGORD("WSOCK32.dll", 21, 5, setsockopt_fn);
    REGORD("WS2_32.dll", 21, 5, setsockopt_fn);

    auto shutdown_fn = [](Cpu& c) -> uint32_t {
        if (!wx86_net_enabled()) return 0u;
        int fd = wx86_sock_fd(c.arg(0));
        if (fd >= 0) ::shutdown(fd, (int)c.arg(1));
        return 0u;
    };
    REGORD("WSOCK32.dll", 22, 2, shutdown_fn);
    REGORD("WS2_32.dll", 22, 2, shutdown_fn);

    // ioctlsocket(s,cmd,u_long*): only FIONBIO is meaningful here (guest
    // blocking-mode flag; the host fd stays non-blocking regardless, see
    // wx86_recv_blocking). WSOCK32 and WS2_32 disagree on the ordinal
    // (#12 vs #10) — same body, two explicit registrations.
    auto ioctlsocket_fn = [](Cpu& c) -> uint32_t {
        if (!wx86_net_enabled()) return 0xFFFFFFFFu;
        WsockHandle h;
        if (!wx86_sock_get(c.arg(0), h)) { wx86_net_set_last_error(10038); return 0xFFFFFFFFu; }
        if (c.arg(1) == 0x8004667eu) {   // FIONBIO
            uint32_t v = c.arg(2) ? c.read_u32(c.arg(2)) : 0;
            wx86_sock_set_nonblock(c.arg(0), v != 0);
        }
        return 0u;
    };
    REGORD("WSOCK32.dll", 12, 3, ioctlsocket_fn);
    REGORD("WS2_32.dll", 10, 3, ioctlsocket_fn);

    // --- address helpers: inet_addr / inet_ntoa / gethostbyname ------------
    // Ils rendent des donnees a l'invite, donc ils ont besoin d'ecrire dans
    // la memoire INVITEE : c'est ce qui les retenait chez le consommateur,
    // qui hebergeait l'allocateur de brouillon. Cet allocateur appartient
    // maintenant au moteur (guest_scratch.h), et la raison tombe : rien
    // dans ces trois fonctions ne connait D2 ni Battle.net.
    //
    // Les ordinaux des deux DLL ne coincident PAS ici, contrairement au
    // reste du fichier :
    //        WSOCK32 : 10=inet_addr 11=inet_ntoa 12=ioctlsocket
    //        WS2_32  : 10=ioctlsocket 11=inet_addr 12=inet_ntoa
    // D'ou quatre inscriptions explicites, chacune sur le BON ordinal. Le
    // consommateur plantait auparavant, par effet de bord de son helper a
    // double inscription, un inet_addr transitoire sur WS2_32!#10 et un
    // inet_ntoa transitoire sur WS2_32!#11, aussitot ecrases par la bonne
    // fonction. Ces deux inscriptions mortes disparaissent : la table
    // EFFECTIVE est inchangee (meme corps gagnant partout), seule la
    // multiplicite de ces deux cles retombe de 2 a 1.
    auto inet_addr_fn = [](Cpu& c) -> uint32_t {
        std::string s = wx86_guest_cstr(c, c.arg(0));
        in_addr a;
        if (!s.empty() && inet_aton(s.c_str(), &a)) return a.s_addr;
        return 0xFFFFFFFFu;   // INADDR_NONE
    };
    REGORD("WSOCK32.dll", 10, 1, inet_addr_fn);
    REGORD("WS2_32.dll", 11, 1, inet_addr_fn);

    // inet_ntoa(in_addr) -> char* : la chaine doit survivre au retour, donc
    // elle vit dans le brouillon invite. UNE entree de cache par adresse
    // distincte : sans cela chaque appel fuirait ~16 octets definitivement
    // (l'allocateur ne libere jamais — cf. le contrat de guest_scratch.h), et
    // un client qui formate une adresse a chaque image finirait par epuiser
    // la plage. Le consommateur rendait ici une chaine "127.0.0.1" ECRITE EN
    // DUR, quel que soit l'argument : c'etait un bouchon, pas une
    // implementation, et sur l'ordinal WSOCK32 uniquement — l'ordinal WS2
    // avait deja la vraie. Les deux sont desormais la vraie.
    auto inet_ntoa_fn = [](Cpu& c) -> uint32_t {
        static std::map<uint32_t, uint32_t> cache;   // addr reseau -> adresse invitee
        const uint32_t a = c.arg(0);
        auto it = cache.find(a);
        if (it != cache.end()) return it->second;
        char b[20];
        std::snprintf(b, sizeof b, "%u.%u.%u.%u",
                      a & 0xff, (a >> 8) & 0xff, (a >> 16) & 0xff, (a >> 24) & 0xff);
        const uint32_t p = wx86_scratch_put_cstr(c, b);
        if (p) cache[a] = p;
        return p;
    };
    REGORD("WSOCK32.dll", 11, 1, inet_ntoa_fn);
    REGORD("WS2_32.dll", 12, 1, inet_ntoa_fn);

    // gethostbyname(name) -> hostent* : meme histoire, en plus gros. La
    // structure rendue (hostent + liste d'adresses + liste d'alias + copie du
    // nom) faisait CINQ allocations definitives A CHAQUE APPEL chez le
    // consommateur. Un client qui re-resout son serveur a chaque tentative de
    // connexion marchait donc droit vers l'epuisement de la plage — c'est
    // exactement le defaut que le commentaire C6 de misc() signalait sans le
    // corriger. Corrige ici : une entree de cache par nom, la structure est
    // batie UNE fois. Le cache ne reflete volontairement pas les changements
    // DNS : ces structures Winsock ont de toute facon une duree de vie
    // processus, c'est ce que leur contrat autorise.
    auto gethostbyname_fn = [](Cpu& c) -> uint32_t {
        if (!wx86_net_enabled()) return 0u;
        const std::string host = wx86_guest_cstr(c, c.arg(0));
        static std::map<std::string, uint32_t> cache;
        auto it = cache.find(host);
        if (it != cache.end()) return it->second;
        const uint32_t addr = wx86_net_resolve(host.c_str());
        // Le moteur est muet : l'echec de resolution part a l'observateur,
        // pas dans un journal qu'il ne connait pas. C'est le premier echec
        // attendu sur une plateforme embarquee, il ne doit pas etre invisible.
        if (!addr) {
            wx86_net_set_last_error(11001);   // WSAHOST_NOT_FOUND
            wx86_net_notify(WX86_NET_RESOLVE, &c, 0, -1, 0, 0, 0, -1, 11001,
                            (const uint8_t*)host.c_str(), (int)host.size());
            return 0u;
        }
        // hostent { char* h_name; char** h_aliases; short h_addrtype;
        //           short h_length; char** h_addr_list; }
        const uint32_t namep = wx86_scratch_put_cstr(c, host.c_str());
        const uint32_t addrbuf = wx86_scratch_alloc(4);
        const uint32_t addrlist = wx86_scratch_alloc(8);
        const uint32_t aliases = wx86_scratch_alloc(4);
        const uint32_t he = wx86_scratch_alloc(16);
        if (!namep || !addrbuf || !addrlist || !aliases || !he) {
            wx86_net_set_last_error(11001);
            return 0u;   // brouillon epuise : echouer proprement, pas ecrire en 0
        }
        c.write_u32(addrbuf, addr);
        c.write_u32(addrlist, addrbuf);
        c.write_u32(addrlist + 4, 0);
        c.write_u32(aliases, 0);
        c.write_u32(he + 0, namep);
        c.write_u32(he + 4, aliases);
        uint16_t at = 2 /*AF_INET*/, ln = 4;
        c.write(he + 8, &at, 2);     // h_addrtype et h_length sont des SHORT
        c.write(he + 10, &ln, 2);    // accoles, pas deux mots de 32 bits
        c.write_u32(he + 12, addrlist);
        cache[host] = he;
        wx86_net_notify(WX86_NET_RESOLVE, &c, 0, -1, addr, addr, 0, 0, 0,
                        (const uint8_t*)host.c_str(), (int)host.size());
        return he;
    };
    REGORD("WSOCK32.dll", 52, 1, gethostbyname_fn);
    REGORD("WS2_32.dll", 52, 1, gethostbyname_fn);

    // socket(af,type,proto): real socket, host fd kept non-blocking
    // (protects the cooperative scheduler — see wx86_recv_blocking for how
    // guest-visible blocking mode is still honored).
    auto socket_fn = [](Cpu& c) -> uint32_t {
        if (!wx86_net_enabled()) return 0xFFFFFFFFu;
        int fd = ::socket(AF_INET, (int)c.arg(1) == 2 ? SOCK_DGRAM : SOCK_STREAM, 0);
        if (fd < 0) { wx86_net_set_last_error(wx86_wsa_from_errno(errno)); return 0xFFFFFFFFu; }
        wx86_net_set_nonblock(fd, true);
        return wx86_sock_alloc(fd);
    };
    REGORD("WSOCK32.dll", 23, 3, socket_fn);
    REGORD("WS2_32.dll", 23, 3, socket_fn);

    // connect(s, sockaddr*, len). Generic all the way through: read the
    // destination, apply the generic route override if one is set, report the
    // attempt to the observer, then do the host connect — completing a
    // non-blocking one synchronously, because a guest whose own timeout is
    // driven by an emulated clock can "time out" while the real TCP handshake
    // is still in flight. No port literal, no protocol knowledge: which ports
    // matter, and what to do when one of them connects, is the embedder's
    // business and reaches it through the observer.
    auto connect_fn = [](Cpu& c) -> uint32_t {
        if (!wx86_net_enabled()) return 0xFFFFFFFFu;
        int fd = wx86_sock_fd(c.arg(0));
        if (fd < 0) { wx86_net_set_last_error(10038); return 0xFFFFFFFFu; }
        uint8_t sa[16]; c.read(c.arg(1), sa, 16);
        uint32_t dip; std::memcpy(&dip, sa + 4, 4);
        uint16_t dport = (uint16_t)((sa[2] << 8) | sa[3]);
        wx86_sock_set_port(c.arg(0), dport);
        uint32_t rip = dip;
        if (g_routeIp) {
            // Loopback, "any" and broadcast are left alone: rewriting those
            // would break a guest talking to itself.
            uint8_t hi = dip & 0xff;
            if (hi != 127 && hi != 0 && dip != 0xffffffffu) {
                rip = g_routeIp;
                std::memcpy(sa + 4, &rip, 4);
                c.write(c.arg(1), sa, 16);
            }
        }
        // Verrou EN AVAL de la route : c'est la destination reellement composee
        // qui est jugee, et un refus n'a AUCUN effet de bord (aucun SYN ne part).
        if (!wx86_net_allow(&c, c.arg(0), fd, rip, dport, WX86_NET_CONNECT))
            return 0xFFFFFFFFu;
        wx86_net_notify(WX86_NET_CONNECT, &c, c.arg(0), fd, dip, rip, dport, 0, 0, nullptr, 0);
        int r = ::connect(fd, (sockaddr*)sa, (socklen_t)c.arg(2));
        if (r == 0) {
            wx86_net_notify(WX86_NET_CONNECT_DONE, &c, c.arg(0), fd, dip, rip, dport, 0, 0, nullptr, 0);
            return 0u;
        }
        if (errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EALREADY) {
            uint32_t we = 0;
            int rc = wx86_connect_wait(fd, 8000, &we);
            wx86_net_notify(WX86_NET_CONNECT_DONE, &c, c.arg(0), fd, dip, rip, dport,
                            rc == 0 ? 1 : -1, rc == 0 ? 0u : we, nullptr, 0);
            return rc == 0 ? 0u : 0xFFFFFFFFu;
        }
        uint32_t e = wx86_wsa_from_errno(errno);
        wx86_net_set_last_error(e);
        wx86_net_notify(WX86_NET_CONNECT_DONE, &c, c.arg(0), fd, dip, rip, dport, -1, e, nullptr, 0);
        return 0xFFFFFFFFu;
    };
    REGORD("WSOCK32.dll", 4, 3, connect_fn);
    REGORD("WS2_32.dll", 4, 3, connect_fn);

    auto recv_fn = [](Cpu& c) -> uint32_t {
        if (!wx86_net_enabled()) return 0xFFFFFFFFu;
        WsockHandle h;
        if (!wx86_sock_get(c.arg(0), h)) { wx86_net_set_last_error(10038); return 0xFFFFFFFFu; }
        std::vector<uint8_t> b(c.arg(2));
        uint32_t we = 0;
        int n = wx86_recv_blocking(h.fd, b.data(), (uint32_t)b.size(), !h.nonblock, 15000, &we);
        if (n > 0) {
            c.write(c.arg(1), b.data(), (uint32_t)n);
            wx86_net_notify(WX86_NET_RECV, &c, c.arg(0), h.fd, 0, 0, h.port, n, 0, b.data(), n);
            return (uint32_t)n;
        }
        wx86_net_notify(WX86_NET_RECV, &c, c.arg(0), h.fd, 0, 0, h.port,
                        n, n < 0 ? we : 0u, nullptr, 0);
        return n == 0 ? 0u : 0xFFFFFFFFu;
    };
    REGORD("WSOCK32.dll", 16, 4, recv_fn);
    REGORD("WS2_32.dll", 16, 4, recv_fn);

    auto send_fn = [](Cpu& c) -> uint32_t {
        if (!wx86_net_enabled()) return 0xFFFFFFFFu;
        int fd = wx86_sock_fd(c.arg(0));
        if (fd < 0) { wx86_net_set_last_error(10038); return 0xFFFFFFFFu; }
        std::vector<uint8_t> b(c.arg(2));
        if (c.arg(2)) c.read(c.arg(1), b.data(), c.arg(2));
        uint32_t we = 0;
        int n = wx86_send_simple(fd, b.data(), (uint32_t)b.size(), &we);
        wx86_net_notify(WX86_NET_SEND, &c, c.arg(0), fd, 0, 0, wx86_sock_port(c.arg(0)),
                        n, n < 0 ? we : 0u, b.data(), (int)b.size());
        return n >= 0 ? (uint32_t)n : 0xFFFFFFFFu;
    };
    REGORD("WSOCK32.dll", 19, 4, send_fn);
    REGORD("WS2_32.dll", 19, 4, send_fn);

    // ---- sendto (#20) / recvfrom (#17) — LE TROU UDP ----------------------
    // Ces deux ordinaux n'ont JAMAIS ete inscrits, ni ici ni chez le
    // consommateur : la table allait 1-16, 19, 21-23, 52, 57, 101, 111, 112,
    // 115, 116 et sautait 17 et 20. Un appel tombait donc sur le shim par
    // defaut — arret controle ET derive ESP, puisque l'argc est inconnu. Or
    // tout protocole qui fait un test d'accessibilite UDP passe par la.
    // L'argc est la seule chose qui compte ici : un shim stdcall mal dimensionne
    // decale la pile a CHAQUE appel.
    //   int sendto  (SOCKET, const char* buf, int len, int flags,
    //                const struct sockaddr* to, int tolen);        -> 6
    //   int recvfrom(SOCKET, char* buf, int len, int flags,
    //                struct sockaddr* from, int* fromlen);         -> 6
    auto sendto_fn = [](Cpu& c) -> uint32_t {
        if (!wx86_net_enabled()) return 0xFFFFFFFFu;
        int fd = wx86_sock_fd(c.arg(0));
        if (fd < 0) { wx86_net_set_last_error(10038); return 0xFFFFFFFFu; }
        const uint32_t len = c.arg(2), pto = c.arg(4), tolen = c.arg(5);
        uint8_t sa[16] = {0};
        if (pto) c.read(pto, sa, tolen > 16 ? 16 : (tolen ? tolen : 16));
        uint32_t dip; std::memcpy(&dip, sa + 4, 4);
        const uint16_t dport = (uint16_t)((sa[2] << 8) | sa[3]);
        // MEME route generique que connect : un banc local doit pouvoir
        // detourner un test UDP sans toucher au jeu.
        uint32_t rip = dip;
        if (g_routeIp) {
            const uint8_t hi = dip & 0xff;
            if (hi != 127 && hi != 0 && dip != 0xffffffffu) {
                rip = g_routeIp; std::memcpy(sa + 4, &rip, 4);
            }
        }
        if (!wx86_net_allow(&c, c.arg(0), fd, rip, dport, WX86_NET_SEND))
            return 0xFFFFFFFFu;               // rien ne part
        std::vector<uint8_t> b(len);
        if (len) c.read(c.arg(1), b.data(), len);
        ssize_t n = ::sendto(fd, b.data(), b.size(), MSG_NOSIGNAL,
                             (sockaddr*)sa, pto ? (socklen_t)16 : (socklen_t)0);
        uint32_t we = 0;
        if (n < 0) { we = wx86_wsa_from_errno(errno); wx86_net_set_last_error(we); }
        wx86_net_notify(WX86_NET_SEND, &c, c.arg(0), fd, dip, rip, dport,
                        (int)n, we, b.data(), (int)b.size());
        return n >= 0 ? (uint32_t)n : 0xFFFFFFFFu;
    };
    REGORD("WSOCK32.dll", 20, 6, sendto_fn);
    REGORD("WS2_32.dll", 20, 6, sendto_fn);

    auto recvfrom_fn = [](Cpu& c) -> uint32_t {
        if (!wx86_net_enabled()) return 0xFFFFFFFFu;
        WsockHandle h;
        if (!wx86_sock_get(c.arg(0), h)) { wx86_net_set_last_error(10038); return 0xFFFFFFFFu; }
        const uint32_t len = c.arg(2), pfrom = c.arg(4), pflen = c.arg(5);
        std::vector<uint8_t> b(len);
        sockaddr_in sa{}; socklen_t sl = sizeof sa;
        ssize_t n = ::recvfrom(h.fd, b.data(), b.size(), 0, (sockaddr*)&sa, &sl);
        if (n < 0 && !h.nonblock && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // Attente BORNEE (3 s) : un test UDP sans reponse doit degrader
            // proprement, jamais figer le runtime.
            pollfd pf{h.fd, POLLIN, 0};
            wx86_poll_gilfree(&pf, 3000);
            sl = sizeof sa;
            n = ::recvfrom(h.fd, b.data(), b.size(), 0, (sockaddr*)&sa, &sl);
        }
        if (n < 0) {
            const uint32_t we = wx86_wsa_from_errno(errno);
            wx86_net_set_last_error(we);
            wx86_net_notify(WX86_NET_RECV, &c, c.arg(0), h.fd, 0, 0, h.port, -1, we, nullptr, 0);
            return 0xFFFFFFFFu;
        }
        // Defense en profondeur : sous verrou, un datagramme VENANT d'une
        // adresse non privee est jete — sinon le verrou ne tiendrait que la
        // moitie de la conversation.
        const uint32_t sip = (uint32_t)sa.sin_addr.s_addr;
        if (!wx86_net_allow(&c, c.arg(0), h.fd, sip, (uint16_t)ntohs(sa.sin_port), WX86_NET_RECV)) {
            wx86_net_set_last_error(10035);   // WSAEWOULDBLOCK : « rien pour toi »
            return 0xFFFFFFFFu;
        }
        if (n > 0) c.write(c.arg(1), b.data(), (uint32_t)n);
        if (pfrom) {
            uint8_t out[16] = {0}; out[0] = 2; out[1] = 0;
            const uint16_t sp = (uint16_t)ntohs(sa.sin_port);
            out[2] = (uint8_t)(sp >> 8); out[3] = (uint8_t)(sp & 0xff);
            std::memcpy(out + 4, &sa.sin_addr.s_addr, 4);
            const uint32_t cap = pflen ? c.read_u32(pflen) : 16u;
            c.write(pfrom, out, cap < 16 ? cap : 16);
            if (pflen) c.write_u32(pflen, 16);
        }
        wx86_net_notify(WX86_NET_RECV, &c, c.arg(0), h.fd, sip, sip,
                        (uint16_t)ntohs(sa.sin_port), (int)n, 0, b.data(), (int)n);
        return (uint32_t)n;
    };
    REGORD("WSOCK32.dll", 17, 6, recvfrom_fn);
    REGORD("WS2_32.dll", 17, 6, recvfrom_fn);

    // accept/bind/listen: real POSIX passthrough. No D2 client ever
    // exercises this path (D2 is a pure Winsock client), so these are
    // implemented correctly but validated only in isolation (a standalone
    // connect/accept round-trip against this build), never by real D2
    // traffic — see the commit message for exactly what that isolation
    // test covered.
    auto bind_fn = [](Cpu& c) -> uint32_t {
        if (!wx86_net_enabled()) return 0xFFFFFFFFu;
        int fd = wx86_sock_fd(c.arg(0));
        if (fd < 0) { wx86_net_set_last_error(10038); return 0xFFFFFFFFu; }
        uint8_t sa[16] = {0}; c.read(c.arg(1), sa, 16);
        if (::bind(fd, (sockaddr*)sa, (socklen_t)c.arg(2)) < 0) { wx86_net_set_last_error(wx86_wsa_from_errno(errno)); return 0xFFFFFFFFu; }
        return 0u;
    };
    REGORD("WSOCK32.dll", 2, 3, bind_fn);
    REGORD("WS2_32.dll", 2, 3, bind_fn);

    auto listen_fn = [](Cpu& c) -> uint32_t {
        if (!wx86_net_enabled()) return 0xFFFFFFFFu;
        int fd = wx86_sock_fd(c.arg(0));
        if (fd < 0) { wx86_net_set_last_error(10038); return 0xFFFFFFFFu; }
        if (::listen(fd, (int)c.arg(1)) < 0) { wx86_net_set_last_error(wx86_wsa_from_errno(errno)); return 0xFFFFFFFFu; }
        return 0u;
    };
    REGORD("WSOCK32.dll", 13, 2, listen_fn);
    REGORD("WS2_32.dll", 13, 2, listen_fn);

    auto accept_fn = [](Cpu& c) -> uint32_t {
        if (!wx86_net_enabled()) return 0xFFFFFFFFu;
        int fd = wx86_sock_fd(c.arg(0));
        if (fd < 0) { wx86_net_set_last_error(10038); return 0xFFFFFFFFu; }
        uint8_t sa[16] = {0}; socklen_t sl = 16;
        int nfd = ::accept(fd, (sockaddr*)sa, &sl);
        if (nfd < 0) { wx86_net_set_last_error(wx86_wsa_from_errno(errno)); return 0xFFFFFFFFu; }
        wx86_net_set_nonblock(nfd, true);
        uint32_t gh = wx86_sock_alloc(nfd);
        if (c.arg(1)) c.write(c.arg(1), sa, 16);
        if (c.arg(2)) c.write_u32(c.arg(2), 16);
        return gh;
    };
    REGORD("WSOCK32.dll", 1, 3, accept_fn);
    REGORD("WS2_32.dll", 1, 3, accept_fn);

    // WSAGetLastError(): 10093 (WSANOTINITIALISED) when networking is off,
    // else the shared last-error store every shim above (and d2vita's own
    // connect/recv/send) sets on failure. Trivially generic itself — moved
    // here alongside the store it reads, so there is one owner instead of
    // this ordinal reading a d2vita-local copy of winx86-owned state.
    auto wsagetlasterror_fn = [](Cpu&) -> uint32_t {
        return wx86_net_enabled() ? wx86_net_last_error() : 10093u;
    };
    REGORD("WSOCK32.dll", 111, 0, wsagetlasterror_fn);
    REGORD("WS2_32.dll", 111, 0, wsagetlasterror_fn);
}
