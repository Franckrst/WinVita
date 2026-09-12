// tools/net_resolve_guard_selftest.cpp — le verrou de sortie couvre-t-il la
// RESOLUTION DE NOM ?
//
// CE QUI EST TESTE
// ----------------
// wx86_net_resolve() de src/runtime/net_nonblock.cpp, c'est-a-dire le SEUL
// point du moteur d'ou une requete de resolution part. Le verrou
// wx86_net_set_private_only promet que « rien ne part vers l'internet
// public » ; il etait applique a connect/sendto/recvfrom, donc APRES la
// resolution. Un nom d'hote public sortait donc en clair de la machine, alors
// meme que la connexion qui aurait suivi aurait ete refusee.
//
// CE QUI EST OBSERVE
// ------------------
// La SORTIE du code teste : la valeur rendue par wx86_net_resolve, et le
// compteur wx86_net_resolves_refused qui sert de temoin POSITIF. Sans ce
// compteur, « aucune requete n'est sortie » et « la fonction n'a jamais ete
// appelee » rendraient le meme silence, et un zero ne prouverait rien.
//
// CE QUI N'EST PAS TESTE ICI
// --------------------------
// Le predicat wx86_net_private_only() lui-meme : son unite de traduction
// (win32_shims_wsock32.cpp) tire tout le pont et le processeur, et n'est pas
// batissable dans ce filet-ci. Il est donc fourni ici par une definition de
// test. C'est l'ENTREE de la decision, pas sa sortie : ce filet prouve que
// wx86_net_resolve honore le verrou, pas que le verrou s'arme correctement —
// ce dernier point est couvert par les portes reseau du portage.
//
// AUCUN NOM PUBLIC N'EST RESOLU PAR CE FILET. La seule resolution reelle
// porte sur « localhost », qui ne quitte pas la machine.
#include "runtime/net_nonblock.h"
#include <arpa/inet.h>
#include <cstdio>

// --- entree de la decision, fournie par le test (voir en-tete) -------------
static bool g_privateOnly = false;
bool wx86_net_private_only() { return g_privateOnly; }

static int fails = 0;
static void check(bool ok, const char* quoi) {
    if (!ok) { std::printf("  ECHEC : %s\n", quoi); ++fails; }
}

int main() {
    using namespace d2rt;
    const uint32_t attendu10 = inet_addr("10.0.0.1");

    // 1. Desarme : un litteral prive repond, et ne compte pour aucun refus.
    g_privateOnly = false;
    unsigned long long r0 = wx86_net_resolves_refused();
    check(wx86_net_resolve("10.0.0.1") == attendu10, "litteral prive, desarme");
    check(wx86_net_resolves_refused() == r0, "un litteral ne compte pas comme refus");

    // 2. Desarme : un NOM se resout. « localhost » ne quitte pas la machine.
    const uint32_t lo = wx86_net_resolve("localhost");
    check(lo != 0, "localhost se resout quand le verrou est desarme");
    check(wx86_net_resolves_refused() == r0, "aucun refus tant que le verrou dort");

    // 3. Arme : le litteral repond TOUJOURS — il ne consulte personne.
    g_privateOnly = true;
    check(wx86_net_resolve("10.0.0.1") == attendu10, "litteral prive, arme");
    check(wx86_net_resolves_refused() == r0, "le litteral ne declenche pas le verrou");

    // 4. Arme : le meme NOM qui marchait en 2 est refuse, et le refus se VOIT.
    //    C'est la jambe qui coupe : si la garde disparaissait, elle rendrait
    //    la meme valeur non nulle qu'au point 2 et le compteur ne bougerait pas.
    check(wx86_net_resolve("localhost") == 0, "un nom est refuse quand le verrou est arme");
    check(wx86_net_resolves_refused() == r0 + 1, "le refus est compte (temoin positif)");

    // 5. Le refus n'est pas un etat collant : desarme, le nom repond a nouveau.
    g_privateOnly = false;
    check(wx86_net_resolve("localhost") == lo, "le verrou leve rend le meme resultat qu'avant");
    check(wx86_net_resolves_refused() == r0 + 1, "et ne compte pas un refus de plus");

    if (fails) { std::printf("net_resolve_guard : %d ECHEC(S)\n", fails); return 1; }
    std::printf("net_resolve_guard : 10 verifications, tout passe\n");
    return 0;
}
