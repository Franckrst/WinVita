// The mixer's sample rate must come from the caller. A hardcoded rate would
// have any game sampled at a different rate play at the wrong pitch, with no
// error.
//
// Method: builds two WAV files through the sink's path (dsound::selftest) at
// two different rates and reads back the rate actually written in each RIFF
// header. If a hardcoded rate were used instead, both headers would match.
//
// Negative control: the test verifies the two rates differ. A test that
// just checked "22050 expected, 22050 found" would pass against a hardcoded
// rate just as easily as against the real one.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "runtime/ds_emul.h"
#include "runtime/bridge.h"

// What this test does not cover: dsound::selftest() only opens a WAV sink
// and never installs a COM vtable, so dsound::install() is never called.
// The three Bridge methods ds_emul.cpp references from install() have no
// real body here (bridge.cpp drags in the dynarec, which doesn't compile on
// desktop) — they're defined empty purely to satisfy the linker. If one were
// ever called, the abort below would end the test.
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
    // 60ms is enough: this checks the header, not the content.
    const int rc_a = d2rt::dsound::selftest(a, 60, 11025);
    const int rc_b = d2rt::dsound::selftest(b, 60, 44100);
    ok(rc_a == 0, "selftest a 11025 Hz rend 0");
    ok(rc_b == 0, "selftest a 44100 Hz rend 0");
    const int fa = lire_freq_wav(a), fb = lire_freq_wav(b);
    std::printf("ds_rate: en-tetes RIFF lus -> %d Hz et %d Hz\n", fa, fb);
    ok(fa == 11025, "le WAV demande a 11025 Hz est ECRIT a 11025 Hz");
    ok(fb == 44100, "le WAV demande a 44100 Hz est ECRIT a 44100 Hz");
    ok(fa != fb,    "CONTROLE NEGATIF : deux demandes differentes donnent deux en-tetes differents");
    // Missing rate = explicit refusal, never a guessed value.
    ok(d2rt::dsound::selftest("/tmp/wx86_ds_rate_c.wav", 60, 0) != 0,
       "frequence non fournie : le test REFUSE au lieu de deviner");
    std::printf("ds_rate: %d verifications, %d echec(s)\n", verifs, erreurs);
    return erreurs ? 1 : 0;
}
