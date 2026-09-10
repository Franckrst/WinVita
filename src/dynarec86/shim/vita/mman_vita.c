/* src/dynarec86/shim/vita/mman_vita.c — mmap façade over Vita memblocks.
 * See sys/mman.h in this directory for the contract.
 *
 * PROT_EXEC requests come only from Box86's AllocDynarecMap (RWX chunks for
 * emitted ARM code): served from the VM domain (sceKernelAllocMemBlockForVM),
 * the one mechanism homebrew has for runnable generated code. The domain is
 * opened once and kept open — Box86 interleaves emission and execution, and
 * per-block Open/Close would serialize every translation.  VM sizes are
 * rounded to 1 MiB (hardware alloc granularity for the VM type); plain RW
 * requests round to 4 KiB USER_RW blocks.
 */
#include "sys/mman.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>   /* getenv : knob D2_VMBRACKET */
#include <string.h>

#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>
#include <pthread.h>

/* Registry of live blocks: munmap frees whole blocks by exact base (that is
 * the only pattern custommem.c uses), and the cache-sync needs base+uid. */
#define DYN86_MAXBLK 256
/* boot-progress logger (vita_present.cpp). Weak so any link without the
 * presentation layer still resolves; guarded at each call site. An alloc
 * failure MUST reach boot_progress.txt: on hardware there is no stderr and a
 * silent MAP_FAILED looks like "the app closes at launch" (2026-08-24). */
__attribute__((weak)) void d2vita_progress_c(const char* msg);
/* `pool` : sous-bloc de la piscine JIT. L'uid reste celui de la PISCINE — la
 * synchronisation du domaine VM (dyn86_vita_clear_cache) en a besoin, et un
 * uid bidon y faisait echouer le sync : le code emis ne devenait jamais
 * executable et le demarrage mourait juste apres la reservation (constate). */
typedef struct { void* base; size_t size; SceUID uid; int vm; int pool; } Blk;
static Blk g_blk[DYN86_MAXBLK];
/* Registry lock (audit T12 — allocator-path hardening, same commit as the
 * custommem mutex_dynarec): mmap() is reached under TWO different upstream
 * locks — customMalloc holds mutex_blocks (RW blocks), AllocDynarecMap holds
 * mutex_dynarec (VM blocks) — so two threads can be in here CONCURRENTLY.
 * The slot scan + insert (and munmap's find + clear) mutate g_blk with no
 * lock of their own: a slot collision unregisters a live block, and a block
 * missing from the registry makes dyn86_vita_clear_cache return WITHOUT
 * syncing = stale icache for freshly emitted code. qemu never sees this
 * (real kernel mmap, no registry). Leaf lock, cold paths (chunk creation,
 * per-translation cache sync). pte handles the lazy-init sentinel. */
static pthread_mutex_t g_blk_mx = PTHREAD_MUTEX_INITIALIZER;

/* --- VM domain: PER-THREAD open (console root cause, session 3 coredumps) ---
 * The 14 rtc+3s "allocBlock crashes" of audit T12 §11 re-read: BOTH parsed
 * dumps show a byte-perfect HEALTHY blockmark chain (tail free mark =
 * chunk_size - offset - 2 marks, exact: 0x1ffe90 @+0x160 and 0x1bd230
 * @+0x42dc0) and the fault is the FIRST WRITE (strb [r2,#7], allocBlock's
 * s->next.fill=1) into a mapped VM chunk. That is a write-PERMISSION Data
 * abort, not corruption: sceKernelOpenVMDomain grants VM-domain write access
 * via the DACR, which lives in each thread's saved CPU context — the open is
 * effective for the CALLING thread. The old code opened it ONCE globally
 * (g_vm_open), i.e. only for whichever thread performed the first VM mmap
 * (the boot main). Under coop a single thread ever writes JIT memory: stable
 * for years. Under D2SCHED=native, the first pte runner that translates
 * (Game.exe's first threads, ~rtc+3s) writes the arena with a virgin DACR ->
 * deterministic Data abort in allocBlock. qemu: real kernel mmap, no domain.
 * Vita3K: no DACR emulation. Only the console fires it — exactly the
 * observed tier split. Fix: every thread that may WRITE JIT memory calls
 * this before its first write (mmap covers the allocating thread; the
 * native scheduler calls it at main+runner entry). Idempotent per thread;
 * rc logged on first failure only (a repeat-open rc quirk must not spam). */
