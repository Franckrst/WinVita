// src/runtime/ds_emul.cpp — émulation DirectSound : socle COM invité + mélangeur.
// Voir runtime/ds_emul.h pour le principe. Rien ici ne tourne tant que D2_SON
// n'est pas armé, HORMIS l'allocation des 32 créneaux de trap (qui ne touche
// aucun octet invité et fige la numérotation pour les deux jambes d'un A/B).
#include "runtime/ds_emul.h"
#include "runtime/guest_scratch.h"
#include "runtime/audio_sink.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/gil.h"
#include "runtime/host_clock.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <cmath>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace d2rt { namespace dsound {

namespace {

// ---- constantes du flux ----------------------------------------------------
// FRÉQUENCE DU FLUX MÉLANGÉ. Elle n'est PLUS une constante : c'est l'embarqueur
// qui la donne (HostOps::mix_rate), parce que c'est une propriété de SON jeu —
// la fréquence à laquelle ses échantillons ne demandent aucun rééchantillonnage.
// Elle valait 22050 en dur jusqu'au 2026-09-12, le chiffre du premier
// consommateur ; 0 ici veut dire « personne ne l'a dite », et install() refuse
// alors de s'armer plutôt que de deviner (voir ds_emul.h HostOps::mix_rate).
//
// kRate n'est jamais une borne de tableau (seul kGrain l'est) : le passage de
// constexpr à variable ne change aucune taille, seulement des divisions par un
// entier chargé au lieu d'un entier immédiat, hors boucle de trame.
int kRate = 0;
constexpr int kOutCh = 2;
constexpr int kGrain = 512;          // 23,2 ms — 43 réveils/s, ~46 ms de latence

constexpr uint32_t DS_OK              = 0x00000000u;
constexpr uint32_t DSERR_NODRIVER     = 0x88780078u;
constexpr uint32_t DSERR_INVALIDPARAM = 0x80070057u;
constexpr uint32_t E_NOINTERFACE      = 0x80004002u;
constexpr uint32_t E_NOTIMPL          = 0x80004001u;

constexpr uint32_t MAGIC_DEV = 0x56445344u;   // 'DSDV'
constexpr uint32_t MAGIC_BUF = 0x46425344u;   // 'DSBF'

// ---- état ------------------------------------------------------------------
struct Voice {
    uint32_t obj = 0;            // objet COM invité (16 o de misc)
    uint32_t buf = 0;            // tampon PCM invité (arène VA)
    uint32_t cap = 0;            // octets RÉELLEMENT alloués (réemploi)
    uint32_t len = 0;            // octets du tampon vus par le jeu
    uint32_t cursor = 0;         // curseur de LECTURE, en octets
    uint64_t posAcc = 0;         // reste de la conversion de fréquence
    uint32_t flags = 0;
    int      ch = 2, bits = 16;
    uint32_t rate = kRate;
    uint32_t blockAlign = 4;
    bool     primary = false;
    bool     playing = false, looping = false;
    bool     alive = false;
    int32_t  volmB = 0, panmB = 0;
    uint32_t gL = 65536, gR = 65536;
    uint32_t refs = 0;
    const uint8_t* host = nullptr;   // vue hôte plate du tampon invité (Box86)
    // Une voix issue de DuplicateSoundBuffer PARTAGE les échantillons de son
    // original (c'est le contrat DirectSound : mêmes octets, curseur et gains
    // indépendants). Elle n'est donc PAS propriétaire de `buf`, et sa
    // libération ne doit surtout pas rendre ce tampon au réemploi — il est
    // encore lu par l'autre.
    bool     ownsBuf = true;
    uint32_t shares = 0;         // PROPRIETAIRE : duplicatas vivants sur ce tampon
    size_t   owner = (size_t)-1; // DUPLICATA : creneau du proprietaire
    // Kio VERROUILLES PAR CETTE VOIX. Le total global ne pouvait pas repondre a
    // « la pompe de flux alimente-t-elle les anneaux de 256 Kio ? », qui est la
    // seule question qui separe « la musique ne sort pas » de « la musique n'est
    // jamais ecrite ». Publie par le champ flux=.
    uint64_t lockKio = 0;
    bool     volSaid = false;    // premiere SetVolume journalisee (journal verbeux)
};

// Reglage du moteur, avec repli sur l'ancien nom du portage (cf. install()).
inline const char* env2(const char* neuf, const char* ancien) {
    const char* v = getenv(neuf); return v ? v : getenv(ancien);
}

// Journal : formate ici, l'embarqueur decide ou ca va (cf. set_logger).
LogFn g_logger = nullptr;
ExtraStatFn g_extra = nullptr;
CapsObserverFn g_capsObs = nullptr;
void jpline(const char* fmt, ...) {
    char line[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (g_logger) g_logger(line);
    else          std::printf("%s\n", line);
}

HostOps           g_ops;
bool              g_installed = false;
bool              g_on = false;              // D2_SON armé
bool              g_log = false;             // D2_SONLOG
int               g_voiceCap = 0;            // D2_SONVOICES, 0 = pas de plafond
uint64_t          g_maxFrames = 0;           // D2_SONMAXS : borne du puits, en trames (0 = illimite)
std::string       g_sinkWant;                // "wav" / "null" / "vita"
std::string       g_dumpPath;

// g_sink est ATOMIQUE et n'est JAMAIS detruit sous un fil vivant : le fil audio
// le relit a chaque grain, le demontage l'echange contre nullptr AVANT toute
// fermeture, et ne ferme/detruit QUE si le fil a REELLEMENT joint (§ demontage).
std::atomic<audio::Sink*> g_sink{nullptr};
bool              g_selfPaced = false;      // INFORMATIF (journal) : le chemin de
                                            // decision lit s->self_paced(), jamais ceci.
std::atomic<int>  g_threadRun{0};
bool              g_built = false;           // vtables POSÉES en mémoire invitée

std::mutex        g_mx;                      // petit verrou HÔTE (jamais le GIL)
std::vector<Voice> g_voices;
std::vector<size_t> g_freeVoices;
// Créneaux d'objet rendus par des voix NON propriétaires : leur tampon
// appartient à quelqu'un d'autre, elles n'ont donc rien à offrir au réemploi
// ordinaire (qui choisit sur la capacité). Les mêler à g_freeVoices ferait
// soit ressortir un tampon partagé, soit encombrer la liste d'entrées que
// personne ne peut satisfaire.
std::vector<size_t> g_freeDupVoices;

uint32_t g_vtDS[11]  = {0};
uint32_t g_vtBuf[21] = {0};
uint32_t g_vtDSva = 0, g_vtBufVA = 0;
uint32_t g_devObj = 0;

// compteurs — TOUS publies par stat_line() (chien de garde Vita, fenetre 10 s)
// et par la ligne de fin. Aucun compteur ANNONCE qui ne soit ALIMENTE et LU :
// un compteur menteur coute plus cher qu'un compteur absent.
std::atomic<unsigned long long> c_created{0}, c_grains{0}, c_famine{0}, c_play{0},
    c_stop{0}, c_frames{0}, c_lockKio{0}, c_nohost{0}, c_mixus{0}, c_outus{0},
    c_mixed{0}, c_restmin{0xffffffffull},
    // --- les trois modes de panne que la relecture a nommes ------------------
    c_playnl{0},     // Play SANS DSBPLAY_LOOKING : voix a UN COUP (§ boucle)
    c_endnl{0},      // voix a un coup arrivee au bout et ARRETEE par le melangeur
    c_resamp{0},     // grains melanges pour une voix dont rate != 22050 Hz
    // --- L'AMPLITUDE REELLEMENT ENVOYEE AU PUITS -----------------------------
    // Tous les compteurs precedents restaient PARFAITS avec un g_out
    // INTEGRALEMENT NUL : « le melangeur produit du silence » et « le port ne
    // joue pas » etaient indiscernables sur console. Ces quatre-la ferment le
    // trou, et ils sont publies par crete= / rms= de la ligne de compteurs.
    c_peak{0}, c_peakall{0}, c_sqsum{0}, c_sqn{0},
    c_gain0{0};      // voix melangees dont gL == gR == 0 (volume au plancher)

// tampons de sortie/accumulation (fil audio OU tick d'image, jamais les deux :
// le puits auto-cadencé désarme le tirage par image)
// alignas(64) : g_out part TEL QUEL dans l'appel de sortie du pilote, qui fait
// du DMA. Rien dans l'en-tete du SDK ne promet qu'un alignement de 2 octets
// suffit, et un refus a cet endroit serait silencieux. Ligne de cache ARM =
// 64 o. NON MESURE : aucune preuve que le pilote l'exigeait.
alignas(64) int32_t  g_acc[kGrain * kOutCh];
alignas(64) int16_t  g_out[kGrain * kOutCh];

// REPLI DIFFERE du fil audio. Quand la cascade en ligne de audio::thread_start()
// est allee au bout, le puits temps reel est ferme et remplace par le puits NUL
// (le jeu reste jouable et muet). Ces trois variables permettent de REESSAYER
// plus tard, hors du chemin d'init : la cascade se joue pendant la creation du
// peripherique son, au creux de la courbe memoire. Elles ne sont touchees que
// depuis le fil INVITE (open_sink et frame_pump), jamais depuis le fil audio —
// qui, dans ce cas, n'existe pas.
bool     g_deferArmed = false;      // le puits temps reel voulu a echoue
int      g_deferLeft  = 0;          // essais differes restants
uint32_t g_deferNext  = 0;          // prochaine echeance, en ms invitees

// horloge du tirage par image
bool     g_pumpStarted = false;
uint32_t g_pumpT0 = 0;
uint64_t g_produced = 0;

// ---- utilitaires -----------------------------------------------------------
uint32_t gain_q16(int32_t mb) {
    if (mb <= -10000) return 0;
    if (mb >= 0) return 65536;
    return (uint32_t)(65536.0 * std::pow(10.0, (double)mb / 2000.0) + 0.5);
}
void recompute_gains(Voice& v) {
    const uint32_t base = gain_q16(v.volmB);
    uint32_t l = base, r = base;
    // DSBPAN : > 0 atténue la GAUCHE, < 0 atténue la DROITE (contrat DirectSound).
    if (v.panmB > 0)      l = (uint32_t)((uint64_t)base * gain_q16(-v.panmB) >> 16);
    else if (v.panmB < 0) r = (uint32_t)((uint64_t)base * gain_q16( v.panmB) >> 16);
    v.gL = l; v.gR = r;
}

Voice* voice_of(Cpu& c, uint32_t self) {          // appelé sous g_mx
    if (!self) return nullptr;
    if (c.read_u32(self + 8) != MAGIC_BUF) return nullptr;
    uint32_t i = c.read_u32(self + 4);
    if (i >= g_voices.size()) return nullptr;
    Voice* v = &g_voices[i];
    return v->alive ? v : nullptr;
}
bool is_device(Cpu& c, uint32_t self) {
    return self && c.read_u32(self + 8) == MAGIC_DEV;
}

void guest_zero(Cpu& c, uint32_t va, uint32_t n) {
    if (uint8_t* h = (uint8_t*)c.hostptr(va, n)) { std::memset(h, 0, n); return; }
    static const uint8_t z[4096] = {0};
    for (uint32_t o = 0; o < n; o += (uint32_t)sizeof z)
        c.write(va + o, z, (uint32_t)((n - o < sizeof z) ? (n - o) : sizeof z));
}

// ---- LE MÉLANGEUR ----------------------------------------------------------
// Un instantané est pris SOUS le verrou hôte (quelques µs) ; le mélange et
// l'écriture au puits se font DEHORS. Le fil audio ne prend jamais le GIL ; les
// corps de shim prennent g_mx alors qu'ils tiennent DÉJÀ le GIL — ordre unique
// GIL -> g_mx, aucun interblocage possible.
// `acc`/`rate` : le RÉÉCHANTILLONNAGE. Le pointeur de lecture n'avance PAS d'une
// trame source par trame de sortie — il avance de rate/22050, par la MÊME
// arithmétique entière que le curseur (`posAcc`), donc la position finale du
// mélange et la position finale du curseur sont EXACTEMENT la même. À
// rate == 22050 (les 4412 WAV de D2 1.14d) `acc` reste nul et le parcours est
// octet pour octet celui d'avant. `loop` : une voix SANS DSBPLAY_LOOPING ne
// reboucle pas — elle se tait au bout et le mélangeur l'ARRÊTE (voir plus bas).
struct Snap { const uint8_t* host; uint32_t len, cur, blockAlign, rate; uint64_t acc;
              int ch; uint32_t gL, gR; bool loop; };
std::vector<Snap> g_snap;

void mix_grain(Cpu* c, int frames) {
    // GRAIN PARTIEL : borne dure. `frames` vient soit du fil audio (kGrain fixe),
    // soit du rattrapage par image (au plus kGrain). Tout le reste de la fonction
    // écrit dans g_acc/g_out dimensionnés à kGrain — un grain hors borne serait un
    // débordement, un grain <= 0 un puits nourri de rien.
    if (frames <= 0) return;
    if (frames > kGrain) frames = kGrain;
    const uint64_t t0 = wx86_now_us();
    int nmix = 0;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_snap.clear();
        for (Voice& v : g_voices) {
            if (!v.alive || !v.playing || v.primary || !v.len) continue;
            // Vue hôte : mise en cache à la création, RÉ-ESSAYÉE sur le fil
            // INVITÉ (Play et Lock, qui ont un Cpu&) — voir voice_rehost().
            // Ici la reprise n'existe que pour le tirage par image ; sur le
            // chemin auto-cadencé c == nullptr et il n'y a RIEN à rattraper,
            // c'est pourquoi le rattrapage a été déplacé chez l'appelant invité.
            if (!v.host && c) v.host = (const uint8_t*)c->hostptr(v.buf, v.len);
            const uint64_t acc0 = v.posAcc;
            if (!v.host) c_nohost.fetch_add(1, std::memory_order_relaxed);
            else if (!g_voiceCap || (int)g_snap.size() < g_voiceCap) {
                g_snap.push_back(Snap{v.host, v.len, v.cursor, v.blockAlign, v.rate, acc0,
                                      v.ch, v.gL, v.gR, v.looping});
                if (v.rate != (uint32_t)kRate) c_resamp.fetch_add(1, std::memory_order_relaxed);
            }
            // AVANCE DU CURSEUR — pour TOUTE voix qui joue, mélangée ou non.
            // Une voix dont le curseur ne bouge pas n'est jamais vue comme finie
            // par la boucle de service 20 Hz du jeu : il n'appelle jamais Stop et
            // le réservoir de 16 voix sfx se tarit DÉFINITIVEMENT, sans une
            // ligne d'erreur. C'est un mode de panne totalement silencieux.
            v.posAcc += (uint64_t)frames * v.rate;
            uint32_t adv = (uint32_t)(v.posAcc / (uint64_t)kRate) * v.blockAlign;
            v.posAcc %= (uint64_t)kRate;
            if (adv) {
                const uint64_t p = (uint64_t)v.cursor + adv;
                if (v.looping) {
                    v.cursor = (uint32_t)(p % v.len);
                } else if (p >= v.len) {
                    // VOIX À UN COUP ARRIVÉE AU BOUT. Sans ceci elle rebouclait
                    // indéfiniment : un sfx qui ne finit jamais, une voix jamais
                    // rendue au réservoir — l'exacte panne silencieuse que
                    // l'avance du curseur ci-dessus était censée fermer.
                    v.cursor = v.len ? v.len - v.blockAlign : 0;
                    v.playing = false; v.posAcc = 0;
                    c_endnl.fetch_add(1, std::memory_order_relaxed);
                } else {
                    v.cursor = (uint32_t)p;
                }
            }
        }
        nmix = (int)g_snap.size();
        int n0 = 0;
        for (const Snap& sn : g_snap) if (!sn.gL && !sn.gR) n0++;
        c_gain0.store((unsigned long long)n0, std::memory_order_relaxed);
        // L'ETAT INVISIBLE : tout marche, et TOUTES les voix sont a gain 0.
        // grains=, voix=, rest= et famine= seraient parfaits. Le gain rend
        // EXACTEMENT 0 au plancher, donc un silence NUMERIQUE total. Ce cri est
        // le seul moyen de le distinguer d'un port muet.
        if (nmix > 0 && n0 == nmix) {
            static bool cried = false;
            if (!cried) { cried = true;
                jpline("[son] TOUTES les voix melangees (%d) sont a GAIN NUL."
                       " Le melangeur produit un silence NUMERIQUE : ce n'est pas le port.", nmix); }
        }
    }
    c_mixed.store((unsigned long long)nmix, std::memory_order_relaxed);

    const int ns = frames * kOutCh;
    std::memset(g_acc, 0, sizeof(int32_t) * (size_t)ns);
    for (const Snap& s : g_snap) {
        if (!s.blockAlign) continue;
        // LARGEUR RÉELLEMENT LUE, distincte de blockAlign : un tampon dont
        // nBlockAlign ment (ou dont la longueur n'est pas un multiple) ferait
        // lire 2 ou 4 octets APRÈS la fin de la vue hôte. Le pas d'avance reste
        // blockAlign — c'est le contrat DirectSound —, la garde est sur la lecture.
        const uint32_t width = (s.ch == 2) ? 4u : 2u;
        if (s.len < width) continue;
        uint32_t pos = s.cur % s.len;
        if (pos + width > s.len) pos = 0;
        uint64_t acc = s.acc;
        bool done = false;
        for (int i = 0; i < frames && !done; i++) {
            if (s.ch == 2) {
                int16_t l, r; std::memcpy(&l, s.host + pos, 2); std::memcpy(&r, s.host + pos + 2, 2);
                g_acc[2*i]   += (int32_t)(((int64_t)l * (int64_t)s.gL) >> 16);
                g_acc[2*i+1] += (int32_t)(((int64_t)r * (int64_t)s.gR) >> 16);
            } else {
                int16_t v; std::memcpy(&v, s.host + pos, 2);
                g_acc[2*i]   += (int32_t)(((int64_t)v * (int64_t)s.gL) >> 16);
                g_acc[2*i+1] += (int32_t)(((int64_t)v * (int64_t)s.gR) >> 16);
            }
            // Avance rate/22050, arithmétique entière EXACTE (aucune dérive).
            acc += s.rate;
            while (acc >= (uint64_t)kRate) {
                acc -= (uint64_t)kRate;
                pos += s.blockAlign;
                if (pos + width > s.len) {                 // fin du tampon
                    if (s.loop) { pos = 0; }
                    else { done = true; break; }           // voix à un coup : SILENCE
                }
            }
        }
    }
    for (int k = 0; k < ns; k++) {
        int32_t v = g_acc[k];
        g_out[k] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
    }
    // GRAIN PARTIEL, deuxième verrou : le puits console consomme TOUJOURS son
    // outLen_ (512 trames), quel que soit `frames`. La queue de g_out doit donc
    // être MUETTE, sinon le pilote rejoue la fin du grain PRÉCÉDENT.
    // CRETE ET ENERGIE DU GRAIN, mesurees sur ce qui part VRAIMENT au puits.
    // Sans cette mesure, la console ne peut pas distinguer un melangeur muet
    // d'un port muet. Le cout (1024 valeurs absolues 43 fois par seconde) est
    // sous le bruit.
    { int pk = 0; uint64_t sq = 0;
      for (int k = 0; k < ns; k++) { const int v = g_out[k]; const int a = v < 0 ? -v : v;
                                     if (a > pk) pk = a; sq += (uint64_t)((int64_t)v * (int64_t)v); }
      unsigned long long cur = c_peak.load(std::memory_order_relaxed);
      while ((unsigned long long)pk > cur
             && !c_peak.compare_exchange_weak(cur, (unsigned long long)pk)) {}
      // c_peak est remis a zero a chaque fenetre ; c_peakall ne l'est JAMAIS —
      // c'est lui que lit la ligne de fin, sinon « crete » n'y vaudrait que les
      // dernieres secondes du run.
      cur = c_peakall.load(std::memory_order_relaxed);
      while ((unsigned long long)pk > cur
             && !c_peakall.compare_exchange_weak(cur, (unsigned long long)pk)) {}
      c_sqsum.fetch_add(sq, std::memory_order_relaxed);
      c_sqn.fetch_add((unsigned long long)ns, std::memory_order_relaxed); }
    if (ns < kGrain * kOutCh)
        std::memset(g_out + ns, 0, sizeof(int16_t) * (size_t)(kGrain * kOutCh - ns));
    const uint64_t t1 = wx86_now_us();
    c_mixus.fetch_add(t1 - t0, std::memory_order_relaxed);
    // UNE SEULE relecture de g_sink, dans un local : le démontage l'échange
    // contre nullptr et ne détruit qu'après jonction du fil, donc ce pointeur
    // reste valide jusqu'au bout de la fonction.
    audio::Sink* sk = g_sink.load(std::memory_order_acquire);
    // TROISIÈME VERROU, ET LE PLUS FORT parce qu'il est AU SITE DE L'ÉCRITURE.
    // `c != nullptr` <=> on est sur le FIL INVITÉ (le fil audio n'a pas de Cpu&,
    // il appelle toujours mix_grain(nullptr, …)). Un puits temps réel écrit avec
    // un appel BLOQUANT ; le fil invité tient le GIL. La conjonction des deux est
    // interdite ici, définitivement, sans qu'il faille remonter la chaîne
    // d'appels pour s'en convaincre. Les deux autres verrous (frame_pump qui
    // interroge self_paced(), open_sink qui substitue le puits nul) font que ce
    // cas ne devrait jamais se présenter — celui-ci le rend IMPOSSIBLE.
    if (c && sk && sk->self_paced()) {
        static bool cried = false;
        if (!cried) { cried = true;
            jpline("[son] REFUS: puits temps-reel atteint depuis le fil INVITE — ecriture ignoree"
                   " (aucun appel bloquant ne s'execute sous le GIL)"); }
        sk = nullptr;
    }
    // LA BORNE DE DUREE AVALAIT LE SON SANS UNE LIGNE : passe le seuil, le
    // melange continuait, les curseurs avancaient, et plus un octet ne sortait.
    // Elle n'existe que pour borner la taille du WAV de preuve sous horloge
    // virtuelle ; sur un puits TEMPS REEL elle n'a aucun sens et elle est
    // desormais INERTE. Sur les autres, son entree en vigueur est CRIEE une fois.
    const bool capped = g_maxFrames && sk && !sk->self_paced() && c_frames.load() >= g_maxFrames;
    if (capped) { static bool cried = false;
        if (!cried) { cried = true;
            jpline("[son] borne de duree atteinte (%llu trames) : le puits n'est PLUS alimente"
                   " (le melange continue, les curseurs avancent)", (unsigned long long)g_maxFrames); } }
    if (sk && !capped) {
        const int rest = sk->rest_samples();
        if (rest == 0) c_famine.fetch_add(1, std::memory_order_relaxed);
        if (rest >= 0) { unsigned long long r = (unsigned long long)rest, cur = c_restmin.load();
                         while (r < cur && !c_restmin.compare_exchange_weak(cur, r)) {} }
        sk->write(g_out, frames);
    }
    c_outus.fetch_add(wx86_now_us() - t1, std::memory_order_relaxed);
    c_grains.fetch_add(1, std::memory_order_relaxed);
    c_frames.fetch_add((unsigned long long)frames, std::memory_order_relaxed);
}

// Corps du fil hôte (puits auto-cadencé). Il s'enregistre dans la table de piles
// du GIL — non pour le prendre (il ne le prend JAMAIS) mais pour que
// l'observabilité §19 ne le confonde avec personne.
void audio_body() {
    char here;
    const int slot = gil::register_stack((uintptr_t)&here - 0x4000, (uintptr_t)&here, 0xA0D10u);
    while (g_threadRun.load(std::memory_order_relaxed)) mix_grain(nullptr, kGrain);
    gil::unregister_stack(slot);
}

// ---- fabrique d'objets COM -------------------------------------------------
uint32_t alloc_obj(Cpu& c, uint32_t vt, uint32_t magic, uint32_t index) {
    uint32_t o = wx86_scratch_alloc(16);
    if (!o) return 0;
    c.write_u32(o + 0, vt);
    c.write_u32(o + 4, index);
    c.write_u32(o + 8, magic);
    c.write_u32(o + 12, 1);
    return o;
}

void build_vtables(Cpu& c) {
    if (g_built) return;
    g_vtDSva  = wx86_scratch_alloc(sizeof g_vtDS);
    g_vtBufVA = wx86_scratch_alloc(sizeof g_vtBuf);
    if (!g_vtDSva || !g_vtBufVA) { jpline("[son] MISC epuise : vtables non posees"); return; }
    c.write(g_vtDSva,  g_vtDS,  (uint32_t)sizeof g_vtDS);
    c.write(g_vtBufVA, g_vtBuf, (uint32_t)sizeof g_vtBuf);
    g_built = true;
}

// ---- corps des méthodes ----------------------------------------------------
uint32_t m_qi(Cpu& c) {                       // QueryInterface (les deux vtables)
    // 3D matérielle et EAX REFUSÉS : GetCaps rend dwMaxHw3DAllBuffers=0 et
    // l'échelle de repli d'InitDS (0x5146A3) retombe seule sur le 2D stéréo, où
    // le jeu calcule LUI-MÊME son panoramique (0x5158F9/0x51592E/0x515940).
    if (uint32_t pp = c.arg(2)) c.write_u32(pp, 0);
    return E_NOINTERFACE;
}
uint32_t m_addref(Cpu& c) {
    uint32_t s = c.arg(0); if (!s) return 0;
    uint32_t r = c.read_u32(s + 12) + 1; c.write_u32(s + 12, r); return r;
}
uint32_t m_ds_release(Cpu& c) {
    uint32_t s = c.arg(0); if (!s) return 0;
    uint32_t r = c.read_u32(s + 12); if (r) --r; c.write_u32(s + 12, r); return r;
}
uint32_t m_buf_release(Cpu& c) {
    uint32_t s = c.arg(0); if (!s) return 0;
    uint32_t r = c.read_u32(s + 12); if (r) --r; c.write_u32(s + 12, r);
    if (!r) {
        std::lock_guard<std::mutex> lk(g_mx);
        if (Voice* v = voice_of(c, s)) {
            v->playing = false; v->alive = false;
            // LISTE DE RÉEMPLOI : misc() ne libère JAMAIS et est borné à 2 MiB,
            // l'arène VA est à 200/230 MiB. Un pool de voix qui se recrée à
            // chaque changement d'acte grignoterait les deux. L'objet et le
            // tampon sont donc rendus au réemploi, pas jetés.
            // Un tampon PARTAGE ne retourne au reemploi que lorsque plus
            // personne ne le lit. Sans ce compte, liberer l'original pendant
            // qu'un duplicata joue reproposerait ses octets au prochain
            // CreateSoundBuffer — et le duplicata continuerait a lire par
            // dessus l'epaule du nouveau venu.
            const size_t slot = (size_t)c.read_u32(s + 4);
            if (v->ownsBuf) {
                if (!v->shares) g_freeVoices.push_back(slot);
                // sinon : creneau retenu, c'est le dernier duplicata qui le rendra
            } else {
                const size_t os = v->owner;
                if (os < g_voices.size() && g_voices[os].shares) {
                    if (--g_voices[os].shares == 0 && !g_voices[os].alive)
                        g_freeVoices.push_back(os);
                }
                g_freeDupVoices.push_back(slot);
            }
        }
    }
    return r;
}
uint32_t m_ds_setcooplevel(Cpu&) { return DS_OK; }
uint32_t m_ds_compact(Cpu&)      { return DS_OK; }
uint32_t m_ds_initialize(Cpu&)   { return DS_OK; }
uint32_t m_ds_getspeaker(Cpu& c) { if (uint32_t p = c.arg(1)) c.write_u32(p, 1u /*DSSPEAKER_STEREO*/); return DS_OK; }
uint32_t m_ds_setspeaker(Cpu&)   { return DS_OK; }
// IDirectSound::DuplicateSoundBuffer(original, ppDuplicate).
//
// C'ETAIT UN BOUCHON E_NOTIMPL, justifié par « 1.14d ne l'appelle jamais ».
// C'est le motif exact d'accept/bind/listen côté réseau, qui étaient bouchonnés
// pour la même raison et se sont révélés parfaitement implémentables. Un moteur
// générique n'a pas à refuser une méthode parce qu'UN consommateur ne s'en sert
// pas : le suivant s'en servira.
//
// Contrat DirectSound : le duplicata PARTAGE les échantillons de l'original —
// écrire dans l'un se voit dans l'autre — mais possède son propre curseur de
// lecture, son volume, son panoramique et sa fréquence. C'est exactement ce
// qu'il faut pour jouer deux fois le même son en même temps sans recopier les
// octets, et c'est pour ça que les jeux l'utilisent.
uint32_t m_ds_duplicate(Cpu& c) {
    uint32_t src = c.arg(1), ppdup = c.arg(2);
    if (!src || !ppdup) return DSERR_INVALIDPARAM;
    std::lock_guard<std::mutex> lk(g_mx);
    Voice* o = voice_of(c, src);
    if (!o) return DSERR_INVALIDPARAM;
    // Le tampon PRIMAIRE n'est pas duplicable (DirectSound rend DSERR_INVALIDCALL ;
    // INVALIDPARAM est le refus le plus proche que ce socle expose, et le jeu
    // qui tenterait le coup n'a de toute façon rien à y gagner : le primaire
    // n'est jamais mélangé ici).
    if (o->primary) return DSERR_INVALIDPARAM;

    size_t idx = (size_t)-1;
    if (!g_freeDupVoices.empty()) { idx = g_freeDupVoices.back(); g_freeDupVoices.pop_back(); }
    else {
        g_voices.push_back(Voice());
        idx = g_voices.size() - 1;
        // `o` peut avoir été invalidé par la réallocation du vecteur.
        o = voice_of(c, src);
        if (!o) return DSERR_INVALIDPARAM;
        uint32_t obj = alloc_obj(c, g_vtBufVA, MAGIC_BUF, (uint32_t)idx);
        if (!obj) { g_voices.pop_back(); return DSERR_NODRIVER; }
        g_voices[idx].obj = obj;
    }
    const size_t oslot = (size_t)c.read_u32(src + 4);
    g_voices[oslot].shares += 1;
    Voice& d = g_voices[idx];
    const uint32_t obj = d.obj;
    d.owner = oslot;
    // Échantillons PARTAGÉS : même adresse invitée, même vue hôte, et surtout
    // cap = 0 pour que la liste de réemploi ordinaire ne puisse jamais
    // reproposer ce tampon (elle choisit sur `cap >= bytes`).
    d.obj = obj; d.buf = o->buf; d.host = o->host; d.cap = 0; d.ownsBuf = false;
    d.len = o->len; d.flags = o->flags;
    d.ch = o->ch; d.bits = o->bits; d.rate = o->rate; d.blockAlign = o->blockAlign;
    d.primary = false; d.alive = true;
    // État de LECTURE indépendant : c'est tout l'intérêt d'un duplicata.
    d.cursor = 0; d.posAcc = 0; d.playing = false; d.looping = false;
    d.volmB = o->volmB; d.panmB = o->panmB; d.refs = 1;
    // Kio verrouilles : compteur PROPRE au duplicata. Il ne verrouille pas le
    // meme tampon que son original — le contrat DirectSound veut qu'ils ecrivent
    // chacun pour soi — donc heriter du compte de l'original ferait croire que
    // son anneau est alimente alors que personne ne l'a touche.
    d.lockKio = 0; d.volSaid = false;
    recompute_gains(d);
    c.write_u32(obj + 12, 1);
    c.write_u32(ppdup, obj);
    c_created.fetch_add(1, std::memory_order_relaxed);
    if (g_log) jpline("[son] DuplicateSoundBuffer obj=0x%08x -> obj=0x%08x (tampon 0x%08x partage)",
                      src, obj, d.buf);
    return DS_OK;
}

uint32_t m_ds_getcaps(Cpu& c) {
    uint32_t p = c.arg(1); if (!p) return DSERR_INVALIDPARAM;
    uint32_t caps[24] = {0};                 // DSCAPS, dwSize = 0x60
    caps[0]  = 0x60;
    // DSCAPS_PRIMARYSTEREO 0x02 | DSCAPS_PRIMARY16BIT 0x08
    // | DSCAPS_SECONDARYSTEREO 0x200 | DSCAPS_SECONDARY16BIT 0x800.
    // L'ancienne valeur 0xC02 annoncait DSCAPS_SECONDARY8BIT (0x400) — un format
    // que le melangeur REFUSE (bits forces a 16) — et cachait le stereo secondaire.
    caps[1]  = 0x00000A0Au;
    caps[2]  = 100; caps[3] = 100000;        // min/max secondary sample rate
    caps[4]  = 1;                            // dwPrimaryBuffers
    // caps[5..10] = mixage matériel : 0. caps[11] = dwMaxHw3DAllBuffers = 0 :
    // c'est CE zéro qui fait échouer volontairement l'échelon 3D d'InitDS
    // (test >= 16 en 0x5140D0) et retomber le jeu sur le 2D stéréo — le mode le
    // moins coûteux et le seul qu'on implémente.
    c.write(p, caps, (uint32_t)sizeof caps);
    // Dit UNE FOIS, au moment ou la cause est posee. Le moteur enonce le fait
    // generique ; l'embarqueur, s'il en a un, nomme ce que cela change dans le
    // menu de SON jeu.
    static bool said = false;
    if (!said) { said = true;
        jpline("[son] GetCaps: dwMaxHw3DAllBuffers=0 (VOULU) -> le jeu retombera sur son chemin"
               " 2D stereo, et ses options 3D materielles seront inactives.");
        if (g_capsObs) g_capsObs(); }
    return DS_OK;
}

uint32_t m_ds_createbuffer(Cpu& c) {
    uint32_t pdesc = c.arg(1), ppbuf = c.arg(2);
    if (!pdesc || !ppbuf) return DSERR_INVALIDPARAM;
    const uint32_t flags = c.read_u32(pdesc + 4);
    uint32_t bytes = c.read_u32(pdesc + 8);
    const uint32_t pwfx = c.read_u32(pdesc + 16);
    const bool primary = (flags & 0x1u) != 0;

    int ch = 2, bits = 16; uint32_t rate = kRate, blockAlign = 4;
    if (pwfx) {
        uint32_t w0 = c.read_u32(pwfx);            // wFormatTag | nChannels<<16
        ch   = (int)(w0 >> 16); if (ch < 1 || ch > 2) ch = 2;
        rate = c.read_u32(pwfx + 4); if (!rate) rate = kRate;
        uint32_t w3 = c.read_u32(pwfx + 12);       // nBlockAlign | wBitsPerSample<<16
        blockAlign = w3 & 0xffffu; bits = (int)(w3 >> 16);
        if (!blockAlign) blockAlign = (uint32_t)ch * 2u;
        if (bits != 16) bits = 16;                 // D2 refuse tout autre format (0x4df630)
    }
    // Le tampon PRIMAIRE n'est JAMAIS mélangé (niveau coopératif PRIORITY, le
    // jeu n'y écrit que du silence) mais il doit être un vrai tampon invité
    // inscriptible : Lock/Unlock du silence + GetCaps cohérent.
    if (primary && !bytes) bytes = 0x8000;
    if (!bytes || bytes > 0x400000u) return DSERR_INVALIDPARAM;

    std::lock_guard<std::mutex> lk(g_mx);
    // réemploi
    size_t idx = (size_t)-1;
    for (size_t k = 0; k < g_freeVoices.size(); k++) {
        Voice& f = g_voices[g_freeVoices[k]];
        if (f.cap >= bytes && f.obj) { idx = g_freeVoices[k]; g_freeVoices.erase(g_freeVoices.begin() + (long)k); break; }
    }
    if (idx == (size_t)-1) {
        if (!g_ops.va_alloc) return DSERR_NODRIVER;
        uint32_t buf = g_ops.va_alloc(bytes);
        if (!buf) { jpline("[son] arene VA epuisee : CreateSoundBuffer(%u o) refuse", bytes); return DSERR_NODRIVER; }
        g_voices.push_back(Voice());
        idx = g_voices.size() - 1;
        Voice& nv = g_voices[idx];
        nv.buf = buf; nv.cap = bytes;
        nv.obj = alloc_obj(c, g_vtBufVA, MAGIC_BUF, (uint32_t)idx);
        if (!nv.obj) return DSERR_NODRIVER;
    }
    Voice& v = g_voices[idx];
    v.len = bytes; v.cursor = 0; v.posAcc = 0; v.flags = flags;
    v.ch = ch; v.bits = bits; v.rate = rate; v.blockAlign = blockAlign;
    v.primary = primary; v.playing = false; v.looping = false; v.alive = true;
    v.volmB = 0; v.panmB = 0; v.refs = 1; v.lockKio = 0; v.volSaid = false; recompute_gains(v);
    // Un creneau repris vient toujours de g_freeVoices, donc d'un proprietaire
    // sans part vivante ; on le redit quand meme, pour que l'etat de partage
    // ne puisse pas survivre a un reemploi.
    v.ownsBuf = true; v.shares = 0; v.owner = (size_t)-1;
    v.host = (const uint8_t*)c.hostptr(v.buf, v.len);
    if (rate != (uint32_t)kRate) {
        // Le mélangeur RÉÉCHANTILLONNE (pas d'interpolation : plus proche
        // voisin) — la hauteur est juste, le grain est celui d'une carte son de
        // 1999. Mais le recensement dit que les 4412 WAV de D2 1.14d sont à
        // 22050 Hz : si cette ligne sort, le recensement est faux.
        static bool cried = false;
        if (!cried) { cried = true;
            jpline("[son] tampon a %u Hz (attendu %d) : REECHANTILLONNAGE actif"
                   " — voir resamp= du chien de garde", rate, kRate); }
    }
    c.write_u32(v.obj + 12, 1);
    guest_zero(c, v.buf, v.len);
    c.write_u32(ppbuf, v.obj);
    if (!primary) c_created.fetch_add(1, std::memory_order_relaxed);
    if (g_log) jpline("[son] CreateSoundBuffer %s %u o %dch/%u Hz -> obj=0x%08x buf=0x%08x",
                      primary ? "PRIMAIRE" : "secondaire", bytes, ch, rate, v.obj, v.buf);
    return DS_OK;
}

uint32_t m_buf_getcaps(Cpu& c) {
    uint32_t p = c.arg(1); if (!p) return DSERR_INVALIDPARAM;
    std::lock_guard<std::mutex> lk(g_mx);
    Voice* v = voice_of(c, c.arg(0));
    uint32_t caps[5] = {0x14, v ? v->flags : 0u, v ? v->len : 0u, 0, 0};   // DSBCAPS
    c.write(p, caps, (uint32_t)sizeof caps);
    return DS_OK;
}
// LE CURSEUR D'ÉCRITURE. Il ne doit JAMAIS valoir le curseur de lecture : la
// pompe DDA (0x419c00) croirait tout l'anneau libre et écraserait ce qu'elle
// joue (son haché). Avance fixe de deux grains, bornée au quart du tampon.
// UNE SEULE définition, partagée par GetCurrentPosition ET par
// DSBLOCK_FROMWRITECURSOR : deux formules divergentes rouvriraient exactement le
// trou que celle-ci ferme. Appelé sous g_mx.
uint32_t write_cursor(const Voice& v) {
    if (!v.len) return 0;
    uint32_t lead = (uint32_t)(2 * kGrain) * v.blockAlign;
    if (lead > v.len / 4) lead = v.len / 4;
    if (lead < v.blockAlign) lead = v.blockAlign;
    return (uint32_t)(((uint64_t)v.cursor + lead) % v.len);
}

// RATTRAPAGE DE LA VUE HÔTE, sur le fil INVITÉ. Le fil audio n'a pas de Cpu& :
// si `hostptr` a échoué au CreateSoundBuffer (arène pas encore commitée), la
// voix resterait muette À VIE sur console — le mélangeur ne pouvait rien
// rattraper puisqu'il y est toujours appelé avec c == nullptr. On rattrape donc
// ICI, aux deux seuls endroits que le jeu traverse forcément avant d'entendre
// quoi que ce soit : Lock (il y écrit ses octets) et Play. Si le rattrapage
// échoue quand même, la voix est comptée dans `sansvue=` de la ligne du chien de
// garde, son curseur avance (donc le jeu la voit finir et la rend au réservoir)
// et elle est INAUDIBLE — panne visible, pas silencieuse. Appelé sous g_mx.
void voice_rehost(Cpu& c, Voice& v) {
    if (v.host || !v.buf || !v.len) return;
    v.host = (const uint8_t*)c.hostptr(v.buf, v.len);
    if (!v.host) {
        static bool cried = false;
        if (!cried) { cried = true;
            jpline("[son] VUE HOTE ABSENTE pour buf=0x%08x (%u o) : voix INAUDIBLE"
                   " (curseur avance quand meme) — voir sansvue= du chien de garde", v.buf, v.len); }
    }
}

uint32_t m_buf_getpos(Cpu& c) {
    std::lock_guard<std::mutex> lk(g_mx);
    Voice* v = voice_of(c, c.arg(0));
    if (!v) return DSERR_INVALIDPARAM;
    if (uint32_t p = c.arg(1)) c.write_u32(p, v->cursor);
    if (uint32_t p = c.arg(2)) c.write_u32(p, write_cursor(*v));
    return DS_OK;
}
uint32_t m_buf_getformat(Cpu& c) {
    std::lock_guard<std::mutex> lk(g_mx);
    Voice* v = voice_of(c, c.arg(0));
    uint32_t p = c.arg(1), sz = c.arg(2), pw = c.arg(3);
    if (v && p && sz >= 18) {
        uint32_t w[5];
        w[0] = 1u | ((uint32_t)v->ch << 16);
        w[1] = v->rate;
        w[2] = v->rate * v->blockAlign;
        w[3] = v->blockAlign | (16u << 16);
        w[4] = 0;
        c.write(p, w, 18);
    }
    if (pw) c.write_u32(pw, 18);
    return DS_OK;
}
uint32_t m_buf_getvolume(Cpu& c) { std::lock_guard<std::mutex> lk(g_mx); Voice* v = voice_of(c, c.arg(0));
    if (uint32_t p = c.arg(1)) c.write_u32(p, (uint32_t)(v ? v->volmB : 0)); return DS_OK; }
uint32_t m_buf_getpan(Cpu& c) { std::lock_guard<std::mutex> lk(g_mx); Voice* v = voice_of(c, c.arg(0));
    if (uint32_t p = c.arg(1)) c.write_u32(p, (uint32_t)(v ? v->panmB : 0)); return DS_OK; }
uint32_t m_buf_getfreq(Cpu& c) { std::lock_guard<std::mutex> lk(g_mx); Voice* v = voice_of(c, c.arg(0));
    if (uint32_t p = c.arg(1)) c.write_u32(p, v ? v->rate : (uint32_t)kRate); return DS_OK; }
uint32_t m_buf_getstatus(Cpu& c) {
    std::lock_guard<std::mutex> lk(g_mx);
    Voice* v = voice_of(c, c.arg(0));
    uint32_t st = 0;
    if (v && v->playing) { st |= 1u; if (v->looping) st |= 4u; }   // PLAYING | LOOPING
    // Jamais DSBSTATUS_BUFFERLOST : nos tampons ne se perdent pas, sinon la
    // boucle de 0x51603B tourne avec Sleep(10).
    if (uint32_t p = c.arg(1)) c.write_u32(p, st);
    return DS_OK;
}
uint32_t m_buf_initialize(Cpu&) { return DS_OK; }
uint32_t m_buf_lock(Cpu& c) {
    std::lock_guard<std::mutex> lk(g_mx);
    Voice* v = voice_of(c, c.arg(0));
    if (!v) return DSERR_INVALIDPARAM;
    uint32_t off = c.arg(1), bytes = c.arg(2);
    const uint32_t pp1 = c.arg(3), pb1 = c.arg(4), pp2 = c.arg(5), pb2 = c.arg(6);
    const uint32_t lf = c.arg(7);
    voice_rehost(c, *v);
    if (lf & 0x2u) { off = 0; bytes = v->len; }                 // DSBLOCK_ENTIREBUFFER
    // DSBLOCK_FROMWRITECURSOR : le CURSEUR D'ÉCRITURE, pas celui de lecture.
    // Avec v->cursor le jeu recevait EXACTEMENT les octets en train d'être joués
    // et les écrasait — le son haché que write_cursor() existe pour éviter.
    else if (lf & 0x1u) { off = write_cursor(*v); }             // DSBLOCK_FROMWRITECURSOR
    if (v->len) off %= v->len;
    if (bytes > v->len) bytes = v->len;
    uint32_t b1 = bytes, b2 = 0;
    if (off + b1 > v->len) { b1 = v->len - off; b2 = bytes - b1; }
    if (!pp2) { b2 = 0; }                                       // pas de second segment demandé
    if (pp1) c.write_u32(pp1, v->buf + off);
    if (pb1) c.write_u32(pb1, b1);
    if (pp2) c.write_u32(pp2, b2 ? v->buf : 0);
    if (pb2) c.write_u32(pb2, b2);
    c_lockKio.fetch_add((b1 + b2) >> 10, std::memory_order_relaxed);
    v->lockKio += (uint64_t)((b1 + b2) >> 10);
    return DS_OK;
}
uint32_t m_buf_unlock(Cpu&) { return DS_OK; }   // écriture directe : rien à recopier
uint32_t m_buf_play(Cpu& c) {
    std::lock_guard<std::mutex> lk(g_mx);
    Voice* v = voice_of(c, c.arg(0));
    if (!v) return DSERR_INVALIDPARAM;
    voice_rehost(c, *v);
    v->playing = true; v->looping = (c.arg(3) & 0x1u) != 0;      // DSBPLAY_LOOPING
    if (!v->looping) {
        // D2 1.14d joue TOUT en DSBPLAY_LOOPING (0x1) et coupe lui-même. Si
        // cette assertion tombe un jour, elle tombe A VOIX HAUTE : le mélangeur
        // gère le cas (arrêt en fin de tampon, c_endnl) et on le DIT.
        if (c_playnl.fetch_add(1, std::memory_order_relaxed) == 0)
            jpline("[son] Play SANS DSBPLAY_LOOPING (flags=0x%x) : voix a UN COUP,"
                   " arret automatique en fin de tampon", c.arg(3));
    }
    c_play.fetch_add(1, std::memory_order_relaxed);
    return DS_OK;
}
uint32_t m_buf_stop(Cpu& c) {
    std::lock_guard<std::mutex> lk(g_mx);
    Voice* v = voice_of(c, c.arg(0));
    if (!v) return DSERR_INVALIDPARAM;
    v->playing = false;
    c_stop.fetch_add(1, std::memory_order_relaxed);
    return DS_OK;
}
uint32_t m_buf_setpos(Cpu& c) {
    std::lock_guard<std::mutex> lk(g_mx);
    Voice* v = voice_of(c, c.arg(0));
    if (!v || !v->len) return DSERR_INVALIDPARAM;
    v->cursor = c.arg(1) % v->len; v->posAcc = 0;
    return DS_OK;
}
uint32_t m_buf_setformat(Cpu& c) {
    std::lock_guard<std::mutex> lk(g_mx);
    Voice* v = voice_of(c, c.arg(0));
    uint32_t pwfx = c.arg(1);
    if (!v || !pwfx) return DSERR_INVALIDPARAM;
    uint32_t w0 = c.read_u32(pwfx), w3 = c.read_u32(pwfx + 12);
    int ch = (int)(w0 >> 16); if (ch >= 1 && ch <= 2) v->ch = ch;
    uint32_t r = c.read_u32(pwfx + 4); if (r) v->rate = r;
    uint32_t ba = w3 & 0xffffu; v->blockAlign = ba ? ba : (uint32_t)v->ch * 2u;
    return DS_OK;
}
uint32_t m_buf_setvolume(Cpu& c) {
    std::lock_guard<std::mutex> lk(g_mx);
    Voice* v = voice_of(c, c.arg(0));
    if (!v) return DSERR_INVALIDPARAM;
    int32_t mb = (int32_t)c.arg(1);
    if (mb > 0) mb = 0; if (mb < -10000) mb = -10000;           // DSBVOLUME_MIN
    v->volmB = mb; recompute_gains(*v);
    // Le seul chemin « reglage applicatif -> son » est cet appel. Il n'etait
    // journalise NULLE PART, meme en mode verbeux : l'etat « tout marche, tout
    // est a gain 0 » etait rigoureusement invisible. Une ligne par voix a sa
    // PREMIERE SetVolume, plus toutes celles qui atteignent le plancher.
    if (g_log && (!v->volSaid || mb <= -10000)) {
        v->volSaid = true;
        jpline("[son] SetVolume obj=0x%08x %d mB -> gL=%u gR=%u%s", c.arg(0), (int)mb, v->gL, v->gR,
               (!v->gL && !v->gR) ? "  (SILENCE NUMERIQUE)" : "");
    }
    return DS_OK;
}
uint32_t m_buf_setpan(Cpu& c) {
    std::lock_guard<std::mutex> lk(g_mx);
    Voice* v = voice_of(c, c.arg(0));
    if (!v) return DSERR_INVALIDPARAM;
    int32_t mb = (int32_t)c.arg(1);
    if (mb > 10000) mb = 10000; if (mb < -10000) mb = -10000;
    v->panmB = mb; recompute_gains(*v);
    if (g_log && (!v->gL || !v->gR))
        jpline("[son] SetPan obj=0x%08x %d mB -> gL=%u gR=%u (une voie eteinte)", c.arg(0), (int)mb, v->gL, v->gR);
    return DS_OK;
}
uint32_t m_buf_setfreq(Cpu& c) {                                 // jamais appelé en 1.14d
    std::lock_guard<std::mutex> lk(g_mx);
    if (Voice* v = voice_of(c, c.arg(0))) {
        uint32_t f = c.arg(1);
        // DSBFREQUENCY_ORIGINAL == 0 : revenir à la fréquence du format.
        if (f && f != v->rate) {
            v->rate = f;
            static bool cried = false;
            if (!cried) { cried = true;
                jpline("[son] SetFrequency(%u) : REECHANTILLONNAGE actif (attendu %d Hz)", f, kRate); }
        }
    }
    return DS_OK;
}
uint32_t m_buf_restore(Cpu&) { return DS_OK; }

// ---- ouverture du puits ----------------------------------------------------
// RÈGLE DE FORME, écrite ici parce que c'est ici qu'on l'a violée : un puits
// TEMPS RÉEL (self_paced) ne doit JAMAIS finir branché sur le tirage par image.
// Le tirage par image tourne dans frameTick, donc dans un corps de shim, donc
// AVEC LE GIL TENU : y enchaîner des sceAudioOutOutput bloquantes (~23 ms
// chacune, jusqu'à 43 par image) bloquerait le jeu jusqu'à une seconde par
// image présentée. Si le fil qui doit cadencer le puits ne démarre pas, on ne
// dégrade pas — on FERME le puits et on lui substitue le puits NUL. Le jeu reste
// jouable et redevient muet ; c'est le seul repli défendable.
void open_sink() {
    if (g_sink.load()) return;
    audio::Sink* s = nullptr;
    if (g_sinkWant == "vita") {
        s = audio::make_vita_sink();
        if (!s) jpline("[son] puits 'vita' indisponible dans ce binaire -> repli nul");
    } else if (g_sinkWant == "wav") {
        s = audio::make_wav_sink(g_dumpPath.c_str());
    }
    if (!s) s = audio::make_null_sink();
    if (!s->open(kRate, kOutCh, kGrain)) { delete s; s = audio::make_null_sink(); s->open(kRate, kOutCh, kGrain); }

    if (s->self_paced()) {
        // Publier AVANT de démarrer : le corps du fil lit g_sink dès son premier
        // grain. En cas d'échec on dépublie, et personne d'autre ne l'a vu (rien
        // ne tourne encore : open_sink() est appelé depuis DirectSoundCreate).
        g_sink.store(s, std::memory_order_release);
        g_threadRun.store(1);
        if (!audio::thread_start(audio_body)) {
            g_threadRun.store(0);
            g_sink.store(nullptr, std::memory_order_release);
            jpline("[son] fil audio NON demarre -> puits %s FERME et remplace par le puits NUL"
                   " (aucun appel bloquant ne peut atteindre le fil du jeu ; le jeu reste MUET)", s->name());
            s->close();
            delete s;                       // aucun fil ne l'a jamais lu
            s = audio::make_null_sink();
            s->open(kRate, kOutCh, kGrain);
            g_sink.store(s, std::memory_order_release);
            // DERNIER BARREAU DE LA CASCADE, et le seul qui ne soit PAS dans le
            // chemin d'init : trois nouvelles tentatives, une toutes les 5 s de
            // temps invite, depuis frame_pump.
            g_deferArmed = true; g_deferLeft = 3; g_deferNext = 0;
            jpline("[son] repli differe ARME : %d nouvelles tentatives de fil audio,"
                   " une toutes les 5 s, hors du chemin d'init", g_deferLeft);
        }
    } else {
        g_sink.store(s, std::memory_order_release);
    }
    g_selfPaced = s->self_paced();
    jpline("[son] puits=%s cadence=%s grain=%d (%d Hz %dch)", s->name(),
           g_selfPaced ? "temps-reel" : "tick-image", kGrain, kRate, kOutCh);
}

uint32_t ds_create(Cpu& c) {
    if (!g_on) return DSERR_NODRIVER;             // DÉFAUT : à l'octet près comme avant
    build_vtables(c);
    if (!g_built) return DSERR_NODRIVER;
    open_sink();
    uint32_t pp = c.arg(1);
    if (!pp) return DSERR_INVALIDPARAM;
    if (!g_devObj) g_devObj = alloc_obj(c, g_vtDSva, MAGIC_DEV, 0);
    if (!g_devObj) return DSERR_NODRIVER;
    c.write_u32(g_devObj + 12, c.read_u32(g_devObj + 12) + 1);
    c.write_u32(pp, g_devObj);
    jpline("[son] DirectSoundCreate -> DS_OK (objet invite 0x%08x, vtable 0x%08x)", g_devObj, g_vtDSva);
    return DS_OK;
}
uint32_t ds_enum(Cpu&) { return DS_OK; }          // callback jamais rappelé (contournements 1999)
// CAPTURE : il n'y en a pas. L'ordinal 6 (DirectSoundCaptureCreate) était câblé
// sur ds_create, qui rendait DS_OK et un IDirectSound là où l'appelant attend un
// IDirectSoundCapture — il aurait déréférencé une vtable de 11 méthodes en
// croyant en avoir 8 d'un autre contrat. 1.14d n'importe que les ordinaux 1 et 2,
// donc c'était inoffensif ET faux ; docs/FIDELITY_AUDIT.md affirmait d'ailleurs
// le contraire du code. On rend DSERR_NODRIVER : « pas de carte de capture »,
// ce que le pilote d'une machine sans micro répond aussi.
uint32_t ds_capture(Cpu&) { return DSERR_NODRIVER; }

} // namespace

