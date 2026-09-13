// Does the outbound-traffic lock cover name resolution?
//
// What's tested: wx86_net_resolve() in src/runtime/net_nonblock.cpp is the
// only point in the engine where a resolution request originates. The
// wx86_net_set_private_only lock promises that nothing reaches the public
// internet, but resolution happens before connect/sendto/recvfrom — so a
// public hostname can still leave the machine in the clear even when the
// connection that would follow gets refused.
//
// What's observed: the return value of wx86_net_resolve, and the
// wx86_net_resolves_refused counter as a positive witness. Without that
// counter, "no request went out" and "the function was never called" would
// look identical, and a zero would prove nothing.
//
// The lock lives in runtime/net_guard.cpp, a leaf unit with no dependencies,
// so this test links the real lock and arms it through its real function —
// not a redefinition of its own.
//
// No public name is resolved by this test — the only real resolution is
// "localhost", which never leaves the machine.
#include "runtime/net_nonblock.h"
#include "runtime/net_guard.h"
#include <arpa/inet.h>
#include <cstdio>

static int fails = 0;
static void check(bool ok, const char* quoi) {
    if (!ok) { std::printf("  ECHEC : %s\n", quoi); ++fails; }
}

int main() {
    using namespace d2rt;
    const uint32_t attendu10 = inet_addr("10.0.0.1");

    // 1. Disarmed: a private literal resolves, and doesn't count as a refusal.
    wx86_net_set_private_only(false);
    unsigned long long r0 = wx86_net_resolves_refused();
    check(wx86_net_resolve("10.0.0.1") == attendu10, "litteral prive, desarme");
    check(wx86_net_resolves_refused() == r0, "un litteral ne compte pas comme refus");

    // 2. Disarmed: a name resolves. "localhost" never leaves the machine.
    const uint32_t lo = wx86_net_resolve("localhost");
    check(lo != 0, "localhost se resout quand le verrou est desarme");
    check(wx86_net_resolves_refused() == r0, "aucun refus tant que le verrou dort");

    // 3. Armed: the literal always resolves — it consults nothing.
    wx86_net_set_private_only(true);
    check(wx86_net_resolve("10.0.0.1") == attendu10, "litteral prive, arme");
    check(wx86_net_resolves_refused() == r0, "le litteral ne declenche pas le verrou");

    // 4. Armed: the same name that worked in step 2 is refused, visibly. This
    //    is the case that matters: without the guard, it would return the
    //    same nonzero value as step 2 and the counter wouldn't move.
    check(wx86_net_resolve("localhost") == 0, "un nom est refuse quand le verrou est arme");
    check(wx86_net_resolves_refused() == r0 + 1, "le refus est compte (temoin positif)");

    // 5. Refusal isn't sticky: disarmed, the name resolves again.
    wx86_net_set_private_only(false);
    check(wx86_net_resolve("localhost") == lo, "le verrou leve rend le meme resultat qu'avant");
    check(wx86_net_resolves_refused() == r0 + 1, "et ne compte pas un refus de plus");

    if (fails) { std::printf("net_resolve_guard : %d ECHEC(S)\n", fails); return 1; }
    std::printf("net_resolve_guard : 10 verifications, tout passe\n");
    return 0;
}
