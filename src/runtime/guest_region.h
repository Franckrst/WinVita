// src/runtime/guest_region.h — allocateur de REGIONS invitees.
//
// POURQUOI C'EST UN PRIMITIF DU MOTEUR, ET PAS DU CONSOMMATEUR.
// Un programme Win32 alloue et LIBERE : HeapAlloc/HeapFree, VirtualAlloc/
// VirtualFree, LocalAlloc, GlobalAlloc. Sur une machine reelle c'est le
// gestionnaire de tas du systeme qui repond. Ici le systeme n'existe pas :
// c'est le moteur qui l'incarne, donc c'est au moteur de savoir decouper une
// plage d'adresses INVITEES en blocs, les rendre, et les recoudre.
//
// Ce n'est pas le meme besoin que guest_scratch.h, et les deux coexistent :
//   * guest_scratch — UNE plage, allocation DEFINITIVE, jamais de free.
//     Pour les valeurs de retour a duree de vie processus (une chaine rendue
//     par GetCommandLineA, une struct hostent mise en cache).
//   * guest_region (ce fichier) — PLUSIEURS plages independantes, avec free,
//     fusion des blocs voisins et suivi d'occupation. Pour le tas invite et
//     l'arene d'adresses virtuelles, qui vivent tout le long de la session.
//
// Tout portage vers ce moteur a exactement le meme besoin : le laisser chez
// le consommateur obligeait chaque portage a le reecrire. C'est la meme
// erreur de placement que misc() avant son demenagement (guest_scratch).
//
// PARTAGE DES ROLES. Le MOTEUR possede le decoupage (allocation, liberation,
// fusion, mesure). Le CONSOMMATEUR possede les INSTANCES et les plages
// qu'elles couvrent : ou commence le tas invite, quelle taille lui donner,
// combien d'arenes ouvrir — cela depend du plan memoire du portage, qui n'a
// rien d'universel (celui de d2vita est une arene compacte contrainte par la
// console). Le moteur ne declare donc AUCUNE instance globale : il fournit la
// classe, l'embarqueur instancie ce dont il a besoin.
//
// ALIGNEMENT. Chaque region a le sien, fixe a l'init : 16 octets pour un tas
// facon HeapAlloc, une page (0x1000) ou un bloc de reservation (0x10000) pour
// une arene facon VirtualAlloc. C'est le consommateur qui sait lequel, parce
// que c'est lui qui sait ce que le jeu attend de MEM_RESERVE.
#pragma once
#include <cstdint>
#include <map>

namespace wx86 {

// Signalement d'EPUISEMENT. Le moteur est muet par construction : il ne
// connait ni le journal de l'embarqueur, ni sa console, ni son format. Il
// appelle ce rappel a CHAQUE allocation refusee, avec de quoi ecrire une
// ligne utile — et surtout de quoi distinguer « la region est pleine » de
// « la region est fragmentee » : si `largest` est confortable alors que
// `want` est petit, ce n'est pas un manque de place, c'est un emiettement.
// Non arme = refus silencieux, alloc() rend 0 comme toujours.
struct RegionFail {
    const char* name;      // nom de la region, tel que passe a init()
    uint32_t    base;      // adresse invitee du premier octet de la region
    uint32_t    want;      // octets demandes (taille BRUTE, avant alignement)
    uint32_t    cur;       // octets vivants a cet instant
    uint32_t    peak;      // maximum d'octets vivants depuis l'init
    uint32_t    largest;   // plus grand bloc libre d'un seul tenant
    uint32_t    used;      // somme des blocs alloues (recomptee, cf. used_bytes)
};
typedef void (*RegionFailFn)(const RegionFail&);

// Rappel PARTAGE par toutes les regions : l'embarqueur n'a qu'un journal, il
// n'a pas besoin d'un rappel par instance — le champ `name` dit laquelle a
// refuse.
void region_set_fail_handler(RegionFailFn cb);

class GuestRegion {
public:
    // b = adresse INVITEE du premier octet, sz = taille en octets, al =
    // alignement des blocs rendus, nm = nom pour le diagnostic (il doit
    // survivre a la region : un litteral, pas un tampon).
    void init(uint32_t b, uint32_t sz, uint32_t al, const char* nm = "region") {
        name_ = nm; base_ = b; limit_ = b + sz; align_ = al; freeb_[b] = sz;
    }