/* VERDICT CONSOLE session 4 (2026-08-29, audit T12 §13.1/§13.2) : ce remede
 * NE MARCHE PAS et le journal de la console le dit lui-meme. Le PREMIER appel
 * du processus rend 0 ; TOUT appelant suivant (les runners pte qui traduisent)
 * rend rc=0x80010058 (famille errno SCE, 0x58 = 88 = ENOSYS), et le crash
 * rtc+3s est INCHANGE, au motif identique (chaine blockmark saine a l'octet
 * pres, ldrb reussi / strb refuse au meme octet). Le diagnostic de §12.1
 * (faute de PERMISSION, DACR) tient ; c'est le mecanisme choisi ici qui tombe :
 * sceKernelOpenVMDomain semble n'accepter qu'UNE ouverture par PROCESSUS.
 * A trancher AVANT tout autre correctif, par une sonde de deux lignes : le main
 * appelle sceKernelOpenVMDomain() DEUX fois et journalise les deux rc (un second
 * refus depuis le MEME fil = one-shot par processus ; un second succes = c'est
 * bien une affaire de fil). Pistes ensuite : fil traducteur unique dedie,
 * paire Close/Open sous verrou (attention : Close rend les memblocks VM NON
 * EXECUTABLES pour tout le processus), ou double mapping RW/RX.
 * Ni qemu (pas de domaine) ni Vita3K (HLE rend 0 a tout fil) ne voient ce
 * point : seule la console juge. La fonction reste en place — elle est
 * inoffensive et c'est elle qui IMPRIME le refus, la seule mesure qu'on ait.
 * CORRECTION (session 6, audit §16) : l'hypothese « une ouverture par
 * PROCESSUS » de ce paragraphe est FAUSSE, la sonde l'a refutee — c'est une
 * bascule a etat RE-ARMABLE dont la portee est le FIL. Voir le bloc suivant. */
/* --- BRACKET Close+Open : DEFAUT ON, kill-switch D2_VMBRACKET=0 -----------
 * Recherche du 2026-08-29 (docs/audit T12 §14) : le commentaire du header
 * vitasdk sur ce couple de fonctions est INVERSE. Les deux seules sources
 * primaires disent l'inverse et sont d'accord entre elles :
 *   - gist yifanlu 43a35324f3b76391cd15c6b96ae8b831 (auteur HENkaku/vitasdk,
 *     l'exemple dynarec de reference) : Open = « set domain to be writable by
 *     user », Close = « set domain back to read-only » ;
 *   - vita-luajit (hyln9), src/lj_mcode.c, le seul VRAI JIT Vita publie :
 *       mcode_setprot(..., MCPROT_RX)  -> sceKernelCloseVMDomain()
 *       mcode_setprot(..., MCPROT_RWX) -> sceKernelOpenVMDomain()
 * Donc Open = RWX (sur-ensemble), Close = RX : « ldrb OK puis strb fautant au
 * meme octet » (le crash de allocBlock, audit §13.1) est EXACTEMENT la
 * signature d'un domaine ferme. Et vita-luajit appelle la PAIRE des milliers
 * de fois : la bascule est l'usage NOMINAL, « ouvrir une fois et garder
 * ouvert » (notre choix historique) est l'exception.
 * MESURE SUR CONSOLE REELLE (session 6, audit §16, decision utilisateur §17) :
 * la sonde D2_VMPROBE a tranche l'arbre §14.9 sur la branche A2 / ligne 1 —
 * open#1=0, open#2=0x80010058 (deja ouvert), close=0, open#3=0, et les phases
 * F (fil Sce), G (fil pte) et G2 (le main, sans se re-armer, APRES les
 * brackets de F et G) ecrivent TOUTES OK. Donc : la permission d'ecriture VM
 * (DACR) a pour portee le FIL et non le processus ni le coeur (phase H), et
 * Close ne desarme QUE son appelant — le bracket n'ouvre aucune fenetre
 * mortelle pour les autres fils. Arme, il donne sur matériel un solo natif
 * complet (premiere image a 283 s, CLEAN EXIT frame=600), un soak 9000 images
 * sans un seul Halt, et un FAMINETEST PASS (detections=4). Sans lui, le natif
 * meurt a rtc+3 s sur le premier fil traducteur.
 * D'ou le DEFAUT ON (convention D2_FAMINE, cf. tools/rt_boot.cpp : absence de
 * la variable = arme, kill-switch explicite a '0'). Le mode historique
 * « ouverture unique » n'est conserve QUE comme kill-switch d'A/B sur
 * matériel : D2_VMBRACKET=0. Les deux modes s'estampillent dans
 * boot_progress — jamais de changement de mode silencieux.
 * Ni qemu (pas de domaine) ni Vita3K (HLE rend 0 a tout fil) ne voient ce
 * knob : seule la console juge, ce fichier n'est compile que pour __vita__.
 * Verrou obligatoire autour de la paire : sans lui, deux fils qui se croisent
 * (Close/Close/Open/Open) laissent le second Open refuse et le premier fil
 * non arme. */
