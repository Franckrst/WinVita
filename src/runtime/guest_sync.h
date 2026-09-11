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
#include <array>
#include <deque>
#include <map>
#include <vector>
#include "runtime/guest_thread.h"
#include "runtime/cpu.h"

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

// Attente MULTIPLE — WaitForMultipleObjects(nCount, ..., bWaitAll).
//
// Ce type manquait au moteur alors que l'en-tete ci-dessus citait deja son
// invariant d'atomicite comme PREUVE que ces structures sont generiques. Les
// deux portages le definissaient, avec des corps identiques a la ligne pres
// (28 lignes de part et d'autre, verifie par diff le 2026-09-11) : le citer en
// preuve sans le fournir laissait chaque portage le reecrire.
struct WxMultiWait : d2rt::Waitable {
    std::vector<d2rt::Waitable*> objs; bool all=false; uint32_t last=0;
    // INVARIANT D'ATOMICITE EN DEUX PASSES : les deux passes doivent tourner
    // dans UNE SEULE section critique (le verrou d'ordonnanceur / la GIL —
    // jamais des verrous par objet). Si un backend natif serialisait par
    // objet, un autre fil pourrait consommer l'objet A entre la passe 1 et la
    // passe 2 : le try_acquire(A) de la passe 2 echoue en silence (retour
    // ignore) et l'attente-de-tous annonce un succes sans posseder A — une
    // acquisition fantome, pire que le bug de consommation partielle qu'elle
    // remplace. Les retours de la passe 2 sont ignores DELIBEREMENT : sous
    // l'invariant, ils ne peuvent echouer que pour un handle duplique — n'en
    // faites PAS une assertion (le cas du doublon la ferait sauter
    // legitimement).
    // Infidelite deliberee : le meme handle deux fois dans une liste
    // bWaitAll — le vrai Windows REFUSE l'attente (WAIT_FAILED,
    // ERROR_INVALID_PARAMETER ; MSDN : lpHandles « may not contain multiple
    // copies of the same handle ») ; on l'accepte et on consomme une fois.
    // Plus permissif que Windows, benin (aucun programme reel ne peut s'y
    // fier puisque ca echoue la-bas), garde tel quel.
    bool try_acquire(d2rt::GuestThread* t) override {
        if(all){ for(auto* o:objs) if(!o->ready(t)) return false;   // passe 1 : sonder seulement
                 for(auto* o:objs) o->try_acquire(t);               // passe 2 : tout consommer
                 last=0; return true; }
        for(size_t i=0;i<objs.size();++i) if(objs[i]->try_acquire(t)){ last=(uint32_t)i; return true; }
        return false; }
    bool ready(d2rt::GuestThread* t) override {
        if(all){ for(auto* o:objs) if(!o->ready(t)) return false; return true; }
        for(auto* o:objs) if(o->ready(t)) return true; return false; }
    uint32_t result() const override { return last; }   // WAIT_OBJECT_0 + index
    const char* kind() const override { return "multi"; } };

// Port d'achevement d'entrees-sorties (GetQueuedCompletionStatus). Meme
// histoire que WxMultiWait : 16 lignes identiques dans les deux portages.
// Les trois mots ecrits au reveil (octets, cle, overlapped) le sont dans la
// memoire INVITEE, d'ou le Cpu* porte par l'objet.
struct WxIocp : d2rt::Waitable {
    std::deque<std::array<uint32_t,3>> q;                              // {octets,cle,overlapped}
    std::map<d2rt::GuestThread*,std::array<uint32_t,3>> outp;          // attendeur -> {pOctets,pCle,pOv}
    d2rt::Cpu* cpu=nullptr;
    bool try_acquire(d2rt::GuestThread* t) override {
        if(q.empty()) return false;
        auto c=q.front(); q.pop_front();
        auto it=outp.find(t);
        if(it!=outp.end()){ auto&o=it->second;
            if(o[0]) cpu->write_u32(o[0],c[0]);
            if(o[1]) cpu->write_u32(o[1],c[1]);
            if(o[2]) cpu->write_u32(o[2],c[2]); }
        return true; }
    bool ready(d2rt::GuestThread*) override { return !q.empty(); }
    uint32_t result() const override { return 1; }                     // GQCS rend TRUE
    const char* kind() const override { return "iocp"; } };

bool wx86_is_kind(d2rt::Waitable* w, const char* kind);

// ---- Table de handles -------------------------------------------------------
// Le portage n'a plus a la posseder : c'est ce qui debloque le deplacement des
// shims de synchronisation un par un.
uint32_t          wx86_handle_add(d2rt::Waitable* w);
d2rt::Waitable*   wx86_handle_find(uint32_t h);
void              wx86_handle_erase(uint32_t h);
unsigned          wx86_handle_count();

// Reserve un identifiant SANS rien ranger dans la table. Un portage a
// forcement des handles a lui qui ne sont pas des objets attendables — un
// instantane de processus, par exemple. Ils doivent etre numerotes par le MEME
// compteur, sinon deux handles distincts finissent par porter le meme numero.
// C'est la raison d'etre de cette fonction : une seule table, un seul compteur.
uint32_t          wx86_handle_next_id();

// ---- Sections critiques -----------------------------------------------------
// Cache MRU a une entree : l'acces aux sections critiques est tres repetitif
// (la meme section prise et relachee coup sur coup, contention mesuree a
// 0,0005 %), ce qui evite la descente d'arbre dans le cas courant.
WxCrit* wx86_crit_for(uint32_t cs_va);      // cree si absent
WxCrit* wx86_crit_lookup(uint32_t cs_va);   // ne cree pas ; nullptr si inconnue
void    wx86_crit_forget(uint32_t cs_va);
// Parcours des sections actuellement DETENUES (diagnostic de fin de run d'un
// portage). Rappel appele une fois par section detenue.
void    wx86_crit_each_held(void (*fn)(uint32_t va, uint32_t owner, int count, void* ud), void* ud);

// UN SEUL corps pour la sortie de section critique.
//
// Ce point existe a cause d'un piege precis. Ces trois fonctions ont un DOUBLE
// chemin : le shim normal, et un chemin rapide servi directement dans la
// repartition de trappes du dynarec. Le chemin rapide n'est pas une copie du
// shim — il ABANDONNE (et laisse le shim faire) des que le cas sort du
// nominal : trace active, contention, fil termine. Mais pour le cas non
// contendu, les deux portaient le MEME code recopie : meme cache MRU, meme
// decrement, meme reveil. Deux corps qu'il fallait garder identiques a la
// main, sur le chemin le plus chaud du projet.
//
// Ils cessent ici d'etre deux corps. Le moteur porte le coeur ; le shim en est
// une enveloppe mince, et le chemin rapide, c'est le meme coeur plus son
// epilogue de trappe. Ils ne PEUVENT plus diverger.
//
// Rend true si la section a ete effectivement relachee (compte retombe a zero).
bool wx86_crit_leave(uint32_t cs_va, d2rt::ThreadScheduler* sched);

// ---- Observateur ------------------------------------------------------------
enum {
    WX86_SYNC_EVENT_SET = 0,   // SetEvent : obj = l'evenement
    WX86_SYNC_EVENT_RESET,     // ResetEvent
    WX86_SYNC_WAIT_DONE,       // une attente vient d'aboutir : obj = l'objet obtenu
    WX86_SYNC_OBJ_CREATE,      // creation : obj = l'objet, handle = son handle
    WX86_SYNC_OBJ_CLOSE,
    WX86_SYNC_EVENT_PULSE,
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