    // Rend l'adresse INVITEE d'un bloc de n octets, ou 0. Premier ajustement
    // (first-fit) sur la carte des blocs libres, qui est ordonnee par adresse.
    uint32_t alloc(uint32_t n) {
        // (n + align - 1) DEBORDE pour n >= 0xFFFFFFF1 : une demande
        // « negative » reussirait alors avec un bloc minuscule. Refuser tout
        // ce que la region ne peut pas contenir ferme les deux cas d'un coup.
        if (n > (limit_ - base_)) { record_fail(n); return 0; }
        uint32_t sz = (n + align_ - 1) & ~(align_ - 1);
        if (!sz) sz = align_;
        for (auto it = freeb_.begin(); it != freeb_.end(); ++it)
            if (it->second >= sz) {
                uint32_t a = it->first, rem = it->second - sz;
                freeb_.erase(it);
                if (rem) freeb_[a + sz] = rem;
                used_[a] = sz;
                cur_ += sz; if (cur_ > peak_) peak_ = cur_;
                return a;
            }
        record_fail(n);
        return 0;
    }

    // Rend true si a designait bien le debut d'un bloc alloue. Le bloc libere
    // est recousu a son voisin de droite puis a celui de gauche : sans cette
    // fusion, une region qui alloue et libere en boucle finit emiettee en
    // blocs inutilisables alors qu'elle est presque vide.
    bool free(uint32_t a) {
        auto it = used_.find(a); if (it == used_.end()) return false;
        uint32_t sz = it->second; used_.erase(it); cur_ -= sz;
        auto nx = freeb_.lower_bound(a);
        if (nx != freeb_.end() && a + sz == nx->first) { sz += nx->second; nx = freeb_.erase(nx); }
        if (nx != freeb_.begin()) {
            auto pv = std::prev(nx);
            if (pv->first + pv->second == a) { pv->second += sz; return true; }
        }
        freeb_[a] = sz; return true;
    }

    // Base du bloc qui CONTIENT a, 0 si aucun. VirtualFree(MEM_RELEASE) en a
    // besoin : le jeu y passe une adresse interieure, pas forcement le debut.
    uint32_t block_of(uint32_t a) {
        auto it = used_.upper_bound(a); if (it == used_.begin()) return 0; --it;
        return (a >= it->first && a < it->first + it->second) ? it->first : 0;
    }
    uint32_t size_of(uint32_t a) { auto it = used_.find(a); return it == used_.end() ? 0 : it->second; }
    uint32_t largest_free() { uint32_t m = 0; for (auto& p : freeb_) if (p.second > m) m = p.second; return m; }
    // Recompte la somme des blocs alloues. cur() donne la meme chose en O(1) ;
    // used_bytes() reste le releve de reference (il ne peut pas deriver) et
    // sert au diagnostic de fin de session.
    uint32_t used_bytes() { uint32_t t = 0; for (auto& p : used_) t += p.second; return t; }

    uint32_t cur()  const { return cur_; }
    uint32_t peak() const { return peak_; }
    uint32_t base() const { return base_; }
    // Carte des blocs alloues, pour le recensement des gros occupants
    // (« qui tient l'arene ? »). Lecture seule par convention.
    const std::map<uint32_t, uint32_t>& used_map() const { return used_; }

private:
    void record_fail(uint32_t n);

    const char* name_ = "region";
    uint32_t base_ = 0, limit_ = 0, align_ = 16;
    uint32_t cur_ = 0, peak_ = 0;
    std::map<uint32_t, uint32_t> used_, freeb_;   // adresse -> taille
};

} // namespace wx86
