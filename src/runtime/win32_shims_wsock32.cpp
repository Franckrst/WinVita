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
// Ce qui reste cote consommateur, avec une vraie raison a chaque fois :
//   - select (WSOCK32.dll!#18) : son coeur (traduction fd_set <-> pollfd) est
//     generique, mais c'est aussi le site exact d'une famine reseau reelle
//     deja corrigee (2026-08-30, D2_SELECTBLOCK) dont la signature de
//     regression ne se voit que sous charge reseau reelle. Non touche.
//   - inet_addr/inet_ntoa (#10/#11) et gethostbyname (#52) : dependent de
//     l'allocateur de brouillon invite du consommateur (misc()/put_cstr) pour
//     les structures qu'ils rendent a l'invite ; inet_addr porte en plus un
//     effet de bord d'inscription documente cote consommateur.
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

void wx86_net_set_redirect(uint32_t ip) { g_routeIp = ip; }
uint32_t wx86_net_redirect() { return g_routeIp; }
void wx86_net_set_observer(WsockObserverFn cb) { g_observer = cb; }

// Single dispatch point. Zero cost when nobody observes.
static void wx86_net_notify(int kind, Cpu* c, uint32_t handle, int fd,
                            uint32_t ip, uint32_t routeIp, uint16_t port,
                            int result, uint32_t wsaErr,
                            const uint8_t* data, int len) {
    if (!g_observer) return;
    WsockEvent e{kind, c, handle, fd, ip, routeIp, port, result, wsaErr, data, len};
    g_observer(e);
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
