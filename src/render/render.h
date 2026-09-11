// src/render/render.h — la couture entre une traduction d'API graphique
// invitee (Glide, Direct3D...) et le GPU de l'hote.
//
// ETAT : PARTIELLE, ET ASSUMEE COMME TELLE.
// Cette couche ne pretend pas couvrir la 3D en general. Elle couvre EXACTEMENT
// ce que deux portages reels soumettent aujourd'hui, parce qu'elle a ete
// deduite de leurs deux backends existants plutot qu'imaginee a l'avance. Ce
// qui manque est liste en bas de fichier, nommement.
//
// POURQUOI ELLE EXISTE
// --------------------
// Deux consommateurs du moteur font de la 3D, chacun avec son backend :
//   * l'un traduit un anneau de commandes Glide vers le GPU de la console, avec
//     un atlas de textures 8 bits palettisees et une palette appliquee dans le
//     nuanceur ;
//   * l'autre traduit un tampon d'execution Direct3D vers une couche OpenGL,
//     avec des textures ARGB1555 et un tampon de profondeur.
// Leurs deux interfaces de backend ont ete comparees champ a champ. Le
// vocabulaire ci-dessous est leur INTERSECTION REELLE, elargie la ou l'un des
// deux avait strictement plus (la profondeur, la palette) plutot que de forcer
// le plus pauvre des deux.
//
// CE QUI RESTE CHEZ LE PORTAGE, ET POURQUOI
// -----------------------------------------
// La traduction de l'API invitee elle-meme (decoder un anneau Glide, decoder un
// tampon d'execution Direct3D, savoir quels etats le jeu emet vraiment) est
// specifique au jeu et le reste. Ce fichier commence APRES ce decodage : il
// parle de sommets, de lots et de textures, jamais d'opcodes.
//
// CE QUI EST « VITA » ET NON « JEU »
// ----------------------------------
// Le GPU de la console, sa couche OpenGL, ses contraintes memoire appartiennent
// au MOTEUR — le moteur cible cette console. Un backend concret vit donc ici,
// pas chez le portage ; seul le decodage de l'API invitee reste la-bas.
#pragma once
#include <cstdint>

namespace wx86 {
namespace render {

// ---- sommet ---------------------------------------------------------------
// Position en ESPACE ECRAN. Les deux portages font leur transformation en
// logiciel (mesure des deux cotes) : il n'y a aucune matrice a poser ici.
//
// `w` porte la correction de perspective : un portage 2D pose w=1 et z=0, un
// portage 3D pose la position PREMULTIPLIEE par w. Passer la position en trois
// composantes donnerait un texturage affine, et les surfaces se tordraient —
// c'est pour ca que le champ existe meme si un des deux portages ne s'en sert
// pas.
//
// `layer` selectionne une palette ou une couche de tableau de textures. Vaut 0
// quand la texture porte deja ses couleurs.
struct Vertex {
    float    x, y, z, w;
    uint32_t rgba;        // octets R,G,B,A dans l'ordre memoire
    float    u, v;
    float    layer;
};

// ---- etats de dessin ------------------------------------------------------
enum class Blend : uint8_t {
    Opaque = 0,
    Alpha,        // src.a / 1-src.a
    Additive,
    Multiply,
};

enum class Filter : uint8_t { Nearest = 0, Bilinear };

// Drapeaux de lot. Bits volontairement stables : ils voyagent dans des traces
// et des journaux de comparaison A/B.
enum KeyFlags : uint8_t {
    KF_ColorKey  = 1u << 0,   // le texel de cle (noir, ou index 0) est transparent
    KF_Modulate  = 1u << 1,   // couleur = texture x couleur du sommet
    KF_ConstAlpha= 1u << 2,   // alpha pris dans `constColor`
    KF_AlphaTest = 1u << 3,   // rejet par seuil, voir alphaFunc/alphaRef
    KF_ZWrite    = 1u << 4,   // ecrit dans le tampon de profondeur
};

// Deux dessins de meme cle sont fusionnables ; deux dessins de cle differente
// ne le sont pas. C'est la seule propriete que les deux backends exigent.
struct DrawKey {
    uint32_t texture;      // 0 = aplat sans texture
    uint32_t constColor;   // 0xAARRGGBB
    Blend    blend;
    Filter   filter;
    uint8_t  flags;        // KeyFlags
    uint8_t  alphaFunc;    // 0 = defaut du backend ; sinon comparaison du portage
    uint8_t  alphaRef;
    uint8_t  pad[3];
};

// ---- textures -------------------------------------------------------------
enum class TexFormat : uint8_t {
    Idx8,       // 8 bits indexes ; la palette est posee par palette_set()
    Argb1555,
    Rgba8888,
};

// ---- le dos-d'ane que chaque cible implemente -----------------------------
// Un backend concret (GPU de la console, couche OpenGL, comptage seul) remplit
// cette structure. Le portage ne voit que ca.
//
// Aucune methode ne renvoie d'erreur autre que par `init` : un backend qui
// echoue a l'initialisation doit rendre false, et l'appelant retombe alors sur
// le backend de comptage — jamais sur un ecran noir.
struct Backend {
    // Ouvre le contexte pour une cible de `w` x `h`. false = indisponible.
    bool (*init)(int w, int h);
    void (*shutdown)();