// ---------------------------------------------------------------------------
void set_logger(LogFn cb) { g_logger = cb; }
void set_extra_stat(ExtraStatFn cb) { g_extra = cb; }
void set_caps_observer(CapsObserverFn cb) { g_capsObs = cb; }

void install(Bridge& br, Cpu& cpu, const HostOps& ops) {
    if (g_installed) return;
    g_installed = true;
    g_ops = ops;
    // LA FREQUENCE VIENT DE L'APPELANT. Posee ici, avant tout ce qui la lit
    // (g_maxFrames plus bas, l'ouverture du puits, le pas de rechantillonnage).
    kRate = ops.mix_rate;

    // Reglages : nom du moteur d'abord, ancien nom du portage en repli. Meme
    // convention que le reste du moteur (gil.cpp, cpu_box86.cpp) — les scripts
    // et les journaux dates qui posent encore D2_SON* continuent de marcher,
    // sans message de depreciation.
    const char* k     = env2("WX86_SON",      "D2_SON");
    const char* dump0 = env2("WX86_SONDUMP",  "D2_SONDUMP");
    // « SONDUMP implique SON=wav s'il est seul » : c'est ce que le doc
    // annonce, et desormais ce que le code FAIT. Avant, SONDUMP seul ne
    // levait pas g_on, DirectSoundCreate rendait DSERR_NODRIVER et aucun WAV
    // n'etait cree — un knob qui ne fait pas ce qu'il dit coute une soiree.
    // SON=0 reste MAITRE : il eteint tout, SONDUMP present ou non.
    g_on  = (k && *k) ? (std::strcmp(k, "0") != 0)
                      : (dump0 && *dump0);
    // Un socle arme SANS frequence ne peut que jouer a la mauvaise hauteur.
    // Le moteur ne devine pas : il refuse, et il le DIT (un knob qui ne fait
    // pas ce qu'il annonce coute une soiree — cf. le cas SONDUMP ci-dessus).
    if (g_on && kRate <= 0) {
        jpline("[son] REFUS : HostOps::mix_rate absent — socle DirectSound DESARME."
               " La frequence d'echantillonnage appartient au jeu, pas au moteur.");
        g_on = false;
    }
    g_log = env2("WX86_SONLOG", "D2_SONLOG") != nullptr;
    if (const char* n = env2("WX86_SONVOICES", "D2_SONVOICES")) g_voiceCap = atoi(n);
    // Sous horloge virtuelle, 4000 images du banc valent ~4000 SECONDES de temps
    // invite : le WAV de preuve pese alors 328 Mio. SONMAXS borne ce qui est
    // ECRIT (le melange, lui, continue : couper le melangeur figerait les
    // curseurs et taris  le reservoir de voix — voir la regle (a) plus haut).
    if (const char* n = env2("WX86_SONMAXS", "D2_SONMAXS")) g_maxFrames = (uint64_t)atoi(n) * (uint64_t)kRate;
    const char* dump = dump0;
    if (k && (!std::strcmp(k, "null") || !std::strcmp(k, "nul"))) g_sinkWant = "null";
    else if (k && !std::strcmp(k, "wav"))                          g_sinkWant = "wav";
    else if (k && !std::strcmp(k, "vita"))                         g_sinkWant = "vita";
    else if (dump)                                                 g_sinkWant = "wav";
#ifdef __vita__
    else                                                           g_sinkWant = "vita";
#else
    else                                                           g_sinkWant = "null";
#endif
    if (g_sinkWant == "wav") {
        if (dump && *dump) g_dumpPath = dump;
        else g_dumpPath = std::string(ops.write_root ? ops.write_root : "/tmp") + "/d2_son.wav";
    }

    // --- les 32 créneaux de vtable, ALLOUÉS INCONDITIONNELLEMENT --------------
    // alloc_trap avance de 16 octets par créneau (bridge.cpp:226) : un
    // enregistrement conditionné au knob déplacerait la numérotation de TOUS les
    // créneaux suivants et les deux jambes d'un A/B differeraient par autre chose
    // que le knob. L'allocation ne touche AUCUN octet invité ; elle est donc
    // gratuite pour la jambe muette (preuve : D2_DUMPTRAPS=1 rend la MÊME table
    // avec et sans D2_SON).
    int slot = 0;
    auto R = [&](uint32_t* vt, const char* name, uint32_t nparams, uint32_t (*fn)(Cpu&)) {
        Shim s; s.argc = 1 + nparams; s.stdcall_cleanup = true;
        s.tag = std::string("DSOUNDVT.dll!") + name;
        s.fn = fn;
        br.register_shim("DSOUNDVT.dll", name, s);
        vt[slot++] = br.shim_trap("DSOUNDVT.dll", name);
    };
    slot = 0;
    R(g_vtDS, "IDS_QueryInterface",      2, m_qi);
    R(g_vtDS, "IDS_AddRef",              0, m_addref);
    R(g_vtDS, "IDS_Release",             0, m_ds_release);
    R(g_vtDS, "IDS_CreateSoundBuffer",   3, m_ds_createbuffer);
    R(g_vtDS, "IDS_GetCaps",             1, m_ds_getcaps);
    R(g_vtDS, "IDS_DuplicateSoundBuffer",2, m_ds_duplicate);
    R(g_vtDS, "IDS_SetCooperativeLevel", 2, m_ds_setcooplevel);
    R(g_vtDS, "IDS_Compact",             0, m_ds_compact);
    R(g_vtDS, "IDS_GetSpeakerConfig",    1, m_ds_getspeaker);
    R(g_vtDS, "IDS_SetSpeakerConfig",    1, m_ds_setspeaker);
    R(g_vtDS, "IDS_Initialize",          1, m_ds_initialize);
    slot = 0;
    R(g_vtBuf, "IDSB_QueryInterface",    2, m_qi);
    R(g_vtBuf, "IDSB_AddRef",            0, m_addref);
    R(g_vtBuf, "IDSB_Release",           0, m_buf_release);
    R(g_vtBuf, "IDSB_GetCaps",           1, m_buf_getcaps);
    R(g_vtBuf, "IDSB_GetCurrentPosition",2, m_buf_getpos);
    R(g_vtBuf, "IDSB_GetFormat",         3, m_buf_getformat);
    R(g_vtBuf, "IDSB_GetVolume",         1, m_buf_getvolume);
    R(g_vtBuf, "IDSB_GetPan",            1, m_buf_getpan);
    R(g_vtBuf, "IDSB_GetFrequency",      1, m_buf_getfreq);
    R(g_vtBuf, "IDSB_GetStatus",         1, m_buf_getstatus);
    R(g_vtBuf, "IDSB_Initialize",        2, m_buf_initialize);
    R(g_vtBuf, "IDSB_Lock",              7, m_buf_lock);
    R(g_vtBuf, "IDSB_Play",              3, m_buf_play);
    R(g_vtBuf, "IDSB_SetCurrentPosition",1, m_buf_setpos);
    R(g_vtBuf, "IDSB_SetFormat",         1, m_buf_setformat);
    R(g_vtBuf, "IDSB_SetVolume",         1, m_buf_setvolume);
    R(g_vtBuf, "IDSB_SetPan",            1, m_buf_setpan);
    R(g_vtBuf, "IDSB_SetFrequency",      1, m_buf_setfreq);
    R(g_vtBuf, "IDSB_Stop",              0, m_buf_stop);
    R(g_vtBuf, "IDSB_Unlock",            4, m_buf_unlock);
    R(g_vtBuf, "IDSB_Restore",           0, m_buf_restore);

    // --- les deux SEULS exports que Game.exe importe (par ordinal) ------------
    { Shim s; s.argc = 3; s.stdcall_cleanup = true; s.tag = "DSOUND.dll!DirectSoundCreate";
      s.fn = ds_create;
      br.register_shim("DSOUND.dll", "DirectSoundCreate", s);
      br.register_shim_ordinal("DSOUND.dll", 1, s);
      Shim cap; cap.argc = 3; cap.stdcall_cleanup = true; cap.tag = "DSOUND.dll!DirectSoundCaptureCreate";
      cap.fn = ds_capture;
      br.register_shim("DSOUND.dll", "DirectSoundCaptureCreate", cap);
      br.register_shim_ordinal("DSOUND.dll", 6, cap);
      Shim e; e.argc = 2; e.stdcall_cleanup = true; e.tag = "DSOUND.dll!DirectSoundEnumerateA";
      e.fn = ds_enum;
      br.register_shim("DSOUND.dll", "DirectSoundEnumerateA", e);
      br.register_shim_ordinal("DSOUND.dll", 2, e);
      br.register_shim_ordinal("DSOUND.dll", 3, e);   // EnumerateW : 2 args aussi
      br.register_shim_ordinal("DSOUND.dll", 7, e); }
    (void)cpu;

    if (g_on) std::printf("DSOUND: 32 creneaux de vtable alloues ; son ARME (D2_SON=%s, puits=%s%s%s)\n",
                          k ? k : "(D2_SONDUMP seul)", g_sinkWant.c_str(), g_dumpPath.empty() ? "" : " -> ", g_dumpPath.c_str());
    else      std::printf("DSOUND: 32 creneaux de vtable alloues (numerotation figee) ;"
                          " son MUET par defaut (DirectSoundCreate -> DSERR_NODRIVER)\n");
}

