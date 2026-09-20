/* tools/jitpool_selftest.c — desktop proof of the JIT pool's growth policy.
 *
 * WHAT IS UNDER TEST: the REAL src/dynarec86/shim/vita/mman_vita.c, compiled
 * as-is against the fake kernel in tools/tests/fake_psp2/. Nothing is copied
 * or re-implemented here; if the policy in that file changes, this test
 * changes with it or fails.
 *
 * WHY IT IS NEEDED: mman_vita.c is console-only code. The qemu-arm oracle
 * links the Linux mmap instead, and the desktop CMake build never compiles
 * the file at all — so until now the code that decides how big the JIT pool
 * gets had NO executable check below "ship it and read the crash reports".
 * It got read the hard way: on 0.1.6, eight consoles, the second pool segment
 * was requested exactly once, at 2690 s, and refused; the pool never exceeded
 * 16 MiB parc-wide, and a translation that finds no memory kills the guest
 * thread outright (this build has no interpreter).
 *
 * WHAT THE FAKE KERNEL MODELS, and why those two dials and not others:
 *   - a USER memory budget (free_user), because the floor logic reads it;
 *   - a MAXIMUM VM BLOCK SIZE that SHRINKS as translation proceeds. That
 *     second dial is the whole point: the field reports show a 2 MiB VM
 *     request refused with 5120 KiB still reported free
 *     (sce=0x80024B0B = MEMBLOCK_OVERFLOW). The scarce resource late in a
 *     session is contiguous VM address space, not bytes — so "ask later"
 *     fails even when the byte count says it should succeed. A model with
 *     only a byte budget would make the bug disappear and the test lie.
 *
 * Each scenario runs in its own fork(): mman_vita.c keeps its pool state in
 * file statics with no reset hook, and adding one just for a test would be
 * test code leaking into the shipped path.
 *
 * Exit code 0 = every scenario asserted. Nothing prints "ok" unless the
 * assertion behind it actually held.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>

#include <psp2/kernel/sysmem.h>      /* the FAKE one: tools/tests/fake_psp2 */
#include <psp2/kernel/processmgr.h>
#include "sys/mman.h"    /* the shim's, not the system's */

/* ---- fake kernel ------------------------------------------------------- */

#define FAKE_MAXBLK 64
static struct { void* base; size_t size; int live; } g_fk[FAKE_MAXBLK];
static long   g_free_user  = 0;           /* bytes */
static size_t g_vm_max     = 0;           /* biggest VM block grantable NOW   */
static size_t g_vm_translated = 0;        /* VM bytes handed out so far       */
static size_t g_vm_close_at   = (size_t)-1;  /* past this, VM grants stop     */

static int fk_new(size_t size) {
    for (int i = 1; i < FAKE_MAXBLK; ++i) if (!g_fk[i].live) {
        void* p = calloc(1, size ? size : 1);
        if (!p) return -1;
        g_fk[i].base = p; g_fk[i].size = size; g_fk[i].live = 1;
        return i;
    }
    return -1;
}