static pthread_mutex_t g_vmdom_mx = PTHREAD_MUTEX_INITIALIZER;

void dyn86_vita_open_vm_thread(void) {
    static __thread int t_vm_open = 0;
    if (t_vm_open) return;
    t_vm_open = 1;
    static int bracket = -1;   /* course benigne : deux fils calculent la meme valeur */
    /* Defaut ON : l'ABSENCE de la variable arme le bracket (convention
     * D2_FAMINE). Seul un '0' explicite retombe sur le mode historique. */
    if (bracket < 0) { const char* e = getenv("WX86_VMBRACKET"); if (!e) e = getenv("D2_VMBRACKET"); bracket = (e && *e == '0') ? 0 : 1; }
    int rcC = 0, rc;
    if (bracket) {
        pthread_mutex_lock(&g_vmdom_mx);
        rcC = sceKernelCloseVMDomain();
        rc  = sceKernelOpenVMDomain();
        pthread_mutex_unlock(&g_vmdom_mx);
    } else {
        rc = sceKernelOpenVMDomain();
    }
    static int announced = 0;   /* one line for the console log, first thread only */
    if (!announced) {
        announced = 1;
        if (d2vita_progress_c) {
            /* Estampille dans LES DEUX SENS (doctrine : jamais de changement
             * de mode silencieux) — un journal dit toujours quel mode a
             * tourne. Libelle conditionnel (review) : un rc<0 ne doit pas se
             * lire « arme » ni « active ». */
            char m[160];
            snprintf(m, sizeof m,
                     rc < 0 ? (bracket ? "vm-domain: BRACKET arme mais EN ECHEC (premier rc=0x%08x)"
                                       : "vm-domain: BRACKET DESACTIVE (D2_VMBRACKET=0) - mode legacy open-unique EN ECHEC (premier rc=0x%08x)")
                            : (bracket ? "vm-domain: BRACKET close+open arme (premier rc=0x%08x)"
                                       : "vm-domain: BRACKET DESACTIVE (D2_VMBRACKET=0) - mode legacy open-unique (premier rc=0x%08x)"), (unsigned)rc);
            d2vita_progress_c(m);
        }
    } else if (rc < 0 && d2vita_progress_c) {
        /* a per-thread failure is exactly the crash we are fixing: say it */
        char m[160];
        if (bracket)
            snprintf(m, sizeof m, "vm-domain: BRACKET close=0x%08x puis open rc=0x%08x sur un fil ecrivain JIT",
                     (unsigned)rcC, (unsigned)rc);
        else
            snprintf(m, sizeof m, "vm-domain: [legacy D2_VMBRACKET=0] OpenVMDomain rc=0x%08x sur un fil ecrivain JIT",
                     (unsigned)rc);
        d2vita_progress_c(m);
    }
}
/* Live JIT (VM) and RW bytes held by this façade. Exposed (non-static) so the
 * watchdog heartbeat can chart the translation cache's real footprint: the JIT
 * shares the user budget with the guest arena, and starving it is a silent
 * perf collapse (fill_fail storm -> retranslation every visit). Any future
 * budget reshuffle must keep a floor >= the measured in-game peak + margin. */
