// src/runtime/cpu.h — x86-32 CPU backend interface for the D2 runtime.
//
// Two backends implement this:
//   * CpuUnicorn  (desktop dev/test + differential oracle)   — cpu_unicorn.cpp
//   * CpuDynarec  (PS Vita ARMv7, the shipping engine)        — cpu_dynarec_arm.cpp (WIP)
//
// The bridge (bridge.h) is backend-agnostic: it registers a "trap" address
// range; whenever guest execution transfers control into that range, the
// backend halts, invokes Cpu::Trap, and resumes at whatever EIP the trap
// handler leaves in the register file. That single mechanism carries both
// the x86→native import calls and the "call finished" sentinel.

#pragma once
#include <cstdint>
#include <functional>

namespace d2rt {

// x86 register indices we care about (subset).
enum Reg {
    R_EAX, R_ECX, R_EDX, R_EBX, R_ESP, R_EBP, R_ESI, R_EDI, R_EIP, R_EFLAGS,
    R_COUNT
};

enum Prot { P_R = 1, P_W = 2, P_X = 4, P_RW = 3, P_RWX = 7 };

// Called when guest control enters the trap range. `trap_va` is the address
// jumped to. The handler mutates CPU state (via the Cpu&) — typically it
// reads args off the guest stack, performs the native call, writes EAX/EDX,
// and sets EIP to the guest return address. Returns:
//   true  → resume emulation at the (possibly new) EIP
//   false → stop the run entirely (used by the final-return sentinel)
struct Cpu;
using TrapFn = std::function<bool(Cpu& cpu, uint32_t trap_va)>;

struct Cpu {
    virtual ~Cpu() = default;

    // Memory. `init` may be null (zero-filled). Region must not overlap.
    virtual bool map(uint32_t va, uint32_t size, const void* init, int prot) = 0;
    virtual bool protect(uint32_t va, uint32_t size, int prot) = 0;
    virtual bool read(uint32_t va, void* dst, uint32_t n) = 0;
    virtual bool write(uint32_t va, const void* src, uint32_t n) = 0;
    // Direct host view of guest memory (Box86: va+membase; null if the backend
    // has no stable host mapping — callers MUST fall back to read()/write()).
    // For DATA buffers only: writing code through this bypasses the dynarec
    // dirty-page tracking (write() handles that; hostptr writes must never
    // touch guest bytes that may have been translated).
    // `n` = how many bytes the caller will touch: backends that can tell refuse
    // (null) a range escaping the guest arena, so bulk shim I/O can never memcpy
    // into a host object behind a wild guest pointer (C3, deep review).
    virtual void* hostptr(uint32_t va, uint32_t n = 1) { (void)va; (void)n; return nullptr; }
    // Stable pointer to the guest EIP storage (diagnostics: the Vita watchdog
    // samples it racily to name where a hung guest is spinning). Null if the
    // backend has no stable register file.
    virtual const uint32_t* ip_ptr() const { return nullptr; }
    // Is `va` inside a map()ed guest region? Backends that can't tell return
    // true (their read/write already fail soft on unmapped addresses).
    virtual bool mapped(uint32_t va) { (void)va; return true; }

    // Invalidate any translated (dynarec) code covering [va, va+size): the next
    // execution re-hashes/re-translates from the current guest bytes. The honest
    // hook for FlushInstructionCache / VirtualProtect(PAGE_EXECUTE_*) so a
    // well-behaved self-patcher (Storm, a dynamically loaded module) never runs
    // a stale translation. Mirrors the write() barrier. No-op on backends
    // without a code cache. Same guest-VA keying as read()/write().
    virtual void invalidate_code(uint32_t va, uint32_t size) { (void)va; (void)size; }