bool enabled() { return g_on; }
const char* sink_name() { audio::Sink* s = g_sink.load(); return s ? s->name() : "-"; }

// LE REPLI DIFFERE, joue sur le FIL INVITE depuis frame_pump. Le puits en place
// est le puits NUL (personne d'autre ne le lit : le fil audio n'existe pas), on
// peut donc l'echanger sans precaution particuliere. En cas d'echec on remet
// EXACTEMENT l'etat d'avant. Aucun appel bloquant : ouvrir un port et creer un
// fil rendent la main tout de suite ; la seule fonction bloquante du puits est
// write(), qui n'est jamais atteinte ici (le troisieme verrou de mix_grain
// interdit d'ecrire dans un puits temps reel depuis le fil invite, et frame_pump
// se desarme des que le puits l'est).
static void try_deferred_sink(Cpu& cpu) {
    const uint32_t now = g_ops.tick_ms ? g_ops.tick_ms(cpu) : 0u;
    if (!g_deferNext) { g_deferNext = now + 5000u; return; }
    if ((int32_t)(now - g_deferNext) < 0) return;
    g_deferNext = now + 5000u;
    if (g_deferLeft <= 0) {
        g_deferArmed = false;
        jpline("[son] repli differe EPUISE : le fil audio n'a jamais demarre, le jeu reste MUET"
               " (les rc de chaque essai sont dans le journal)");
        return;
    }
    const int no = 4 - g_deferLeft;   // 1, 2, 3
    --g_deferLeft;
    audio::Sink* ns = audio::make_vita_sink();
    if (!ns) { g_deferArmed = false; return; }          // pas de puits console dans ce binaire
    if (!ns->open(kRate, kOutCh, kGrain)) {
        jpline("[son] essai differe %d : ouverture du puits %s REFUSEE", no, ns->name());
        ns->close(); delete ns; return;
    }
    audio::Sink* old = g_sink.load(std::memory_order_acquire);
    g_sink.store(ns, std::memory_order_release);
    g_threadRun.store(1);
    if (audio::thread_retry()) {
        g_selfPaced = true;
        if (old) { old->close(); delete old; }          // le puits NUL, que plus personne ne lit
        jpline("[son] essai differe %d : FIL AUDIO PARTI — puits=%s cadence=temps-reel", no, ns->name());
        g_deferArmed = false;
        return;
    }
    // Echec : on remet le puits nul, a l'identique.
    g_threadRun.store(0);
    g_sink.store(old, std::memory_order_release);
    ns->close(); delete ns;
    jpline("[son] essai differe %d : fil audio toujours refuse (%d restant(s))", no, g_deferLeft);
}

