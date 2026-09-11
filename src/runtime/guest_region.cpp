// src/runtime/guest_region.cpp — voir guest_region.h.
//
// Corps repris de la struct RegionAlloc de tools/rt_boot.cpp (d2vita),
// deplace ici le 2026-09-11 : decouper une plage d'adresses invitees est un
// primitif du moteur, pas du jeu. La logique d'allocation, de liberation et
// de fusion est identique a l'octet pres ; seule change la sortie du
// diagnostic d'epuisement — rappel de l'embarqueur au lieu d'un appel direct
// a fprintf + d2_crashlog, que le moteur ne connait pas.
//
// Les instances (le tas invite, l'arene VA) restent chez l'embarqueur : ce
// fichier ne declare AUCUNE region globale. C'est deliberé. Trois fois cette
// semaine, un deplacement a laisse le type au moteur et la table chez le
// portage, le moteur en recevant une seconde, vide — carte des sections
// critiques, table de handles, compteur d'identifiants. Une region globale
// ici que personne n'alimente serait exactement le meme piege : invisible a
// la compilation, et mortelle le jour ou un appelant s'en sert.
#include "runtime/guest_region.h"

namespace wx86 {

namespace { RegionFailFn g_fail = nullptr; }

void region_set_fail_handler(RegionFailFn cb) { g_fail = cb; }

void GuestRegion::record_fail(uint32_t n) {
    if (!g_fail) return;
    g_fail(RegionFail{ name_, base_, n, cur_, peak_, largest_free(), used_bytes() });
}

} // namespace wx86