    // JETER les traductions de [va, va+size) — pas « marquer sales », JETER.
    //
    // invalidate_code() suit le contrat SMC de box86 : il ne fait quelque chose
    // que si la plage est ENTIEREMENT sous PROT_DYNAREC (isprotectedDB rend 0
    // des qu'UNE page ne l'est pas). C'est prevu pour un auto-patch de quelques
    // octets ; c'est un NO-OP SILENCIEUX pour ce dont un rechargement d'image
    // complet a besoin — remplacer un exe de plusieurs Mo dont seules quelques
    // pages ont ete traduites laisse le nouveau processus executer les
    // dynablocks du PRECEDENT aux memes adresses (bug confirme sur carn-vita,
    // 2026-09-06 : l'enchainement de deux .exe partait en vrille juste apres
    // le premier ecran). D'ou cette methode, qui DETRUIT sans condition les
    // dynablocks de la plage. Cher : a n'appeler qu'a un changement d'image.
    virtual void discard_code(uint32_t va, uint32_t size) { invalidate_code(va, size); }

    uint32_t read_u32(uint32_t va) { uint32_t v = 0; read(va, &v, 4); return v; }
    void     write_u32(uint32_t va, uint32_t v) { write(va, &v, 4); }

    // Registers.
    virtual uint32_t reg(int r) = 0;
    virtual void set_reg(int r, uint32_t v) = 0;

    // Segment base for FS (Windows TIB/TEB). Default no-op; Unicorn overrides.
    virtual void set_fs_base(uint32_t base) { (void)base; }

    // True if set_fs_base() gives each guest thread a real per-thread FS base
    // (dynarec backend: segs_offs[_FS]); false when FS is emulated at a fixed
    // linear address (Unicorn: the scheduler swaps the TIB page at linear 0).
    virtual bool fs_base_is_direct() const { return false; }


    // Per-instruction callback (debug tracing). Called with the EIP about to
    // execute. Default no-op; Unicorn installs a UC_HOOK_CODE.
    virtual void set_code_hook(std::function<void(uint32_t)> fn) { (void)fn; }

    // Memory-write callback (debug). Called with (addr, size, value). Default
    // no-op; Unicorn installs a UC_HOOK_MEM_WRITE.
    virtual void set_write_hook(std::function<void(uint32_t,int,uint32_t)> fn) { (void)fn; }

    // Per-thread context save/restore — a threading backend swaps guest threads
    // by saving the running thread's X86 state and loading another's. Default
    // impl uses reg()/set_reg(); backends may override for speed.
    virtual void save_context(struct X86Context& c);
    virtual void load_context(const struct X86Context& c);

    // Convenience stack ops on the guest ESP.
    uint32_t pop_u32() { uint32_t esp = reg(R_ESP); uint32_t v = read_u32(esp); set_reg(R_ESP, esp + 4); return v; }
    void     push_u32(uint32_t v) { uint32_t esp = reg(R_ESP) - 4; write_u32(esp, v); set_reg(R_ESP, esp); }
    // Read the k-th cdecl stack argument (0-based), given ESP already points
    // at the return address (i.e. just after the call, before prologue).
    uint32_t arg(uint32_t k) { return read_u32(reg(R_ESP) + 4 + 4 * k); }

    // Lecture GROUPEE des huit registres GENERAUX (R_EAX..R_EDI) : remplit
    // out[R_EAX]..out[R_EDI]. Meme motif que trap_retaddr/trap_epilogue, et
    // pour la meme raison : un crochet natif lisait ESP, puis ECX, puis EDX en
    // TROIS appels, donc trois resolutions d'etat par fil (chaine emutls,
    // 0,43-0,58 us mesurees par appel sur console). Une seule suffit.
    //
    // AUCUN EFFET DE BORD, et c'est ce qui rend le groupage sur : la lecture
    // des huit generaux est pure. R_EFLAGS est EXCLU A DESSEIN — le lire
    // materialise les drapeaux differes de Box86 (UpdateFlags), donc il n'a
    // rien a faire dans une lecture speculative de tout le fichier.
    //
    // L'IMPLEMENTATION PAR DEFAUT EST L'ANCIEN CODE, registre par registre :
    // un backend qui ne surcharge pas reste correct sans qu'on y touche.
    virtual void regs_gp(uint32_t* out) {
        for (int r = R_EAX; r <= R_EDI; ++r) out[r] = reg(r);
    }