void frame_pump(Cpu& cpu) {
    // LE TEST QUI COMPTE est `s->self_paced()`, PAS le drapeau g_selfPaced :
    // un drapeau peut être remis à faux par un chemin d'erreur alors que le
    // puits, lui, est resté temps réel — et le tirage par image enchaînerait
    // alors des écritures BLOQUANTES sur le fil du jeu, GIL tenu. On interroge
    // l'objet, jamais la copie.
    if (g_on && g_deferArmed) try_deferred_sink(cpu);
    audio::Sink* s = g_sink.load(std::memory_order_acquire);
    if (!g_on || !s || s->self_paced()) return;
    const uint32_t now = g_ops.tick_ms ? g_ops.tick_ms(cpu) : 0u;
    if (!g_pumpStarted) { g_pumpStarted = true; g_pumpT0 = now; g_produced = 0; return; }
    const uint64_t elapsed = (uint64_t)(uint32_t)(now - g_pumpT0);
    const uint64_t target  = elapsed * (uint64_t)kRate / 1000ull;
    if (target <= g_produced) return;
    uint64_t need = target - g_produced;
    // Un saut d'horloge invitée ne doit jamais fabriquer une minute de son :
    // on borne à 1 s et on recale le compteur (le WAV reste calé sur le temps
    // du JEU, pas sur celui de qemu).
    if (need > (uint64_t)kRate) { g_produced = target - (uint64_t)kRate; need = (uint64_t)kRate; }
    while (need) {
        const int n = (int)(need > (uint64_t)kGrain ? (uint64_t)kGrain : need);
        mix_grain(&cpu, n);
        g_produced += (uint64_t)n; need -= (uint64_t)n;
    }
}

