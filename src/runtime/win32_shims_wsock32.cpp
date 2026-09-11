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
// What is STILL d2vita-side, and why that's a real boundary and not
// leftover caution:
//   - connect (WSOCK32.dll!#4): the BNCS/D2GS gateway-port literals
//     (6112/4000), the D2BNCS_LOCAL redirect, the virtual/real clock flip,
//     g_bnetConnected — D2's own login-flow orchestration. It calls
//     wx86_connect_wait() for the generic "wait for the handshake to
//     finish" part, same as before, just no longer duplicating that logic.
//   - recv/send (WSOCK32.dll!#16/#19): D2_NETWATCH observation and a
//     port==4000 diagnostic log line are d2vita-only; they now wrap calls
//     to wx86_recv_blocking()/wx86_send_simple() for the generic transfer.
//   - select (WSOCK32.dll!#18): NOT touched this pass either. Its core
//     (Windows fd_set <-> pollfd translation) is generic, but it is also
//     the exact site of a real, already-fixed starvation bug
//     (2026-08-30, D2_SELECTBLOCK) whose regression signature only shows
//     under real network load timing — qemu-arm boot-to-title-screen
//     cannot prove a split here didn't reintroduce it, and this pass's
//     online validation (see commit message) covers connect/recv/send/
//     socket/accept/bind/listen, not select's timing-sensitive path.
//     Left exactly as before, deliberately, not by default.
//   - inet_addr/inet_ntoa (WSOCK32.dll!#10/#11): still not here — moving
//     them needs the d2vita-hosted guest scratch allocator (misc()/
//     put_cstr) for inet_ntoa's returned string, and out of scope for a
//     WSOCK32-layer pass to drag that allocator along.
//   - gethostbyname (WSOCK32.dll!#52): same allocator dependency for its
//     marshalled hostent struct; calls the now-winx86-hosted
//     wx86_net_resolve() (net_nonblock.h) for the actual resolution.
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
