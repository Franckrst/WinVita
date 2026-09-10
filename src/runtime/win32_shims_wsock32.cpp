// src/runtime/win32_shims_wsock32.cpp — see win32_shims_wsock32.h. Split out
// of d2vita's tools/rt_boot.cpp WSOCK32/WS2_32 ordinal-shim group
// (2026-09-10), after a dedicated audit (session record: "audit WSOCK32
// générique vs BNCS"). That audit found the group is NOT a good bulk-move
// candidate — most of it is genuinely entangled with D2/BNCS specifics
// (connect's hardcoded gateway/D2GS ports and clock-flip logic, recv/send's
// D2_NETWATCH instrumentation, accept/listen/bind's "the D2 client never
// listens" shortcut) or with d2vita-hosted socket-table state (g_socks,
// gsock_fd) — all of that STAYS in tools/rt_boot.cpp.
//
// This file holds only the sliver that is honestly a function of its
// arguments alone, with no socket-table lookup and no D2/BNCS literal:
// byte-order conversion, WSAStartup/WSACleanup (fill/no-op on a
// caller-supplied buffer), two harmless stubs, and the platform (Vita, not
// D2) hostname. Every one of these ordinals has the SAME number for both
// WSOCK32.dll and WS2_32.dll, so moving them has no interaction with the
// WSOCK32/WS2_32 ordinal reordering below.
//
// inet_addr and inet_ntoa are NOT here, even though inet_addr in particular
// is just as argument-pure (read-only guest-string parse, no allocation).
// Two reasons, both deliberate:
//   - inet_ntoa's real implementation formats a fresh string and returns a
//     guest pointer to it, which needs a guest scratch allocator — this
//     codebase's only one (misc()/put_cstr) is d2vita-hosted.
//   - WSOCK32.dll and WS2_32.dll disagree on the ordinal number for
//     inet_addr/inet_ntoa/ioctlsocket (10/11/12 vs 10=ioctlsocket/
//     11=inet_addr/12=inet_ntoa), and d2vita's WSOCK32 table has a SIDE
//     EFFECT on ordinal WS2_32.dll!#10 (its dual-registration helper
//     touches both DLL names for every ordinal it handles, including the
//     one that's really ioctlsocket on the WS2_32 side, before an override
//     further down corrects it back). Moving inet_addr out cleanly would
//     mean either replicating that exact side effect at exactly the right
//     position relative to that override (fragile, two-part call site,
//     easy to get subtly wrong) or accepting a change in registration
//     COUNT for that ordinal — which tools/shim_seq.py's ENSEMBLE check
//     correctly treats as fatal with no override mechanism, on purpose: a
//     count change on a key that's already been silently shadowed once
//     before (see the WinVerifyTrust incident this project's shim registry
//     already carries scar tissue from) is exactly what that check exists
//     to catch. Left in tools/rt_boot.cpp, unmoved, rather than risk it.
//
// DESIGN SKETCH for select/recv/send/connect (NOT extracted — none of this
// was implemented, this is analysis only, for whoever picks it up next):
// qemu-arm boot-to-title-screen (this file's validation gate) never opens a
// real socket, so it cannot catch a networking regression; a real
// extraction here needs an actual online session (BNCS login through to
// player_join on a GS, the "vrai serveur jaenster" milestone already
// documented in docs/ONLINE_JAENSTER.md) run before AND after the change.
// That infrastructure (realmd+d2ingress+GS+postgres+redis containers, plus
// the iptables rules that keep real Blizzard traffic blocked) exists in
// this environment but was stopped, and standing it back up unsupervised
// overnight — for a mechanical cleanup task — was judged out of proportion
// to the risk. Not attempted.
//   - select (WSOCK32.dll!#18): the core (BSD select -> ::poll) is generic;
//     everything around it (g_sel.* counters, sel_report(), d2_crashlog(),
//     the D2_SELECTBLOCK knob) is d2vita's own instrumentation AND the exact
//     site of a real, already-fixed famine bug (2026-08-30). A clean split
//     would expose a generic wx86_poll(pollfds, timeout_ms) primitive from
//     winx86 and let d2vita wrap it with its counters/knob — but the
//     famine bug was only ever visible under real network load timing, so
//     "the split didn't reintroduce it" is not something qemu-arm can show.
//   - recv/send (#16/#19): core is a plain passthrough (recv/send, with
//     recv emulating a blocking wait via d2rt_poll_gilfree on EAGAIN); the
//     D2_NETWATCH observation hook and recv's port==4000 diagnostic log
//     line are d2vita-only. A clean split would add a generic post-call
//     hook winx86 invokes after every recv/send, which d2vita registers
//     its NETWATCH observer into — same shape as the extension points
//     already used elsewhere (Bridge::register_shim override pattern).
//   - connect (#4): NOT a good split candidate at all, and likely never
//     will be — the BNCS/D2GS gateway-port literals (6112/4000), the
//     D2BNCS_LOCAL redirect, the virtual-to-real clock flip on gateway
//     connect, and g_bnetConnected are D2's own login-flow orchestration,
//     not parameters of a generic connect(). It already calls the real
//     host ::connect() directly — there's no "generic core" left to
//     extract beyond what already exists.
#include "win32_shims_wsock32.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include <cstdint>
#include <functional>
#include <string>
using namespace d2rt;

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
}
