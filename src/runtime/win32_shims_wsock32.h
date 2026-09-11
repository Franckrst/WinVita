// src/runtime/win32_shims_wsock32.h — Winsock 1.1/2 ordinal shims
// (WSOCK32.dll / WS2_32.dll) and the generic socket-handle table backing
// them. See win32_shims_wsock32.cpp for the exact ordinal-by-ordinal split
// rationale versus what stays d2vita-side (connect/recv/send/select carry
// real D2/BNCS orchestration and are registered from tools/rt_boot.cpp, but
// use the table and helpers declared here instead of keeping their own).
#pragma once
#include <cstdint>
namespace d2rt { class Bridge; }

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
// d2vita's connect/recv/send/select) so there is exactly one "is networking
// on" bit instead of two that could drift apart.
bool wx86_net_enabled();
void wx86_net_set_enabled(bool on);

// --- small generic helpers shared with d2vita's connect/recv/send -------
uint32_t wx86_wsa_from_errno(int e);   // POSIX errno -> Winsock error code

// Single canonical "last Winsock error" store (what WSAGetLastError, itself
// now registered here, returns). Every socket shim in this file sets it on
// failure; d2vita's connect/recv/send (kept d2vita-side) set it too, via
// the same setter, instead of keeping their own local copy — one store, not
// two that could read stale after a call that only updates one of them.
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

void win32_shims_wsock32_install(d2rt::Bridge& br);
