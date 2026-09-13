// The console audio sink (sceAudioOut) and its host thread.
//
// sceAudioOut rather than NGS: this SDK's psp2/ngs_internal.h only declares
// opaque structs (no body), and there is no psp2/ngs.h — NGS can be linked
// but not called, since none of its parameters can be allocated.
// psp2/audiodec.h (AT9/MP3/AAC/CELP) is out of scope: the audio source here
// is uncompressed PCM, not a compressed codec.
//
// Three constraints shape the thread:
//   (a) sceAudioOutOutput blocks (~23ms at the grain used here), so it must
//       never run on the guest runners' core (USER_0);
//   (b) every host thread must pin itself: a mask set by the creator returns
//       rc=0 but reads back as 0, and the thread migrates;
//   (c) it must never hold the GIL — the mixer reads guest memory through a
//       flat host view, exactly the same race a real sound card has via DMA,
//       and the DirectSound contract (Lock only returns what isn't playing)
//       makes that race benign.
#include "runtime/audio_sink.h"

#ifdef __vita__

// Progress logging and core pinning are engine services (platform/vita_host.h),
// called directly here — every port gets them without providing anything.
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

// Fallback rate only, used when the caller doesn't provide a usable one.
constexpr int kFallbackRate = 22050;

// Rates the SDK header (psp2/audioout.h) declares valid for a BGM port.
// Anything else is guaranteed to fail, so check before calling and fall back
// to the resampling path instead.
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
        // The BGM port accepts several rates (see rate_supported), so the
        // default path needs no resampling. The MAIN port fallback forces
        // 48000 per the SDK header, hence linear interpolation — it exists
        // because whether the BGM port gets attenuated while the system
        // music player is running isn't documented, only observable on
        // hardware.
        main_ = (pw && !std::strcmp(pw, "main"));
        // The caller's rate is the output rate whenever the console accepts
        // it: no resampling, no pitch drift, no cost.
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
        // rs_ holds 2048 stereo frames. A wider port can't be served without
        // overflowing it, so refuse at open time rather than discover the
        // overflow on console.
        if (len * 2 > (int)(sizeof rs_ / sizeof rs_[0])) {
            std::snprintf(m, sizeof m, "audio: outLen=%d > capacite du tampon de sortie — port refuse", len);
            wx86_progress(m);
            sceAudioOutReleasePort(port_); port_ = -1;
            return false;
        }
        outLen_ = len;
        // The port volume's rc is read back and published. A port opened but
        // silently assumed to be at 0dB is exactly the kind of step that can
        // swallow sound without a single log line.
        volRc_ = set_port_volume();
        std::snprintf(m, sizeof m, "audio: port %s ouvert (port=%d len=%d %d Hz stereo%s) volume 0dB rc=0x%08x",
                      main_ ? "MAIN" : "BGM", port_, outLen_, outRate_,
                      outRate_ == srcRate_ ? ", sans reechantillonnage" : ", AVEC reechantillonnage",
                      (unsigned)volRc_);
        wx86_progress(m);
        return true;
    }

    // Partial grain: sceAudioOutOutput always consumes outLen_ frames no
    // matter what's actually passed in. A buffer with fewer valid frames
    // would have the driver replay stale trailing memory (the end of the
    // previous grain), so the rest is zeroed after copying what's available.
    void write(const int16_t* pcm, int frames) override {
        if (port_ < 0 || !pcm) return;
        // Re-assert port volume roughly every ~10s: if the system or another
        // component lowers it after open, nothing else would catch that. The
        // call is idempotent and costs less than one grain in 430.
        if (++sinceVol_ >= 430) { sinceVol_ = 0; set_port_volume(); }
        // Resampling is decided by rate equality, not by port type.
        if (outRate_ == srcRate_) {
            if (frames == outLen_) { out(pcm); return; }
            if (frames < 0) frames = 0;
            if (frames > outLen_) frames = outLen_;
            std::memcpy(rs_, pcm, (size_t)frames * 2u * sizeof(int16_t));
            std::memset(rs_ + 2 * frames, 0, (size_t)(outLen_ - frames) * 2u * sizeof(int16_t));
            out(rs_);
            return;
        }
        // Linear interpolation to outRate_ — fallback path only.
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
        // Draining waits for the last grain to finish (a blocking call), and
        // only makes sense if something was written. Without this guard, the
        // path that closes the sink from DirectSoundCreate — shim code,
        // holding the GIL — would make this the one remaining blocking call
        // on the game thread when nothing was ever written.
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
    // sceAudioOutOutput's rc is checked because it returns a queued byte
    // count or a negative code. An ignored negative rc returns immediately:
    // since this blocking call is the audio thread's only clock, that turns
    // into a 100%-core busy loop, playback cursors advance far too fast, and
    // sound disappears with nothing in the log to explain why.
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
        // Anti-busy-wait: the sink is the thread's clock, so if it keeps
        // returning without waiting, sleep for one grain (23ms) instead of
        // spinning a core. The thread stays stoppable since the stop flag is
        // re-read every loop iteration.
        if (consec_ >= 4) sceKernelDelayThread(23000);
    }

    int port_ = -1, srcRate_ = kFallbackRate, outRate_ = kFallbackRate, ch_ = 2, grain_ = 512, outLen_ = 512;
    bool main_ = false, wrote_ = false;
    int  volRc_ = 0, lastRc_ = 0, sinceVol_ = 0;
    unsigned long long nout_ = 0, nerr_ = 0, consec_ = 0;
    // int16_t[] naturally aligns to 2 bytes, but the audio driver does DMA
    // and nothing in the header guarantees it accepts less than 4. alignas(64)
    // (an ARM cache line) costs nothing and closes the question — defensive,
    // not proven necessary.
    alignas(64) int16_t rs_[4096];   // at most 2048 stereo frames (grain 512 -> 1115 at 48000)
};

