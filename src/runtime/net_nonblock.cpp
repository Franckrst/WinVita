// src/runtime/net_nonblock.cpp — see net_nonblock.h. Bodies copied
// byte-for-byte from d2vita's src/platform/vita_net.cpp
// d2vita_net_set_nonblock/d2vita_net_resolve (2026-09-11, WSOCK32
// genericity pass); only names and the two diagnostic globals
// (g_nbMethod/g_resolveRc, now exposed via getters instead of d2vita
// reaching into a d2vita-local static) changed.
#include "runtime/net_nonblock.h"
#include "runtime/net_guard.h"   // wx86_net_private_only() — unite FEUILLE, voir cet en-tete
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#ifndef __vita__
#include <sys/ioctl.h>
#endif
#include <netinet/in.h>
#include <arpa/inet.h>
#ifdef __vita__
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2common/net.h>
#else
#include <netdb.h>
#include <cstring>
#endif

namespace d2rt {

static int g_nbMethod = 0;
static int g_resolveRc = 0;
static unsigned long long g_resolveRefused = 0;

int wx86_net_nonblock_method() { return g_nbMethod; }
unsigned long long wx86_net_resolves_refused() { return g_resolveRefused; }
int wx86_net_last_resolve_rc() { return g_resolveRc; }

bool wx86_net_set_nonblock(int fd, bool on) {
    // Reset before every attempt: otherwise a failing call would still
    // display the previous success's method.
    g_nbMethod = 0;
    // 1) POSIX fcntl, VERIFIED by read-back: on Vita the call can "succeed"
    //    without changing anything — a read-back costs one syscall and
    //    avoids a false diagnostic.
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) {
        int want = on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
        if (fcntl(fd, F_SETFL, want) == 0) {
            int back = fcntl(fd, F_GETFL, 0);
            if (back >= 0 && ((back & O_NONBLOCK) != 0) == on) { g_nbMethod = 1; return true; }
        }
    }
#ifndef __vita__
    // 2) ioctl FIONBIO. Host only: the VitaSDK has neither <sys/ioctl.h> nor
    //    FIONBIO, and the console equivalent is already covered by method 1.
    { int v = on ? 1 : 0; if (ioctl(fd, FIONBIO, &v) == 0) { g_nbMethod = 2; return true; } }
#endif
    // NO third method via sceNetSetsockopt: it would be wrong AND
    // dangerous. Proven by disassembling the SDK's libc:
    //   - socket.o: socket() stores the id returned by sceNetSocket in
    //     __vita_fdmap and returns a libc descriptor INDEX, not the Sony
    //     id. Passing that raw fd to sceNetSetsockopt would therefore
    //     target a different object, possibly a valid third-party socket —
    //     in which case the read-back would confirm success on the WRONG
    //     socket, this function would return true for a socket that stayed
    //     blocking, and the next recv would freeze the single runner.
    //   - lib_a-fcntl.o: fcntl() translates the descriptor
    //     (__vita_fd_grab) then calls sceNetGetsockopt/sceNetSetsockopt on
    //     option 0x1100 (SCE_NET_SO_NBIO). Method 1 IS therefore already
    //     the Sony option, applied to the right id.
    // A method that cannot work but can lie is worse than no method.
    return false;
}

uint32_t wx86_net_resolve(const char* host) {
    if (!host || !*host) return 0;
    in_addr a;
    if (inet_aton(host, &a)) return a.s_addr;

    // --- verrou de sortie : une REQUETE DNS est deja un depart ------------
    // Le verrou wx86_net_set_private_only promet que « rien ne part vers
    // l'internet public ». Il etait applique a connect/sendto/recvfrom,
    // c'est-a-dire APRES la resolution — donc une resolution de nom sortait
    // de la machine, en clair, en nommant l'hote vise, alors meme que la
    // connexion qui aurait suivi aurait ete refusee. Le nom demande est
    // souvent plus revelateur que l'adresse obtenue.
    //
    // L'en-tete du verrou anticipait ce cas a la lettre : wx86_net_addr_is_private
    // y est expose « pour qu'un embarqueur puisse poser la meme question
    // ailleurs (une resolution de nom, par exemple) ». La question etait
    // ecrite ; personne ne la posait. Elle est posee ici, au seul point ou
    // la requete part.
    //
    // Arme, seul le litteral pointe ci-dessus repond — il ne consulte
    // personne. Tout NOM est refuse sans qu'aucun paquet ne parte. Ce n'est
    // pas une restriction gratuite : la maniere fidele de pointer un client
    // vers un serveur prive est de lui donner l'adresse dans sa propre
    // configuration (voir wx86_net_set_redirect et le commentaire des
    // passerelles dans win32_shims_wsock32.cpp), pas de faire resoudre un
    // nom public. Un embarqueur qui a vraiment besoin d'un nom de reseau
    // local desarme le verrou : c'est SA politique, pas celle du moteur.
    if (::wx86_net_private_only()) {
        ++g_resolveRefused;
        g_resolveRc = 0;   // aucun resolveur n'a ete cree : pas de code a rapporter
        return 0;
    }
#ifdef __vita__
    // On console we do NOT call gethostbyname — we do ourselves what it
    // does. Disassembly of lib_a-gethostbyname.o: it IS
    // sceNetResolverCreate + sceNetResolverStartNtoa, called with a delay
    // of ZERO and zero retries. It is therefore the file's only unbounded
    // call, and would run first: under the cooperative scheduler, a call
    // that never returns freezes the whole game, with no diagnostic. There
    // is no second path here that would catch the first — it's the same
    // call, bounded.
    { int rid = sceNetResolverCreate("winx86", nullptr, 0);
        g_resolveRc = rid;   // < 0 here = creation failed, not the delay
        if (rid >= 0) {
            // addr starts at 0: a StartNtoa that returns >= 0 without
            // writing anything must read as a failure, never as the
            // 0.0.0.0 address that would then reach connect().
            SceNetInAddr addr; addr.s_addr = 0;
            // UNIT NOT DOCUMENTED by the header or SDK reference; the only
            // other known use (libc above) passes zero. Risk is
            // asymmetric, so use the value that fails fast under the WORST
            // reading: with 3, a "seconds" reading gives 3s, a
            // "microseconds" reading gives an immediate abort.
            // UNIT SETTLED BY MEASUREMENT (console, 2026-08-28): with 3,
            // resolution failed in 0ms — so the value is consumed as a
            // very short duration, not 3 seconds. MICROSECONDS: 5s =
            // 5,000,000. This is no longer a hypothesis, it is a
            // measurement on the device.
            int r = sceNetResolverStartNtoa(rid, host, &addr, 5 * 1000 * 1000, 1, 0);
            g_resolveRc = r;   // >= 0 but addr null = resolved without an answer
            sceNetResolverDestroy(rid);
            if (r >= 0 && addr.s_addr != 0) return addr.s_addr;
        } }
#else
    if (hostent* he = gethostbyname(host))
        if (he->h_addr_list && he->h_addr_list[0]) {
            uint32_t v; std::memcpy(&v, he->h_addr_list[0], 4); return v;
        }
#endif
    return 0;
}

} // namespace d2rt
