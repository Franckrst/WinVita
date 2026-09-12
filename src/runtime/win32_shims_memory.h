// src/runtime/win32_shims_memory.h — LE PLAN MEMOIRE vu du programme invite :
// tas (Heap/Local/Global), arene d'adresses virtuelles (Virtual*), inventaire
// des regions (VirtualQuery) et description de la machine (GetSystemInfo,
// GlobalMemoryStatus).
//
// POURQUOI C'EST DU MOTEUR. Un programme Win32 alloue et libere ; sur une
// vraie machine c'est le systeme qui repond, ici c'est le moteur qui incarne
// le systeme. Les quinze corps rassembles ici sont IDENTIQUES au caractere
// pres chez les deux consommateurs du moteur (verifies un par un ; le seul
// ecart est le NOM d'un alias d'allocation). Ce n'est pas un raisonnement,
// c'est une constatation.
//
// CE QUI RESTE AU CONSOMMATEUR, ET POURQUOI. Le PLAN lui-meme : ou commence le
// tas, quelle taille lui donner, ou vit l'arene, comment decrire les regions.
// Cela depend de la console et du portage (guest_region.h le dit deja pour
// l'allocateur). Le consommateur le fournit par Wx86MemoryPlan.
//
// L'INSTRUMENTATION NE SUIT PAS LES CORPS. Les compteurs d'occupation, les
// lignes de journal « [VirtualAlloc 4096 KB ...] », le nommage du site fautif
// dans un journal de plantage : rien de tout cela ne DECIDE quoi que ce soit.
// Ce sont des observateurs, et ils passent par wx86_mem_set_observer(). Le
// moteur raconte, il ne demande jamais d'avis — meme regle que l'observateur
// de synchronisation (guest_sync.h) et l'observateur reseau.
#pragma once
#include <cstdint>

namespace d2rt { class Bridge; class Cpu; }
namespace wx86 { class GuestRegion; }

// ---- Remplissage a zero d'une plage INVITEE ---------------------------------
// Vit ici parce que c'est la garantie Win32 de ces shims-la (HEAP_ZERO_MEMORY,
// LMEM_ZEROINIT, MEM_COMMIT) qui l'exige, et parce que les deux portages en
// portaient la meme copie. Passe par hostptr + une barriere d'invalidation de
// code quand c'est possible (strictement equivalent a une boucle de write(),
// moins un appel virtuel et une copie par tranche), repli sur write() sinon.
void wx86_gzero(d2rt::Cpu& c, uint32_t va, uint32_t n);

// ---- Le plan que le consommateur concede ------------------------------------
struct Wx86MemoryPlan {
    // Region servant le tas invite (HeapAlloc/HeapFree/Local*/Global*).
    wx86::GuestRegion* heap = nullptr;
    // Region servant l'arene d'adresses virtuelles (VirtualAlloc/VirtualFree).
    wx86::GuestRegion* va   = nullptr;
    // Description d'une region pour VirtualQuery. Le moteur connait le FORMAT
    // (MEMORY_BASIC_INFORMATION, 28 octets) ; il ne peut pas connaitre le plan.
    // Rend false pour ce que Windows refuse d'introspecter (espace noyau).
    bool (*vquery)(uint32_t addr, uint32_t& base, uint32_t& size, uint32_t& state,
                   uint32_t& type, uint32_t& protect, uint32_t& allocbase) = nullptr;
};

// ---- Observateur -------------------------------------------------------------
enum {
    WX86_MEM_VA_COMMIT_IN = 0,  // MEM_COMMIT a l'interieur d'une reservation existante
    WX86_MEM_VA_RESERVE,        // MEM_RESERVE (hors reservation existante)
    WX86_MEM_VA_COMMIT_FRESH,   // MEM_COMMIT hors de toute reservation
    WX86_MEM_VA_BIG,            // demande >= 1 MiB : le consommateur en fait une ligne
    WX86_MEM_VA_GARBAGE,        // demande >= 2 GiB : valeur manifestement corrompue
    WX86_MEM_VA_FAIL,           // l'arene a refuse
    WX86_MEM_VA_RELEASE,        // MEM_RELEASE honore
    WX86_MEM_VA_RELEASE_MISS,   // MEM_RELEASE tombe a cote : fuite d'arene
    WX86_MEM_VA_DECOMMIT,       // MEM_DECOMMIT (le support reste, un recommit zerotera)
};
struct WxMemEvent {
    int         kind;
    d2rt::Cpu*  cpu;      // CPU au site d'appel
    uint32_t    addr;     // adresse concernee (hint, bloc libere, premiere page)
    uint32_t    size;     // octets concernes
    uint32_t    type;     // dwAllocationType / dwFreeType d'origine
    uint32_t    protect;  // flProtect d'origine (0 quand sans objet)
    uint32_t    ra;       // adresse de retour de l'appelant invite (0 si non lue)
};
typedef void (*WxMemObserverFn)(const WxMemEvent&);
// UN SEUL observateur, comme partout ailleurs : un second appel REMPLACE le
// premier plutot que de s'ajouter en silence.
void wx86_mem_set_observer(WxMemObserverFn cb);

// ---- Memoire physique annoncee ----------------------------------------------
// GlobalMemoryStatus dit au jeu combien de RAM la machine a. Beaucoup de jeux
// dimensionnent leurs caches la-dessus, donc la valeur est une DECISION du
// portage, pas une constante universelle : il la pose ici. Defaut 256 MiB,
// la valeur qu'avaient les deux portages.
void wx86_mem_set_total_phys_mb(uint32_t mb);

void win32_shims_memory_install(d2rt::Bridge& br, const Wx86MemoryPlan& plan);