void (*g_body)(void) = nullptr;
SceUID g_th = -1;

int audio_thread(SceSize, void*) {
    // First instruction: self-pinning. A mask set by the creator reads back
    // as 0; the "cores:" line's rc and especially d= (lastExecutedCpuId) are
    // what settle it, never m=0x0.
    unsigned relu = 0;
    const char* cs = getenv("WX86_SONCPU"); if (!cs) cs = getenv("D2_SONCPU");
    // Default placement: USER_1, deliberately. With the default core scheme,
    // the presenter, watchdog, and anti-starvation heartbeat all sit on
    // USER_2, and guest runners on USER_0 — USER_1 carries no production
    // thread. It's the only core where a thread that blocks 23ms out of every
    // 23ms steals nobody's time. Never USER_0: blocking there would steal the
    // game's own core.
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
    // The label stays "d2_audio": a port's validation scripts grep the
    // "cores:" log line for this exact string, so renaming it here would be
    // a behavior change, not just a cleanup.
    wx86_vita_core_register("d2_audio", g_th, (unsigned)mask, rc);
    if (g_body) g_body();
    wx86_progress("audio: fil termine");
    return 0;
}

} // namespace

Sink* make_vita_sink() { return new VitaSink(); }

// The cascade: each rung logs what it attempts and what it gets; the final
// verdict reports every rung's rc, so a thread that fails to start is never
// unexplained.
//
// Thread priority encoding: bit 0x10000000 means "relative to the process
// default" (0x10000100 here); the legal window is DEFAULT-32 (0x100000E0) to
// DEFAULT+31 (0x1000011F). A raw offset like 0xA0 is only legal if treated as
// an absolute priority (range 64..191) — combined with the relative flag it
// falls out of range and the kernel refuses the thread. This SDK defines no
// priority constants, so nothing catches a mixed encoding at compile time.
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
    if (g_exhausted) return false;          // the cascade already ran to completion
    g_body = body;

    // Context first: free memory and host thread count are the two
    // hypotheses that rc alone can't distinguish between.
    log_context("avant creation du fil");

    // Two knobs for console A/B testing, layered behind the default: they
    // only override the first rung; the fallbacks stay the proven baseline.
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
        g_th = th;                                   // audio_thread reads g_th to register itself
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
    // Verdict: all four rungs' rc on one line — the only thing that
    // distinguishes an illegal priority from low memory from UID exhaustion.
    int n = std::snprintf(m, sizeof m, "audio: FIL AUDIO NON DEMARRE apres %d essais — rc:", kRungs_n);
    for (int i = 0; i < kRungs_n && n > 0 && n < (int)sizeof m; i++)
        n += std::snprintf(m + n, sizeof m - (size_t)n, " [%d]=0x%08x", i + 1, (unsigned)g_rungRc[i]);
    wx86_progress(m);
    log_context("apres l'echec de la cascade");
    g_exhausted = true;
    return false;
}

// Deferred retry, called outside the init path (see ds_emul::frame_pump).
// The inline cascade runs during device creation, at the low point of the
// memory curve; a few seconds later the heap has more room. This replays the
// full cascade once per call, and is only reached if the first attempt ran
// to completion.
bool thread_retry(void) {
    if (g_th >= 0) return true;
    g_exhausted = false;
    for (int i = 0; i < kRungs_n; i++) g_rungRc[i] = 0;
    return thread_start(g_body);
}

bool thread_stop(void) {
    if (g_th < 0) return true;
    // The body exits its loop on ds_emul's stop flag, but can be blocked for
    // up to one grain (23ms) inside sceAudioOutOutput. Bounded wait: a
    // teardown that hangs is the worst possible shutdown behavior.
    SceUInt tmo = 2000000;   // 2 s
    const int rc = sceKernelWaitThreadEnd(g_th, nullptr, &tmo);
    if (rc < 0) {
        // Didn't join in time. Nothing is destroyed — not the thread, nor
        // (in the caller) the sink it's still reading. An incomplete
        // shutdown never crashed a process; a thread reading freed memory
        // will.
        wx86_progress("audio: le fil n'a pas joint en 2 s — puits laisse en place");
        return false;
    }
    sceKernelDeleteThread(g_th);
    g_th = -1;
    return true;
}

}} // namespace d2rt::audio

#endif // __vita__
