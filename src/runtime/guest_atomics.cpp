// src/runtime/guest_atomics.cpp — voir guest_atomics.h.
//
// Porte depuis tools/rt_boot.cpp de d2vita (2026-09-11), a l'identique.
// Les DEUX portages existants (d2vita et carn-vita) hebergeaient ce meme code
// mot pour mot, commentaires compris : c'est du moteur duplique, pas du jeu.
//
// ORDRE MEMOIRE — RELAXED par defaut. Raisonnement conserve de l'original :
// sur une cible ou tous les fils qui executent du code invite sont epingles
// sur le meme cœur, ils ne s'executent jamais vraiment en parallele, donc
// l'ORDONNANCEMENT est acquis gratuitement par le materiel. Ce qui reste
// necessaire, et que RELAXED fournit, c'est l'ATOMICITE : un RMW nu peut etre
// preempte entre sa lecture et son ecriture, et c'est cela qui perdait des
// compteurs de references.
//
// CE QUE SEQ_CST COUTAIT (mesure A/B sur materiel, 2026-08-31, meme scene,
// seul le binaire changeait) : 13,7 -> 11,0 images/s, soit -20 %, et le fil
// en attente brulait 3,2x plus de blocs traduits. Deux barrieres materielles
// par appel sur ce qui est l'instruction la plus chaude du profil en ligne.
//
// SI L'EPINGLAGE MONO-CŒUR TOMBE, CET ORDRE DEVIENT FAUX. WX86_ILK_SEQCST=1
// retablit SEQ_CST sans reconstruire — c'est aussi la jambe temoin de l'A/B.
#include "guest_atomics.h"
#include "runtime/cpu.h"
extern "C" { int dyn86_protectdb(void); }
#include <cstdlib>
using namespace d2rt;

static unsigned long long g_ilkAtomic = 0, g_ilkFallback = 0, g_ilkUnaligned = 0;

bool wx86_ilk_seqcst(){
    // Ancien nom D2_ILK_SEQCST conserve en repli : meme convention que le
    // reste des variables d'environnement du moteur.
    static const bool v = [](){
        const char* e = getenv("WX86_ILK_SEQCST");
        if(!e) e = getenv("D2_ILK_SEQCST");
        return e && *e=='1'; }();
    return v;
}

uint32_t* wx86_ilk_ptr(Cpu& c, uint32_t va){
    if(va & 3u){ ++g_ilkUnaligned; ++g_ilkFallback; return nullptr; }
    uint32_t* hp = (uint32_t*)c.hostptr(va, 4);
    if(!hp){ ++g_ilkFallback; return nullptr; }
    // Semantique de Cpu::write() conservee : une page qui porte du code traduit
    // est protegee en ecriture cote hote et doit etre deprotegee + salie avant
    // qu'on y ecrive. dyn86_protectdb() est a 0 par defaut (aucun gestionnaire
    // SIGSEGV porte) — un chargement global et un branchement, pas une
    // recherche d'arbre, sur le chemin le plus chaud du profil en ligne.
    if(dyn86_protectdb()) c.invalidate_code(va, 4);
    ++g_ilkAtomic;
    return hp;
}

#define WX86_ILK(name, expr_seq, expr_rel) \
    uint32_t name(uint32_t* h, uint32_t v){ \
        return wx86_ilk_seqcst() ? (expr_seq) : (expr_rel); }
WX86_ILK(wx86_ilk_add_fetch, __atomic_add_fetch(h,v,__ATOMIC_SEQ_CST), __atomic_add_fetch(h,v,__ATOMIC_RELAXED))
WX86_ILK(wx86_ilk_sub_fetch, __atomic_sub_fetch(h,v,__ATOMIC_SEQ_CST), __atomic_sub_fetch(h,v,__ATOMIC_RELAXED))
WX86_ILK(wx86_ilk_exchange,  __atomic_exchange_n(h,v,__ATOMIC_SEQ_CST), __atomic_exchange_n(h,v,__ATOMIC_RELAXED))
WX86_ILK(wx86_ilk_fetch_add, __atomic_fetch_add(h,v,__ATOMIC_SEQ_CST), __atomic_fetch_add(h,v,__ATOMIC_RELAXED))
WX86_ILK(wx86_ilk_fetch_or,  __atomic_fetch_or (h,v,__ATOMIC_SEQ_CST), __atomic_fetch_or (h,v,__ATOMIC_RELAXED))
WX86_ILK(wx86_ilk_fetch_and, __atomic_fetch_and(h,v,__ATOMIC_SEQ_CST), __atomic_fetch_and(h,v,__ATOMIC_RELAXED))
WX86_ILK(wx86_ilk_fetch_xor, __atomic_fetch_xor(h,v,__ATOMIC_SEQ_CST), __atomic_fetch_xor(h,v,__ATOMIC_RELAXED))
#undef WX86_ILK

// Rend l'ANCIENNE valeur dans les deux cas : expected est inchangee en cas de
// succes (donc egale au comparand, donc a l'ancienne) et rechargee avec la
// valeur reelle en cas d'echec.
uint32_t wx86_ilk_cas(uint32_t* h, uint32_t expected, uint32_t desired){
    if(wx86_ilk_seqcst()) __atomic_compare_exchange_n(h,&expected,desired,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
    else                  __atomic_compare_exchange_n(h,&expected,desired,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED);
    return expected;
}

void wx86_ilk_fence(){
    // Meme raisonnement que les Interlocked* : mono-cœur epingle => la barriere
    // MATERIELLE est acquise, seul le COMPILATEUR doit etre empeche de
    // deplacer nos propres acces.
    if(wx86_ilk_seqcst()) __atomic_thread_fence(__ATOMIC_SEQ_CST);
    else                  __atomic_signal_fence(__ATOMIC_SEQ_CST);
}

void wx86_ilk_stats(unsigned long long* atomic,
                    unsigned long long* fallback,
                    unsigned long long* unaligned){
    if(atomic)    *atomic    = g_ilkAtomic;
    if(fallback)  *fallback  = g_ilkFallback;
    if(unaligned) *unaligned = g_ilkUnaligned;
}
