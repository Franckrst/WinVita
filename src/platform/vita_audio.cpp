// src/platform/vita_audio.cpp — LE PUITS CONSOLE (sceAudioOut) + son fil hote.
//
// Pourquoi sceAudioOut et pas NGS : psp2/ngs_internal.h ne declare que des
// structures OPAQUES (typedef struct X X; sans corps) et il n'existe aucun
// psp2/ngs.h dans ce SDK — on peut LIER NGS mais pas l'APPELER, faute de savoir
// allouer le moindre parametre. psp2/audiodec.h (AT9/MP3/AAC/CELP) est hors
// sujet : les MPQ de D2 contiennent du WAV Storm.
//
// Trois contraintes qui dictent la forme du fil :
//   (a) sceAudioOutOutput est BLOQUANTE (~23 ms au grain retenu) -> interdit sur
//       le coeur des runners invites (USER_0) ;
//   (b) tout fil hote doit s'EPINGLER LUI-MEME : un masque pose par le createur
//       rend rc=0 mais se relit 0 et le fil migre (constat console du 05/09) ;
//   (c) il ne doit JAMAIS tenir le GIL — le melange lit la memoire invitee par
//       vue hote plate, exactement la course qu'une vraie carte son a en DMA,
//       et le contrat DirectSound (Lock ne rend que ce qui ne joue pas) la rend
//       benigne.
#include "runtime/audio_sink.h"

#ifdef __vita__

