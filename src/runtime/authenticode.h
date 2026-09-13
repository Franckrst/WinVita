// src/runtime/authenticode.h — verification Authenticode REELLE d'un fichier PE.
//
// POURQUOI C'EST DU MOTEUR. Un programme Win32 qui demande « ce fichier est-il
// signe, et par qui ? » (WinVerifyTrust, CryptQueryObject...) attend du SYSTEME
// une reponse calculee. Ici le systeme n'existe pas : le moteur l'incarne. Une
// reponse constante (« oui, signe par X ») est une reponse FABRIQUEE ; ce
// fichier la remplace par le calcul que Windows fait, et qui ECHOUE comme
// Windows quand le fichier ou sa signature ont ete touches.
//
// CE QUI EST VERIFIE (dans cet ordre, cf. verify_pe) :
//   1. le repertoire de securite du PE (WIN_CERTIFICATE, revision 2.0, type
//      PKCS_SIGNED_DATA) ;
//   2. le decodage DER du PKCS#7 SignedData, contenu SpcIndirectDataContent
//      (1.3.6.1.4.1.311.2.1.4), un signataire ;
//   3. les attributs authentifies du signataire : contentType, puis
//      messageDigest == H(contenu signe) ;
//   4. la signature RSA PKCS#1 v1.5 des attributs authentifies, avec la cle du
//      certificat signataire (trouve par emetteur + numero de serie) ;
//   5. le condensat Authenticode du PE (SHA-1 ou SHA-256, selon le
//      SpcIndirectDataContent) — checksum, entree du repertoire de securite et
//      table des certificats EXCLUS — compare au condensat signe ;
//   6. l'horodatage s'il existe : contre-signature PKCS#9 (1.2.840.113549.1.9.6)
//      ou jeton RFC 3161 Microsoft (1.3.6.1.4.1.311.3.3.1) ; son empreinte doit
//      couvrir la signature chiffree du signataire et sa propre signature RSA
//      doit etre bonne ;
//   7. la chaine du signataire ET celle de l'horodateur, chaque signature de
//      certificat verifiee, jusqu'a une racine de CONFIANCE (embarquee ou
//      ajoutee par le consommateur), chaque certificat valide A LA DATE
//      D'EVALUATION : la date d'horodatage s'il y en a un, la date courante
//      sinon (politique Authenticode de Windows, hors « lifetime signing ») ;
//   8. l'usage : Code Signing (1.3.6.1.5.5.7.3.3) pour la feuille signataire,
//      Time Stamping (1.3.6.1.5.5.7.3.8) pour la feuille horodateur, quand
//      l'extension EKU est presente ; basicConstraints CA pour les
//      intermediaires.
//
// CE QUI N'EST PAS FAIT, ET CE QUI EST RENDU A LA PLACE :
//   * Revocation (CRL/OCSP) : aucun acces reseau ici. Si l'appelant ne demande
//     pas de revocation (WTD_REVOKE_NONE ou WTD_REVOCATION_CHECK_NONE), rien a
//     faire. S'il en demande une, Windows hors ligne obtient un statut
//     « revocation inconnue / hors ligne » que la politique Authenticode par
//     defaut (State\PolicyFlags = 0x23C00 : WTPF_OFFLINEOK_IND|_COM et
//     WTPF_OFFLINEOKNBU_IND|_COM) ACCEPTE : on rend donc le meme succes, et le
//     rapport le dit (VerifyReport::revocation_unchecked). L'option
//     strict_revocation rend a la place CERT_E_REVOCATION_FAILURE (0x800B010E),
//     ce que Windows rend quand ces bits de politique sont retires.
//   * Magasins « Disallowed », CTL de mise a jour automatique des racines,
//     politique de noms, liste de revocation des racines : absents.
//   * Signatures imbriquees (1.3.6.1.4.1.311.2.4.1) : ignorees, comme
//     WinVerifyTrust sans WSS_VERIFY_SEALING / sans parcours des signatures
//     secondaires (seule la signature primaire decide).
//
// AUCUNE DEPENDANCE : SHA-1, SHA-256, DER, X.509 et l'exponentiation modulaire
// RSA sont ici (cible PS Vita, newlib). Aucun %zu : le snprintf embarque ne le
// substitue pas.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wx86 { namespace ac {

