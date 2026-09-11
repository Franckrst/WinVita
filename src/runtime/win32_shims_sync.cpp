// src/runtime/win32_shims_sync.cpp — voir win32_shims_sync.h.
// Famille evenement / mutex / semaphore de KERNEL32, portee depuis
// tools/rt_boot.cpp de d2vita (2026-09-11).
//
// PROPRIETE, PAS SEULEMENT LE TYPE. Ces corps ne pouvaient pas partir tant que
// la table de handles, la carte des sections critiques et le compteur
// d'identifiants vivaient encore chez le portage : deplacer un type en
// laissant son etat derriere fabrique un jumeau vide cote moteur, invisible a
// la compilation et payable en interblocage. C'est arrive TROIS fois cette
// semaine. L'etat est desormais entierement cote moteur, les corps peuvent
// donc suivre — et il ne reste aucun jumeau au portage.
//
// L'INSTRUMENTATION NE SUIT PAS. Les corps d'origine portaient une trace de
// synchronisation et un journal d'attente, tous deux derriere des interrupteurs
// d'environnement eteints par defaut, plus des compteurs de profilage. Rien de
// tout cela ne decide quoi que ce soit : ce sont des observateurs. Ils passent
// par wx86_sync_notify(), et le portage les rebranche dans SON observateur.
// Le moteur raconte, il ne demande jamais d'avis.
#include "win32_shims_sync.h"
#include "guest_sync.h"
#include "guest_thread_ctx.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include <cstdint>
#include <functional>
#include <string>
using namespace d2rt;

void win32_shims_sync_install(Bridge& br){
    auto K=[&](const char* name,uint32_t ac,std::function<uint32_t(Cpu&)> fn){
        Shim s; s.argc=ac; s.stdcall_cleanup=true; s.tag=std::string("KERNEL32.dll!")+name;
        s.fn=std::move(fn);
        br.register_shim("KERNEL32.dll",name,s); };

    auto mkevent=[](Cpu&c){
        WxEvent* e=new WxEvent(); e->manual=c.arg(1)!=0; e->signaled=c.arg(2)!=0;
        uint32_t h=wx86_handle_add(e);
        wx86_sync_notify({WX86_SYNC_OBJ_CREATE,&c,e,h});
        return h; };
    K("CreateEventA",4,mkevent);
    K("CreateEventW",4,mkevent);

    K("SetEvent",1,[](Cpu&c){ const uint32_t h=c.arg(0);
        Waitable* w=wx86_handle_find(h);
        if(w && wx86_is_kind(w,"event")){
            static_cast<WxEvent*>(w)->signaled=true;
            wx86_sync_notify({WX86_SYNC_EVENT_SET,&c,w,h});
            if(ThreadScheduler* s=wx86_sched()) s->notify(w); }
        return 1u; });

    K("ResetEvent",1,[](Cpu&c){ const uint32_t h=c.arg(0);
        Waitable* w=wx86_handle_find(h);
        if(w && wx86_is_kind(w,"event")){
            static_cast<WxEvent*>(w)->signaled=false;
            wx86_sync_notify({WX86_SYNC_EVENT_RESET,&c,w,h}); }
        return 1u; });

    // PulseEvent — infidelite DELIBEREE conservee telle quelle : ne reveille
    // PERSONNE, se contente de laisser l'evenement non signale. Le vrai
    // PulseEvent de Windows est lui-meme reconnu comme non fiable ; aucun
    // appelant observe ici n'en depend.
    K("PulseEvent",1,[](Cpu&c){ const uint32_t h=c.arg(0);
        Waitable* w=wx86_handle_find(h);
        if(w && wx86_is_kind(w,"event")){
            static_cast<WxEvent*>(w)->signaled=false;
            wx86_sync_notify({WX86_SYNC_EVENT_PULSE,&c,w,h});
            return 1u; }
        return 0u; });

    // Mutex modelise par un evenement auto-reset : l'acquisition consomme le
    // signal, la liberation le repose. Fidele pour un mutex non recursif.
    auto mkmutex=[](Cpu&c){
        WxEvent* e=new WxEvent(); e->manual=false; e->signaled=(c.arg(1)==0);
        uint32_t h=wx86_handle_add(e); wx86_set_lasterr(c,0);
        wx86_sync_notify({WX86_SYNC_OBJ_CREATE,&c,e,h});
        return h; };
    K("CreateMutexA",3,mkmutex);
    K("CreateMutexW",3,mkmutex);
    K("ReleaseMutex",1,[](Cpu&c){
        Waitable* w=wx86_handle_find(c.arg(0));
        if(w && wx86_is_kind(w,"event")){
            static_cast<WxEvent*>(w)->signaled=true;
            if(ThreadScheduler* s=wx86_sched()) s->notify(w);
            return 1u; }
        return 0u; });

    auto mksema=[](Cpu&c){
        WxSemaphore* s=new WxSemaphore(); s->count=(int)c.arg(1); s->maxc=(int)c.arg(2);
        uint32_t h=wx86_handle_add(s); wx86_set_lasterr(c,0);
        wx86_sync_notify({WX86_SYNC_OBJ_CREATE,&c,s,h});
        return h; };
    K("CreateSemaphoreA",4,mksema);
    K("CreateSemaphoreW",4,mksema);

    K("GetExitCodeThread",2,[](Cpu&c){
        Waitable* w=wx86_handle_find(c.arg(0));
        uint32_t code=259;   // STILL_ACTIVE
        if(w && wx86_is_kind(w,"thread")){
            WxThread* k=static_cast<WxThread*>(w);
            if(k->gt && k->gt->state==GuestThread::State::Finished) code=k->gt->exit_code; }
        if(uint32_t p=c.arg(1)) c.write_u32(p,code);
        return 1u; });
}
