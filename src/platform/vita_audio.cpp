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
#include <psp2/kernel/sysmem.h>
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
        // Le rc du VOLUME DU PORT est RELU et publie. Un port ouvert mais laisse
        // a 0 dB « par supposition » est un des points de la chaine qui avalent
        // le son sans une ligne de journal.
        volRc_ = set_port_volume();
        std::snprintf(m, sizeof m, "audio: port %s ouvert (port=%d len=%d %d Hz stereo%s) volume 0dB rc=0x%08x",
                      main_ ? "MAIN" : "BGM", port_, outLen_, outRate_,
                      outRate_ == srcRate_ ? ", sans reechantillonnage" : ", AVEC reechantillonnage",
                      (unsigned)volRc_);
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
        // REAFFIRMATION DU VOLUME DU PORT, toutes les ~10 s. Il n'etait pose
        // qu'a l'ouverture et jamais relu : si le systeme ou un autre composant
        // le baisse, personne ne le saurait. L'appel est idempotent et coute
        // moins qu'un grain sur 430.
        if (++sinceVol_ >= 430) { sinceVol_ = 0; set_port_volume(); }
        // LE CRITERE EST L'EGALITE DES FREQUENCES, pas le type de port : c'est
        // `main_` qui servait de critere, ce qui liait le reechantillonnage a
        // un knob au lieu de le lier au fait qui le commande.
        if (outRate_ == srcRate_) {
            if (frames == outLen_) { out(pcm); return; }
            if (frames < 0) frames = 0;
            if (frames > outLen_) frames = outLen_;
            std::memcpy(rs_, pcm, (size_t)frames * 2u * sizeof(int16_t));
            std::memset(rs_ + 2 * frames, 0, (size_t)(outLen_ - frames) * 2u * sizeof(int16_t));
            out(rs_);
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
        out(rs_);
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
        char mc[160];
        std::snprintf(mc, sizeof mc, "audio: port %s (ecrit=%llu erreurs=%llu derniere rc=0x%08x)",
                      wrote_ ? "draine et relache"
                             : "relache sans drainage (rien n'a ete ecrit)",
                      (unsigned long long)nout_, (unsigned long long)nerr_, (unsigned)lastRc_);
        wx86_progress(mc);
    }
    const char* name() const override { return main_ ? "vita-main" : "vita-bgm"; }

    int  rest_samples() override { return port_ < 0 ? -1 : sceAudioOutGetRestSample(port_); }
    bool self_paced() const override { return true; }
    unsigned long long write_errors() const override { return nerr_; }

private:
    int set_port_volume() {
        int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
        return sceAudioOutSetVolume(port_,
            (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), vol);
    }
    // LE rc DE LA SORTIE, TESTE. sceAudioOutOutput rend le nombre d'octets mis
    // en file, ou un code negatif. Un rc negatif ignore rend l'appel IMMEDIAT :
    // la boucle du fil audio, qui n'a pas d'autre horloge que cet appel
    // bloquant, devient une attente active a 100 % d'un coeur, les curseurs de
    // lecture avancent des dizaines de fois trop vite, toutes les voix
    // « finissent » aussitot — et le son disparait SANS UNE LIGNE.
    void out(const int16_t* p) {
        const int rc = sceAudioOutOutput(port_, p);
        lastRc_ = rc;
        if (rc >= 0) { wrote_ = true; nout_++; consec_ = 0; return; }
        nerr_++;
        if (consec_ < 1000000) consec_++;
        if (nerr_ == 1 || (nerr_ % 256) == 0) {
            char m[160];
            std::snprintf(m, sizeof m, "audio: sceAudioOutOutput rc=0x%08x (erreur %llu, %llu d'affilee)",
                          (unsigned)rc, (unsigned long long)nerr_, (unsigned long long)consec_);
            wx86_progress(m);
        }
        // ANTI-ATTENTE-ACTIVE. Le puits est l'horloge du fil ; s'il rend la main
        // sans attendre, on remet l'horloge a la main (23 ms = un grain) plutot
        // que de bruler un coeur. Le fil reste sortable : le drapeau d'arret est
        // relu a chaque tour de boucle.
        if (consec_ >= 4) sceKernelDelayThread(23000);
    }

    int port_ = -1, srcRate_ = kFallbackRate, outRate_ = kFallbackRate, ch_ = 2, grain_ = 512, outLen_ = 512;
    bool main_ = false, wrote_ = false;
    int  volRc_ = 0, lastRc_ = 0, sinceVol_ = 0;
    unsigned long long nout_ = 0, nerr_ = 0, consec_ = 0;
    // ALIGNEMENT. int16_t[] a un alignement naturel de 2 octets ; le pilote
    // audio fait du DMA et rien dans l'en-tete ne promet qu'il accepte moins de
    // 4. Un refus a cet endroit serait silencieux (le rc etait jete avant ce
    // correctif) : alignas(64) — la ligne de cache ARM — coute zero et ferme la
    // question. NON MESURE : aucune preuve que le pilote l'exigeait.
    alignas(64) int16_t rs_[4096];   // 2048 trames stereo au plus (grain 512 -> 1115 a 48000)
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