// ---- Codes Windows (HRESULT) rendus par la verification ---------------------
enum : uint32_t {
    AC_OK                        = 0x00000000u,
    TRUST_E_PROVIDER_UNKNOWN     = 0x800B0001u,
    TRUST_E_ACTION_UNKNOWN       = 0x800B0002u,
    TRUST_E_SUBJECT_FORM_UNKNOWN = 0x800B0003u,
    TRUST_E_NOSIGNATURE          = 0x800B0100u,
    CERT_E_EXPIRED               = 0x800B0101u,
    CERT_E_UNTRUSTEDROOT         = 0x800B0109u,
    CERT_E_CHAINING              = 0x800B010Au,
    CERT_E_REVOCATION_FAILURE    = 0x800B010Eu,
    CERT_E_WRONG_USAGE           = 0x800B0110u,
    TRUST_E_NO_SIGNER_CERT       = 0x80096002u,
    TRUST_E_CERT_SIGNATURE       = 0x80096004u,
    TRUST_E_TIME_STAMP           = 0x80096005u,
    TRUST_E_BAD_DIGEST           = 0x80096010u,
    NTE_BAD_SIGNATURE            = 0x80090006u,
    NTE_BAD_ALGID                = 0x80090008u,
    CRYPT_E_HASH_VALUE           = 0x80091007u,
    CRYPT_E_UNEXPECTED_MSG_TYPE  = 0x8009200Au,
    CRYPT_E_NO_MATCH             = 0x80092009u,
    CRYPT_E_ASN1_BADTAG          = 0x8009310Bu,
    CRYPT_E_ASN1_EOD             = 0x80093102u,
    CRYPT_E_ASN1_CORRUPT         = 0x80093103u,
};

// ---- Condensats -------------------------------------------------------------
enum HashAlg { HASH_NONE = 0, HASH_SHA1 = 1, HASH_SHA256 = 2 };
size_t hash_len(HashAlg a);                     // 20, 32, 0

struct Sha1   { uint32_t h[5]; uint64_t n; uint8_t buf[64]; size_t fill;
                void init(); void update(const uint8_t* p, size_t len); void final(uint8_t out[20]); };
struct Sha256 { uint32_t h[8]; uint64_t n; uint8_t buf[64]; size_t fill;
                void init(); void update(const uint8_t* p, size_t len); void final(uint8_t out[32]); };
struct Hasher {
    HashAlg alg = HASH_NONE; Sha1 s1; Sha256 s2;
    void init(HashAlg a);
    void update(const uint8_t* p, size_t len);
    size_t final(uint8_t* out);                  // rend la longueur ecrite
};
void sha1(const uint8_t* p, size_t n, uint8_t out[20]);
void sha256(const uint8_t* p, size_t n, uint8_t out[32]);

// ---- RSA --------------------------------------------------------------------
// Operation publique brute : out = sig^e mod n, sur nlen octets (grand-boutiste).
// Rend false si sig >= n, n pair, ou tailles hors bornes (n <= 8192 bits).
bool rsa_public(const uint8_t* n, size_t nlen, const uint8_t* e, size_t elen,
                const uint8_t* sig, size_t siglen, std::vector<uint8_t>& out);
// Verification PKCS#1 v1.5 (EMSA-PKCS1-v1_5, DigestInfo complet exige).
bool rsa_pkcs1v15_verify(const uint8_t* n, size_t nlen, const uint8_t* e, size_t elen,
                         const uint8_t* sig, size_t siglen,
                         HashAlg alg, const uint8_t* digest, size_t dlen);

// ---- DER ----------------------------------------------------------------------
// Tranche d'un tampon : decalage + longueur. Jamais de pointeur, pour qu'une
// structure reste valide quand son tampon proprietaire est copie.
struct Span { size_t off = 0, len = 0; bool empty() const { return len == 0; } };

struct Tlv {
    uint8_t tag = 0;
    Span    all;    // en-tete + valeur
    Span    val;    // valeur seule
};
// Lit un TLV DER a buf[pos], borne a end. Formes definies uniquement.
bool der_read(const uint8_t* buf, size_t pos, size_t end, Tlv& t);
std::string oid_to_string(const uint8_t* p, size_t n);   // "1.2.840..." ; "" si mal forme

// ---- X.509 --------------------------------------------------------------------
struct Extension { std::string oid; bool critical = false; Span value; };  // value = contenu de l'OCTET STRING

struct Certificate {
    std::vector<uint8_t> der;
    Span tbs;                    // TLV complet du TBSCertificate (ce qui est signe)
    uint32_t version = 0;        // 0 = v1, 2 = v3 (valeur de l'INTEGER)
    Span serial;                 // contenu de l'INTEGER, grand-boutiste, tel qu'encode
    std::string tbsSigAlgOid;    Span tbsSigAlgParams;   // params = TLV complet (ou vide)
    Span issuer, subject;        // TLV complets des Name
    int64_t notBefore = 0, notAfter = 0;                 // secondes Unix UTC
    Span spki;                   // TLV complet SubjectPublicKeyInfo
    std::string spkiAlgOid;      Span spkiAlgParams;
    Span pubKey;                 // contenu du BIT STRING sans l'octet de bits inutilises
    uint8_t pubKeyUnusedBits = 0;
    Span rsaN, rsaE;             // contenus des INTEGER (zeros de tete retires)
    std::vector<Extension> exts;
    std::string sigAlgOid;       // AlgorithmIdentifier externe
    Span sig;                    // signature (BIT STRING sans l'octet de bits inutilises)
    bool hasBasicConstraints = false, isCA = false;
    bool hasEku = false;         std::vector<std::string> eku;

