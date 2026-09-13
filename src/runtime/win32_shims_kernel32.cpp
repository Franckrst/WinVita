// src/runtime/win32_shims_kernel32.cpp — voir win32_shims_kernel32.h.
// Premiere vague de l'extraction du groupe KERNEL32 de tools/rt_boot.cpp
// (2026-09-11). KERNEL32 etait le plus gros bloc jamais audite : 252 cles,
// dont 248 encore cote portage.
//
// CRITERE DE CETTE VAGUE — deux conditions cumulees, pas une intuition :
//   1. le corps est IDENTIQUE (hors commentaires et espaces) a celui du
//      second consommateur du moteur, carn-vita, qui porte un tout autre
//      jeu. carn-vita descend du meme socle mais en a RETIRE la part
//      specifique a Diablo II (CheckRevision, Authenticode, automatisation
//      Battle.net) : ce qui y a survecu intact n'est donc pas du jeu.
//      C'est une constatation, pas un raisonnement.
//   2. le corps ne depend d'AUCUN symbole exterieur — ni helper heberge par
//      le portage, ni etat global, ni ordonnanceur, ni horloge. Il ne touche
//      que l'objet Cpu et des constantes.
//
// Les fonctions qui echouent a la condition 2 sont laissees au portage pour
// cette vague : les atomiques (ilk_ptr/ilk_add_fetch), le TLS et la derniere
// erreur (cur_tib, donc l'ordonnanceur), les fichiers (chemins de l'hote),
// la synchronisation et l'horloge. Elles sont generiques pour la plupart,
// mais leur deplacement demande d'abord d'exposer proprement le TIB courant
// et les atomiques cote moteur — un point d'extension a concevoir pour LES
// DEUX portages, pas un simple deplacement de texte.
#include "win32_shims_kernel32.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "guest_atomics.h"
#include "guest_thread_ctx.h"
extern "C" {
#include "my_cpuid.h"     // wx86_cpuid_features : les bits que NOTRE cpuid annonce
}
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
using namespace d2rt;

// ---- identite process / fils (voir l'en-tete) ---------------------------
// 0x0AE4 = 2788 : multiple de 4, != 1, dans la plage ou un vrai Windows
// distribue ses PID. 0x0AF0 pour le premier fil, puis +4 par fil.
static const uint32_t WX86_PID  = 0x00000AE4u;
static const uint32_t WX86_TID0 = 0x00000AF0u;
bool wx86_fid_avant() {
    static int st = -1;
    if (st < 0) st = (getenv("WX86_FID_AVANT") || getenv("D2_FID_AVANT")) ? 1 : 0;
    return st == 1;
}
uint32_t wx86_win_pid() { return wx86_fid_avant() ? 1u : WX86_PID; }
uint32_t wx86_win_tid(uint32_t schedId) {
    return wx86_fid_avant() ? schedId : (WX86_TID0 + 4u * schedId);
}
uint32_t wx86_sched_tid(uint32_t winTid) {
    if (wx86_fid_avant()) return winTid;
    return (winTid >= WX86_TID0 && ((winTid - WX86_TID0) & 3u) == 0)
         ? (winTid - WX86_TID0) / 4u : 0u;
}

// Alias locaux : les corps de la vague 2 sont deplaces mot pour mot depuis le
// portage, ou ces noms courts designaient les memes fonctions. Les definir ici
// evite de reecrire les corps, donc de les relire a l'aveugle.
static inline uint32_t* ilk_ptr(Cpu& c, uint32_t va){ return wx86_ilk_ptr(c,va); }
static inline uint32_t ilk_add_fetch(uint32_t* h, uint32_t v){ return wx86_ilk_add_fetch(h,v); }
static inline uint32_t ilk_sub_fetch(uint32_t* h, uint32_t v){ return wx86_ilk_sub_fetch(h,v); }
static inline uint32_t ilk_exchange (uint32_t* h, uint32_t v){ return wx86_ilk_exchange (h,v); }
static inline uint32_t ilk_fetch_add(uint32_t* h, uint32_t v){ return wx86_ilk_fetch_add(h,v); }
static inline uint32_t ilk_fetch_or (uint32_t* h, uint32_t v){ return wx86_ilk_fetch_or (h,v); }
static inline uint32_t ilk_fetch_and(uint32_t* h, uint32_t v){ return wx86_ilk_fetch_and(h,v); }
static inline uint32_t ilk_fetch_xor(uint32_t* h, uint32_t v){ return wx86_ilk_fetch_xor(h,v); }
static inline uint32_t ilk_cas(uint32_t* h, uint32_t e, uint32_t d){ return wx86_ilk_cas(h,e,d); }
static inline void set_lasterr(Cpu& c, uint32_t v){ wx86_set_lasterr(c,v); }
static inline uint32_t get_lasterr(Cpu& c){ return wx86_get_lasterr(c); }

