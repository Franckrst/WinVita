// src/runtime/guest_atomics.h — atomiques sur la memoire INVITEE.
//
// Les Interlocked* de KERNEL32 doivent etre reellement atomiques vis-a-vis
// des autres fils invites. Cela demande un pointeur HOTE sur la case invitee
// (Cpu::hostptr) et le respect du contrat SMC du dynarec (une page qui porte
// du code traduit doit etre salie avant ecriture) — deux choses que seul le
// moteur sait faire. Ce module les porte donc, au lieu que chaque portage en
// recopie sa version : les deux portages existants avaient jusqu'ici le MEME
// code, au commentaire pres.
//
// wx86_ilk_ptr() rend nullptr quand la case n'est pas atomisable (non alignee,
// ou sans pointeur hote) : l'appelant retombe alors sur un read/write invite
// non atomique, ce qui est le comportement d'origine.
#pragma once
#include <cstdint>
namespace d2rt { class Cpu; }

uint32_t* wx86_ilk_ptr(d2rt::Cpu& c, uint32_t va);

// Ordre memoire : RELAXED par defaut. Sur une cible ou tous les fils invites
// sont epingles sur le meme cœur, la barriere MATERIELLE est acquise et seul
// le COMPILATEUR doit etre empeche de reordonner. WX86_ILK_SEQCST=1 (ou
// D2_ILK_SEQCST=1, ancien nom) retablit la barriere complete.
bool wx86_ilk_seqcst();

uint32_t wx86_ilk_add_fetch  (uint32_t* h, uint32_t v);
uint32_t wx86_ilk_sub_fetch  (uint32_t* h, uint32_t v);
uint32_t wx86_ilk_exchange   (uint32_t* h, uint32_t v);
uint32_t wx86_ilk_fetch_add  (uint32_t* h, uint32_t v);
uint32_t wx86_ilk_fetch_or   (uint32_t* h, uint32_t v);
uint32_t wx86_ilk_fetch_and  (uint32_t* h, uint32_t v);
uint32_t wx86_ilk_fetch_xor  (uint32_t* h, uint32_t v);
uint32_t wx86_ilk_cas        (uint32_t* h, uint32_t expected, uint32_t desired);
void     wx86_ilk_fence();

// Compteurs de diagnostic : le portage les affiche dans son propre rapport de
// fin de run, il faut donc pouvoir les lire.
void wx86_ilk_stats(unsigned long long* atomic,
                    unsigned long long* fallback,
                    unsigned long long* unaligned);