// DÉMONTAGE. Idempotent (appelé deux fois : chemin normal et filet ExitProcess).
// L'ORDRE est le fond du sujet :
//   1. baisser le drapeau et ATTENDRE la jonction du fil (bornée à 2 s) ;
//   2. DÉPUBLIER g_sink par un échange ATOMIQUE — c'est la seule écriture, et
//      mix_grain n'en fait qu'UNE lecture, dans un local ;
//   3. ne fermer et ne détruire QUE si le fil a réellement joint.
// Fermer sous un fil non joint relâcherait le port sceAudioOut pendant que ce
// fil est peut-être BLOQUÉ dans sceAudioOutOutput sur ce même port : le
// démontage qui plante à la fermeture, en famille connue.
void shutdown() {
    // La ligne des codecs sort TOUJOURS, son armé ou non : c'est elle qui prouve
    // que les trois crochets natifs ne tirent PAS dans le binaire muet (test de
    // VACUITÉ) et qu'ils tirent dans le binaire sonore (NON-vacuité). Un oracle
    // qui n'assertait que « les deux WAV ont le même md5 » resterait vert si les
    // crochets devenaient muets des deux côtés.

    bool joined = true;
    if (g_threadRun.exchange(0)) joined = audio::thread_stop();
    audio::Sink* s = g_sink.exchange(nullptr, std::memory_order_acq_rel);
    if (!s) return;
    jpline("[son] arret: grains=%llu trames=%llu (%.1f s) voix creees=%llu famine=%llu"
           " sansvue=%llu uncoup=%llu/%llu resamp=%llu crete=%llu errsortie=%llu",
           (unsigned long long)c_grains.load(), (unsigned long long)c_frames.load(),
           (double)c_frames.load() / (double)kRate,
           (unsigned long long)c_created.load(), (unsigned long long)c_famine.load(),
           (unsigned long long)c_nohost.load(), (unsigned long long)c_endnl.load(),
           (unsigned long long)c_playnl.load(), (unsigned long long)c_resamp.load(),
           (unsigned long long)c_peakall.load(), s->write_errors());
    if (joined) { s->close(); delete s; }
    else jpline("[son] le fil audio n'a PAS joint : puits NI ferme NI detruit"
                " (le port reste au noyau, le processus sort juste apres) — VOLONTAIRE");
}

