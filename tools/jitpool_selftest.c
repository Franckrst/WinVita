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
static struct { void* base; size_t size; int live; int phy; } g_fk[FAKE_MAXBLK];
static long   g_free_user  = 0;           /* bytes */
/* Budget PHYCONT, d'ou la piscine RW des metadonnees de box86 se sert. Sur
 * console il vaut 26 624 Ko et AUCUNE session ne l'a jamais vu bouger — c'est
 * precisement ce qui en fait la reserve candidate. Modelise a part de
 * g_free_user, sans quoi les scenarios ne pourraient pas montrer ce qui compte :
 * la piscine tient alors meme que la RAM utilisateur est a zero. */
static long   g_free_phycont = 0;         /* bytes */
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
    (void)name; (void)opt;
    /* Le TYPE decide de la partition ponctionnee : c'est tout l'interet de la
     * piscine RW en phycont, et un faux noyau qui ignorerait le type ferait
     * passer les scenarios pour de mauvaises raisons. */
    const int phy = (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW);
    long* budget = phy ? &g_free_phycont : &g_free_user;
    if ((long)size > *budget) return (SceUID)0x80020190; /* generic ENOMEM-ish */
    int u = fk_new(size);
    if (u < 0) return (SceUID)0x80020190;
    *budget -= (long)size;
    g_fk[u].phy = phy;
    return u;
}
int sceKernelFreeMemBlock(SceUID uid) {
    if (uid <= 0 || uid >= FAKE_MAXBLK || !g_fk[uid].live) return -1;
    if (g_fk[uid].phy) g_free_phycont += (long)g_fk[uid].size;
    else               g_free_user    += (long)g_fk[uid].size;
    free(g_fk[uid].base); memset(&g_fk[uid], 0, sizeof g_fk[uid]);
    return 0;
}
int sceKernelGetMemBlockBase(SceUID uid, void** base) {
    if (uid <= 0 || uid >= FAKE_MAXBLK || !g_fk[uid].live) return -1;
    *base = g_fk[uid].base; return 0;
}
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo* info) {
    /* NEGATIF si le budget est depasse : c'est ce que la console rend
     * reellement (« libre user=-2048 Ko »), et le pincer a zero ici
     * effacerait le seul cas ou le plancher est interrogeable. */
    info->size_user = (int)g_free_user;
    info->size_cdram = 0;
    info->size_phycont = (int)g_free_phycont;
    return 0;
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
    /* dyn86_pool_near_full() (dynablock.c) lit ce meme couple used/size a
     * >=95% pour declencher l'eviction meme sans refus de l'allocateur — la
     * scene qui a motive ce chantier est exactement celle-ci : la piscine EST
     * pleine ici, le seuil doit donc lire vrai. */
    CHECK(dyn86_jitpool_used * 100ull >= (uint64_t)dyn86_jitpool_size * 95ull,
          "0.1.7 : la piscine pleine franchit bien le seuil 95% de dyn86_pool_near_full");
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

/* --- scenario 3 : SANS PLANCHER, l'anticipation prend ce qui est la.
 * C'etait le scenario du plancher : avec 4 Mo libres apres le segment 1 et un
 * plancher de 3072 Ko, seul 1 Mo pouvait etre pris. Le plancher a ete retire
 * — les metadonnees de box86 qu'il reservait vivent maintenant en phycont —
 * donc la descente d'echelle prend le plus grand palier qui TIENNE, soit
 * 4 Mo. C'est l'effet recherche : sur console, le segment 2 echouait a ses
 * cinq tailles alors que quelques megaoctets etaient la. */
static void sc_anticipe_sans_plancher(void) {
    g_free_user  = (16u << 20) + (4u << 20);
    g_vm_max     = 16u << 20;
    translate(10u << 20);
    CHECK(dyn86_jitpool_size == (20u << 20), "sans plancher : 4 Mo pris, pas 1");
    CHECK(!log_has("ajourne"),               "sans plancher : plus d'ajournement de politique");
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


/* ---- piscine RW (metadonnees de box86) --------------------------------- */
/* box86 demande ses blocs de metadonnees par MMAPSIZE = 64 Kio, en RW pur
 * (custommem.c, customMalloc). C'est cette demande-la, et elle seule, que la
 * piscine RW doit servir. */
extern unsigned int dyn86_rwpool_size, dyn86_rwpool_used;
static void* rw64(void) {
    return mmap(NULL, 64u << 10, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
}
/* Combien de blocs de 64 Kio on obtient d'affilee avant un refus. */
static int rw_runs(int n) {
    int got = 0;
    for (int i = 0; i < n; ++i) { if (rw64() == MAP_FAILED) break; ++got; }
    return got;
}

/* --- scenario 6 : la situation de la console. RAM utilisateur a ZERO (le
 * journal de terrain dit « libre user=-2048 Ko » des la 13e seconde), phycont
 * intact. Avant la piscine, le premier bloc de 64 Kio demande au noyau etait
 * refuse et customMalloc ecrivait a travers MAP_FAILED. Attendu : la piscine
 * est prise en phycont, les 32 blocs passent, et la RAM utilisateur n'est pas
 * touchee d'un octet. */
static void sc_rwpool_phycont(void) {
    g_free_user    = 0;                          /* partition user epuisee */
    g_free_phycont = 26u << 20;                  /* ce que la console annonce */
    int got = rw_runs(32);
    CHECK(got == 32,                             "phycont : 32 blocs de 64 Kio servis malgre user a zero");
    CHECK(dyn86_rwpool_size == (8u << 20),       "phycont : 8 Mo reserves");
    CHECK(log_has("en PHYCONT"),                 "phycont : le journal dit d'ou vient la memoire");
    CHECK(g_free_user == 0,                      "phycont : pas un octet pris a la RAM utilisateur");
    CHECK(g_free_phycont == (18L << 20),         "phycont : 18 Mo de phycont laisses libres");
}

/* --- scenario 7 : phycont indisponible (firmware, fragmentation, un service
 * systeme qui l'a pris). La piscine doit se replier sur USER_RW plutot que de
 * renoncer — et le dire, sans quoi un banc attribuerait au phycont une mesure
 * faite en user. */
static void sc_rwpool_repli(void) {
    g_free_user    = 16u << 20;
    g_free_phycont = 0;
    int got = rw_runs(32);
    CHECK(got == 32,                             "repli : les blocs passent quand meme");
    CHECK(log_has("USER_RW (repli)"),            "repli : le journal nomme le repli");
    CHECK(dyn86_rwpool_size > 0,                 "repli : une piscine a bien ete prise");
}

/* --- scenario 8 : les deux partitions refusent. La piscine ne doit pas etre
 * un nouveau mode de mort : on retombe exactement sur le comportement
 * d'avant elle — le noyau refuse, mmap rend MAP_FAILED, et c'est a
 * customMalloc de le traiter (ce qu'il fait desormais). */
static void sc_rwpool_absente(void) {
    g_free_user    = 0;
    g_free_phycont = 0;
    void* p = rw64();
    CHECK(p == MAP_FAILED,                       "absente : le refus remonte, il n'est pas masque");
    CHECK(log_has("REFUSEE en phycont ET en user"), "absente : le journal nomme les deux refus");
    CHECK(dyn86_rwpool_size == 0,                "absente : aucune piscine fantome");
}

int main(void) {
    static const struct { const char* nom; void (*fn)(void); } SC[] = {
        { "0.1.7 tel que livre (anticipation desarmee)", sc_0_1_7 },
        { "0.1.8 anticipation a 50%",                    sc_anticipe },
        { "anticipation sans plancher",                  sc_anticipe_sans_plancher },
        { "refus anticipe : pas de latch",               sc_eager_nonlatch },
        { "piscine RW en phycont (user a zero)",         sc_rwpool_phycont },
        { "piscine RW : repli en USER_RW",               sc_rwpool_repli },
        { "piscine RW : les deux refus, pas de mort",    sc_rwpool_absente },
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
