/* src/dynarec86/dyn86_memintrin.c — see dyn86_memintrin.h for the contract.
 * Box86 is (c) ptitSeb, MIT license — see third_party/box86-dynarec/LICENSE
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "debug.h"                 /* dyn86_membase / DYN86_G2H */
#include "regs.h"
#include "emu/x86emu_private.h"
#include "dyn86_memintrin.h"

int       dyn86_memintrin = 0;
uintptr_t dyn86_mi_cpy_va = 0, dyn86_mi_set_va = 0;
uint32_t  dyn86_mi_span   = 0;
uint32_t  dyn86_mi_maxn   = 0x01000000u;   /* 16 MiB */

unsigned long long dyn86_mi_cpy_served=0, dyn86_mi_cpy_fb=0, dyn86_mi_cpy_bytes=0;
unsigned long long dyn86_mi_set_served=0, dyn86_mi_set_fb=0, dyn86_mi_set_bytes=0;
unsigned long long dyn86_mi_rej[4] = {0,0,0,0};
unsigned long long dyn86_mi_cpy_hist[DYN86_MI_HBITS];
unsigned long long dyn86_mi_set_hist[DYN86_MI_HBITS];
unsigned long long dyn86_mi_cpy_align[4];
unsigned long long dyn86_mi_cpy_overlap = 0;
unsigned long long dyn86_mi_cpy_small   = 0;
unsigned long long dyn86_mi_cpy_maxseen = 0;
uint32_t dyn86_mi_sse2_va = 0;
unsigned long long dyn86_mi_cpy_sse[2] = {0,0}, dyn86_mi_set_sse[2] = {0,0};

/* ---- HOT-PATH MIRRORS ------------------------------------------------------
 * Every global read on the served path used to cost 3 to 5 ARM instructions:
 * the build is compiled PIC (Debian's arm-linux-gnueabihf defaults to -fPIE),
 * so an `extern` living in ANOTHER translation unit is reached through the GOT
 * (ldr literal / add pc / ldr slot / ldr value). Mirroring the four values the
 * hot path needs into FILE-LOCAL variables puts them in this file's single
 * data anchor: one base register, one LDR each.
 * They are refreshed by every publisher (arm / set_span / arm_verify), which
 * are all called at boot, before the first translated block runs. */
static uintptr_t mi_base   = 0;            /* dyn86_membase snapshot         */
static uint32_t  mi_lim    = 0xffffffffu;  /* first guest address PAST the arena */
static uint32_t  mi_maxn_c = 0x01000000u;  /* min(dyn86_mi_maxn, mi_lim)     */
static int       mi_serve  = 0;            /* 1 = the fast path may serve    */

int dyn86_mi_fast = 0;                     /* read by the TRANSLATOR (mode 3) */
int dyn86_mi_fastchk = 0;
unsigned long long dyn86_mi_fast_blocks = 0;

static void mi_refresh(void)
{
    mi_base = dyn86_membase;
    mi_lim  = dyn86_mi_span ? dyn86_mi_span : 0xffffffffu;
    mi_maxn_c = (dyn86_mi_maxn < mi_lim) ? dyn86_mi_maxn : mi_lim;
    /* PROFILE (2) and the cross-check oracle both need EVERY call to reach the
     * cold path, so they simply clear the "may serve" flag. */
    mi_serve = ((dyn86_memintrin == 1 || dyn86_memintrin == DYN86_MI_MODE_FAST)
                && !dyn86_mi_verify) ? 1 : 0;
    /* The inline sequence is emitted only when it can be ARMED AND CHECKED:
     * never under the cross-check oracle (it would bypass it silently), never
     * without a plausible arena bound. */
    dyn86_mi_fast = (dyn86_memintrin == DYN86_MI_MODE_FAST && !dyn86_mi_verify
                     && (dyn86_mi_span == 0 || dyn86_mi_span > 0x10000u)) ? 1 : 0;
}

