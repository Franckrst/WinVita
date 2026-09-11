// src/runtime/guest_sync.h — objets noyau Win32 attendables, table de handles,
// et le point d'observation qui permet a un portage d'instrumenter tout cela
// sans en posseder une ligne.
//
// POURQUOI CE MODULE EXISTE. Les shims de synchronisation de KERNEL32
// (CreateEvent, SetEvent, WaitForSingleObject, les sections critiques...) ne
// peuvent pas se deplacer UN PAR UN : ils partagent tous la meme table de
// handles et les memes types d'objets. Tant que la table vit chez le portage,
// aucun d'eux ne peut rejoindre le moteur. La table est donc le verrou, et
// c'est elle qu'on ouvre ici.
//
// CE QUI EST GENERIQUE, ET POURQUOI ON EN EST SUR. Les deux portages qui
// utilisent ce moteur definissaient ces memes structures avec des corps
// identiques au caractere pres, commentaires compris — y compris l'invariant
// d'atomicite en deux passes de KMultiWait et le constat, paye par un
// interblocage reel, que l'etat d'une section critique doit rester INVISIBLE
// au code invite. Ce n'est pas du jeu : c'est de la semantique Win32.
//
// CE QUI N'EST PAS GENERIQUE. Toute l'instrumentation que les portages posent
// autour : trace de synchronisation, chronometrage des attentes, profilage des
// chargements asynchrones, compteurs de famine. Elle ne DECIDE jamais rien,
// elle observe — elle passe donc par l'observateur ci-dessous, sur le modele
// deja valide de l'observateur reseau (win32_shims_wsock32.h).
#pragma once
#include <cstdint>
#include "runtime/guest_thread.h"
namespace d2rt { class Cpu; }

// ---- Objets noyau -----------------------------------------------------------
struct WxEvent : d2rt::Waitable {
    bool signaled=false, manual=false;
    bool try_acquire(d2rt::GuestThread*) override {
        if(signaled){ if(!manual) signaled=false; return true; } return false; }
    bool ready(d2rt::GuestThread*) override { return signaled; }
    const char* kind() const override { return "event"; } };

// Semaphore COMPTANT (un vrai compte, et non un evenement binaire deguise) :
// une attente decremente, une liberation ajoute. Honnete pour un compte > 1.
struct WxSemaphore : d2rt::Waitable {
    int count=0, maxc=0x7fffffff;
    bool try_acquire(d2rt::GuestThread*) override { if(count>0){ count--; return true; } return false; }
    bool ready(d2rt::GuestThread*) override { return count>0; }
    const char* kind() const override { return "sema"; } };

struct WxThread : d2rt::Waitable {
    d2rt::GuestThread* gt=nullptr;
    bool try_acquire(d2rt::GuestThread*) override { return gt && gt->state==d2rt::GuestThread::State::Finished; }
    bool ready(d2rt::GuestThread*) override { return gt && gt->state==d2rt::GuestThread::State::Finished; }
    const char* kind() const override { return "thread"; } };

// Section critique. L'etat (proprietaire/compte) vit en C++, INVISIBLE au code
// invite. La variante « visible » (ecrire OwningThread/RecursionCount dans la
// vraie structure) a ete essayee puis ANNULEE : du code Blizzard inspecte ces
// champs en ligne, et un OwningThread non nul le fait basculer sur des chemins
// que des Enter/Leave a base de shims n'equilibrent jamais — le chargement de
// niveau partait en interblocage. Garder les champs a zero preserve le
// comportement valide.
struct WxCrit : d2rt::Waitable {
    uint32_t va=0, owner=0; int count=0;
    bool try_acquire(d2rt::GuestThread* t) override { uint32_t me=t?t->id:1u;
        if(owner==0||owner==me){ owner=me; count++; return true; } return false; }
    bool ready(d2rt::GuestThread* t) override { uint32_t me=t?t->id:1u; return owner==0||owner==me; }
    const char* kind() const override { return "critsec"; } };

bool wx86_is_kind(d2rt::Waitable* w, const char* kind);

// ---- Table de handles -------------------------------------------------------
// Le portage n'a plus a la posseder : c'est ce qui debloque le deplacement des
// shims de synchronisation un par un.
uint32_t          wx86_handle_add(d2rt::Waitable* w);
d2rt::Waitable*   wx86_handle_find(uint32_t h);
void              wx86_handle_erase(uint32_t h);

// ---- Sections critiques -----------------------------------------------------
// Cache MRU a une entree : l'acces aux sections critiques est tres repetitif
// (la meme section prise et relachee coup sur coup, contention mesuree a
// 0,0005 %), ce qui evite la descente d'arbre dans le cas courant.
WxCrit* wx86_crit_for(uint32_t cs_va);
void    wx86_crit_forget(uint32_t cs_va);

// ---- Observateur ------------------------------------------------------------
enum {
    WX86_SYNC_EVENT_SET = 0,   // SetEvent : obj = l'evenement
    WX86_SYNC_EVENT_RESET,     // ResetEvent
    WX86_SYNC_WAIT_DONE,       // une attente vient d'aboutir : obj = l'objet obtenu
    WX86_SYNC_OBJ_CREATE,      // creation : obj = l'objet, handle = son handle
    WX86_SYNC_OBJ_CLOSE,
};
struct WxSyncEvent {
    int               kind;
    d2rt::Cpu*        cpu;      // CPU au site d'appel, pour le diagnostic du portage
    d2rt::Waitable*   obj;
    uint32_t          handle;
};
typedef void (*WxSyncObserverFn)(const WxSyncEvent&);
// UN SEUL observateur, volontairement : un registre qui accepte en silence un
// second crochet sur la meme cle est ce qui avait fait perdre une regression de
// connexion a ce projet. Un second appel REMPLACE le premier.
void wx86_sync_set_observer(WxSyncObserverFn cb);
void wx86_sync_notify(const WxSyncEvent& e);   // appele par le moteur ET par le portage
