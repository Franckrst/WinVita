// src/platform/vita_gpumem.h — memoire GPU de la console (blocs noyau + GXM).
//
// TROISIEME CATEGORIE. Ce fichier n'est ni generique ni propre a un jeu : il est
// propre a la CONSOLE, et le moteur cible cette console. Allouer un bloc noyau,
// le mapper pour le GPU, le mapper dans l'espace USSE — rien la-dedans ne
// depend du jeu qu'on porte.
//
// POURQUOI ICI PLUTOT QUE CHEZ UN PORTAGE
// ---------------------------------------
// L'idiome est partage par les deux portages reels, constate avant d'ecrire ce
// fichier et non suppose :
//   * l'un alloue ses tampons GXM (sommets, indices, profondeur, carrousel
//     d'affichage) en preferant la CDRAM et en retombant sur la RAM utilisateur ;
//   * l'autre alloue son tampon d'affichage exactement de la meme facon —
//     CDRAM d'abord, repli RAM principale, et un journal a chaque echec.
// Les deux ont la meme raison d'etre aussi bavards : sur cette console la RAM
// utilisateur libre se compte en unites de Mio, et un pointeur nul rendu en
// silence donne un ecran noir eternel qu'aucune trace n'explique.
//
// CE QUI N'EST PAS ICI
// --------------------
// Le choix des TAILLES, celui des formats, et ce qu'on met dans les blocs
// appartiennent a l'appelant : ce sont des decisions du portage. Ce fichier ne
// sait qu'obtenir de la memoire et la rendre.
//
// DEPENDANCE DE LIEN. Les corps referencent sceGxm*. Ils vivent dans une
// ARCHIVE : un portage qui n'appelle aucune de ces fonctions n'attire pas
// l'objet, donc n'a pas besoin de lier le stub GXM. C'est ce qui permet a ce
// fichier de cohabiter avec un portage qui passe par une couche OpenGL.
#pragma once
#include <cstdint>

#ifdef __vita__
#include <psp2/kernel/sysmem.h>
#include <psp2/gxm.h>

namespace wx86 {
namespace vita {

// Un bloc memoire visible du GPU. `uid` < 0 = bloc vide.
struct GpuBlock {
    SceUID   uid   = -1;
    void*    p     = nullptr;
    uint32_t size  = 0;
    bool     cdram = false;
};

inline uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1u) & ~(a - 1u); }

// Alloue un bloc du TYPE demande et le mappe pour le GPU.
//
// CHAQUE code de retour est lu et dit : un gel console sans trace ne permet pas
// de savoir si une allocation avait echoue et si l'on continuait avec un
// pointeur nul. En cas d'echec, le journal nomme l'appel exact et son rc, et la
// fonction rend false — a l'appelant de s'arreter.
bool gpu_alloc(GpuBlock& b, SceKernelMemBlockType type, uint32_t size,
               uint32_t attribs, const char* name);

// Alloue la ou c'est le mieux : CDRAM si elle est preferee (defaut), repli sur
// la RAM utilisateur non cachee. La CDRAM est la memoire du GPU et elle est
// abondante quand la RAM utilisateur ne l'est pas.
bool gpu_alloc_best(GpuBlock& b, uint32_t size, uint32_t attribs, const char* name);

// Preference CDRAM. Un portage la coupe pour reproduire un temoin d'avant.
//
// Le LECTEUR existe pour une raison precise : sans lui, un portage qui veut
// afficher son reglage garde SA copie du booleen, et deux sources de verite
// pour un seul etat finissent toujours par diverger. La preference vit ici et
// nulle part ailleurs.
void gpu_prefer_cdram(bool on);
bool gpu_prefers_cdram();

// Memoire USSE. Le SDK n'expose pas les helpers d'allocation USSE des exemples
// officiels (ce sont des fonctions DES EXEMPLES, pas de la bibliotheque) : il
// faut allouer le bloc soi-meme puis le mapper dans l'espace USSE.
// ⚠️ L'USSE n'accepte PAS la CDRAM — ces blocs restent en RAM utilisateur.
bool usse_alloc(GpuBlock& b, uint32_t size, bool fragment, unsigned int* offset,
                const char* name);

void gpu_free(GpuBlock& b);

// Memoire libre, en Kio. Sur cette console le chiffre EST le diagnostic : il
// dit a quelle etape d'initialisation on a manque de place.
void mem_free_kb(int* user_kb, int* cdram_kb);

}  // namespace vita
}  // namespace wx86

#endif  // __vita__