int stat_line(char* out, unsigned n) {
    if (!g_on || !out || !n) return 0;
    static unsigned long long p_gr = 0, p_fa = 0, p_mix = 0, p_out = 0, p_fr = 0;
    const unsigned long long gr = c_grains.load(), fa = c_famine.load(),
        mu = c_mixus.load(), ou = c_outus.load(), fr = c_frames.load();
    const unsigned long long dgr = gr - p_gr, dfa = fa - p_fa,
        dmu = mu - p_mix, dou = ou - p_out, dfr = fr - p_fr;
    p_gr = gr; p_fa = fa; p_mix = mu; p_out = ou; p_fr = fr;
    unsigned long long rmin = c_restmin.load(); c_restmin.store(0xffffffffull);
    char rbuf[24];
    if (rmin == 0xffffffffull) std::snprintf(rbuf, sizeof rbuf, "-");
    else                       std::snprintf(rbuf, sizeof rbuf, "%llu", rmin);
    // AMPLITUDE DE LA FENETRE. crete = |echantillon| max envoye au puits (0 = le
    // melangeur produit un silence NUMERIQUE, ce qui n'accuse PAS le port) ;
    // rms = racine de l'energie moyenne. Les deux sont remis a zero ici : ce
    // sont des mesures de FENETRE, pas des cumuls.
    const unsigned long long pk = c_peak.exchange(0, std::memory_order_relaxed);
    const unsigned long long sq = c_sqsum.exchange(0, std::memory_order_relaxed);
    const unsigned long long sn = c_sqn.exchange(0, std::memory_order_relaxed);
    const double rms = sn ? std::sqrt((double)sq / (double)sn) : 0.0;
    // ANNEAUX DE FLUX ALIMENTES. Un tampon >= 64 Kio a la forme d'un anneau de
    // musique/flux ; s'ils sont tous a zero Kio verrouille, la pompe du jeu ne
    // tourne pas et la musique n'est PAS un probleme de SORTIE.
    unsigned nflux = 0, nfluxOk = 0;
    { std::lock_guard<std::mutex> lk(g_mx);
      for (const Voice& v : g_voices) {
          if (!v.alive || v.primary || v.len < 64u * 1024u) continue;
          nflux++; if (v.lockKio) nfluxOk++; } }
    audio::Sink* sk = g_sink.load(std::memory_order_acquire);
    int r = std::snprintf(out, n,
        "audio(10s): voix=%llu/%llu creees=%llu grains=%llu famine=%llu us/grain=%llu sortie=%llu"
        " rest=%s crete=%llu rms=%.0f gain0=%llu errsortie=%llu flux=%u/%u"
        " verrou=%lluKio play=%llu stop=%llu s=%.1f sansvue=%llu"
        " uncoup=%llu/%llu resamp=%llu puits=%s",
        (unsigned long long)c_mixed.load(), (unsigned long long)g_voices.size(),
        (unsigned long long)c_created.load(), dgr, dfa,
        dgr ? dmu / dgr : 0ull, dgr ? dou / dgr : 0ull,
        rbuf, pk, rms,
        (unsigned long long)c_gain0.load(),
        sk ? sk->write_errors() : 0ull, nfluxOk, nflux,
        (unsigned long long)c_lockKio.load(), (unsigned long long)c_play.load(),
        (unsigned long long)c_stop.load(), (double)dfr / (double)kRate,
        (unsigned long long)c_nohost.load(),
        (unsigned long long)c_endnl.load(), (unsigned long long)c_playnl.load(),
        (unsigned long long)c_resamp.load(), sink_name());
    // La QUEUE de l'embarqueur (chez d2vita : le decodage Storm, poste n 1 et
    // sur le fil du jeu — sans lui on attribuerait au melangeur ce qui vient
    // du codec). Le moteur ne sait pas ce que c'est, il lui fait juste de la
    // place. snprintf a pu TRONQUER : r serait alors >= n, d'ou la borne.
    if (g_extra && r > 0 && (unsigned)r + 8u < n)
        r += g_extra(out + r, n - (unsigned)r);
    return r < 0 ? 0 : r;
}

