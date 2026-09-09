// src/runtime/prof.h — PROF_COUNTERS profiling (docs/perf/optimization_audit.md §1).
//
// Deterministic-friendly counters + wall-clock accumulators at the C++ hot
// spots (bridge trap, scheduler slice, sync shims). NOTHING on the dynarec
// per-instruction path — bias stays <5% (§1.1). EIP hot-block sampling piggy-
// backs on quantum preemption (one sample per preempted slice), NOT a per-insn
// hook, so the sample distribution matches real execution.
//
// Everything compiles away without -DPROF_COUNTERS: the validated default
// build is bit-identical to before this header existed.
#pragma once
#include <cstdint>

namespace d2rt { namespace prof {

#ifdef PROF_COUNTERS
uint64_t now_ns();                       // monotonic (qemu/linux + Vita)

// Time split. run_ns covers cpu->run() (dynarec + OS traps executed inside);
// trap_ns covers only the OS-shim bodies — so pure dynarec = run_ns - trap_ns.
extern uint64_t run_ns;
extern uint64_t trap_ns;
extern uint64_t trap_calls;

// Sync behaviour under the (mono-runner) scheduler: how often a Wait* was
// satisfiable immediately vs actually blocked the thread. This is THE number
// that decides whether ESync/CS-fast-path style optimizations can pay (§2.1).
extern uint64_t wait_calls, wait_immediate;
extern uint64_t cs_enter, cs_contended;

// EIP histogram sampled at preemption (bucketed to 64 bytes).
void   sample_eip(uint32_t eip);
// Iterate top-N buckets, hottest first: cb(eip_bucket, hits).
void   top_eips(int n, void (*cb)(uint32_t eip, uint64_t hits, void* u), void* u);
uint64_t eip_samples();

// PAR FIL INVITE (06/09, chantier fil d'E/S Storm). Le meme echantillon de
// preemption, attribue au fil qui tournait, a l'EIP EXACT (pas de seau) ; et
// le compte EXACT de blocs traduits executes par fil (delta du compteur
// dyn86_blocks autour de chaque tranche coop). Un echantillon par tranche =
// un echantillon par QUANTUM blocs : c'est un tirage uniforme sur les blocs,
// pas sur le temps (cf. feedback « le profil EIP compte des blocs »).
void   sample_eip_tid(uint32_t eip, uint32_t tid);
void   add_blocks_tid(uint32_t tid, uint32_t blocks);
void   note_thread(uint32_t tid, uint32_t entry);
// cb(tid, entry, samples, blocks) pour chaque fil vu, par tid croissant.
void   threads(void (*cb)(uint32_t tid, uint32_t entry, uint64_t samples, uint64_t blocks, void* u), void* u);
// cb(eip, hits) : les n EIP exacts les plus frequents du fil (n<0 : tous).
void   top_eips_tid(uint32_t tid, int n, void (*cb)(uint32_t eip, uint64_t hits, void* u), void* u);

#else
inline uint64_t now_ns() { return 0; }
inline void sample_eip(uint32_t) {}
inline void top_eips(int, void (*)(uint32_t, uint64_t, void*), void*) {}
inline uint64_t eip_samples() { return 0; }
inline void sample_eip_tid(uint32_t, uint32_t) {}
inline void add_blocks_tid(uint32_t, uint32_t) {}
inline void note_thread(uint32_t, uint32_t) {}
inline void threads(void (*)(uint32_t, uint32_t, uint64_t, uint64_t, void*), void*) {}
inline void top_eips_tid(uint32_t, int, void (*)(uint32_t, uint64_t, void*), void*) {}
#endif

}} // namespace d2rt::prof