SceUID sceKernelAllocMemBlockForVM(const char* name, SceSize size) {
    (void)name;
    /* VM address space first: this is what says 0x80024B0B in the reports. */
    if (g_vm_translated >= g_vm_close_at) return (SceUID)SCE_KERNEL_ERROR_MEMBLOCK_OVERFLOW;
    if ((size_t)size > g_vm_max)          return (SceUID)SCE_KERNEL_ERROR_MEMBLOCK_OVERFLOW;
    if ((long)size > g_free_user)         return (SceUID)SCE_KERNEL_ERROR_MEMBLOCK_OVERFLOW;
    int u = fk_new(size);
    if (u < 0) return (SceUID)SCE_KERNEL_ERROR_MEMBLOCK_OVERFLOW;
    g_free_user -= (long)size;
    return u;
}
SceUID sceKernelAllocMemBlock(const char* name, int type, SceSize size, void* opt) {
    (void)name; (void)type; (void)opt;
    if ((long)size > g_free_user) return (SceUID)0x80020190; /* generic ENOMEM-ish */
    int u = fk_new(size);
    if (u < 0) return (SceUID)0x80020190;
    g_free_user -= (long)size;
    return u;
}
int sceKernelFreeMemBlock(SceUID uid) {
    if (uid <= 0 || uid >= FAKE_MAXBLK || !g_fk[uid].live) return -1;
    g_free_user += (long)g_fk[uid].size;
    free(g_fk[uid].base); memset(&g_fk[uid], 0, sizeof g_fk[uid]);
    return 0;
}
int sceKernelGetMemBlockBase(SceUID uid, void** base) {
    if (uid <= 0 || uid >= FAKE_MAXBLK || !g_fk[uid].live) return -1;
    *base = g_fk[uid].base; return 0;
}
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo* info) {
    info->size_user = (SceSize)(g_free_user < 0 ? 0 : g_free_user);
    info->size_cdram = 0; info->size_phycont = 0; return 0;
}
int sceKernelOpenVMDomain(void)  { return 0; }
int sceKernelCloseVMDomain(void) { return 0; }
int sceKernelSyncVMDomain(SceUID uid, void* base, SceSize size) { (void)uid;(void)base;(void)size; return 0; }
uint64_t sceKernelGetProcessTimeWide(void) { static uint64_t t; return (t += 1000); }

/* The engine log sink mman_vita.c writes its decisions to. Captured so the
 * assertions can be made on WHAT THE CONSOLE WOULD PRINT, which is the only
 * thing the field actually gives us back. */
#define LOGMAX 128
static char g_log[LOGMAX][256];
static int  g_logn = 0;
void wx86_vita_progress_c(const char* msg) {
    if (g_logn < LOGMAX) snprintf(g_log[g_logn++], sizeof g_log[0], "%s", msg);
    printf("      | %s\n", msg);
}
static int log_has(const char* needle) {
    for (int i = 0; i < g_logn; ++i) if (strstr(g_log[i], needle)) return 1;
    return 0;
}

/* ---- the driver -------------------------------------------------------- */

/* box86's AllocDynarecMap asks for DYNMMAPSZ at a time (custommem.c). */
#define CHUNK (2u << 20)

extern unsigned int dyn86_jitpool_size, dyn86_jitpool_used;

/* Translate until `want` bytes are mapped or the pool refuses. Returns the
 * number of bytes actually obtained. */
static size_t translate(size_t want) {
    size_t got = 0;
    while (got < want) {
        void* p = mmap(NULL, CHUNK, PROT_READ|PROT_WRITE|PROT_EXEC,
                       MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
        if (p == MAP_FAILED) break;
        got += CHUNK; g_vm_translated += CHUNK;
    }
    return got;
}

static int fails = 0;
#define CHECK(cond, what) do { \
    if (cond) printf("   OK   %s\n", what); \
    else { printf("   ECHEC %s\n", what); fails = 1; } } while (0)

/* --- scenario 1: 0.1.7 as shipped. Anticipation off (D2_JITPOOL_PCT=0).
 * The second segment is asked for only at exhaustion, by which time the VM
 * window has closed — exactly the 0.1.6 timeline. Expect: pool frozen at
 * 16 MiB and a REFUSED translation, i.e. the death of a guest thread. */
static void sc_0_1_7(void) {
    setenv("D2_JITPOOL_PCT", "0", 1);            /* anticipation disabled */
    g_free_user  = 23u << 20;                    /* 16 MiB seg1 + 7 MiB left, as at 13 s */
    g_vm_max     = 16u << 20;
    g_vm_close_at = 9u << 20;                    /* VM space closes early in the session */
    size_t got = translate(20u << 20);
    CHECK(dyn86_jitpool_size == (16u << 20), "0.1.7 : piscine figee a 16 Mo");
    CHECK(got < (20u << 20),                  "0.1.7 : une traduction est REFUSEE (fil invite mort)");
    CHECK(log_has("piscine figee"),           "0.1.7 : le refus est nomme dans le journal");
}

/* --- scenario 2: the fix. Same kernel timeline, anticipation at 50%. The
 * grow leaves at 8 MiB translated, while the VM window is still open. */
