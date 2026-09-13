// tools/authenticode_selftest.cpp — filet de src/runtime/authenticode.cpp.
//
// CE QUI EST TESTE, SANS AUCUN FICHIER EXTERIEUR :
//   * SHA-1 / SHA-256 contre les vecteurs FIPS 180 (dont le million de 'a'),
//     et l'egalite « d'un coup » == « par morceaux » ;
//   * l'exponentiation RSA contre pow() de Python (vecteur 1024 bits) ;
//   * le decodage OID et des temps DER (bascule UTCTime 1950/2049) ;
//   * le condensat Authenticode d'un PE signe de test, contre une implementation
//     python independante de la specification ;
//   * verify_pe sur ce PE : non ancre -> CERT_E_CHAINING ; racine ajoutee ->
//     0 ; apres expiration sans horodatage -> CERT_E_EXPIRED ; 1 octet de .text
//     -> TRUST_E_BAD_DIGEST ; 1 octet de signature -> TRUST_E_CERT_SIGNATURE ;
//     checksum PE modifie -> toujours 0 (champ exclu) ; non-PE, PE non signe,
//     repertoire de securite tronque.
// TEMOIN POSITIF : chaque cas negatif est precede du meme fichier qui PASSE,
// sinon « echoue » et « n'a jamais pu reussir » rendraient le meme verdict.
//
// OPTIONNEL : AC_REFS=<dossier> ajoute des fichiers signes reels (ex. les
// binaires d'un portage), verifies a la date courante ; on n'y affirme que ce
// qui est stable (aucun code attendu n'est code en dur pour eux).
#include "runtime/authenticode.h"
#include "tests/authenticode_fixture.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace wx86::ac;

static int fails = 0, checks = 0;
static void check(bool ok, const char* what) {
    ++checks;
    if (!ok) { std::printf("  ECHEC : %s\n", what); ++fails; }
}
static std::string hexs(const uint8_t* p, size_t n) {
    static const char* H = "0123456789abcdef"; std::string s;
    for (size_t i = 0; i < n; i++) { s += H[p[i] >> 4]; s += H[p[i] & 15]; }
    return s;
}
static void check_hr(uint32_t got, uint32_t want, const char* what) {
    ++checks;
    if (got != want) { std::printf("  ECHEC : %s : 0x%08x au lieu de 0x%08x\n", what, (unsigned)got, (unsigned)want); ++fails; }
}