    // ---- Entree et sortie de trap, GROUPEES --------------------------------
    // Le chemin de trap manipulait les registres UN PAR UN : 2 reg() + 3
    // set_reg() par trap, chacun repayant sa propre resolution d'etat par fil.
    // Mesure console en jeu (2026-08-31, histogramme par appelant) :
    // reg()+set_reg() portaient 73 % des 9,92 chaines emutls par prise de GIL.
    //
    // L'IMPLEMENTATION PAR DEFAUT EST L'ANCIEN CODE, effet pour effet : un
    // backend qui ne surcharge pas reste correct sans qu'on y touche. C'est le
    // cas de CpuUnicorn, l'oracle de verification — sa semantique ne bouge pas
    // d'un bit, et c'est ce qui rend ce changement d'interface sur.
    virtual uint32_t trap_retaddr() { return read_u32(reg(R_ESP)); }
    // esp_add : ce que `ret [imm]` retire de la pile (4, plus 4*argc en stdcall).
    // eip     : la destination FINALE, redirection deja arbitree par l'appelant.
    virtual void trap_epilogue(uint32_t eax, uint32_t esp_add, uint32_t eip) {
        set_reg(R_EAX, eax);
        set_reg(R_ESP, reg(R_ESP) + esp_add);
        set_reg(R_EIP, eip);
    }

    // ---- B5 : entree ET sortie d'un trap stdcall argc=0, en UN SEUL appel ---
    // Le couple trap_retaddr() + trap_epilogue() coute DEUX resolutions d'etat
    // par fil : le backend Box86 les groupe chacune de son cote, mais il n'a
    // aucun moyen de savoir que les deux appels concernent le meme trap. Pour
    // le chemin d'horloge — le creneau le plus pris de tout le binaire — la
    // valeur rendue ne depend PAS de l'adresse de retour : on peut donc lire
    // la pile et publier EAX/ESP/EIP dans la meme resolution.
    //
    // L'IMPLEMENTATION PAR DEFAUT EST L'ANCIEN CODE, effet pour effet et dans
    // le meme ordre observable (lecture de ESP, lecture de [ESP], puis les
    // trois ecritures) : un backend qui ne la surcharge pas reste correct.
    // C'est ce qui rend ce changement d'interface sur pour tout appelant.
    //
    // PRECONDITION, la meme que trap_retaddr() : ESP pointe sur l'adresse de
    // retour (juste apres le `call`, aucun argument empile — argc=0).
    virtual void trap_ret0(uint32_t eax) {
        const uint32_t esp = reg(R_ESP);
        const uint32_t ret = read_u32(esp);
        set_reg(R_EAX, eax);
        set_reg(R_ESP, esp + 4);
        set_reg(R_EIP, ret);
    }

    // Install the trap handler and the [lo,hi) guest VA range that triggers it.
    virtual void set_trap(uint32_t lo, uint32_t hi, TrapFn fn) = 0;

    // Stop the current run() cleanly from inside a hook (cooperative preemption).
    // The run returns as if it stopped normally, resumable at the next EIP.
    // NATIVE-backend contract: shutdown-grade and MONOTONIC — dyn86's g_stop is
    // never cleared in native mode (dyn86_set_native gates the slice_begin
    // reset), so after one call every LinkNext seam diverts to the epilog
    // before translating, forever, and the async quit=1 broadcast can make
    // ANOTHER thread's DynaRun exit with no break marker (looks like a clean
    // stop). Call it ONLY for process shutdown — never for an ordinary thread
    // exit (ExitThread/TerminateThread have their own paths).
    virtual void request_stop() {}

