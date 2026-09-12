// tools/alt_table_selftest.cpp — l'oracle de src/dynarec86/alt_table.c.
//
// CE QU'IL PROUVE, ET CE QU'IL NE PROUVE PAS.
//
// Il prouve deux choses sur le corps que la bibliotheque embarque :
//   1. AUCUNE BORNE. 1 000 alternates distincts se posent tous, se relisent
//      tous, et dyn86_alt_lost() reste a 0. C'est le defaut corrige : la table
//      s'arretait a 52 et avalait la suite sans un mot.
//   2. UN ECHEC SE VOIT. Quand l'allocation est refusee, la pose est REFUSEE
//      (le compte n'avance pas), dyn86_alt_lost() avance, et le journal recoit
//      une ligne qui nomme la perte.
//
// INJECTION DE FAUTE. alt_table.c est compile avec -Dmalloc=wx86_selftest_malloc
// (voir tools/selftest.sh) : l'allocateur est celui de CE fichier, et il refuse
// a la demande. La fausse main porte sur l'allocateur, pas sur la fonction
// testee — le corps de dyn86_set_alternate est celui de la bibliotheque, ligne
// pour ligne. La contrepartie, a dire : l'objet teste differe de l'objet livre
// par ce seul drapeau. Une injection SANS recompilation est impossible sur un
// chemin d'allocation.
//
// CONTROLE NEGATIF : le test verifie que la faute CHANGE le verdict (sans elle,
// lost==0 ; avec elle, lost>0, puis lost cesse de monter des qu'on la retire).
// Un filet qui rend le meme resultat dans les deux jambes ne prouve rien.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

extern "C" {
    void dyn86_set_alternate(uintptr_t from, uintptr_t to);
    int  dyn86_alt_count(void);
    int  dyn86_alt_lost(void);
    extern uintptr_t *dyn86_alt_from;
    extern uintptr_t *dyn86_alt_to;

    // L'allocateur que voit alt_table.c (-Dmalloc=wx86_selftest_malloc).
    int  g_refuser = 0;
    void* wx86_selftest_malloc(size_t n) { return g_refuser ? nullptr : std::malloc(n); }

    // Le journal de la console : ici, un compteur de lignes.
    int  g_lignes = 0;
    char g_derniere[128] = {0};
    void wx86_vita_progress_c(const char* msg) {
        ++g_lignes;
        std::snprintf(g_derniere, sizeof g_derniere, "%s", msg ? msg : "(null)");
    }
}

static int erreurs = 0, verifs = 0;
static void ok(bool c, const char* quoi) {
    ++verifs;
    if (!c) { std::printf("  ECHEC: %s\n", quoi); ++erreurs; }
}

static uintptr_t cible(int i) { return (uintptr_t)0x00400000u + (uintptr_t)i * 16u; }

int main() {
    // ---- 1. aucune borne : 1 000 poses, 1 000 retrouvailles ---------------
    const int N = 1000;
    for (int i = 0; i < N; ++i) dyn86_set_alternate(cible(i), cible(i) + 8u);
    ok(dyn86_alt_count() == N, "les 1000 alternates sont poses");
    ok(dyn86_alt_lost() == 0,  "aucun alternate perdu");
    ok(g_lignes == 0,          "aucune ligne de journal sur le chemin nominal");
    int retrouves = 0;
    for (int i = 0; i < N; ++i)
        for (int k = 0; k < dyn86_alt_count(); ++k)
            if (dyn86_alt_from[k] == cible(i)) {
                if (dyn86_alt_to[k] == cible(i) + 8u) ++retrouves;
                break;
            }
    ok(retrouves == N, "les 1000 redirections se relisent a la bonne valeur");

    // Re-poser une cle DEJA presente met a jour sans consommer de place.
    const int avant = dyn86_alt_count();
    dyn86_set_alternate(cible(7), 0xDEADBEEFu);
    ok(dyn86_alt_count() == avant, "re-poser une cle connue n'ajoute pas d'entree");
    bool maj = false;
    for (int k = 0; k < dyn86_alt_count(); ++k)
        if (dyn86_alt_from[k] == cible(7)) { maj = (dyn86_alt_to[k] == 0xDEADBEEFu); break; }
    ok(maj, "re-poser une cle connue met la valeur a jour");

    // ---- 2. l'echec se voit ----------------------------------------------
    const int nAvant     = dyn86_alt_count();
    const int perduAvant = dyn86_alt_lost();
    g_refuser = 1;
    int poses = 0;
    // Au plus une croissance separe deux tailles : quelques poses suffisent a
    // atteindre la capacite courante. On en tente largement assez.
    for (int i = 0; i < 4096; ++i) {
        dyn86_set_alternate(cible(100000 + i), 1u);
        if (dyn86_alt_lost() > perduAvant) break;
        ++poses;
    }
    g_refuser = 0;
    ok(dyn86_alt_lost() > perduAvant, "une allocation refusee est COMPTEE");
    ok(g_lignes > 0,                  "une allocation refusee est JOURNALISEE");
    ok(std::strstr(g_derniere, "PERDU") != nullptr, "la ligne nomme la perte");
    ok(dyn86_alt_count() == nAvant + poses,
       "une pose refusee n'entre pas dans la table");

    // ---- 3. controle negatif : sans la faute, le verdict est different -----
    const int perduApres = dyn86_alt_lost();
    for (int k = 0; k < 256; ++k) dyn86_set_alternate(cible(200000 + k), 2u);
    ok(dyn86_alt_lost() == perduApres,
       "faute retiree : plus aucune perte (le filet ne crie pas tout seul)");

    std::printf("alt_table: %d verifications, %d echec(s)\n", verifs, erreurs);
    return erreurs ? 1 : 0;
}