unsigned int dyn86_jit_cur = 0;   /* bytes in VM (PROT_EXEC) blocks */
/* ⚡ 08/09 : PISCINE JIT RESERVEE AU DEMARRAGE.
 * Le probleme corrige ici : le cache JIT et le tas newlib puisaient dans la
 * MEME reserve libre, chacun grandissant de son cote. Le dernier qui demande
 * perd — et quand c'est le JIT, le fil invite MEURT, parce qu'il n'y a pas
 * d'interpreteur dans ce build (shim_impl.c : Run() met quit=1). Le jeu
 * mourait donc apres un temps variable selon ce que le joueur explorait.
 * On reserve desormais la piscine EN UNE FOIS au premier besoin, puis on la
 * sous-alloue. Consequences :
 *   - le tas ne peut plus affamer le JIT, ni l'inverse ;
 *   - un budget insuffisant se voit AU DEMARRAGE, pas au bout de dix minutes ;
 *   - box86 ne rend jamais ses morceaux (FreeDynarecMap ne libere qu'A
 *     L'INTERIEUR d'un morceau), donc une allocation par bond est exactement
 *     equivalente a l'existant, sans la competition.
 * Si la reservation echoue, on retombe sur l'ancien chemin bloc-par-bloc :
 * aucune regression possible. */
static void*  g_jitpool      = 0;
static size_t g_jitpool_size = 0, g_jitpool_used = 0;
static SceUID g_jitpool_uid  = -1;
static int    g_jitpool_tried = 0;
unsigned int  dyn86_jitpool_size = 0, dyn86_jitpool_used = 0;   /* pour la jauge */
unsigned int dyn86_rw_cur  = 0;   /* bytes in plain RW blocks */

