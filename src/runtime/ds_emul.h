// src/runtime/ds_emul.h — LE SOCLE COM DirectSound, côté invité.
//
// Game.exe 1.14d n'importe QUE DEUX symboles de DSOUND.dll (ordinaux 1 et 2,
// IAT 0x6CC084/0x6CC088) : il n'y a pas d'API à shimmer, tout le reste passe
// par des vtables COM. On les FABRIQUE : `Bridge::shim_trap` rend une VA
// invitée par shim, et une table de ces VA EST une vtable. Les méthodes COM x86
// sont stdcall avec `this` empilé en premier, donc Shim{argc = 1 + nparams,
// stdcall_cleanup = true} et cpu.arg(0) == this — pas une ligne de modification
// du Bridge, pas un octet d'assembleur écrit à la main.
//
// DÉFAUT INCHANGÉ. Sans D2_SON, DirectSoundCreate rend DSERR_NODRIVER
// (0x88780078) exactement comme avant : aucun fil, aucune bibliothèque, aucune
// vtable posée en mémoire invitée. SEULS les 32 créneaux de trap sont alloués,
// INCONDITIONNELLEMENT — alloc_trap avance de 16 octets par créneau, donc un
// enregistrement conditionnel ferait différer les deux jambes d'un A/B par
// autre chose que le knob (le piège que shim_trap_existing documente,
// bridge.h:60-66). Allouer un créneau ne touche AUCUN octet invité.
#pragma once
#include <cstdint>

namespace d2rt { class Bridge; class Cpu; }