    // Native instruction budget for the NEXT run() (0 = unlimited). Backends
    // that support it stop after ~n guest instructions WITHOUT a per-insn
    // hook (Unicorn: uc_emu_start count — 10-50x faster than a code hook).
    // take_limit_hit() reports (and clears) whether the last run() ended by
    // exhausting that budget rather than by trap/sentinel/yield.
    virtual void set_run_limit(uint64_t) {}
    virtual bool take_limit_hit() { return false; }

    // Filet anti-famine (spec 2026-08-29 §3.3) : zéroe le budget de blocs d'UN
    // emu (handle thread_emu_create, ou nullptr = emu de base) pour forcer sa
    // sortie du code traduit au prochain prologue — préemption à la demande.
    // CONTRAT : ne touche JAMAIS quit ni g_stop — ils sont MONOTONES en natif,
    // réservés au shutdown (voir request_stop ci-dessus) ; nudge est répétable
    // et sans effet sur la sémantique invitée (il raccourcit une tranche, rien
    // d'autre). Course RMW bénigne assumée : le prologue dyn86 fait
    // load-décrément-store sur le budget ; un zéro écrit entre le load et le
    // store est perdu — rattrapé à la fenêtre de détection suivante (§3.3).
    // Appelant : tient le GIL et parcourt le registre append-only des emus
    // (même discipline que request_stop appelé sous GIL). No-op par défaut
    // (backends sans budget de blocs — l'ex-CpuUnicorn en héritait).
    virtual void nudge_thread_budget(void* emu) { (void)emu; }

    // ---- Native-scheduler backend support (one x86emu per guest thread) ----
    // Create an independent emu sharing the process context (block cache,
    // custommem). Null = backend has no multi-emu support (Unicorn): the
    // native scheduler refuses to start on it.
    // Registry contract: append-only, never freed (request_stop iterates it
    // from arbitrary GIL holders and thread_emu_ip pointers are handed to the
    // lock-free watchdog reader — freeing would race both; the leak is bounded
    // by the scheduler's thread cap). Call under the runtime GIL, or before
    // any second runner exists.
    virtual void* thread_emu_create() { return nullptr; }
    // Bind `emu` as the CURRENT HOST THREAD's register file: every reg()/
    // set_reg()/run()/save/load on this host thread now targets it. nullptr
    // rebinds the base emu. TLS — affects only the calling host thread.
    virtual void thread_emu_bind(void* emu) { (void)emu; }
    // Racily-sampleable EIP storage of one emu (per-thread watchdog dumps).
    virtual const uint32_t* thread_emu_ip(void* emu) { (void)emu; return nullptr; }
    // Vivacité PAR FIL (diagnostic famine/interblocage du backend natif).
    // Rend dans *out le nombre de blocs traduits exécutés par l'emu de ce fil
    // depuis le boot — MONOTONE (modulo 2^32 : seules les DIFFÉRENCES sont
    // exploitées, et une différence non signée reste exacte à travers le
    // repli). Le compteur est dérivé du budget de blocs, que le prologue de
    // CHAQUE bloc traduit décrémente déjà : il bouge dès que le fil exécute du
    // code traduit, y compris une boucle qui ne franchit AUCUN shim, et il ne
    // coûte rien sur le chemin chaud (une addition par TRANCHE, pas par bloc).
    // POURQUOI MONOTONE et pas le budget nu : le filet anti-famine ZÉROTE le
    // budget de tous les fils Running à chaque détection, donc deux relevés
    // successifs d'un fil qui tourne à débit constant affichaient la MÊME
    // valeur (« blocs depuis le dernier poke ») — et la règle de lecture
    // « aucun compteur ne bouge => interblocage » désignait alors comme
    // interbloqué le fil qui AFFAME les autres (revue I1).
    // Lecture racy assumée (même régime que thread_emu_ip), MAIS ordonnée :
    // l'implémentation lit le total, PUIS l'armement, PUIS le budget, et
    // l'écrivain désarme avant d'accumuler (cpu_box86.cpp, ordre commenté sur
    // place). Conséquence, et c'est le contrat du champ : le seul relevé faux
    // possible est une SOUS-estimation d'une tranche, sur une fenêtre de
    // quelques instructions ; JAMAIS un bond en avant, qui se lirait « ce fil
    // galope » — le sens faux. Toute réimplémentation doit garder cet ordre.
    // Rend false = backend sans compteur de blocs (champ ABSENT, pas un zéro
    // menteur). nullptr = emu de base (main).
    virtual bool thread_emu_blocks(void* emu, uint32_t* out) { (void)emu; (void)out; return false; }
    // coeur2 (06/09/2026) : densite de traps et contention du GIL PAR FIL —
    // traps = entrees dans la fenetre de trap (une prise du GIL chacune),
    // cont = prises contendues (trylock echoue), wait_us = attente cumulee sur
    // ces prises. Comptes UNIQUEMENT sous D2_FILSTAT=1 ; rend false si le
    // backend ne compte pas (champ ABSENT, pas un zero menteur). Lecture racy
    // assumee (meme regime que thread_emu_blocks). nullptr = emu de base.
    virtual bool thread_emu_filstat(void* emu, uint32_t* traps, uint32_t* cont, uint32_t* wait_us) {
        (void)emu; (void)traps; (void)cont; (void)wait_us; return false; }