    const uint8_t* p(const Span& s) const { return der.data() + s.off; }
    bool parse(const uint8_t* d, size_t n);   // copie les octets
};

// Premier attribut `oid` d'un Name (TLV complet) converti en UTF-16 ; false si absent.
bool name_find_attr(const uint8_t* name, size_t n, const std::string& oid, std::u16string& out);
// Chaine d'un attribut de DirectoryString (Printable/IA5/UTF8/BMP/T61...) -> UTF-16.
bool directory_string_to_u16(const uint8_t* tlv, size_t n, std::u16string& out);

// ---- PKCS#7 ---------------------------------------------------------------------
struct Attribute { std::string oid; std::vector<Span> values; };   // values = TLV complets

struct SignerInfo {
    uint32_t version = 0;
    Span issuer;                 // TLV complet du Name
    Span serial;                 // contenu de l'INTEGER, grand-boutiste
    std::string digestAlgOid;    Span digestAlgParams;
    Span authAttrsTlv;           // [0] IMPLICIT complet (vide si absent)
    std::vector<Attribute> authAttrs;
    std::string encAlgOid;       Span encAlgParams;
    Span encryptedDigest;        // contenu de l'OCTET STRING
    std::vector<Attribute> unauthAttrs;
};

struct SignedData {
    std::vector<uint8_t> buf;    // ContentInfo complet ; toutes les Span y renvoient
    uint32_t version = 0;
    std::string contentType;     // eContentType
    Span content;                // TLV complet du contenu interne (sous le [0] EXPLICIT)
    std::vector<Certificate> certs;
    std::vector<SignerInfo> signers;
    const uint8_t* p(const Span& s) const { return buf.data() + s.off; }
    // Decode un ContentInfo{signedData}. En cas d'echec, *err = code CRYPT_E_*.
    bool parse(const uint8_t* d, size_t n, uint32_t* err);
};
// Decode un SignerInfo situe dans `buf` (utilise aussi pour les contre-signatures).
bool parse_signer_info(const uint8_t* buf, const Tlv& t, SignerInfo& si);

// ---- PE -------------------------------------------------------------------------
// Extrait le PKCS#7 de la premiere WIN_CERTIFICATE PKCS_SIGNED_DATA.
// *err : TRUST_E_SUBJECT_FORM_UNKNOWN (pas un PE), TRUST_E_NOSIGNATURE (pas de
// signature), CRYPT_E_ASN1_* (repertoire corrompu).
bool pe_extract_pkcs7(const std::vector<uint8_t>& file, std::vector<uint8_t>& pkcs7, uint32_t* err);
// Condensat Authenticode (Microsoft PE/COFF, « Calculating the PE Image Hash »).
bool pe_image_digest(const std::vector<uint8_t>& file, HashAlg alg, std::vector<uint8_t>& out, std::string* why);

// ---- Magasin de confiance -------------------------------------------------------
// Racines embarquees (authenticode_roots.h) + racines ajoutees. Ajouter une
// racine revient a la poser dans le magasin « Root » d'un Windows : c'est une
// DECISION de l'embarqueur, qu'il doit documenter. Rend false si le DER est
// illisible. Idempotent (meme DER ajoute deux fois = une entree).
bool add_trusted_root(const uint8_t* der, size_t n);
size_t trusted_root_count();

// ---- Verification -------------------------------------------------------------------
struct VerifyOptions {
    int64_t now = 0;                   // secondes Unix ; 0 = std::time(nullptr)
    bool revocation_requested = false; // fdwRevocationChecks != NONE et pas WTD_REVOCATION_CHECK_NONE
    bool strict_revocation = false;    // voir en-tete : Windows sans WTPF_OFFLINEOK_*
    bool hash_only = false;            // WTD_HASH_ONLY_FLAG : condensat de l'image seulement
    bool lifetime_signing = false;     // WTD_LIFETIME_SIGNING_FLAG : chaines jugees a `now` meme horodatees
};
struct VerifyReport {
    uint32_t hr = 0;                   // code rendu (= retour de verify_pe)
    uint32_t last_error = 0;           // GetLastError que Windows poserait
    std::string detail;                // phrase humaine (journal)
    HashAlg image_alg = HASH_NONE;
    bool timestamped = false;
    int64_t timestamp = 0;             // secondes Unix de l'horodatage
    int64_t evaluated_at = 0;          // date a laquelle les chaines ont ete jugees
    int signer_cert = -1;              // indice dans SignedData::certs
    std::vector<std::string> chain;    // sujets (CN) de la feuille a la racine
    std::vector<std::string> ts_chain;
    bool revocation_unchecked = false;
};
// Verifie `file`. Rend le HRESULT de WinVerifyTrust (0 = confiance).
uint32_t verify_pe(const std::vector<uint8_t>& file, const VerifyOptions& opt, VerifyReport& rep);

// Temps DER (UTCTime 0x17 / GeneralizedTime 0x18) -> secondes Unix.
bool der_time(const uint8_t* tlv, size_t n, int64_t& out);

}} // namespace wx86::ac