static Blk* blk_find(const void* p) {
    for (int i = 0; i < DYN86_MAXBLK; ++i)
        if (g_blk[i].base && (const char*)p >= (const char*)g_blk[i].base
                          && (const char*)p <  (const char*)g_blk[i].base + g_blk[i].size)
            return &g_blk[i];
    return 0;
}

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
    (void)addr; (void)flags; (void)fd; (void)offset;
    if (!length) { errno = EINVAL; return MAP_FAILED; }

    pthread_mutex_lock(&g_blk_mx);
    int slot = -1;
    for (int i = 0; i < DYN86_MAXBLK; ++i) if (!g_blk[i].base) { slot = i; break; }
    if (slot < 0) { pthread_mutex_unlock(&g_blk_mx); errno = ENOMEM; return MAP_FAILED; }

    SceUID uid;
    int vm = (prot & PROT_EXEC) != 0;
    size_t size;
    if (vm) {
        size = (length + 0xFFFFFu) & ~(size_t)0xFFFFFu;      /* 1 MiB */
        /* ⚠️ 08/09, 3e passe : CES DEUX GARDE-FOUS SONT DESACTIVES PAR DEFAUT.
         * Je les avais introduits en affirmant que refuser une allocation JIT
         * etait « plus lent mais vivant ». C'EST FAUX, et le code le dit noir
         * sur blanc — src/dynarec86/shim/shim_impl.c :
         *     // ---- interpreter stub: no interpreter in the PoC ----
         *     int Run(...) { ... emu->quit = 1; emu->error |= ERR_UNIMPL; }
         * Il n'y a PAS d'interpreteur dans ce build. Un bloc qui echoue mene a
         * Run(), qui met quit=1 et TUE le fil invite. Les deux sessions qui se
         * sont terminees par [ring-final] sans bad_alloc, c'etait ca : mon
         * bridage transformait une mort possible en mort certaine.
         * On les garde comme boutons de diagnostic (D2_JITRESERVE_KB,
         * D2_JITMAX_MB) mais a 0 — inertes — tant qu'aucun interpreteur
         * n'existe. La seule vraie solution est de DONNER de la memoire au JIT.
         *
         * Texte d'origine du plafond :
         * ⚡ 08/09 : PLAFOND DU CACHE JIT. Le code traduit n'avait AUCUNE borne :
         * mesure en partie reelle, il passe de 4 a 13 Mio en cinq minutes et
         * continue, pendant que le tas newlib grimpe de 22 a 30 Mio. Les deux
         * puisent dans les ~11 Mio laisses libres par l'arene, et le processus
         * meurt en std::bad_alloc (TTY_INFO du core dump).
         *
         * Refuser une allocation JIT est SANS DANGER, contrairement a liberer du
         * code qu'un fil execute peut-etre : mmap rend MAP_FAILED, FillBlock
         * echoue, dynablock.c libere le bloc, incremente dyn86_fill_fail (le
         * « fail= » de la ligne alive) et l'emulateur RETOMBE SUR L'INTERPRETEUR
         * pour cette adresse. On echange de la vitesse contre le fait de rester
         * en vie, et le compteur rend l'echange visible au lieu de le cacher. */
        {
            static unsigned cap_mb = 0, said = 0, said2 = 0;
            if (!cap_mb) { const char* e = getenv("WX86_JITMAX_MB"); if (!e) e = getenv("D2_JITMAX_MB");
                           cap_mb = e ? (unsigned)atoi(e) : 0u; }   /* 0 = DESACTIVE */
            /* ⚡ 08/09, 2e passe : LE PLAFOND ABSOLU NE SUFFIT PAS. Session de
             * 920 s : le JIT n'etait qu'a 11 Mio — bien sous les 24 — et c'est
             * la RAM SYSTEME qui s'est epuisee la premiere. Resultat, une
             * tempete d'echecs (« mmap FAIL VM 2048 KB ... free user=-1024 KB »
             * des dizaines de fois par seconde), le jeu fige 3,5 s, puis mort.
             * Une limite absolue ne protege de rien quand le voisin mange le
             * budget : ce qu'il faut borner, c'est CE QU'IL RESTE.
             *
             * On garde donc une reserve plancher de RAM utilisateur. Refuser
             * ici est le chemin GRACIEUX (repli interpreteur, « fail= » monte) ;
             * laisser le systeme refuser est le chemin FATAL. */
            static unsigned res_kb = 0;
            if (!res_kb) { const char* e = getenv("WX86_JITRESERVE_KB"); if (!e) e = getenv("D2_JITRESERVE_KB");
                           res_kb = e ? (unsigned)atoi(e) : 0u; }   /* 0 = DESACTIVE, voir ci-dessous */
            SceKernelFreeMemorySizeInfo fi; fi.size = sizeof fi;
            if (res_kb && sceKernelGetFreeMemorySize(&fi) >= 0 &&
                (long)(fi.size_user >> 10) - (long)(size >> 10) < (long)res_kb) {
                pthread_mutex_unlock(&g_blk_mx);
                if (!said2 && d2vita_progress_c) { said2 = 1; char m[160];
                    snprintf(m, sizeof m,
                        "JIT BRIDE : reserve systeme atteinte (libre %d Ko, plancher %u Ko) — interpretation en repli, voir fail=",
                        (int)(fi.size_user >> 10), res_kb);
                    d2vita_progress_c(m); }
                errno = ENOMEM; return MAP_FAILED;
            }
            if (cap_mb && dyn86_jit_cur + size > (size_t)cap_mb << 20) {
                pthread_mutex_unlock(&g_blk_mx);
                if (!said && d2vita_progress_c) { said = 1; char m[152];
                    snprintf(m, sizeof m,
                        "JIT PLAFONNE a %u Mo (en cours %u Mo) — interpretation en repli, voir fail= ; D2_JITMAX_MB pour changer",
                        cap_mb, (unsigned)(dyn86_jit_cur >> 20));
                    d2vita_progress_c(m); }
                errno = ENOMEM; return MAP_FAILED;
            }
        }
        /* --- piscine : reserver une fois, puis sous-allouer --- */
        if (!g_jitpool_tried) {
            g_jitpool_tried = 1;
            const char* e = getenv("WX86_JITPOOL_MB"); if (!e) e = getenv("D2_JITPOOL_MB");
            size_t want = (size_t)((e ? (unsigned)atoi(e) : 16u)) << 20;
            if (want) {
                SceUID u = sceKernelAllocMemBlockForVM("dyn86_jitpool", want);
                void* pb = 0;
                if (u >= 0 && sceKernelGetMemBlockBase(u, &pb) >= 0 && pb) {
                    g_jitpool = pb; g_jitpool_size = want; g_jitpool_uid = u;
                    dyn86_jitpool_size = (unsigned int)want;
                } else if (u >= 0) { sceKernelFreeMemBlock(u); }
                if (d2vita_progress_c) { char m[144];
                    snprintf(m, sizeof m, g_jitpool
                        ? "JIT: piscine de %u Mo reservee (sous-allocation ; le tas ne peut plus l'affamer)"
                        : "JIT: piscine de %u Mo REFUSEE — repli bloc-par-bloc (ancien comportement)",
                        (unsigned)(want >> 20));
                    d2vita_progress_c(m); }
            }
        }
        if (g_jitpool && g_jitpool_used + size <= g_jitpool_size) {
            void* p = (char*)g_jitpool + g_jitpool_used;
            g_jitpool_used += size;
            dyn86_jitpool_used = (unsigned int)g_jitpool_used;
            dyn86_jit_cur += (unsigned int)size;
            g_blk[slot].base = p;   g_blk[slot].size = size;
            g_blk[slot].uid  = g_jitpool_uid;   /* uid de la PISCINE : requis par le sync VM */
            g_blk[slot].vm   = 1;
            g_blk[slot].pool = 1;              /* ne PAS rendre au noyau au munmap */
            pthread_mutex_unlock(&g_blk_mx);
            dyn86_vita_open_vm_thread();
            return p;
        }
        uid = sceKernelAllocMemBlockForVM("dyn86_jit", size);
    } else {
        size = (length + 0xFFFu) & ~(size_t)0xFFFu;          /* 4 KiB */
        uid = sceKernelAllocMemBlock("dyn86_rw", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                     size, 0);
    }
    if (uid < 0) {
        pthread_mutex_unlock(&g_blk_mx);
        if (d2vita_progress_c) {
            SceKernelFreeMemorySizeInfo fi; fi.size = sizeof fi;
            int frc = sceKernelGetFreeMemorySize(&fi);
            char m[144];
            snprintf(m, sizeof m, "mmap FAIL %s %u KB: sce=0x%08x free user=%d KB cdram=%d KB phycont=%d KB",
                     vm ? "VM" : "RW", (unsigned)(size >> 10), (unsigned)uid,
                     frc < 0 ? -1 : fi.size_user >> 10,
                     frc < 0 ? -1 : fi.size_cdram >> 10,
                     frc < 0 ? -1 : fi.size_phycont >> 10);
            d2vita_progress_c(m);
        }
        errno = ENOMEM; return MAP_FAILED;
    }
    if (vm) dyn86_jit_cur += (unsigned int)size; else dyn86_rw_cur += (unsigned int)size;
    if (size >= (64u << 20) && d2vita_progress_c) {   /* big blocks: confirm on HW */
        char m[96];
        snprintf(m, sizeof m, "memblock %s %u MB ok (uid=0x%08x)",
                 vm ? "VM" : "RW", (unsigned)(size >> 20), (unsigned)uid);
        d2vita_progress_c(m);
    }

    void* base = 0;
    if (sceKernelGetMemBlockBase(uid, &base) < 0 || !base) {
        pthread_mutex_unlock(&g_blk_mx);
        sceKernelFreeMemBlock(uid); errno = ENOMEM; return MAP_FAILED;
    }
    if (vm) dyn86_vita_open_vm_thread();   /* per-thread — see the comment above */

    g_blk[slot].base = base; g_blk[slot].size = size;
    g_blk[slot].uid = uid;   g_blk[slot].vm = vm;  g_blk[slot].pool = 0;
    pthread_mutex_unlock(&g_blk_mx);
    /* mmap(MAP_ANONYMOUS) — the desktop/qemu path every run was validated against —
     * always hands back ZERO-filled pages. sceKernelAllocMemBlock does NOT: it may
     * return stale/garbage bytes. The guest relies on the zero baseline that Windows
     * itself guarantees (VirtualAlloc MEM_COMMIT + BSS + fresh heap all read as zero),
     * so a non-zeroed arena makes the guest read garbage where it expects NUL — e.g.
     * the Rogue level-load's freshly-committed VirtualAlloc pages, which trip a Fog
     * "Unrecoverable internal error" Halt on Vita but not on qemu. Zero non-exec
     * blocks (the guest arena/heap) to restore mmap-ANON parity. JIT (vm) blocks are
     * fully overwritten by the emitter, so they need no clear. */
    if (!vm) memset(base, 0, size);
    return base;
}