    // Cree ou redimensionne une texture. `id` est choisi par l'appelant et lui
    // sert de poignee ensuite. Rend false si la creation echoue.
    bool (*texture_create)(uint32_t id, int w, int h, TexFormat fmt);
    // Televerse un rectangle. `src` est un pointeur HOTE ; `srcPitch` est en
    // OCTETS. Le televersement peut etre paresseux cote backend.
    void (*texture_upload)(uint32_t id, int x, int y, int w, int h,
                           const void* src, int srcPitch);
    // Palette pour les textures Idx8. `slot` correspond au `layer` du sommet.
    // `argb256` = 256 mots 0xAARRGGBB.
    void (*palette_set)(int slot, const uint32_t* argb256);

    // Un lot. `idx` indexe `verts`. Le backend n'a pas le droit de conserver
    // les pointeurs au-dela de l'appel.
    void (*draw)(const DrawKey& key,
                 const Vertex* verts, uint32_t vertCount,
                 const uint16_t* idx, uint32_t idxCount);

    void (*clear_color)(uint32_t argb);
    void (*clear_depth)();
    // Presente l'image. `frame` est le numero d'image de l'appelant : c'est lui
    // qui date les ressources, donc lui qui sert de repere aux barrieres
    // ci-dessous.
    void (*present)(uint64_t frame);

    // ---- soumission en pipeline (facultative) -----------------------------
    // PREMIERE image encore en vol (soumise, pas encore terminee par le GPU).
    // Toute ressource utilisee par une image >= a ce numero ne doit pas etre
    // reecrite : elle donnerait un rendu faux, de facon intermittente. Rend
    // UINT64_MAX quand rien n'est en vol — ce que fait un backend synchrone.
    uint64_t (*in_flight_from)();
    // Attend que tout soit presente. Une attente COMPTEE vaut mieux qu'une
    // ressource reecrite sous le GPU.
    void (*drain)();

    // Ligne de compteurs pour un journal periodique. Rend le nombre d'octets
    // ecrits dans `out`.
    int (*counters)(char* out, unsigned n);
};

// Le backend de COMPTAGE : compile partout, ne dessine rien, compte tout.
// C'est le backend du bureau et de qemu, et le repli de tout backend dont
// l'init echoue. Les deux portages en avaient deja un, ecrit deux fois.
const Backend& null_backend();

// Compteurs du backend de comptage, lisibles pour les tests.
struct NullStats {
    uint64_t frames, draws, verts, indices;
    uint64_t texCreates, texUploadBytes, paletteSets;
    uint64_t clearColors, clearDepths;
};
const NullStats& null_stats();
void null_stats_reset();

// ---- CE QUI N'EST PAS COUVERT, NOMMEMENT ----------------------------------
// A ajouter quand un portage reel en aura besoin, pas avant :
//   * les matrices et l'eclairage materiel — les deux portages font leur
//     transformation en logiciel ;
//   * le filtrage trilineaire et les niveaux de detail — aucun des deux n'en
//     emet ;
//   * les nuanceurs fournis par l'appelant — les deux backends ont un jeu
//     d'etats ferme, volontairement ;
//   * le rendu vers texture, le stencil, les tampons multiples ;
//   * la compression de textures.
// Ces manques sont des ABSENCES, pas des interdits : le vocabulaire ci-dessus
// ne ferme la porte a aucun d'eux.

}  // namespace render
}  // namespace wx86
