// src/runtime/win32_shims_wsock32.h — Winsock 1.1/2 ordinal shims
// (WSOCK32.dll / WS2_32.dll) and the generic socket-handle table backing
// them. See win32_shims_wsock32.cpp for the exact ordinal-by-ordinal split
// rationale versus what stays embedder-side. connect/recv/send live HERE and
// are fully generic since 2026-09-11: application policy reaches them through
// the observer and the connect route declared below. The address helpers
// (inet_addr/inet_ntoa/gethostbyname) joined them the same day, once the
// guest scratch allocator they write through became an engine primitive
// (guest_scratch.h). select is now the only ordinal the embedder still
// registers itself.
#pragma once
#include <cstdint>
namespace d2rt { class Bridge; class Cpu; }

// --- socket-handle table -------------------------------------------------
// A guest Winsock SOCKET is just an opaque uint32_t handle; this table maps
// it to a host fd plus the two bits every Winsock emulation layer needs to
// track per socket (guest-visible blocking mode, and the last connect()
// destination port — generic bookkeeping, not particular to any protocol).
struct WsockHandle { int fd; bool nonblock; uint16_t port; };

uint32_t wx86_sock_alloc(int fd);              // new guest handle for fd (nonblock=false, port=0)
bool     wx86_sock_get(uint32_t h, WsockHandle& out);
int      wx86_sock_fd(uint32_t h);             // -1 if h is not a live handle
uint16_t wx86_sock_port(uint32_t h);           // 0 if h is not a live handle
void     wx86_sock_set_port(uint32_t h, uint16_t port);
bool     wx86_sock_nonblock(uint32_t h);
void     wx86_sock_set_nonblock(uint32_t h, bool nb);
void     wx86_sock_erase(uint32_t h);          // no-op if h is not live (mirrors std::map::erase)

// --- networking-enabled gate ---------------------------------------------
// One flag, owned here, read by every network shim (this file's own and
// any shim the embedder still registers) so there is exactly one "is networking
// on" bit instead of two that could drift apart.
bool wx86_net_enabled();
void wx86_net_set_enabled(bool on);

// --- small generic helpers, also usable by embedder-side shims ----------
uint32_t wx86_wsa_from_errno(int e);   // POSIX errno -> Winsock error code

// Single canonical "last Winsock error" store (what WSAGetLastError, itself
// registered here, returns). Every socket shim sets it on failure through
// this one setter — one store, not two that could read stale after a call
// that only updates one of them. Shims the embedder still registers itself
// (select, the address helpers) use the same setter.
uint32_t wx86_net_last_error();
void wx86_net_set_last_error(uint32_t code);

// Completes a non-blocking ::connect() synchronously: waits (GIL released,
// native scheduler only) up to timeout_ms for the socket to become
// writable, then reads SO_ERROR to tell a real success from a failed
// handshake. Returns 0 on success; -1 on failure or timeout, with a
// Winsock-style code written to *outWsaErr (10060 ETIMEDOUT-style on
// timeout). Callers still make the initial ::connect() call themselves —
// this only owns the generic "wait for it to finish" dance every
// non-blocking Winsock connect() needs.
int wx86_connect_wait(int fd, int timeout_ms, uint32_t* outWsaErr);

// Generic blocking-emulated recv/send core: real Winsock sockets default to
// BLOCKING mode even though this runtime keeps the host fd non-blocking (to
// protect the cooperative scheduler) — recv emulates the wait via a
// GIL-released poll when the guest hasn't asked for non-blocking mode.
// Returns the same shape as POSIX recv/send: >=0 bytes (0 on EOF for recv),
// or -1 with *outWsaErr set. `blocking` is the guest-visible mode (i.e. the
// caller's own !nonblock check on the WsockHandle).
int wx86_recv_blocking(int fd, void* buf, uint32_t len, bool blocking, int wait_ms, uint32_t* outWsaErr);
int wx86_send_simple(int fd, const void* buf, uint32_t len, uint32_t* outWsaErr);

// --- generic connect-time route override ---------------------------------
// The moral equivalent of a hosts file entry or an outbound proxy: when set,
// every connect() to a NON-loopback destination is rewritten to this address,
// keeping the requested port. Off (0) unless set. This is a sandboxing /
// offline-testing facility of the socket layer itself — it knows nothing
// about which protocol or which game is running on top of it.
//
// NOTE for embedders: this is a BACKSTOP, not the normal way to point a
// client at a private server. The faithful way is the one a real PC uses —
// configure the client's own server list (its registry keys / .ini) so it
// asks for your host in the first place. See win32_shims_wsock32.cpp.
void     wx86_net_set_redirect(uint32_t ip);   // 0 = disabled
uint32_t wx86_net_redirect();

// --- generic socket-layer observer ---------------------------------------
// ONE passive observation point over the whole socket layer. The engine
// reports what happened; it never asks the observer for a decision and never
// changes behaviour based on it. Protocol framing, per-application policy,
// logging and instrumentation all belong to the embedder, which registers a
// single observer and dispatches internally. Nothing here names a protocol,
// a port or a product.
enum {
    WX86_NET_CONNECT = 0,    // before the host connect(): ip/port = requested
                             // destination, route_ip = address actually used
                             // (== ip when no redirect applied)
    WX86_NET_CONNECT_DONE,   // after it: result 0 = connected immediately,
                             // 1 = completed after a synchronous wait, -1 = failed
    WX86_NET_SEND,           // after a send: data/len = payload, result = bytes
    WX86_NET_RECV,           // after a recv: data/len = payload, result = bytes
                             // (0 = orderly shutdown, -1 = error)
    WX86_NET_CLOSE,
    WX86_NET_RESOLVE,        // resolution de nom : data/len = le nom demande,
                             // result 0 = resolu (ip = adresse obtenue),
                             // -1 = echec (wsa_err renseigne). Le moteur
                             // etant muet, c'est par ici que l'embarqueur
                             // apprend qu'un nom n'a pas pu etre resolu —
                             // premier echec attendu sur une plateforme
                             // embarquee, il ne doit pas rester invisible.
};
struct WsockEvent {
    int         kind;
    d2rt::Cpu*  cpu;       // CPU at the call site, for embedder-side diagnostics
    uint32_t    handle;    // guest SOCKET
    int         fd;        // host fd
    uint32_t    ip;        // CONNECT: requested destination, network byte order
    uint32_t    route_ip;  // CONNECT: address actually connected to
    uint16_t    port;      // CONNECT: destination port; SEND/RECV: socket's port
    int         result;    // see the kind comments above
    uint32_t    wsa_err;   // Winsock error code when result < 0
    const uint8_t* data;   // SEND/RECV payload, else null
    int         len;       // SEND/RECV payload length, else 0
};
typedef void (*WsockObserverFn)(const WsockEvent&);
// Exactly ONE observer, deliberately: a registry that silently accepts a
// second registration is how this project once lost a login regression to a
// duplicate hook. Setting it twice replaces the first.
void wx86_net_set_observer(WsockObserverFn cb);

void win32_shims_wsock32_install(d2rt::Bridge& br);
