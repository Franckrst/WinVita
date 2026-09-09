// src/runtime/audio_sink.h — LE PUITS AUDIO, remplaçable.
//
// Idée directrice du chantier son : séparer « combien de DirectSound » (le
// socle COM invité, ds_emul.cpp) de « comment ça sort » (ce fichier). Le
// mélangeur ne connaît qu'une interface `write(pcm, frames)` ; derrière, trois
// implémentations :
//   * wav   — écrit un RIFF/WAVE dans le dossier d'écriture. C'est LA preuve
//             hors console : un fichier qu'on écoute sur la machine de dev.
//   * null  — jette les échantillons. Le mélangeur tourne quand même, donc les
//             curseurs de lecture avancent : c'est la jambe qui mesure le coût
//             invité du son AVANT d'avoir écrit une ligne de DSP.
//   * vita  — sceAudioOut (src/platform/vita_audio.cpp), console uniquement.
//
// Aucun puits n'est OUVERT tant que D2_SON n'est pas armé : sans le knob, ce
// fichier ne fait rien du tout.
#pragma once
#include <cstdint>

namespace d2rt { namespace audio {

struct Sink {
    virtual ~Sink() {}
    // freq/channels du flux mélangé (22050/2 par construction : tous les WAV de
    // D2 1.14d sont à 22050 Hz, recensement des 4412 fichiers). `grain` = trames
    // par appel de write() ; le puits peut l'ignorer.
    virtual bool open(int freq, int channels, int grain) = 0;
    // PEUT BLOQUER (sceAudioOutOutput bloque ~23 ms). Jamais appelée avec le GIL.
    virtual void write(const int16_t* interleaved, int frames) = 0;
    virtual void close() = 0;
    virtual const char* name() const = 0;
    // Échantillons encore en vol côté pilote ; -1 = le puits ne sait pas.
    // 0 sur un puits temps réel = SOUS-ALIMENTATION (le seul détecteur offert
    // par le SDK Vita, sceAudioOutGetRestSample).
    virtual int rest_samples() { return -1; }
    // Le puits impose-t-il sa propre cadence (temps réel) ? Faux pour wav/null :
    // ceux-là sont TIRÉS par le tick d'image du jeu, donc la durée produite
    // colle à la chronologie invitée même quand qemu tourne au dixième du temps
    // réel — pas de trou, pas de distorsion de hauteur.
    virtual bool self_paced() const { return false; }
};

// ---- FIL HÔTE DE SORTIE ----------------------------------------------------
// Sur Vita : sceKernelCreateThread + AUTO-ÉPINGLAGE en PREMIÈRE instruction du
// fil (un masque posé par le créateur se relit 0 et le fil migre — constat
// console du 05/09). Partout ailleurs : rend false, et le socle retombe sur le
// puits TIRÉ par le tick d'image — donc AUCUN fil sous qemu, ce qui est
// exactement ce qu'on veut pour une preuve déterministe.
bool thread_start(void (*body)(void));
// Rend true si le fil a REELLEMENT joint. Faux => il tourne peut-etre encore, et
// detruire le puits sous lui serait un usage-apres-liberation pendant le
// demontage — exactement la famille de plantages « a la fermeture » deja vue.
bool thread_stop(void);

Sink* make_wav_sink(const char* path);
Sink* make_null_sink();
// Rendu par src/platform/vita_audio.cpp sur __vita__, null partout ailleurs.
Sink* make_vita_sink();

// ---- VÉRIFICATEUR DE WAV ---------------------------------------------------
// Relit un fichier produit par make_wav_sink et écrit un verdict lisible dans
// `report`. Rend true si le fichier est un WAV PCM cohérent ET porte du signal :
// durée > 0, fréquence attendue, amplitude crête non nulle, pas 100 % de
// silence, pas de saturation massive. C'est exactement ce que demande la preuve
// hors console : « ni silence ni saturation » ne se lit pas dans un journal.
//
// ET UNE MESURE SPECTRALE, parce que tout ce qui précède serait aussi vrai d'un
// BRUIT BLANC. `tone_frac` est la part de l'énergie spectrale portée par les 20
// raies dominantes d'un spectre de 512 raies (FFT 1024 points, fenêtre de Hann,
// moyennée sur tout le fichier). Un bruit blanc étale son énergie : 20/512 ≈
// 4 %. De la musique ou de la parole la concentre : 40 % et plus. C'est
// l'argument qui portait §11.4 du rapport son, calculé à la main et commité
// nulle part — il est maintenant DANS le binaire, donc rejouable et régressible.
struct WavStats {
    bool     ok = false;
    uint32_t rate = 0; int channels = 0, bits = 0;
    uint64_t frames = 0;
    double   seconds = 0;
    int      peak = 0;          // |échantillon| max
    double   rms = 0;           // RMS global (0..32768)
    double   silence_frac = 0;  // part des trames toutes à zéro
    uint64_t clipped = 0;       // échantillons à +-32767/-32768
    double   tone_frac = 0;     // part de l'énergie dans les 20 raies dominantes (0..1)
    int      top_hz = 0;        // fréquence de la raie dominante
    uint32_t windows = 0;       // fenêtres de 1024 points analysées
};
bool wav_check(const char* path, WavStats* st, char* report, unsigned n,
               int want_rate = 22050);

}} // namespace d2rt::audio