// LA CASCADE. Chaque barreau dit ce qu'il TENTE et ce qu'il OBTIENT ; le verdict
// final donne les rc de tous. « Le fil n'a pas demarre » sans chiffre a coute
// une soiree entiere : cela ne peut plus se reproduire.
//
// ⚠️ 06/09, console : ce fil ne se creait PAS et le jeu restait muet. La
// priorite demandee etait 0x100000A0. Le bit 0x10000000 veut dire « RELATIVE au
// defaut du processus », et le defaut est 0x10000100 : la fenetre legale va de
// DEFAUT-32 (0x100000E0) a DEFAUT+31 (0x1000011F). 0xA0 = DEFAUT-96, soit
// 64 crans HORS fenetre — le noyau refuse. Le commentaire d'origine raisonnait
// comme si 0xA0 etait une priorite ABSOLUE (legale, 64..191) en gardant le
// drapeau relatif : deux encodages melanges. Ce SDK ne definit AUCUNE constante
// de priorite, donc rien ne l'a signale a la compilation.
namespace {
struct Rung { int prio; int stackKio; const char* why; };
const Rung kRungs[] = {
    { 0x10000100, 64, "patron prouve (les autres fils hotes du depot)" },
    { 0x10000100, 16, "pile reduite (patron du chien de garde) — hypothese MEMOIRE" },
    { 0x10000100,  4, "pile minimale (patron des sondes) — hypothese MEMOIRE" },
    { 0x100000E0, 16, "priorite relative la plus haute LEGALE (DEFAUT-32)" },
};
constexpr int kRungs_n = (int)(sizeof kRungs / sizeof kRungs[0]);
int  g_rungRc[kRungs_n] = {0};
bool g_exhausted = false;

void log_context(const char* quand) {
    SceKernelFreeMemorySizeInfo fi; std::memset(&fi, 0, sizeof fi); fi.size = sizeof fi;
    const int rcm = sceKernelGetFreeMemorySize(&fi);
    char m[192];
    std::snprintf(m, sizeof m,
        "audio: %s — libre user=%d Kio cdram=%d Kio phycont=%d Kio (rc=0x%08x), fils hotes recenses=%d",
        quand, rcm < 0 ? -1 : (int)(fi.size_user / 1024), rcm < 0 ? -1 : (int)(fi.size_cdram / 1024),
        rcm < 0 ? -1 : (int)(fi.size_phycont / 1024), (unsigned)rcm,
        wx86_vita_core_count());
    wx86_progress(m);
}
} // namespace

