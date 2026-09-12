// tools/ds_rate_selftest.cpp — la frequence du melangeur vient de L'APPELANT.
//
// CE QU'IL PROUVE. Le socle DirectSound du moteur melangeait a 22050 Hz, une
// constante ecrite dans ds_emul.cpp : le chiffre du premier consommateur, dont
// tous les echantillons sont a cette frequence. Un jeu echantillonne ailleurs
// aurait joue a la mauvaise hauteur, EN SILENCE. (Le meme defaut avait deja ete
// corrige un etage plus bas, au puits console, qui recevait la frequence et
// l'ignorait ; le melangeur, lui, etait encore intact.)
//
// Le filet fabrique deux WAV par le chemin du puits (dsound::selftest) a deux
// frequences differentes et lit la frequence REELLEMENT ecrite dans l'en-tete
// RIFF de chacun. Si la constante etait encore la, les deux en-tetes seraient
// identiques.
//
// CONTROLE NEGATIF inclus : le test verifie que les deux valeurs DIFFERENT.
// Un filet qui se contenterait de lire « 22050 attendu, 22050 trouve » aurait
// signe l'ancien code aussi bien que le nouveau.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "runtime/ds_emul.h"
#include "runtime/bridge.h"

// CE QUE CE FILET NE TESTE PAS, dit franchement. dsound::selftest() n'ouvre
// qu'un puits WAV : il ne pose aucune vtable COM, donc dsound::install() n'est
// jamais appele. Les trois methodes de Bridge que ds_emul.cpp reference depuis
// install() n'ont pourtant pas de corps ici (bridge.cpp traine le dynarec, qui
// ne se compile pas sur bureau) : on les definit VIDES, uniquement pour
// l'edition de liens. Si l'une d'elles etait appelee, le test s'arreterait —
// c'est ce que dit l'abort.
namespace d2rt {
static void jamais(const char* quoi) {
    std::printf("  ECHEC: %s appele — ce filet ne couvre pas ce chemin\n", quoi);
    std::fflush(stdout); std::abort();
}
void Bridge::register_shim(const std::string&, const std::string&, Shim) { jamais("register_shim"); }
void Bridge::register_shim_ordinal(const std::string&, uint32_t, Shim) { jamais("register_shim_ordinal"); }
uint32_t Bridge::shim_trap(const std::string&, const std::string&) { jamais("shim_trap"); return 0; }
}

static int lire_freq_wav(const char* chemin) {
    std::FILE* f = std::fopen(chemin, "rb");
    if (!f) return -1;
    unsigned char h[44] = {0};
    const size_t n = std::fread(h, 1, sizeof h, f);
    std::fclose(f);
    if (n < sizeof h) return -2;
    if (std::memcmp(h, "RIFF", 4) || std::memcmp(h + 8, "WAVE", 4)) return -3;
    return (int)(h[24] | (h[25] << 8) | (h[26] << 16) | ((uint32_t)h[27] << 24));
}

static int erreurs = 0, verifs = 0;
static void ok(bool c, const char* quoi) {
    ++verifs; if (!c) { std::printf("  ECHEC: %s\n", quoi); ++erreurs; }
}

int main() {
    const char* a = "/tmp/wx86_ds_rate_a.wav";
    const char* b = "/tmp/wx86_ds_rate_b.wav";
    // 60 ms suffisent : on verifie l'EN-TETE, pas le contenu.
    const int rc_a = d2rt::dsound::selftest(a, 60, 11025);
    const int rc_b = d2rt::dsound::selftest(b, 60, 44100);
    ok(rc_a == 0, "selftest a 11025 Hz rend 0");
    ok(rc_b == 0, "selftest a 44100 Hz rend 0");
    const int fa = lire_freq_wav(a), fb = lire_freq_wav(b);
    std::printf("ds_rate: en-tetes RIFF lus -> %d Hz et %d Hz\n", fa, fb);
    ok(fa == 11025, "le WAV demande a 11025 Hz est ECRIT a 11025 Hz");
    ok(fb == 44100, "le WAV demande a 44100 Hz est ECRIT a 44100 Hz");
    ok(fa != fb,    "CONTROLE NEGATIF : deux demandes differentes donnent deux en-tetes differents");
    // Frequence absente = refus explicite, jamais une valeur devinee.
    ok(d2rt::dsound::selftest("/tmp/wx86_ds_rate_c.wav", 60, 0) != 0,
       "frequence non fournie : le test REFUSE au lieu de deviner");
    std::printf("ds_rate: %d verifications, %d echec(s)\n", verifs, erreurs);
    return erreurs ? 1 : 0;
}