void dyn86_mi_arm(int mode, uintptr_t cpy_va, uintptr_t set_va)
{
    dyn86_mi_cpy_va = cpy_va;
    dyn86_mi_set_va = set_va;
    dyn86_memintrin = mode;
    mi_refresh();
}
void dyn86_mi_set_span(uint32_t span) { dyn86_mi_span = span; mi_refresh(); }
void dyn86_mi_set_sse2va(uint32_t va) { dyn86_mi_sse2_va = va; }
static inline int mi_sse2_on(void)
{ return dyn86_mi_sse2_va && *(const uint32_t*)DYN86_G2H(dyn86_mi_sse2_va); }

/* ---- cross-check oracle D2_MEMVERIFY (contract: dyn86_memintrin.h) ----------- */
int      dyn86_mi_verify = 0;
uint32_t dyn86_mi_verify_trap = 0;
unsigned long long dyn86_mi_ver_n = 0, dyn86_mi_ver_bad = 0, dyn86_mi_ver_skip = 0;
#define MI_VERBUF (256u*1024u)
static uint8_t  mi_vbuf[MI_VERBUF];
static uint32_t mi_v_dst, mi_v_n, mi_v_ra;
static volatile int mi_v_armed = 0;

void dyn86_mi_arm_verify(uint32_t trap_va)
{ dyn86_mi_verify_trap = trap_va; dyn86_mi_verify = trap_va ? 1 : 0; mi_refresh(); }

/* Prepares the comparison: snapshots the expected data, redirects the return
 * address. Returns 1 if the redirect was set (the caller must then FALL
 * BACK, not serve the call). */
