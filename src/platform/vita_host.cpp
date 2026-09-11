// src/platform/vita_host.cpp — services de la console Vita (voir vita_host.h).
//
// Transcription des blocs qui vivaient en double chez les deux portages. Les
// commentaires sont conserves : ils portent des mesures faites sur materiel
// qu'on ne veut pas redecouvrir, et un commentaire perdu se repaie en heures.
#include "platform/vita_host.h"

// Tout ce fichier n'existe que sur cible : hors console il n'y a ni coeurs a
// repartir, ni horloge Sony, ni journal durable a tenir. Meme convention que
// platform/vita_audio.cpp.
#ifdef __vita__

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <pthread.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>

// ===========================================================================
//  JOURNAL DE PROGRESSION DURABLE
// ===========================================================================
// Verrou STATIQUE a initialiseur constant : pas d'initialisation paresseuse,
// donc pas de __cxa_guard_acquire — precisement la famille de pieges que la
// garde `nm` des scripts de build existe pour attraper.
static pthread_mutex_t g_progress_mx = PTHREAD_MUTEX_INITIALIZER;

void wx86_vita_progress(const char* msg) {
    // Pas de chemin = pas de journal. Deviner un chemin ferait ecrire deux
    // portages dans le meme fichier, ce qui est exactement le defaut que le
    // verrou ci-dessous existe pour empecher. Le cas ne se produit que si un
    // portage a omis la definition — le lien echoue alors, ce qui est le bon
    // moment pour l'apprendre.
    if (!wx86_vita_progress_path) return;
    pthread_mutex_lock(&g_progress_mx);
    FILE* f = std::fopen(wx86_vita_progress_path, "a");
    if (f) {
        unsigned s = (unsigned)(sceKernelGetProcessTimeWide() / 1000000ull);
        std::fprintf(f, "[%4u.%02us] ", s, 0u);
        std::fputs(msg, f); std::fputc('\n', f); std::fclose(f);
    }
    pthread_mutex_unlock(&g_progress_mx);
}

// Alias a liaison C pour que le code C du dynarec (reference faible) l'atteigne.
extern "C" void wx86_vita_progress_c(const char* m) { wx86_vita_progress(m); }

// ===========================================================================
//  HORLOGE MONOTONE
// ===========================================================================
uint64_t wx86_vita_now_ms() { return sceKernelGetProcessTimeWide() / 1000ull; }
uint64_t wx86_vita_now_us() { return sceKernelGetProcessTimeWide(); }
void wx86_vita_sleep_ms(uint32_t ms) { if (ms) sceKernelDelayThread(ms * 1000u); }

// ===========================================================================
//  REPARTITION DES FILS HOTES SUR LES COEURS USER
// ===========================================================================
namespace {
struct CoreEnt { char nom[14]; int uid; unsigned wanted; int rc; };
CoreEnt g_core[10];
int     g_coreN = 0;

// Schema lu UNE FOIS. Defaut « 222 » = la repartition historique a l'octet pres.
const char* core_scheme() {
    static const char* s_sch = nullptr;
    static bool s_done = false;
    if (!s_done) {
        s_done = true;
        const char* e = getenv("WX86_COEURS");
        if (!e) e = getenv("D2_COEURS");
        bool ok = e && e[0] && e[1] && e[2] && !e[3];
        for (int i = 0; ok && i < 3; ++i) if (e[i] < '0' || e[i] > '3') ok = false;
        if (e && e[0] && !ok) {
            char m[120];
            std::snprintf(m, sizeof m,
                "coeurs: WX86_COEURS=\"%s\" INVALIDE (3 chiffres 0..3 attendus) — repartition par defaut 222", e);
            wx86_vita_progress(m);
        }
        s_sch = ok ? e : "222";
    }
    return s_sch;
}
} // namespace