static uint64_t g_perfFreq = 1000000ull;
void     wx86_set_perf_frequency(uint64_t hz) { if (hz) g_perfFreq = hz; }
uint64_t wx86_perf_frequency() { return g_perfFreq; }

void win32_shims_kernel32_install(Bridge& br){
    auto K=[&](const char* name,uint32_t ac,std::function<uint32_t(Cpu&)> fn){
        Shim s; s.argc=ac; s.stdcall_cleanup=true; s.tag=std::string("KERNEL32.dll!")+name;
        s.fn=std::move(fn);
        br.register_shim("KERNEL32.dll",name,s); };
    K("CopyFileA",3,[](Cpu&){ return 1u; });
    K("DecodePointer",1,[](Cpu&c){ return c.arg(0); });
    K("DuplicateHandle",7,[](Cpu&){ return 1u; });
    K("EncodePointer",1,[](Cpu&c){ return c.arg(0); });
    K("EnumSystemLocalesA",2,[](Cpu&){ return 1u; });
    K("FreeEnvironmentStringsA",1,[](Cpu&){ return 1u; });
    K("FreeEnvironmentStringsW",1,[](Cpu&){ return 1u; });
    K("FreeLibrary",1,[](Cpu&){ return 1u; });
    K("GetCPInfo",2,[](Cpu&c){ uint32_t p=c.arg(1); c.write_u32(p,1); return 1u; });   // MaxCharSize=1
    K("GetCurrentProcessId",0,[](Cpu&){ return wx86_win_pid(); });   // jamais 1 : ce PID n'existe pas
    K("GetDriveTypeA",1,[](Cpu&){ return 3u; });   // DRIVE_FIXED
    K("GetFileType",1,[](Cpu&){ return 1u; });                           // FILE_TYPE_CHAR
    K("GetProcessAffinityMask",3,[](Cpu&c){ if(c.arg(1))c.write_u32(c.arg(1),1); if(c.arg(2))c.write_u32(c.arg(2),1); return 1u; });
    K("GetProfileStringA",5,[](Cpu&){ return 0u; });
    K("GetStartupInfoA",1,[](Cpu&c){ uint32_t p=c.arg(0); uint8_t z[68]={0}; *(uint32_t*)z=68; c.write(p,z,68); return 0u; });
    K("GetStartupInfoW",1,[](Cpu&c){ uint32_t p=c.arg(0); if(p){ uint8_t z[68]={0}; *(uint32_t*)z=68; c.write(p,z,68);} return 0u; });
    K("GetSystemDirectoryA",2,[](Cpu&c){ uint32_t b=c.arg(0); const char* d="C:\\WINDOWS\\SYSTEM32"; c.write(b,d,(uint32_t)std::strlen(d)+1); return (uint32_t)std::strlen(d); });
    K("GetTempPathA",2,[](Cpu&c){ uint32_t b=c.arg(1); const char* d="C:\\Temp\\"; c.write(b,d,(uint32_t)std::strlen(d)+1); return (uint32_t)std::strlen(d); });
    K("GetThreadPriority",1,[](Cpu&){ return 0u; });   // THREAD_PRIORITY_NORMAL
    K("GetVersionExA",1,[](Cpu&c){ uint32_t p=c.arg(0); /*OSVERSIONINFOA*/
        c.write_u32(p+4,5); c.write_u32(p+8,1); c.write_u32(p+12,2600); c.write_u32(p+16,2); return 1u; });
    K("GetWindowsDirectoryA",2,[](Cpu&c){ uint32_t b=c.arg(0); const char* d="C:\\WINDOWS"; c.write(b,d,(uint32_t)std::strlen(d)+1); return (uint32_t)std::strlen(d); });
    K("HeapDestroy",1,[](Cpu&){ return 1u; });
    K("IsBadCodePtr",1,[](Cpu&){ return 0u; });
    K("IsBadReadPtr",2,[](Cpu&){ return 0u; });
    K("IsBadWritePtr",2,[](Cpu&){ return 0u; });
    K("IsDebuggerPresent",0,[](Cpu&){ return 0u; });                       // not debugged
    // Rendait 0 POUR TOUT, alors que notre propre cpuid annonce FPU/CMOV/MMX/
    // FXSR/SSE/SSE2. Deux vues du meme processeur qui se contredisent : c'est
    // la signature la moins chere de tout l'audit a lever. Les reponses sont
    // DERIVEES de wx86_cpuid_features(), pas choisies — si la feuille 1 change,
    // celles-ci suivent sans qu'on y pense.
    K("IsProcessorFeaturePresent",1,[](Cpu&c)->uint32_t{
        if(wx86_fid_avant()) return 0u;                 // reponse D'AVANT (bouton coupant)
        uint32_t edx=0, ecx=0; wx86_cpuid_features(&edx,&ecx);
        switch(c.arg(0)){
            case 0:  return 0u;                          // PF_FLOATING_POINT_PRECISION_ERRATA
            case 1:  return (edx&1u)?0u:1u;              // PF_FLOATING_POINT_EMULATED : FPU materiel => 0
            case 2:  return (edx>>8)&1u;                 // PF_COMPARE_EXCHANGE_DOUBLE (CX8)
            case 3:  return (edx>>23)&1u;                // PF_MMX_INSTRUCTIONS_AVAILABLE
            case 6:  return (edx>>25)&1u;                // PF_XMMI_INSTRUCTIONS_AVAILABLE (SSE)
            case 8:  return 1u;                          // PF_RDTSC_INSTRUCTION_AVAILABLE (ReadTSC implemente)
            case 10: return (edx>>26)&1u;                // PF_XMMI64_INSTRUCTIONS_AVAILABLE (SSE2)
            case 13: return ecx&1u;                      // PF_SSE3_INSTRUCTIONS_AVAILABLE
            default: return 0u; } });
    K("IsValidCodePage",1,[](Cpu&){ return 1u; });
    K("IsValidLocale",2,[](Cpu&){ return 1u; });
    K("QueryPerformanceFrequency",1,[](Cpu&c){ uint32_t p=c.arg(0); const uint64_t f=wx86_perf_frequency();
        c.write_u32(p,(uint32_t)f); c.write_u32(p+4,(uint32_t)(f>>32)); return 1u; });
    K("RemoveDirectoryA",1,[](Cpu&){ return 1u; });
    K("RtlUnwind",4,[](Cpu&){ return 0u; });
    K("SetCurrentDirectoryA",1,[](Cpu&){ return 1u; });
    K("SetEndOfFile",1,[](Cpu&){ return 1u; });
    K("SetErrorMode",1,[](Cpu&){ return 0u; });
    K("SetFileTime",4,[](Cpu&){ return 1u; });
    K("SetHandleCount",1,[](Cpu&c){ return c.arg(0); });
    K("SetPriorityClass",2,[](Cpu&){ return 1u; });
    K("SetThreadAffinityMask",2,[](Cpu&){ return 1u; });
    K("SetThreadLocale",1,[](Cpu&){ return 1u; });
    K("SetThreadPriority",2,[](Cpu&){ return 1u; });
    K("SuspendThread",1,[](Cpu&){ return 0u; });
    K("TlsFree",1,[](Cpu&){ return 1u; });
    K("UnhandledExceptionFilter",1,[](Cpu&){ return 0u; });      // EXCEPTION_CONTINUE_SEARCH
    K("WriteConsoleW",5,[](Cpu&c){ if(c.arg(3)) c.write_u32(c.arg(3),c.arg(2)); return 1u; });
    K("WritePrivateProfileStringA",4,[](Cpu&){ return 1u; });

    // ---- Vague 2 (2026-09-11) : debloquee par les points d'extension --------
    // Ces corps dependaient de helpers que le portage hebergeait ; ils vivent
    // desormais dans le moteur (guest_atomics.h, guest_thread_ctx.h). Les noms
    // courts restent des alias locaux pour que les corps soient deplaces SANS
    // etre reecrits — un deplacement qu'on peut relire ligne a ligne.
    K("CreateProcessW",10,[](Cpu&c){ set_lasterr(c,2); return 0u; });          // no child processes (crash reporter)
    K("GetLastError",0,[](Cpu&c){ return get_lasterr(c); });
    K("InterlockedAnd",2,[](Cpu&c){ uint32_t p=c.arg(0), v=c.arg(1);
        if(uint32_t* h=ilk_ptr(c,p)) return ilk_fetch_and(h,v);
        uint32_t o=c.read_u32(p); c.write_u32(p,o&v); return o; });
    K("InterlockedDecrement",1,[](Cpu&c){ uint32_t p=c.arg(0);
        if(uint32_t* h=ilk_ptr(c,p)) return ilk_sub_fetch(h,1u);
        uint32_t v=c.read_u32(p)-1; c.write_u32(p,v); return v; });
    K("InterlockedExchange",2,[](Cpu&c){ uint32_t p=c.arg(0), v=c.arg(1);
        if(uint32_t* h=ilk_ptr(c,p)) return ilk_exchange(h,v);
        uint32_t o=c.read_u32(p); c.write_u32(p,v); return o; });
    K("InterlockedExchangeAdd",2,[](Cpu&c){ uint32_t p=c.arg(0), v=c.arg(1);
        if(uint32_t* h=ilk_ptr(c,p)) return ilk_fetch_add(h,v);
        uint32_t o=c.read_u32(p); c.write_u32(p,o+v); return o; });
    K("InterlockedExchangePointer",2,[](Cpu&c){ uint32_t p=c.arg(0), v=c.arg(1);
        if(uint32_t* h=ilk_ptr(c,p)) return ilk_exchange(h,v);
        uint32_t o=c.read_u32(p); c.write_u32(p,v); return o; });
    K("InterlockedIncrement",1,[](Cpu&c){ uint32_t p=c.arg(0);
        if(uint32_t* h=ilk_ptr(c,p)) return ilk_add_fetch(h,1u);
        uint32_t v=c.read_u32(p)+1; c.write_u32(p,v); return v; });
    K("InterlockedOr",2,[](Cpu&c){ uint32_t p=c.arg(0), v=c.arg(1);
        if(uint32_t* h=ilk_ptr(c,p)) return ilk_fetch_or(h,v);
        uint32_t o=c.read_u32(p); c.write_u32(p,o|v); return o; });
    K("InterlockedXor",2,[](Cpu&c){ uint32_t p=c.arg(0), v=c.arg(1);
        if(uint32_t* h=ilk_ptr(c,p)) return ilk_fetch_xor(h,v);
        uint32_t o=c.read_u32(p); c.write_u32(p,o^v); return o; });
    K("MemoryBarrier",0,[](Cpu&){ wx86_ilk_fence(); return 0u; });
    K("OpenEventA",3,[](Cpu&c){ set_lasterr(c,2); return 0u; });
    K("OpenMutexA",3,[](Cpu&c){ set_lasterr(c,2); return 0u; });
    K("OpenProcess",3,[](Cpu&c){ set_lasterr(c,5); return 0u; });            // ERROR_ACCESS_DENIED
    K("SetLastError",1,[](Cpu&c){ set_lasterr(c,c.arg(0)); return 0u; });
}