static int mi_verify_arm(uint32_t esp, uint32_t dst, uint32_t n, const void* expect, int fill)
{
    if(!dyn86_mi_verify || !dyn86_mi_verify_trap) return 0;
    /* Only one call in flight at a time: under the native scheduler two guest
     * threads can enter here concurrently, hence the atomic exchange instead
     * of a plain test. */
    { int z = 0; if(n > MI_VERBUF || !__atomic_compare_exchange_n(&mi_v_armed, &z, 1, 0,
                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) { ++dyn86_mi_ver_skip; return 0; } }
    if(fill) memset(mi_vbuf, *(const int*)expect, n);
    else     memcpy(mi_vbuf, expect, n);
    mi_v_dst = dst; mi_v_n = n;
    { uint32_t* sp = (uint32_t*)DYN86_G2H(esp);
      mi_v_ra = sp[0]; sp[0] = dyn86_mi_verify_trap; }
    return 1;
}

/* Called by the return-trap shim (rt_boot). Compares, then returns the return address. */
uint32_t dyn86_mi_verify_ret(void)
{
    if(!mi_v_armed) return 0;
    ++dyn86_mi_ver_n;
    const uint8_t* got = (const uint8_t*)DYN86_G2H(mi_v_dst);
    if(memcmp(got, mi_vbuf, mi_v_n)) {
        uint32_t i = 0; while(i < mi_v_n && got[i] == mi_vbuf[i]) ++i;
        ++dyn86_mi_ver_bad;
        if(dyn86_mi_ver_bad <= 12)
            printf("[memverify] appel #%llu : divergence a +0x%x sur %u octets "
                   "(dst=%08x) invite=%02x natif=%02x\n",
                   (unsigned long long)dyn86_mi_ver_n, i, mi_v_n, mi_v_dst,
                   got[i], mi_vbuf[i]);
    }
    { uint32_t ra = mi_v_ra;
      __atomic_store_n(&mi_v_armed, 0, __ATOMIC_RELEASE);
      return ra; }
}

/* log2 bucket: 0 => n==0, k => n in [2^(k-1), 2^k) */
static inline int mi_bucket(uint32_t n)
{
    int k = 0;
    while (n) { ++k; n >>= 1; }
    return (k < DYN86_MI_HBITS) ? k : (DYN86_MI_HBITS - 1);
}

/* Census (PROFILE mode only): kept out of mode 1's hot path, where every
 * instruction executes millions of times over a run. */
static void mi_census_copy(uint32_t dst, uint32_t src, uint32_t n)
{
    dyn86_mi_cpy_hist[mi_bucket(n)]++;
    dyn86_mi_cpy_align[(dst | src) & 3u]++;
    if (n < 16u) ++dyn86_mi_cpy_small;
    if (n > dyn86_mi_cpy_maxseen) dyn86_mi_cpy_maxseen = n;
    if (n && ((dst > src && dst - src < n) || (src > dst && src - dst < n)))
        ++dyn86_mi_cpy_overlap;
    /* memcpy's SSE2 branch: n >= 0x100, flag armed, and dst/src share the
     * same residue modulo 16 */
    if (n >= 0x100u && mi_sse2_on() && ((dst & 15u) == (src & 15u)))
        { ++dyn86_mi_cpy_sse[0]; dyn86_mi_cpy_sse[1] += n; }
}

/* ---- COLD PATHS ------------------------------------------------------------
 * Everything that is NOT "serve this call right now on the fast path":
 * PROFILE census, the cross-check oracle, and the three refusal cases. Kept
 * out of line so the served path keeps a minimal prologue and a single data
 * anchor. Behaviour is WORD FOR WORD the pre-existing one, including the case
 * "oracle armed but this call was skipped" -> the call IS served natively. */
static __attribute__((noinline)) int
mi_cold_copy(void* emu, uint32_t esp, uint32_t dst, uint32_t src, uint32_t n)
{
    if (dyn86_memintrin != 1 && dyn86_memintrin != DYN86_MI_MODE_FAST) {
        mi_census_copy(dst, src, n);       /* mode 2: PROFILE, do not serve */
        ++dyn86_mi_rej[2]; ++dyn86_mi_cpy_fb; return 0; }
    if (n > dyn86_mi_maxn)         { ++dyn86_mi_rej[1]; ++dyn86_mi_cpy_fb; return 0; }
    if (dyn86_mi_span && (((uint64_t)dst + n) > dyn86_mi_span ||
                          ((uint64_t)src + n) > dyn86_mi_span))
                                   { ++dyn86_mi_rej[0]; ++dyn86_mi_cpy_fb; return 0; }
    /* Cross-check oracle: let the GUEST do the copy and compare at its return. */
    if (dyn86_mi_verify && mi_verify_arm(esp, dst, n, (const void*)DYN86_G2H(src), 0)) {
        ++dyn86_mi_cpy_fb; return 0; }
    { void* d = (void*)DYN86_G2H(dst); const void* sp = (const void*)DYN86_G2H(src);
      if (dst > src ? (dst - src) < n : (src - dst) < n) memmove(d, sp, n);
      else                                              memcpy (d, sp, n); }
    ((x86emu_t*)emu)->regs[_AX].dword[0] = dst;
    ++dyn86_mi_cpy_served; dyn86_mi_cpy_bytes += n;
    return 1;
}

static __attribute__((noinline)) int
mi_cold_set(void* emu, uint32_t esp, uint32_t dst, uint32_t c, uint32_t n)
{
    if (dyn86_memintrin != 1 && dyn86_memintrin != DYN86_MI_MODE_FAST) {
        dyn86_mi_set_hist[mi_bucket(n)]++;
        /* memset's SSE2 branch: value 0, n >= 0x100, flag armed ->
         * _VEC_memzero, already vectorized on the guest side */
        if ((c & 0xffu) == 0 && n >= 0x100u && mi_sse2_on())
            { ++dyn86_mi_set_sse[0]; dyn86_mi_set_sse[1] += n; }
        ++dyn86_mi_rej[2]; ++dyn86_mi_set_fb; return 0; }
    if (n > dyn86_mi_maxn)    { ++dyn86_mi_rej[1]; ++dyn86_mi_set_fb; return 0; }
    if (dyn86_mi_span && ((uint64_t)dst + n) > dyn86_mi_span)
                              { ++dyn86_mi_rej[0]; ++dyn86_mi_set_fb; return 0; }
    { int fillv = (int)(c & 0xffu);
      if (dyn86_mi_verify && mi_verify_arm(esp, dst, n, &fillv, 1)) {
          ++dyn86_mi_set_fb; return 0; }
      memset((void*)DYN86_G2H(dst), fillv, n); }
    ((x86emu_t*)emu)->regs[_AX].dword[0] = dst;
    ++dyn86_mi_set_served; dyn86_mi_set_bytes += n;
    return 1;
}

#ifdef D2_MIDUMP
/* MEASUREMENT BUILD ONLY (EXTRA=-DD2_MIDUMP). One shot, on the first call: the
 * return address points INTO the emitted block, right after the BLX of the
 * helper-call sequence -- so dumping backwards from it prints the block
 * prologue and the whole inline sequence, exactly as the translator wrote it.
 * Feed the words to arm-linux-gnueabihf-objdump -b binary -m arm. */
static void mi_dump_block(const char* what, const void* ra)
{
    static int done_cpy = 0, done_set = 0;
    int* done = (what[3]=='c') ? &done_cpy : &done_set;
    if(*done) return; *done = 1;
    { const uint32_t* p = (const uint32_t*)((uintptr_t)ra - 4*140);
      int i; printf("[midump] %s bloc autour de %p\n", what, ra);
      for(i=0;i<200;i++) printf("[midump] %s %4d %p %08x\n", what,
                                (i-140)*4, (const void*)(p+i), p[i]); }
}
#endif

int dyn86_mi_copy(void* emu, uint32_t esp)
{
#ifdef D2_MIDUMP
    mi_dump_block("mi_cpy", __builtin_return_address(0));
#endif
    /* cdecl: [esp] = return address, [esp+4] = dst, [esp+8] = src, [esp+0xc] = n */
    const uint32_t* a = (const uint32_t*)(esp + mi_base);
    uint32_t dst = a[1], src = a[2], n = a[3];

    if (__builtin_expect(!mi_serve, 0))      return mi_cold_copy(emu, esp, dst, src, n);
    if (__builtin_expect(n > mi_maxn_c, 0))  return mi_cold_copy(emu, esp, dst, src, n);
    /* Arena guard, 32-bit. mi_maxn_c <= mi_lim by construction, so n <= mi_lim
     * here and `mi_lim - n` cannot wrap. This replaces a pair of 64-bit
     * comparisons (10 ARM instructions) with two 32-bit ones. */
    { uint32_t lim = mi_lim - n;
      if (__builtin_expect((dst > lim) | (src > lim), 0))
          return mi_cold_copy(emu, esp, dst, src, n); }

    { void* d = (void*)(dst + mi_base); const void* sp = (const void*)(src + mi_base);
      /* The guest CRT's memcpy body is the shared memcpy/memmove body: it
       * handles overlap via a backward branch. Overlap is essentially never
       * observed in practice, hence memcpy() on the hot path and memmove()
       * as the fallback. This isn't a gratuitous micro-optimization: on ARM,
       * libc's memmove() is generic C while memcpy() is LDM/NEON assembly.
       * Written as two unsigned wrap-around compares: the previous
       * `dst>src ? dst-src : src-dst` form compiled to nine instructions. */
      if (__builtin_expect(((dst - src) < n) | ((src - dst) < n), 0)) memmove(d, sp, n);
      else                                                           memcpy (d, sp, n); }
    ((x86emu_t*)emu)->regs[_AX].dword[0] = dst;      /* memcpy returns dst */
    ++dyn86_mi_cpy_served; dyn86_mi_cpy_bytes += n;
    return 1;
}

int dyn86_mi_set(void* emu, uint32_t esp)
{
#ifdef D2_MIDUMP
    mi_dump_block("mi_set", __builtin_return_address(0));
#endif
    /* cdecl: [esp] = return address, [esp+4] = dst, [esp+8] = c, [esp+0xc] = n */
    const uint32_t* a = (const uint32_t*)(esp + mi_base);
    uint32_t dst = a[1], c = a[2], n = a[3];

    if (__builtin_expect(!mi_serve, 0))      return mi_cold_set(emu, esp, dst, c, n);
    if (__builtin_expect(n > mi_maxn_c, 0))  return mi_cold_set(emu, esp, dst, c, n);
    if (__builtin_expect(dst > (mi_lim - n), 0)) return mi_cold_set(emu, esp, dst, c, n);

    memset((void*)(dst + mi_base), (int)(c & 0xffu), n);
    ((x86emu_t*)emu)->regs[_AX].dword[0] = dst;      /* memset returns dst */
    ++dyn86_mi_set_served; dyn86_mi_set_bytes += n;
    return 1;
}

/* ---- ORACLE OF THE INLINE SEQUENCE (D2_MEMFASTCHECK) ------------------------
 * The cross-check oracle D2_MEMVERIFY cannot see the inline path: that path
 * never calls this file. So the inline path carries its OWN oracle, emitted
 * only when dyn86_mi_fastchk is armed AT TRANSLATION TIME:
 *   - a PRE call, emitted PAST the acceptance test (so it only runs when the
 *     inline path is really going to do the copy), which snapshots 16 guard
 *     bytes on each side of [dst, dst+n);
 *   - a POST call, after the inline copy, which compares the destination
 *     against the source byte for byte (the inline path refuses overlap, so
 *     the source is still intact) AND re-checks the two guards, which is what
 *     catches a write PAST the requested size.
 * Both return 0 and touch no guest register. */
unsigned long long dyn86_mi_fc_n = 0, dyn86_mi_fc_bad = 0, dyn86_mi_fc_skip = 0;
#define MI_FCG 16u                 /* guard bytes on each side */
static uint8_t  fc_guard[2*MI_FCG];
static uint32_t fc_dst, fc_n, fc_c;
static int      fc_armed = 0;

/* The PRE hook is emitted PAST the acceptance test, so it is only ever
 * reached when the inline path is actually going to do the copy: it must not
 * re-filter anything (that would be a blind spot), only refuse to place the
 * guards when they would fall outside the arena. */
static void fc_pre(uint32_t esp, int is_set)
{
    const uint32_t* a = (const uint32_t*)(esp + mi_base);
    uint32_t dst = a[1], b = a[2], n = a[3];
    fc_armed = 0;
    if (dst < MI_FCG || (uint64_t)dst + n + MI_FCG > (uint64_t)mi_lim) { ++dyn86_mi_fc_skip; return; }
    { const uint8_t* d = (const uint8_t*)(dst + mi_base);
      memcpy(fc_guard, d - MI_FCG, MI_FCG);
      memcpy(fc_guard + MI_FCG, d + n, MI_FCG); }
    (void)is_set; fc_dst = dst; fc_n = n; fc_c = b; fc_armed = 1;
}

static void fc_post(uint32_t esp, int is_set)
{
    if (!fc_armed) return;
    fc_armed = 0;
    ++dyn86_mi_fc_n;
    { const uint32_t* a = (const uint32_t*)(esp + mi_base);
      const uint8_t* d = (const uint8_t*)(fc_dst + mi_base);
      int bad = 0;
      if (is_set) { uint32_t i; uint8_t v = (uint8_t)(fc_c & 0xffu);
                    for (i = 0; i < fc_n; ++i) if (d[i] != v) { bad = 1; break; } }
      else        { const uint8_t* s = (const uint8_t*)(a[2] + mi_base);
                    if (fc_n && memcmp(d, s, fc_n)) bad = 1; }
      if (memcmp(d - MI_FCG, fc_guard, MI_FCG)) bad = 2;
      if (memcmp(d + fc_n, fc_guard + MI_FCG, MI_FCG)) bad = 3;
      if (bad) {
          ++dyn86_mi_fc_bad;
          if (dyn86_mi_fc_bad <= 12)
              printf("[memfast] appel #%llu %s : %s (dst=%08x n=%u)\n",
                     (unsigned long long)dyn86_mi_fc_n, is_set?"memset":"memcpy",
                     bad==1?"contenu different":(bad==2?"garde AVANT ecrasee":"garde APRES ecrasee"),
                     fc_dst, fc_n);
      } }
}

int dyn86_mi_fast_pre_cpy (void* emu, uint32_t esp) { (void)emu; fc_pre (esp, 0); return 0; }
int dyn86_mi_fast_post_cpy(void* emu, uint32_t esp) { (void)emu; fc_post(esp, 0); return 0; }
int dyn86_mi_fast_pre_set (void* emu, uint32_t esp) { (void)emu; fc_pre (esp, 1); return 0; }
int dyn86_mi_fast_post_set(void* emu, uint32_t esp) { (void)emu; fc_post(esp, 1); return 0; }
