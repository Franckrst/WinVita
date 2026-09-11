// src/runtime/guest_str.h — lecture/ecriture de chaines dans la memoire INVITEE.
//
// Toute API Win32 qui prend ou rend une chaine doit la lire ou l'ecrire a une
// adresse x86 invitee, pas hote. Ces trois helpers sont donc un primitif du
// moteur au meme titre que l'allocateur de brouillon (guest_scratch.h) : le
// premier portage les avait, le second les avait aussi, mot pour mot.
//
// Ils etaient chez le consommateur pour une raison purement historique (tout
// est ne dans son monolithe). Plusieurs shims du moteur portent encore un
// commentaire « reste cote portage a cause de gread_mb » : c'est ce blocage-la
// que ce fichier leve.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace d2rt { class Cpu; }

// Chaine multi-octets invitee. len<0 = jusqu'au zero terminal, sinon len octets.
std::string wx86_gread_mb(d2rt::Cpu& c, uint32_t p, int len);

// Chaine large (UTF-16) invitee. len<0 = jusqu'au zero terminal, sinon len unites.
std::vector<uint16_t> wx86_gread_wc(d2rt::Cpu& c, uint32_t p, int len);

// Ecrit une unite UTF-16 a une adresse invitee.
void wx86_gwrite_wc(d2rt::Cpu& c, uint32_t p, uint16_t w);
