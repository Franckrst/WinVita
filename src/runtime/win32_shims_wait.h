// src/runtime/win32_shims_wait.h — ATTENTE : WaitForSingleObject,
// WaitForMultipleObjects, SleepEx, et le coeur de Sleep.
//
// POURQUOI CETTE FAMILLE EST UN CAS D'ECOLE. Les deux consommateurs du moteur
// ont le MEME corps ici — WaitForMultipleObjects est identique au caractere
// pres, commentaires compris, y compris l'invariant d'atomicite en deux passes
// et la note sur l'objet reutilise par fil. Ce qui diffère entre eux n'est pas
// la semantique : c'est l'INSTRUMENTATION posee autour (trace, journal
// d'attente, chronometrage, profilage des chargements asynchrones, compteurs
// de boucle). Rien de tout cela ne decide quoi que ce soit. Le moteur porte
// donc le corps, et raconte par wx86_wait_set_observer() ; le consommateur
// rebranche SON instrumentation dans un observateur unique.
//
// SLEEP RESTE CHEZ LE CONSOMMATEUR, et ce n'est pas de la prudence : son corps
// commence par des raccourcis lies a des ADRESSES DU JEU (« ce Sleep(0)-ci,
// appele depuis tel site de la boucle reseau, se comporte autrement »). Ce
// sont des litteraux propres a un jeu, exactement le critere qui garde un shim
// au portage. Ce qui EST generique dans Sleep — rendre la main, ou dormir un
// delai — est ici, sous wx86_wait_sleep(), et les deux consommateurs
// l'appellent apres leur propre prelude.
#pragma once
#include <cstdint>

namespace d2rt { class Bridge; class Cpu; class Waitable; }

// Le coeur generique de Sleep/SleepEx : ms == 0 rend la main, sinon attend le
// delai. Sans ordonnanceur, ne fait rien (comme avant).
//
// DIFFERENCE DECLAREE : Sleep et SleepEx avaient chacun leur PROPRE evenement
// « jamais signale » sur lequel dormir ; ils partagent desormais le meme. Un
// evenement manuel jamais signale n'a pas d'etat observable — l'etat d'attente
// vit dans le fil, pas dans l'objet — et il etait deja partage par tous les
// fils qui appelaient Sleep.
void wx86_wait_sleep(uint32_t ms);

// ---- Observateur -------------------------------------------------------------
enum {
    WX86_WAIT_SINGLE_ENTER = 0,  // avant le blocage : obj/handle/timeout renseignes
    WX86_WAIT_SINGLE_EXIT,       // apres : ret = le code rendu au programme invite
    WX86_WAIT_SINGLE_UNKNOWN,    // handle inconnu : on rend « signale » SANS attendre
    WX86_WAIT_MULTI_ENTER,       // count/timeout renseignes ; handles = tableau invite
    WX86_WAIT_MULTI_EXIT,
};
struct WxWaitEvent {
    int              kind;
    d2rt::Cpu*       cpu;
    d2rt::Waitable*  obj;      // objet attendu (attente simple), sinon nullptr
    uint32_t         handle;   // handle attendu (attente simple)
    uint32_t         timeout;  // delai demande, 0xFFFFFFFF = infini
    uint32_t         ret;      // code de retour (evenements EXIT)
    uint32_t         count;    // nombre de handles (attente multiple)
    uint32_t         handles;  // adresse INVITEE du tableau de handles
    uint32_t         all;      // bWaitAll (attente multiple)
};
typedef void (*WxWaitObserverFn)(const WxWaitEvent&);
// UN SEUL observateur : un second appel REMPLACE le premier.
void wx86_wait_set_observer(WxWaitObserverFn cb);

void win32_shims_wait_install(d2rt::Bridge& br);