int munmap(void* addr, size_t length) {
    (void)length;
    pthread_mutex_lock(&g_blk_mx);
    Blk* b = blk_find(addr);
    if (!b || b->base != addr) { pthread_mutex_unlock(&g_blk_mx); errno = EINVAL; return -1; }
    if (b->vm) { if (dyn86_jit_cur >= b->size) dyn86_jit_cur -= (unsigned int)b->size; }
    else       { if (dyn86_rw_cur  >= b->size) dyn86_rw_cur  -= (unsigned int)b->size; }
    SceUID uid = b->uid;
    const int was_pool = b->pool;
    memset(b, 0, sizeof *b);
    pthread_mutex_unlock(&g_blk_mx);
    /* Sous-bloc de la piscine : le noyau ne connait que la piscine entiere, et
     * box86 ne rend de toute facon jamais ses morceaux. On relache seulement
     * l'entree du registre — liberer l'uid ici detruirait TOUTE la piscine. */
    if (!was_pool) sceKernelFreeMemBlock(uid);
    return 0;
}

int mprotect(void* addr, size_t len, int prot) {
    (void)addr; (void)len; (void)prot;
    return 0;   /* see header: intentionally a no-op on Vita */
}

uint64_t dyn86_sync_us = 0;   /* cumulative cache-sync wall time (watchdog) */