int wx86_vita_core_mask(int who) {
    if (who < 0 || who > 2) return SCE_KERNEL_CPU_MASK_USER_2;
    const char c = core_scheme()[who];
    // Le battement sur USER_0 : accepte, mais DENONCE. Verdict noyau
    // RUN-TO-BLOCK — un filet anti-famine pose sur le coeur des runners invites
    // n'est pas elu quand un runner ne bloque plus, c'est a dire exactement
    // quand il sert. Un filet inerte EN SILENCE est le mode de panne que ce
    // projet paie le plus cher (« diagnostic qui ment »).
    if (who == 2 && c == '0') {
        static bool s_said = false;
        if (!s_said) { s_said = true; wx86_vita_progress(
            "coeurs: ATTENTION battement anti-famine demande sur USER_0 — sous RUN-TO-BLOCK"
            " le filet ne sera PAS elu quand un runner invite cesse de bloquer (filet inerte)"); }
    }
    // Le chiffre `3` demande le QUATRIEME coeur (0x00080000). Jamais prouve sur
    // console : si le noyau le refuse, le rc de l'epinglage est publie dans la
    // ligne « coeurs: » et le fil reste ou il etait. Ne PAS l'employer avant
    // que la sonde de la meme ligne ait rendu « rc0 » sur materiel.
    return c == '0' ? SCE_KERNEL_CPU_MASK_USER_0
         : c == '1' ? SCE_KERNEL_CPU_MASK_USER_1
         : c == '3' ? WX86_CPU_MASK_USER_3
                    : SCE_KERNEL_CPU_MASK_USER_2;
}

void wx86_vita_core_register(const char* nom, int uid, unsigned wanted, int pin_rc) {
    if (g_coreN >= (int)(sizeof g_core / sizeof g_core[0])) return;
    CoreEnt& e = g_core[g_coreN++];
    std::snprintf(e.nom, sizeof e.nom, "%s", nom ? nom : "?");
    e.uid = uid; e.wanted = wanted; e.rc = pin_rc;
}

extern "C" int wx86_vita_pin_self(int mask, unsigned* relu) {
    const SceUID me = sceKernelGetThreadId();
    const int rc = sceKernelChangeThreadCpuAffinityMask(me, mask);
    const int r  = sceKernelGetThreadCpuAffinityMask(me);
    if (relu) *relu = (r < 0) ? 0u : (unsigned)r;
    return rc;
}

namespace {
// SONDE DU QUATRIEME COEUR, jouee UNE FOIS a la premiere fenetre de 10 s.
//
// Ce qu'elle fait, et ce qu'elle ne fait PAS. Elle cree un fil et NE LE DEMARRE
// JAMAIS : elle ne mesure donc que ce que le NOYAU ACCEPTE (rc du
// ChangeThreadCpuAffinityMask + masque relu), jamais qu'un coeur execute. Un
// fil demarre sur un coeur inexistant ne serait jamais elu, et le nettoyer
// demanderait un WaitThreadEnd borne puis un abandon — un risque gratuit pour
// une question a laquelle le rc repond deja. `activeCpuMask` du noyau est lu au
// passage : c'est LUI qui a dit « quatre bits » et qui a ouvert la question.
//
// Trois masques essayes : USER_2 (TEMOIN — un masque dont on SAIT qu'il est
// accepte ; sans lui, un « rc<0 » sur 0x80000 ne se distinguerait pas d'une
// sonde cassee), 0x00080000 (le 4e coeur seul) et 0x000F0000 (les quatre).
void core_probe_cpu3() {
    SceKernelSystemInfo si; std::memset(&si, 0, sizeof si); si.size = sizeof si;
    const int rsys = sceKernelGetSystemInfo(&si);
    SceUID th = sceKernelCreateThread("probe_c3", [](SceSize, void*) -> int { return 0; },
                                      0x10000100, 0x1000, 0, 0, nullptr);
    if (th < 0) {
        char m[112];
        std::snprintf(m, sizeof m,
            "coeurs: sonde 4e coeur — CreateThread KO (rc=0x%08x), activeCpuMask=0x%08x (rc=0x%08x)",
            (unsigned)th, rsys < 0 ? 0u : (unsigned)si.activeCpuMask, (unsigned)rsys);
        wx86_vita_progress(m);
        return;
    }
    const unsigned masks[3] = { SCE_KERNEL_CPU_MASK_USER_2, WX86_CPU_MASK_USER_3, 0x000F0000u };
    int  rc[3]; unsigned relu[3];
    for (int i = 0; i < 3; ++i) {
        rc[i]   = sceKernelChangeThreadCpuAffinityMask(th, (int)masks[i]);
        relu[i] = (unsigned)sceKernelGetThreadCpuAffinityMask(th);
    }
    sceKernelDeleteThread(th);          // jamais demarre : Delete suffit
    char m[224];
    std::snprintf(m, sizeof m,
        "coeurs: sonde 4e coeur — activeCpuMask=0x%08x (rc=0x%08x) |"
        " temoin 0x40000 rc=0x%08x relu=0x%x | 0x80000 rc=0x%08x relu=0x%x |"
        " 0xF0000 rc=0x%08x relu=0x%x",
        rsys < 0 ? 0u : (unsigned)si.activeCpuMask, (unsigned)rsys,
        (unsigned)rc[0], relu[0], (unsigned)rc[1], relu[1], (unsigned)rc[2], relu[2]);
    wx86_vita_progress(m);
    // Rappel de lecture, ecrit une fois pour ne pas avoir a le redecouvrir :
    // un masque RELU a 0 ne prouve RIEN sur ce firmware. Seul le rc distingue
    // « accepte » de « refuse », et le temoin 0x40000 dit si la sonde
    // elle-meme fonctionne.
    if (rc[0] < 0)
        wx86_vita_progress("coeurs: sonde 4e coeur NON CONCLUANTE — le temoin USER_2 lui-meme est refuse");
    else if (rc[1] >= 0)
        wx86_vita_progress("coeurs: 4e coeur (0x80000) ACCEPTE par le noyau — reste a prouver qu'un fil y COURT"
                           " (chiffre 3 de WX86_COEURS, puis lire le champ d= de la ligne coeurs:)");
    else
        wx86_vita_progress("coeurs: 4e coeur (0x80000) REFUSE par le noyau — trois coeurs user, pas quatre");
}
} // namespace