namespace d2rt { namespace dsound {

// Ce que l'EMBARQUEUR prête au socle. Deux des quatre champs d'origine ont
// disparu le 2026-09-11 en même temps que ce fichier passait dans le moteur :
// `misc` est devenu wx86_scratch_alloc (l'allocateur de brouillon appartient
// au moteur) et l'horloge hôte est devenue wx86_now_us. Ce qui reste est ce
// qui dépend VRAIMENT de l'embarqueur : où vivent ses tampons invités, quelle
// horloge INVITÉE il expose, et où il écrit ses fichiers.
struct HostOps {
    uint32_t (*va_alloc)(uint32_t n) = nullptr; // arène d'adresses invitées — les tampons PCM
    uint32_t (*tick_ms)(Cpu& c)    = nullptr;   // horloge INVITÉE en ms (timeGetTime)
    const char* write_root         = nullptr;   // dossier d'écriture de l'embarqueur
    // FRÉQUENCE DU FLUX MÉLANGÉ, en Hz. Elle appartient au JEU : c'est la
    // fréquence de ses échantillons, celle à laquelle le mélangeur n'a AUCUN
    // rééchantillonnage à faire. Jusqu'au 2026-09-12 le socle imposait 22050 —
    // le chiffre du premier consommateur — à tout le monde ; un jeu échantillonné
    // ailleurs aurait joué à la mauvaise hauteur, en silence. C'est le MÊME
    // défaut que celui corrigé au puits console (VitaSink::open recevait la
    // fréquence et l'ignorait), à un autre étage.
    //
    // 0 = l'embarqueur ne l'a pas dite. Le socle REFUSE alors de s'armer et le
    // dit : le moteur n'a pas d'opinion sur la fréquence d'un jeu, et un
    // défaut choisi ici serait le chiffre d'un jeu imposé aux autres.
    int mix_rate = 0;
};

// Journal. Le moteur est muet par construction : il ne connaît ni la console
// de l'embarqueur, ni son journal de démarrage — et sur Vita un printf est
// INVISIBLE, ce qui rend un moteur non branché parfaitement silencieux là où
// on a le plus besoin de le lire. Non armé : les lignes partent sur la sortie
// standard, ce qui convient au bureau et à qemu.
typedef void (*LogFn)(const char* ligne);
void set_logger(LogFn cb);

// QUEUE de la ligne de compteurs. Le moteur publie ce qu'il mesure lui-même
// (voix, grains, famine, puits…) ; ce que l'embarqueur mesure au-dessus lui
// appartient. Chez d2vita, c'est le décodage Storm — un format Blizzard, donc
// hors du moteur, mais qui a sa place dans la MÊME ligne : il est le poste
// numéro 1 et il tourne sur le fil du jeu, alors sans lui on attribuerait au
// mélangeur ce qui vient du codec. Le rappel écrit au plus n octets dans out
// et rend le nombre d'octets écrits.
typedef int (*ExtraStatFn)(char* out, unsigned n);
void set_extra_stat(ExtraStatFn cb);

// LE MOMENT OU LA CAPACITE EST ANNONCEE. Le socle rend dwMaxHw3DAllBuffers = 0,
// donc AUCUN tampon 3D materiel : tout jeu qui teste cette capacite retombera
// sur son chemin 2D, et les options 3D de son menu seront inactives. C'est
// VOULU, et c'est exactement ce qu'un PC sans carte 3D materielle produit —
// mais vu du joueur, cela ressemble a une panne. Le moteur ne sait pas comment
// s'appellent ces options dans le menu du jeu, ni lesquelles doivent rester
// actives : il se contente de DIRE quand la cause est posee, une fois, et
// l'embarqueur nomme les consequences pour SON jeu. Un seul observateur.
typedef void (*CapsObserverFn)();
void set_caps_observer(CapsObserverFn cb);

// Enregistre les deux exports DSOUND + les 32 méthodes COM et FORCE l'allocation
// de leurs créneaux. À appeler une fois, avant Bridge::link().
void install(Bridge& br, Cpu& cpu, const HostOps& ops);

bool enabled();                 // D2_SON armé ?
const char* sink_name();        // "wav" / "null" / "vita" / "-"

// Tire le puits HÔTE (wav/null) depuis le tick d'image du jeu : à chaque image
// présentée, produire exactement les trames de la chronologie INVITÉE écoulée.
// Zéro fil, zéro épinglage, zéro GIL relâché — et la durée du WAV colle au temps
// du jeu même si qemu tourne au dixième du temps réel. No-op sous puits
// auto-cadencé (vita) et quand le son est éteint.
void frame_pump(Cpu& cpu);

// Arrête le fil audio (puits vita) et ferme le puits. À appeler AVANT les
// rapports de fin, au même endroit que le fil de flush du ring.
void shutdown();

// LA SEULE PUBLICATION DE COMPTEURS. Écrit la ligne de la fenêtre de 10 s dans
// `out` (appelée par le chien de garde Vita, vita_present.cpp) et rend le nombre
// d'octets écrits, 0 si le son est éteint. Champs :
//   voix=<mélangées>/<total>  creees=  grains=  famine=  us/grain=  sortie=
//   rest=  verrou=<Kio>  play=  stop=  s=<secondes de la fenêtre>  sansvue=
//   uncoup=<voix à un coup terminées>/<Play sans DSBPLAY_LOOPING>
//   resamp=<grains mélangés à une fréquence != 22050>  puits=  codec=h/a/repli
// Il n'existe PAS d'autre accesseur : la précédente `d2rt_audio_stat(int)`
// annonçait 24 index dont un (« Kio décodés ») n'avait ni case ni compteur, et
// n'avait AUCUN appelant dans le dépôt. Un compteur menteur coûte plus cher
// qu'un compteur absent — elle a été retirée, pas rafistolée.
int stat_line(char* out, unsigned n);

// Auto-test du PUITS SEUL (D2_SONTEST=1) : une sinusoïde 440 Hz poussée dans un
// puits WAV, puis le fichier RELU et vérifié. Sépare pour toujours « le puits
// marche » de « l'émulation DirectSound marche ». Rend 0 si PASS.
int selftest(const char* path, int ms, int rate);

}} // namespace d2rt::dsound