static void sc_anticipe(void) {
    g_free_user  = 23u << 20;
    g_vm_max     = 16u << 20;
    g_vm_close_at = 9u << 20;                    /* identical to scenario 1 */
    size_t got = translate(20u << 20);
    CHECK(dyn86_jitpool_size == (20u << 20), "anticipe : piscine portee a 20 Mo");
    CHECK(got == (20u << 20),                "anticipe : les 20 Mo de traduction passent");
    CHECK(log_has("(anticipe)"),             "anticipe : le journal dit QUI a obtenu le segment");
    CHECK(!log_has("mmap FAIL"),             "anticipe : aucune allocation refusee");
}

/* --- scenario 3: the floor is honoured and self-tunes the size taken.
 * Free user after seg1 = 4 MiB, floor 3072 KiB => 1 MiB is the largest step
 * that leaves the floor intact; 2 MiB would leave 2048 KiB and must not be
 * taken. This is the guard against fixing the pool by starving what boots
 * after it. */
static void sc_plancher(void) {
    g_free_user  = (16u << 20) + (4u << 20);
    g_vm_max     = 16u << 20;
    translate(10u << 20);
    CHECK(dyn86_jitpool_size == (17u << 20), "plancher : 1 Mo pris, pas 2 (plancher 3072 Ko tenu)");
    CHECK(g_free_user >= (3072L << 10),      "plancher : >= 3072 Ko de RAM user laisses libres");
}

/* --- scenario 4: the floor is a POLICY, never a new way to die. When the
 * floor defers a grow and the pool then runs out for real, the demand path
 * must still take the memory: a thin margin beats a dead thread. */
static void sc_plancher_pas_fatal(void) {
    g_free_user  = (16u << 20) + (2u << 20);     /* 2 MiB left: the floor forbids eager */
    g_vm_max     = 16u << 20;
    translate(18u << 20);
    CHECK(log_has("ajourne"),                 "plancher : l'ajournement est journalise");
    CHECK(dyn86_jitpool_size > (16u << 20),   "plancher : la demande a l'epuisement passe OUTRE le plancher");
    CHECK(!log_has("piscine figee"),          "plancher : pas de mort par politique");
}

/* --- scenario 5: an anticipated grow refused BY THE KERNEL must not steal
 * the demand path's attempt. 0.1.7 always got one try at exhaustion; the new
 * eager try must never be able to remove it. Here the VM window is shut
 * during the eager attempt and reopens before exhaustion. */
static void sc_eager_nonlatch(void) {
    g_free_user  = 23u << 20;
    g_vm_max     = 16u << 20;
    translate(6u << 20);                         /* segment 1 opens; stay under 50% */
    g_vm_max     = 0;                            /* the VM window shuts */
    translate(4u << 20);                         /* crosses 50%: eager tries and is refused */
    CHECK(log_has("anticipation abandonnee"),    "non-latch : le refus anticipe est nomme a part");
    CHECK(!log_has("piscine figee"),             "non-latch : « figee » reste reserve au vrai mur");
    g_vm_max = 4u << 20;                         /* the window reopens */
    size_t got = translate(10u << 20);
    CHECK(dyn86_jitpool_size == (20u << 20),     "non-latch : la demande a l'epuisement obtient encore 4 Mo");
    CHECK(got == (10u << 20),                    "non-latch : la traduction reprend");
}

int main(void) {
    static const struct { const char* nom; void (*fn)(void); } SC[] = {
        { "0.1.7 tel que livre (anticipation desarmee)", sc_0_1_7 },
        { "0.1.8 anticipation a 50%",                    sc_anticipe },
        { "plancher de RAM user respecte",               sc_plancher },
        { "plancher non fatal (la demande passe outre)", sc_plancher_pas_fatal },
        { "refus anticipe : pas de latch",               sc_eager_nonlatch },
    };
    int bad = 0;
    for (unsigned i = 0; i < sizeof SC / sizeof SC[0]; ++i) {
        printf("== %s ==\n", SC[i].nom);
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) { SC[i].fn(); fflush(stdout); _exit(fails ? 1 : 0); }
        int st = 0; waitpid(pid, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) bad = 1;
    }
    printf(bad ? "jitpool: ECHEC\n" : "jitpool: tout passe\n");
    return bad;
}