    // Translation-time GUEST->GUEST redirect: the dynarec sends call/jmp targets
    // equal to `from` to `to`, WITHOUT modifying guest memory. Lets a known
    // function (e.g. the palette blit) run a native shim while the guest .text
    // stays byte-identical to disk (pristine mode). No-op on backends without it.
    virtual void set_alternate(uint32_t from, uint32_t to) { (void)from; (void)to; }

    // Warden-safe "intrinsic" for one OS-shim trap slot. When guest control
    // enters `slot_va`, the backend calls fn(cpu, slot_va) at the dynarec trap
    // dispatch BEFORE the generic Bridge handler. fn returns true when it fully
    // serviced the call (EAX/ESP/EIP already set) — the backend resumes without
    // ever entering the Bridge/std::function shim path; false defers to the
    // normal handler. The guest observes NOTHING new (same IAT, same GetProcAddress
    // result, same registers/memory) — this only makes servicing the trap cheaper.
    // Captures every call site (IAT- and GetProcAddress-resolved alike) because
    // they all resolve to the same trap VA. No-op default. (docs/perf)
    using IntrinsicFn = bool(*)(Cpu& cpu, uint32_t slot_va);
    virtual void set_intrinsic(uint32_t slot_va, IntrinsicFn fn) { (void)slot_va; (void)fn; }
    // Runtime A/B toggle. D2_DISABLE_INTRINSICS=1 forces the historical trap path
    // from the start; this lets a self-test flip it per call for state parity.
    virtual void set_intrinsics_enabled(bool on) { (void)on; }

    // Run starting at `eip` until a trap returns false or a fault occurs.
    // Returns true on clean stop (sentinel), false on fault; `fault` gets a
    // reason string pointer (static) when it returns false.
    virtual bool run(uint32_t eip, const char** fault) = 0;

    // Last fault address (valid after run() returns false).
    virtual uint32_t fault_addr() const = 0;

    // Windows exception code for the last fault (valid after run() returns
    // false), for the SEH dispatcher. 0 = not classifiable / not SEH-eligible
    // (e.g. an emulator gap) -> caller terminates as before. Backends that can
    // tell div0 (0xC0000094) / illegal (0xC000001D) / AV (0xC0000005) override.
    virtual uint32_t fault_code() const { return 0; }
};

Cpu* make_cpu_box86();     // cpu_box86.cpp (ARM only; single instance)

} // namespace d2rt