// ---- ÉTAPE 0 : le puits, TOUT SEUL -----------------------------------------
// Ni DirectSound, ni jeu, ni fil : une sinusoïde 440 Hz écrite dans un WAV, puis
// le fichier RELU et vérifié. Sépare pour toujours « le puits marche » de
// « l'émulation DirectSound marche ».
int selftest(const char* path, int ms, int rate) {
    // Ce test n'installe rien : sa frequence ne peut donc pas venir du socle,
    // elle vient de l'appelant, comme tout le reste depuis le 2026-09-12.
    if (rate <= 0) { std::printf("=== [SONTEST] ECHEC: frequence non fournie\n"); return 2; }
    kRate = rate;
    audio::Sink* s = audio::make_wav_sink(path);
    if (!s->open(kRate, kOutCh, kGrain)) { std::printf("=== [SONTEST] ECHEC: ouverture %s\n", path); delete s; return 2; }
    const int total = (int)((int64_t)ms * kRate / 1000);
    int16_t buf[kGrain * kOutCh];
    double ph = 0.0; const double dp = 2.0 * 3.14159265358979323846 * 440.0 / (double)kRate;
    for (int done = 0; done < total; ) {
        const int n = (total - done) < kGrain ? (total - done) : kGrain;
        for (int i = 0; i < n; i++) {
            const int16_t v = (int16_t)(9000.0 * std::sin(ph)); ph += dp;
            buf[2*i] = v; buf[2*i+1] = v;
        }
        s->write(buf, n); done += n;
    }
    s->close(); delete s;
    audio::WavStats st; char rep[512];
    const bool ok = audio::wav_check(path, &st, rep, sizeof rep, kRate);
    std::printf("=== [SONTEST] %s\n=== [SONTEST] %s : sinusoide 440 Hz, %d ms demandes\n",
                rep, ok ? "PASS" : "ECHEC", ms);
    // Test d'ÉCHELLE du puits : la durée écrite doit valoir la durée demandée.
    const double want = ms / 1000.0;
    if (ok && (st.seconds < want * 0.99 || st.seconds > want * 1.01)) {
        std::printf("=== [SONTEST] ECHEC: duree %.3f s pour %.3f s demandees\n", st.seconds, want);
        return 1;
    }
    return ok ? 0 : 1;
}

}} // namespace d2rt::dsound
