// src/platform/present_scale.h — mise a l'echelle d'une image invitee vers le
// tampon d'affichage de l'hote.
//
// POURQUOI CE FICHIER EXISTE
// --------------------------
// Deux portages consommateurs du moteur ont ecrit la MEME fonction, chacun de
// son cote, et l'ont fait diverger : l'expansion de palette 8 bits, la mise a
// l'echelle 16.16 avec duplication de lignes, et l'echange de tampon. Les deux
// corps ont ete compares ligne a ligne : l'un est un SUR-ENSEMBLE STRICT de
// l'autre (il ajoute le cadrage proportionnel), et le cas du premier est
// exactement le cas degenere du second — destination = ecran entier.
//
// Deux implementations independantes qui convergent sont la meilleure preuve de
// genericite qui existe : ce n'est pas une abstraction inventee a l'avance, mais
// la forme commune constatee apres coup.
//
// CE QUE CETTE FONCTION FAIT, ET NE FAIT PAS
// ------------------------------------------
// Elle convertit et met a l'echelle. Elle ne dessine AUCUNE incrustation (le
// compteur d'images, le clavier a l'ecran) et ne presente RIEN : ces deux
// etapes suivent, chez l'appelant, parce qu'elles dependent de la cible.
// Le remplissage des bandes noires n'est PAS fait ici non plus : il ne doit
// avoir lieu qu'au changement de geometrie, pas a chaque image, et seul
// l'appelant sait quand sa geometrie change.
//
// COUT PAR IMAGE
// --------------
// La fonction est appelee UNE FOIS par image (elle traite l'image entiere), pas
// une fois par pixel : l'appel lui-meme ne se mesure pas. Les boucles internes
// sont transcrites a l'identique de l'existant, y compris le deroulage 4:1 et
// la duplication de ligne par memcpy, pour qu'aucun octet de sortie ne change.
#pragma once
#include <cstdint>

namespace wx86 {

// Format des pixels SOURCE. Ce sont les deux seuls formats que les DIB Windows
// des portages presentent aujourd'hui ; en ajouter un se fait ici, pas chez eux.
enum class SrcFormat {
    Pal8,    // 8 bits indexes + palette B,G,R,0 (l'ordre des DIB Windows)
    Bgra32,  // 32 bits 0x00RRGGBB en memoire, soit B,G,R,X
};

// Rectangle de DESTINATION dans le tampon de l'hote, en pixels.
// Plein ecran = {0, 0, largeur_ecran, hauteur_ecran} : c'est le cas degenere,
// et il redonne octet pour octet le comportement d'un portage qui etire.
struct DstRect { int x, y, w, h; };

// Convertit `src` (srcW x srcH, pas de ligne `srcPitch` en OCTETS) vers `dst`
// (pas de ligne `dstPitch` en PIXELS de 32 bits), mis a l'echelle pour remplir
// exactement `r`. La sortie est en 0xAARRGGBB avec alpha force a 0xFF.
//
// `palette` : 256 quadruplets B,G,R,0. Requis pour Pal8, ignore sinon.
//
// Mise a l'echelle : plus proche voisin en virgule fixe 16.16, avec les lignes
// repetees recopiees depuis la ligne precedente DEJA ECRITE (moins de travail
// qu'un second echantillonnage, et resultat identique par construction).
void scale_blit(uint32_t* dst, int dstPitch, const DstRect& r,
                const uint8_t* src, int srcW, int srcH, int srcPitch,
                SrcFormat fmt, const uint8_t* palette);

// Calcule le rectangle de destination qui conserve les proportions de la source
// et centre l'image dans un ecran de dstW x dstH (bandes noires laterales ou
// horizontales). `stretch` = true rend simplement l'ecran entier.
//
// Vit ici parce que les deux portages en ont besoin des qu'ils cessent
// d'etirer, et que se tromper d'arrondi decale l'image d'un pixel.
DstRect fit_rect(int srcW, int srcH, int dstW, int dstH, bool stretch);

}  // namespace wx86