// LE JOURNAL ET L'EPINGLAGE SONT AU MOTEUR, PAS CHEZ LE PORTAGE.
//
// Ce fichier appelait trois symboles FAIBLES du premier consommateur —
// d2vita_progress_c, d2vita_pin_self_c, d2vita_core_register_c — pour trois
// services que le moteur POSSEDE desormais (platform/vita_host.h). C'etait la
// forme du premier consommateur restee dans le moteur : un second portage, dont
// les symboles ne portent pas ce prefixe, obtenait un journal MUET et un fil
// audio NON EPINGLE, sans la moindre erreur de lien pour l'en avertir — le pire
// mode de panne de ce projet, le « diagnostic qui ment ».
//
// Ces trois services vivent maintenant dans la meme archive et sous garde
// __vita__ identique : l'appel est DIRECT, et tout portage les obtient.
#include "platform/vita_host.h"
static inline void wx86_progress(const char* msg) { wx86_vita_progress(msg); }

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace d2rt { namespace audio {

namespace {

// Frequence de repli, et RIEN DE PLUS. Elle n'est PAS « la » frequence de
// sortie : c'est celle qu'on demande quand l'appelant n'en donne pas
// d'utilisable. Le chiffre venait du premier consommateur (tous ses WAV sont a
// 22050 Hz) et il etait applique a TOUS — open() recevait `freq` et l'ignorait,
// si bien qu'un flux a une autre frequence aurait joue a la mauvaise hauteur
// sans un mot. Le second consommateur est a 22050 lui aussi, ce qui rendait le
// defaut encore plus difficile a voir.
constexpr int kFallbackRate = 22050;

// Les frequences que l'en-tete du SDK (psp2/audioout.h) declare acceptables
// pour un port BGM. Demander autre chose est un echec garanti : autant le
// savoir avant d'appeler, et retomber sur le chemin qui reechantillonne.
inline bool rate_supported(int hz) {
    switch (hz) {
        case 8000: case 11025: case 12000: case 16000: case 22050:
        case 24000: case 32000: case 44100: case 48000: return true;
        default: return false;
    }
}

class VitaSink : public Sink {
public:
    bool open(int freq, int ch, int grain) override {
        srcRate_ = freq; ch_ = ch; grain_ = grain;
        const char* pw = getenv("WX86_SON_PORT"); if (!pw) pw = getenv("D2_SON_PORT");
        // Le port BGM accepte neuf frequences (voir rate_supported) : sur le
        // chemin par defaut il n'y a donc AUCUN reechantillonnage, quelle que
        // soit celle du portage. Le repli MAIN, lui, impose 48000 par l'en-tete
        // du SDK, donc une interpolation lineaire — il existe parce que « le
        // port BGM est-il attenue quand le lecteur de musique systeme tourne »
        // n'est PAS dans l'en-tete et ne se tranche que sur materiel.
        main_ = (pw && !std::strcmp(pw, "main"));
        // LA FREQUENCE DE L'APPELANT EST LA FREQUENCE DE SORTIE quand la
        // console l'accepte : aucun reechantillonnage, aucune derive de
        // hauteur, aucun cout. Elle ne l'etait pas — la constante gagnait.
        outRate_ = main_ ? 48000
                 : rate_supported(srcRate_) ? srcRate_
                                            : kFallbackRate;
        int len = grain;
        if (outRate_ != srcRate_) { len = (int)((int64_t)grain * outRate_ / srcRate_); len = (len + 63) & ~63; }
        if (len < SCE_AUDIO_MIN_LEN) len = SCE_AUDIO_MIN_LEN;
        port_ = sceAudioOutOpenPort(main_ ? SCE_AUDIO_OUT_PORT_TYPE_MAIN : SCE_AUDIO_OUT_PORT_TYPE_BGM,
                                    len, outRate_, SCE_AUDIO_OUT_MODE_STEREO);
        char m[144];
        if (port_ < 0) {
            std::snprintf(m, sizeof m, "audio: sceAudioOutOpenPort(%s,%d,%d) ECHEC rc=0x%08x",
                          main_ ? "MAIN" : "BGM", len, outRate_, (unsigned)port_);
            wx86_progress(m);
            return false;
        }
        // rs_ tient 2048 trames stéréo. Un port plus large que ça ne peut pas
        // être servi sans déborder : on refuse à l'ouverture plutôt que de
        // découvrir le débordement sur console.
        if (len * 2 > (int)(sizeof rs_ / sizeof rs_[0])) {
            std::snprintf(m, sizeof m, "audio: outLen=%d > capacite du tampon de sortie — port refuse", len);
            wx86_progress(m);
            sceAudioOutReleasePort(port_); port_ = -1;
            return false;
        }
        outLen_ = len;
        int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
        sceAudioOutSetVolume(port_, (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), vol);
        std::snprintf(m, sizeof m, "audio: port %s ouvert (port=%d len=%d %d Hz stereo%s)",
                      main_ ? "MAIN" : "BGM", port_, outLen_, outRate_,
                      outRate_ == srcRate_ ? ", sans reechantillonnage" : ", AVEC reechantillonnage");
        wx86_progress(m);
        return true;
    }

    // GRAIN PARTIEL. sceAudioOutOutput consomme TOUJOURS outLen_ trames, quel
    // que soit ce qu'on croit lui donner : passer un tampon de n < outLen_
    // trames fait rejouer par le pilote les trames PÉRIMÉES qui suivent dans la
    // mémoire (la fin du grain précédent). On recopie ce qu'on a et on MET À
    // ZÉRO le reste. Le mélangeur fait déjà la même mise à zéro de son côté :
    // ceinture ET bretelles, parce que ce puits est le seul qui BLOQUE et que
    // c'est le chemin qu'on ne peut pas rejouer sous qemu.
    void write(const int16_t* pcm, int frames) override {
        if (port_ < 0 || !pcm) return;
        // LE CRITERE EST L'EGALITE DES FREQUENCES, pas le type de port : c'est
        // `main_` qui servait de critere, ce qui liait le reechantillonnage a
        // un knob au lieu de le lier au fait qui le commande.
        if (outRate_ == srcRate_) {
            if (frames == outLen_) { wrote_ = true; sceAudioOutOutput(port_, pcm); return; }
            if (frames < 0) frames = 0;
            if (frames > outLen_) frames = outLen_;
            std::memcpy(rs_, pcm, (size_t)frames * 2u * sizeof(int16_t));
            std::memset(rs_ + 2 * frames, 0, (size_t)(outLen_ - frames) * 2u * sizeof(int16_t));
            wrote_ = true;
            sceAudioOutOutput(port_, rs_);
            return;
        }
        // Interpolation lineaire vers outRate_, jambe de REPLI seulement.
        const int n = outLen_;
        for (int i = 0; i < n; i++) {
            const int64_t sp = (int64_t)i * srcRate_;
            const int     s0 = (int)(sp / outRate_);
            const int     fr = (int)(((sp % outRate_) << 12) / outRate_);
            const int     s1 = (s0 + 1 < frames) ? s0 + 1 : frames - 1;
            if (s0 >= frames) { rs_[2*i] = 0; rs_[2*i+1] = 0; continue; }
            rs_[2*i]   = (int16_t)(pcm[2*s0]   + (((pcm[2*s1]   - pcm[2*s0])   * fr) >> 12));
            rs_[2*i+1] = (int16_t)(pcm[2*s0+1] + (((pcm[2*s1+1] - pcm[2*s0+1]) * fr) >> 12));
        }
        wrote_ = true;
        sceAudioOutOutput(port_, rs_);
    }

    void close() override {
        if (port_ < 0) return;
        // Le DRAINAGE attend la fin du dernier grain : c'est un appel BLOQUANT.
        // Il n'a de sens que si l'on a ecrit quelque chose. Sans cette garde, le
        // repli « le fil audio n'a pas demarre » — qui ferme le puits depuis
        // DirectSoundCreate, donc depuis un corps de shim, GIL TENU — ferait le
        // SEUL appel bloquant restant sur le fil du jeu. Rien n'a ete ecrit dans
        // ce cas : on relache le port, point.
        if (wrote_) sceAudioOutOutput(port_, nullptr);
        sceAudioOutReleasePort(port_);
        port_ = -1;
        wx86_progress(wrote_ ? "audio: port draine et relache"
                               : "audio: port relache sans drainage (rien n'a ete ecrit)");
    }
    const char* name() const override { return main_ ? "vita-main" : "vita-bgm"; }

    int  rest_samples() override { return port_ < 0 ? -1 : sceAudioOutGetRestSample(port_); }
    bool self_paced() const override { return true; }

private:
    int port_ = -1, srcRate_ = kFallbackRate, outRate_ = kFallbackRate, ch_ = 2, grain_ = 512, outLen_ = 512;
    bool main_ = false, wrote_ = false;
    int16_t rs_[4096];      // 2048 trames stereo au plus (grain 512 -> 1115 a 48000)
};

void (*g_body)(void) = nullptr;
SceUID g_th = -1;

int audio_thread(SceSize, void*) {
    // PREMIERE INSTRUCTION : l'auto-epinglage. Un masque pose par le createur se
    // relit 0 ; c'est rc et surtout d= (lastExecutedCpuId) de la ligne
    // « coeurs: » qui tranchent, jamais m=0x0.
    unsigned relu = 0;
    const char* cs = getenv("WX86_SONCPU"); if (!cs) cs = getenv("D2_SONCPU");
    // PLACEMENT PAR DEFAUT : USER_1, ET C'EST UN CHOIX, PAS UN HERITAGE.
    // D2_COEURS vaut "222" par defaut : le presentateur, le chien de garde et le
    // battement anti-famine sont TOUS sur USER_2, les runners invites sur
    // USER_0 — USER_1 ne porte AUCUN fil de production. C'est donc le seul
    // emplacement ou un fil qui bloque 23 ms sur 23 ms ne prend le coeur de
    // personne. L'ancien defaut d2vita_core_mask(2) resolvait a USER_2, donc
    // SUR le presentateur, et a une priorite SUPERIEURE a la sienne (0xA0 contre
    // 0x100) : il l'aurait PREEMPTE sur son propre coeur, contre la regle
    // « presentateur seul sur son coeur » (docs/perf/README.md).
    // JAMAIS USER_0 : 23 ms de blocage y voleraient le coeur du jeu.
    int mask = 0x00020000;   // SCE_KERNEL_CPU_MASK_USER_1
    if (cs && *cs) {
        switch (*cs) {
            case '0': mask = 0x00010000; break;
            case '1': mask = 0x00020000; break;
            case '2': mask = 0x00040000; break;
            case '3': mask = 0x00080000; break;
            default: break;
        }
    }
    const int rc = wx86_vita_pin_self(mask, &relu);
    char s[128];
    std::snprintf(s, sizeof s, "audio: auto-epinglage masque=0x%x rc=0x%08x relu=0x%x", (unsigned)mask, (unsigned)rc, relu);
    wx86_progress(s);
    // Le libelle reste « d2_audio » : c'est le nom du premier consommateur, et
    // il porte encore la forme de celui-ci dans un fichier generique. Le
    // renommer ici serait un changement de COMPORTEMENT (la ligne « coeurs: »
    // est grepee par la recette de validation de ce consommateur) glisse sous
    // une etiquette de nettoyage — voir le rapport de vague 4.
    wx86_vita_core_register("d2_audio", g_th, (unsigned)mask, rc);
    if (g_body) g_body();
    wx86_progress("audio: fil termine");
    return 0;
}

} // namespace

Sink* make_vita_sink() { return new VitaSink(); }

bool thread_start(void (*body)(void)) {
    if (g_th >= 0) return true;
    g_body = body;
    // ⚠️ 06/09, console : ce fil ne se creait PAS et le jeu restait muet.
    // La priorite demandee etait 0x100000A0. Le bit 0x10000000 veut dire
    // « RELATIVE au defaut du processus », et le defaut est 0x10000100 : la
    // fenetre legale va de DEFAUT-32 a DEFAUT+31. 0xA0 = DEFAUT-96, soit
    // 64 crans HORS fenetre — le noyau refuse. Le commentaire d'origine
    // raisonnait comme si 0xA0 etait une priorite ABSOLUE (legale, 64..191) en
    // gardant le drapeau relatif : deux encodages melanges. Les 19 autres fils
    // hotes du depot passent tous 0x10000100 et se creent.
    // On prend donc le patron prouve, et on DIT le rc : jeter le code d'erreur
    // etait la vraie faute, elle a coute une soiree de suppositions.
    int prio = 0x10000100;
    if (const char* pe = getenv("WX86_SONPRIO") ? getenv("WX86_SONPRIO") : getenv("D2_SONPRIO")) { long v = strtol(pe, nullptr, 0); if (v) prio = (int)v; }
    g_th = sceKernelCreateThread("d2_audio", audio_thread, prio, 64 * 1024, 0, 0, nullptr);
    if (g_th < 0) {
        char m[160]; std::snprintf(m, sizeof m, "audio: CreateThread(prio=0x%08x pile=64Ko) ECHEC rc=0x%08x",
                                   (unsigned)prio, (unsigned)g_th);
        wx86_progress(m); g_th = -1; return false; }
    { char m[96]; std::snprintf(m, sizeof m, "audio: fil cree (prio=0x%08x pile=64Ko)", (unsigned)prio);
      wx86_progress(m); }
    const int src = sceKernelStartThread(g_th, 0, nullptr);
    if (src < 0) {
        char m[128]; std::snprintf(m, sizeof m, "audio: StartThread ECHEC rc=0x%08x", (unsigned)src);
        wx86_progress(m);
        sceKernelDeleteThread(g_th); g_th = -1; return false;
    }
    return true;
}

bool thread_stop(void) {
    if (g_th < 0) return true;
    // Le corps sort de sa boucle sur le drapeau de ds_emul ; il peut etre bloque
    // jusqu'a un grain (23 ms) dans sceAudioOutOutput. Attente BORNEE : un
    // demontage qui se fige est le pire des rapports de fin.
    SceUInt tmo = 2000000;   // 2 s
    const int rc = sceKernelWaitThreadEnd(g_th, nullptr, &tmo);
    if (rc < 0) {
        // Il n'a pas joint. On NE detruit rien : ni le fil, ni (chez l'appelant)
        // le puits qu'il lit encore. Un rapport de fin incomplet n'a jamais fait
        // tomber un processus ; un fil qui lit un objet libere, si.
        wx86_progress("audio: le fil n'a pas joint en 2 s — puits laisse en place");
        return false;
    }
    sceKernelDeleteThread(g_th);
    g_th = -1;
    return true;
}

}} // namespace d2rt::audio

#endif // __vita__
