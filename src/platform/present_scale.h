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

// Format des pixels SOURCE. Ajouter un format se fait ICI, pas chez le portage.
//
// !! LA LISTE A ETE ECRITE SUR LE SEUL PREMIER CONSOMMATEUR, ET ELLE ETAIT
// INCOMPLETE. Elle affirmait « les deux seuls formats que les DIB Windows des
// portages presentent aujourd'hui » — et c'etait faux au moment ou c'etait
// ecrit : le second consommateur presente un DIB 16 bits, et c'est son chemin
// PAR DEFAUT, pas un cas de repli. Mesure du 2026-09-12 sur son banc qemu :
//     [ddraw] SetDisplayMode 800x600x16
//     [gdi]   blit: ... DIB 1024x768@16 — surface PRESENTEE
// La generalite d'un moteur ne se decrete pas depuis un seul appelant ; elle se
// constate sur le second. Cette enumeration est la trace de la correction.
enum class SrcFormat {
    Pal8,    // 8 bits indexes + palette B,G,R,0 (l'ordre des DIB Windows)
    Bgra32,  // 32 bits 0x00RRGGBB en memoire, soit B,G,R,X
    // 16 bits 5-5-5, bit 15 ignore. « biBitCount = 16 avec biCompression =
    // BI_RGB » vaut 555 PAR DEFINITION Win32 — et c'est MESURE sur le DIB du
    // second consommateur : sur les 209 valeurs 16 bits distinctes d'une image
    // de jeu, le bit 15 n'est JAMAIS pose, ce qu'une lecture en 565
    // n'expliquerait pas. L'elargissement de 5 a 8 bits est « x<<3 | x>>2 »,
    // qui envoie 31 sur 255 ; un simple « <<3 » plafonnerait a 248 et delaverait
    // toute l'image. Ce n'est pas un detail de gout : c'est un ecart visible.
    Rgb555,
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