void dyn86_vita_clear_cache(void* beg, void* end) {
    /* Snapshot the block under the registry lock (a concurrent mmap may be
     * scanning/inserting); the sync itself runs unlocked on the copy — VM
     * blocks are only ever freed at teardown. */
    pthread_mutex_lock(&g_blk_mx);
    Blk* bp = blk_find(beg);
    Blk bcopy; if (bp) bcopy = *bp;
    pthread_mutex_unlock(&g_blk_mx);
    Blk* b = bp ? &bcopy : 0;
    if (!b || !b->vm) return;                    /* non-VM: nothing to sync */
    /* Sync only the pages the emitter touched. A whole-block sync is a 2 MiB
     * dcache clean + icache invalidate: ~25 ms per translated block on the
     * A9, which turned every first visit of new code into a freeze (zone
     * loads = minutes-long translation storms). Page-rounded sub-range,
     * whole-block fallback if the kernel rejects it. */
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    uintptr_t base = (uintptr_t)b->base, top = base + b->size;
    uintptr_t lo = (uintptr_t)beg & ~(uintptr_t)0xFFF;
    uintptr_t hi = ((uintptr_t)end + 0xFFF) & ~(uintptr_t)0xFFF;
    if (lo < base) lo = base;
    if (hi > top)  hi = top;
    if (hi <= lo || sceKernelSyncVMDomain(b->uid, (void*)lo, (SceSize)(hi - lo)) < 0)
        sceKernelSyncVMDomain(b->uid, b->base, b->size);
    dyn86_sync_us += sceKernelGetProcessTimeWide() - t0;
}