int main() {
    // ---- SHA ------------------------------------------------------------------
    uint8_t d20[20], d32[32];
    sha1((const uint8_t*)"abc", 3, d20);
    check(hexs(d20, 20) == "a9993e364706816aba3e25717850c26c9cd0d89d", "SHA-1(abc)");
    sha1((const uint8_t*)"", 0, d20);
    check(hexs(d20, 20) == "da39a3ee5e6b4b0d3255bfef95601890afd80709", "SHA-1(vide)");
    sha256((const uint8_t*)"abc", 3, d32);
    check(hexs(d32, 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256(abc)");
    const char* m448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256((const uint8_t*)m448, std::strlen(m448), d32);
    check(hexs(d32, 32) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "SHA-256(448 bits)");
    sha1((const uint8_t*)m448, std::strlen(m448), d20);
    check(hexs(d20, 20) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1", "SHA-1(448 bits)");
    { std::vector<uint8_t> a(1000000, 'a');
      sha1(a.data(), a.size(), d20);
      check(hexs(d20, 20) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f", "SHA-1(million de a)");
      sha256(a.data(), a.size(), d32);
      check(hexs(d32, 32) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "SHA-256(million de a)");
      Sha256 s; s.init(); for (size_t i = 0; i < a.size(); i += 777) s.update(a.data() + i, std::min<size_t>(777, a.size() - i));
      uint8_t e[32]; s.final(e);
      check(std::memcmp(e, d32, 32) == 0, "SHA-256 par morceaux == d'un coup"); }

    // ---- RSA brut ---------------------------------------------------------------
    { std::vector<uint8_t> out; const uint8_t e[] = {0x01, 0x00, 0x01};
      check(rsa_public(fixture::rsa_n, sizeof fixture::rsa_n, e, 3, fixture::rsa_s, sizeof fixture::rsa_s, out) &&
            out.size() == sizeof fixture::rsa_out && std::memcmp(out.data(), fixture::rsa_out, out.size()) == 0,
            "RSA s^65537 mod n == pow() de Python");
      check(!rsa_public(fixture::rsa_n, sizeof fixture::rsa_n, e, 3, fixture::rsa_n, sizeof fixture::rsa_n, out),
            "RSA refuse une signature >= n"); }

    // ---- OID, temps -----------------------------------------------------------------
    { const uint8_t o1[] = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x07,0x02};
      check(oid_to_string(o1, sizeof o1) == "1.2.840.113549.1.7.2", "OID signedData");
      const uint8_t o2[] = {0x55,0x1d,0x25,0x00};
      check(oid_to_string(o2, sizeof o2) == "2.5.29.37.0", "OID anyExtendedKeyUsage");
      const uint8_t bad[] = {0x2a,0x86};
      check(oid_to_string(bad, sizeof bad).empty(), "OID tronque refuse");
      int64_t t;
      const uint8_t u49[] = {0x17,13,'4','9','1','2','3','1','2','3','5','9','5','9','Z'};
      check(der_time(u49, sizeof u49, t) && t == 2524607999LL, "UTCTime 49 -> 2049");
      const uint8_t u50[] = {0x17,13,'5','0','0','1','0','1','0','0','0','0','0','0','Z'};
      check(der_time(u50, sizeof u50, t) && t == -631152000LL, "UTCTime 50 -> 1950");
      const uint8_t g[] = {0x18,19,'2','0','2','0','0','2','2','5','2','2','0','3','3','8','.','1','2','3','Z'};
      check(der_time(g, sizeof g, t) && t == 1582668218LL, "GeneralizedTime avec fraction"); }

    // ---- PE ---------------------------------------------------------------------------
    const std::vector<uint8_t> pe(fixture::signed_pe, fixture::signed_pe + sizeof fixture::signed_pe);
    { std::vector<uint8_t> dg; std::string why;
      check(pe_image_digest(pe, HASH_SHA256, dg, &why) && dg.size() == 32 &&
            std::memcmp(dg.data(), fixture::pe_digest, 32) == 0, "condensat Authenticode == implementation python"); }

    const int64_t JUIN_2024 = 1717200000LL, JUIN_2026 = 1780272000LL;
    VerifyOptions opt; VerifyReport rep;
    opt.now = JUIN_2024;
    check_hr(verify_pe(pe, opt, rep), CERT_E_CHAINING, "racine de test absente du magasin -> chaine partielle");
    { VerifyOptions ho = opt; ho.hash_only = true;
      check_hr(verify_pe(pe, ho, rep), 0, "WTD_HASH_ONLY_FLAG : condensat seul"); }
    check(add_trusted_root(fixture::test_root, sizeof fixture::test_root), "ajout de la racine de test");
    check_hr(verify_pe(pe, opt, rep), 0, "TEMOIN : racine ajoutee, dans la validite -> 0");
    check(rep.chain.size() == 3 && rep.chain[2] == "Test Root", "chaine feuille -> intermediaire -> racine");
    check(rep.image_alg == HASH_SHA256 && !rep.timestamped, "SHA-256, sans horodatage");
    opt.now = JUIN_2026;
    check_hr(verify_pe(pe, opt, rep), CERT_E_EXPIRED, "sans horodatage, feuille expiree a la date courante");
    { VerifyOptions ro = opt; ro.now = JUIN_2024; ro.revocation_requested = true;
      check_hr(verify_pe(pe, ro, rep), 0, "revocation demandee, hors ligne, politique par defaut -> 0");
      check(rep.revocation_unchecked, "... et le rapport dit que la revocation n'a pas ete verifiee");
      ro.strict_revocation = true;
      check_hr(verify_pe(pe, ro, rep), CERT_E_REVOCATION_FAILURE, "revocation stricte hors ligne -> CERT_E_REVOCATION_FAILURE"); }
    opt.now = JUIN_2024;
    { auto t = pe; t[fixture::text_off + 0x40] ^= 1;
      check_hr(verify_pe(t, opt, rep), TRUST_E_BAD_DIGEST, "1 octet de .text -> TRUST_E_BAD_DIGEST"); }
    { auto t = pe; t[0x58 + 64] ^= 0x5a;
      check_hr(verify_pe(t, opt, rep), 0, "checksum PE modifie -> toujours 0 (champ exclu)"); }
    { auto t = pe; t[t.size() - 20] ^= 1;          // dans la signature RSA (derniers octets du PKCS#7)
      check_hr(verify_pe(t, opt, rep), TRUST_E_CERT_SIGNATURE, "1 octet de signature -> TRUST_E_CERT_SIGNATURE"); }
    { std::vector<uint8_t> t(pe.begin(), pe.begin() + 0x400);   // PE sans table de certificats
      t[0x58 + 128] = t[0x58 + 129] = t[0x58 + 132] = t[0x58 + 133] = 0;
      check_hr(verify_pe(t, opt, rep), TRUST_E_NOSIGNATURE, "PE non signe -> TRUST_E_NOSIGNATURE"); }
    { std::vector<uint8_t> t(pe.begin(), pe.begin() + 0x420);   // repertoire de securite tronque
      check_hr(verify_pe(t, opt, rep), TRUST_E_NOSIGNATURE, "repertoire tronque -> TRUST_E_NOSIGNATURE");
      check(rep.last_error != TRUST_E_NOSIGNATURE, "... avec un GetLastError distinct (signature presente mais illisible)"); }
    { std::vector<uint8_t> t = {'h','e','l','l','o'};
      check_hr(verify_pe(t, opt, rep), TRUST_E_SUBJECT_FORM_UNKNOWN, "non-PE -> TRUST_E_SUBJECT_FORM_UNKNOWN"); }

    // ---- Fichiers reels optionnels ------------------------------------------------------
    if (const char* dir = std::getenv("AC_REFS")) {
        for (const char* f : {"CheckRevision.dll", "Game.exe"}) {
            std::string p = std::string(dir) + "/" + f;
            std::ifstream in(p, std::ios::binary); if (!in) continue;
            std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), {});
            VerifyOptions o; VerifyReport r;
            uint32_t hr = verify_pe(b, o, r);
            std::printf("  [AC_REFS] %s : 0x%08x (%s) horodate=%d\n", f, (unsigned)hr, r.detail.c_str(), (int)r.timestamped);
            for (auto& s : r.chain) std::printf("      signataire : %s\n", s.c_str());
            for (auto& s : r.ts_chain) std::printf("      horodateur : %s\n", s.c_str());
            // MESURE WINDOWS du 2026-09-13 : Get-AuthenticodeSignature = Valid pour les
            // deux fichiers. Game.exe en est la preuve qui coupe : son horodateur
            // remonte a Thawte Timestamping CA, desactivee par Microsoft en 2023.
            check_hr(hr, AC_OK, (std::string(f) + " : Valid comme sous Windows").c_str());
            if (std::string(f) == "Game.exe") {
                VerifyOptions l; l.lifetime_signing = true; VerifyReport lr;   // chaines jugees a la date courante
                check(verify_pe(b, l, lr) != AC_OK, "Game.exe juge a la date courante -> refuse (racine desactivee, certificats expires)");
                std::vector<uint8_t> t = b; t[0x1000] ^= 1;
                check_hr(verify_pe(t, o, lr), TRUST_E_BAD_DIGEST, "Game.exe 1 bit modifie -> TRUST_E_BAD_DIGEST");
            }
        }
    }

    std::printf("authenticode : %d verifications, %d echec(s)\n", checks, fails);
    return fails ? 1 : 0;
}
