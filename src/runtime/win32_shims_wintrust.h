// src/runtime/win32_shims_wintrust.h — WINTRUST.dll!WinVerifyTrust et la
// tranche de CRYPT32.dll qui lit une signature Authenticode embarquee
// (CryptQueryObject, CryptMsgGetParam, CryptMsgClose,
// CertFindCertificateInStore, CertGetNameStringW, CertFreeCertificateContext,
// CertCloseStore).
//
// HONNETES PAR CONSTRUCTION. Chaque reponse est CALCULEE sur les octets du
// fichier demande par runtime/authenticode.cpp : une vraie verification de
// condensat, de signatures RSA et de chaine jusqu'a une racine de confiance.
// Fichier modifie, signature alteree, chaine non ancree : les API ECHOUENT, avec
// les codes et GetLastError de Windows. Les structures rendues (CERT_CONTEXT,
// CERT_INFO, CMSG_SIGNER_INFO) sont de VRAIES structures x86 en memoire invitee,
// remplies depuis le DER du certificat et du SignerInfo.
//
// CE QUE LE CONSOMMATEUR FOURNIT : la resolution d'un chemin INVITE (celui que
// le programme passe, p. ex. « C:\Jeu\module.dll ») vers les octets du fichier
// reel — meme partage des roles que Bridge::set_version_resource_source. Sans
// source, toute demande echoue comme un fichier introuvable.
//
// ALLOCATION. Les CERT_CONTEXT rendus vivent dans le brouillon invite
// (guest_scratch.h), UN par certificat distinct (cle = octets DER), reutilise
// ensuite : la memoire consommee est bornee par le nombre de certificats
// differents, pas par le nombre d'appels. CertFreeCertificateContext ne libere
// donc rien (sur Windows le pointeur devient invalide ; ici il reste lisible —
// seule difference, et elle ne change aucune reponse). Les HCERTSTORE/HCRYPTMSG
// sont des poignees OPAQUES (jamais dereferencees par un appelant Win32
// correct), valides jusqu'a leur fermeture.
//
// NON IMPLEMENTE (echec explicite, jamais une fausse reussite) :
//   * WinVerifyTrust : actions autres que WINTRUST_ACTION_GENERIC_VERIFY_V2
//     -> TRUST_E_PROVIDER_UNKNOWN ; dwUnionChoice autre que WTD_CHOICE_FILE, ou
//     fichier designe seulement par hFile -> HRESULT_FROM_WIN32(
//     ERROR_CALL_NOT_IMPLEMENTED) = 0x80070078 ;
//   * CryptQueryObject : objets BLOB et contenus autres que
//     CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED -> FALSE,
//     ERROR_CALL_NOT_IMPLEMENTED ;
//   * CryptMsgGetParam : parametres autres que TYPE, INNER_CONTENT_TYPE,
//     SIGNER_COUNT, SIGNER_INFO, SIGNER_CERT_INFO, CERT_COUNT, CERT
//     -> FALSE, CRYPT_E_INVALID_MSG_TYPE ;
//   * CertFindCertificateInStore : recherches autres que ANY, SHA1_HASH,
//     SUBJECT_CERT -> NULL, ERROR_CALL_NOT_IMPLEMENTED ;
//   * CertGetNameStringW : types autres que ATTR_TYPE, SIMPLE_DISPLAY,
//     FRIENDLY_DISPLAY -> chaine vide (1) et un signalement a l'observateur.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace d2rt { class Bridge; class Cpu; struct Shim; }

// Source des octets d'un fichier designe par un chemin INVITE (UTF-8). Rend
// false si le fichier n'existe pas. UN SEUL enregistrement : un second appel
// remplace le premier.
typedef std::function<bool(const std::string& guestPath, std::vector<uint8_t>& bytes)> Wx86WintrustFileSource;
void wx86_wintrust_set_file_source(Wx86WintrustFileSource src);

// Horloge « maintenant » (secondes Unix UTC) pour juger une signature NON
// horodatee. Defaut : std::time(nullptr).
void wx86_wintrust_set_clock(int64_t (*now)());

// Observateur : une ligne par appel qui DECIDE quelque chose (chemin, code,
// raison). Le moteur est muet par construction ; au consommateur d'en faire
// un journal. Non arme = silence.
typedef void (*Wx86WintrustObserverFn)(const char* line);
void wx86_wintrust_set_observer(Wx86WintrustObserverFn cb);

// Enregistre les shims aupres d'un receveur quelconque (install() passe
// Bridge::register_shim ; un banc hote passe le sien, sans pont ni CPU reel).
void wx86_wintrust_register(const std::function<void(const char* dll, const char* name, const d2rt::Shim&)>& reg);

void win32_shims_wintrust_install(d2rt::Bridge& br);