// Publie pour CHAQUE fil hote :
//   v=<n>   coeur DEMANDE (0/1/2/3), deduit du masque
//   rc      rc de sceKernelChangeThreadCpuAffinityMask (ok = 0)
//   m=<hex> masque RELU par sceKernelGetThreadCpuAffinityMask
//   c=<n>   currentCpuId  (sceKernelGetThreadInfo)
//   d=<n>   lastExecutedCpuId — LE chiffre qui tranche
//
// PRECAUTION, mesuree sur console : sceKernelGetThreadCpuAffinityMask rend
// 0x00000000 MEME pour un fil epingle avec succes sur ce firmware. Un « m=0x0 »
// n'est donc PAS une preuve d'echec — c'est pour ca que la ligne publie AUSSI
// le rc de la demande et surtout lastExecutedCpuId, qui dit sur quel coeur
// PHYSIQUE le fil a reellement tourne.
void wx86_vita_core_window_line() {
    if (!g_coreN) return;
    static bool s_probed = false;
    if (!s_probed) { s_probed = true; core_probe_cpu3(); }
    char buf[224]; int used = std::snprintf(buf, sizeof buf, "coeurs:");
    int inline_n = 0;
    for (int i = 0; i < g_coreN; ++i) {
        const CoreEnt& e = g_core[i];
        int want = (e.wanted & SCE_KERNEL_CPU_MASK_USER_0) ? 0
                 : (e.wanted & SCE_KERNEL_CPU_MASK_USER_1) ? 1
                 : (e.wanted & SCE_KERNEL_CPU_MASK_USER_2) ? 2
                 : (e.wanted & WX86_CPU_MASK_USER_3)       ? 3 : -1;
        int relu = (e.uid > 0) ? sceKernelGetThreadCpuAffinityMask(e.uid) : 0;
        SceKernelThreadInfo ti; std::memset(&ti, 0, sizeof ti); ti.size = sizeof ti;
        int rti = (e.uid > 0) ? sceKernelGetThreadInfo(e.uid, &ti) : -1;
        char one[96];
        std::snprintf(one, sizeof one, " %s=v%d/rc%d/m0x%x/c%d/d%d",
                      e.nom, want, e.rc < 0 ? -1 : 0, (unsigned)relu,
                      rti < 0 ? -1 : (int)ti.currentCpuId,
                      rti < 0 ? -1 : (int)ti.lastExecutedCpuId);
        const int need = (int)std::strlen(one);
        if (used + need >= (int)sizeof buf - 1 || inline_n >= 3) {
            wx86_vita_progress(buf);
            used = std::snprintf(buf, sizeof buf, "coeurs:"); inline_n = 0;
        }
        std::memcpy(buf + used, one, (size_t)need + 1); used += need; ++inline_n;
    }
    if (used > 7) wx86_vita_progress(buf);
}

#endif // __vita__