bool thread_start(void (*body)(void)) {
    if (g_th >= 0) return true;
    if (g_exhausted) return false;          // la cascade est deja allee au bout
    g_body = body;

    // LE CONTEXTE D'ABORD. Memoire libre et nombre de fils hotes : les deux
    // seules hypotheses que le rc seul ne separe pas.
    log_context("avant creation du fil");

    // Deux knobs pour l'A/B console, DERRIERE le defaut : ils remplacent le
    // PREMIER barreau seulement, les replis restent le patron prouve.
    int p0 = kRungs[0].prio, s0 = kRungs[0].stackKio;
    if (const char* e = getenv("WX86_SONPRIO")  ? getenv("WX86_SONPRIO")  : getenv("D2_SONPRIO"))
        { long v = strtol(e, nullptr, 0); if (v) p0 = (int)v; }
    if (const char* e = getenv("WX86_SONSTACK") ? getenv("WX86_SONSTACK") : getenv("D2_SONSTACK"))
        { long v = strtol(e, nullptr, 0); if (v > 0) s0 = (int)v; }

    char m[192];
    for (int i = 0; i < kRungs_n; i++) {
        const int prio  = (i == 0) ? p0 : kRungs[i].prio;
        const int stack = (i == 0) ? s0 : kRungs[i].stackKio;
        SceUID th = sceKernelCreateThread("d2_audio", audio_thread, prio, stack * 1024, 0, 0, nullptr);
        g_rungRc[i] = (int)th;
        std::snprintf(m, sizeof m, "audio: essai %d/%d CreateThread(prio=0x%08x pile=%d Kio) rc=0x%08x — %s",
                      i + 1, kRungs_n, (unsigned)prio, stack, (unsigned)th, kRungs[i].why);
        wx86_progress(m);
        if (th < 0) continue;
        g_th = th;                                   // audio_thread lit g_th pour son inscription
        const int rs = sceKernelStartThread(th, 0, nullptr);
        std::snprintf(m, sizeof m, "audio: essai %d/%d StartThread(uid=0x%08x) rc=0x%08x",
                      i + 1, kRungs_n, (unsigned)th, (unsigned)rs);
        wx86_progress(m);
        if (rs >= 0) {
            std::snprintf(m, sizeof m, "audio: FIL AUDIO PARTI (essai %d, priorite 0x%08x, pile %d Kio, uid=0x%08x)",
                          i + 1, (unsigned)prio, stack, (unsigned)th);
            wx86_progress(m);
            return true;
        }
        g_rungRc[i] = rs;
        sceKernelDeleteThread(th);
        g_th = -1;
    }
    // VERDICT. Les rc des quatre barreaux sur UNE ligne : c'est elle, et elle
    // seule, qui separe une priorite illegale d'un manque de memoire et d'un
    // epuisement d'UID.
    int n = std::snprintf(m, sizeof m, "audio: FIL AUDIO NON DEMARRE apres %d essais — rc:", kRungs_n);
    for (int i = 0; i < kRungs_n && n > 0 && n < (int)sizeof m; i++)
        n += std::snprintf(m + n, sizeof m - (size_t)n, " [%d]=0x%08x", i + 1, (unsigned)g_rungRc[i]);
    wx86_progress(m);
    log_context("apres l'echec de la cascade");
    g_exhausted = true;
    return false;
}

// REPLI N+1 : LA CREATION DIFFEREE. Appele hors du chemin d'init (voir
// ds_emul::frame_pump). La cascade en ligne se joue pendant la creation du
// peripherique son, c'est-a-dire au creux de la courbe memoire ; quelques
// secondes plus tard le tas a respire. Ce point d'entree REJOUE la cascade
// complete, une fois par appel, et n'est atteint que si la premiere est allee
// au bout.
bool thread_retry(void) {
    if (g_th >= 0) return true;
    g_exhausted = false;
    for (int i = 0; i < kRungs_n; i++) g_rungRc[i] = 0;
    return thread_start(g_body);
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
