---
name: dynarec-internals
description: Use when the fault is in the emulation layer itself (Box86-derived dynarec + scheduler), not in the guest program — a slowdown from cache flushes, cross-thread state corruption, preemption landing badly, a wait returning the wrong value, or fast-forward/real-clock timing. Covers the block-budget preemption seam, per-thread CPU state, icache sync cost, and the wait/wake contract.
---

# Dynarec + scheduler internals

The CPU backend is a Box86-derived x86→ARMv7 dynarec (`third_party/box86-dynarec`,
`src/dynarec86`, `src/runtime/cpu_box86.cpp`) driven by one of two schedulers
(`src/runtime/sched_cooperative.*` or `sched_native.*` + `gil.*`). One `x86emu_t`
serves **all** guest threads; the scheduler swaps a per-thread `X86Context` around
it. This shared-emu design is the source of the subtlest bugs in the engine.

## Per-thread CPU state (the big one)

`X86Context` must carry **everything live across a switch**, not just the GPRs.
When quantum preemption stops a thread mid-computation, the FPU/MMX/XMM state is
live. It was NOT saved originally → the next thread inherited it → corrupt
computation results and wild jumps, but **only under the real clock** (fixed
points in the virtual clock never landed mid-computation, which hid it for
weeks). `save_context`/`load_context` in `cpu_box86.cpp` now copy the full x87
stack + control/status/tags + MMX + XMM + mxcsr.

**Any new live CPU state must join that blob.** The test that identifies this
class of bug: run with preemption effectively off (a huge quantum). If the bug
vanishes, it is a context-not-saved bug — not a logic bug.

## Block-budget preemption

Preemption is a 4-instruction budget check emitted in every block prologue
(`dynarec_arm_pass.c`): `LDR/SUBS/STR dyn86_budget; BGT`. On expiry the block
exits **resumably** (`ip = block start`, nothing of the block ran).

Blocks stay **direct-linked** (no LinkNext round-trip) — fast, but it means a
loop **entirely inside one block** never re-checks the budget and therefore
cannot be preempted. `WX86_NOLINK=1` forces every chain back through the
LinkNext funnel: slow, but it makes every block transition observable, which is
what you want when tracing control flow.

## icache/dcache sync cost

Emitted ARM code needs `__clear_cache`, which on Vita becomes
`sceKernelSyncVMDomain`. The domain wants block-granular ranges; syncing the
**whole 2 MiB JIT block** per translated function measured ~27 ms each — minutes
of freeze on a heavy load. Sync only the **emitted page range**
(`src/platform/mman_vita.c`), whole-block only as a fallback.

This cost is **invisible off-device**: desktop/qemu `__clear_cache` is cheap. It
is one of the clearest cases for the 3-level validation ladder — a change that
looks free on qemu can be a multi-second freeze on hardware.

## The real-time clock

The default (virtual) clock advances at emulation speed, so the guest runs
**fast-forward** on hardware. Real-clock mode makes the guest's millisecond
source follow a monotonic host clock, makes all-blocked idles really sleep, and
makes timed waits expire on real time.

It engages **only at scheduler start** — initialization (DllMain and friends)
needs the per-call virtual tick — so a tick offset keeps the guest's tick
monotonic across the switch. `GetTickCount`/`timeGetTime`/QPC must read real
time in this mode; an inlined stub reading a tick page that is frozen between
rounds silently re-introduces fast-forward and hangs.

!!! note "The knob names live in the consumer, not here"
    The engine provides the *mechanism*. Which clock a port runs on, and the
    environment variable that selects it, is the port's policy — look in the
    port's boot binary, not in winx86. The native scheduler is real-clock **by
    construction**; a port that also offers a virtual-clock bench must make the
    combination a hard error rather than silently ignoring one of the two.

## The wait/wake contract

A shim that blocks calls the scheduler's `wait(waitable, timeout)`; the thread's
EAX is set on wake. Two traps here, both already paid for:

- **EAX clobber** — the wake writes `ctx.gpr[0]` (result, or the timeout code).
  If the shim already returned its own value (a message-peek returning 0, say),
  the wake overwrites it. `wake_writes_eax=false` (`wait_noresult`) suppresses it.
- **BOOL vs wait-code** — `WAIT_TIMEOUT` is `0x102`, which a BOOL-returning shim
  reads as TRUE → a phantom success. `wake_timeout_eax` lets such a shim receive
  `0` on timeout instead.

## Frame pacing

A guest that paces frames by spinning on an empty message-peek (thousands of
peeks per frame is normal for that idiom) will, in real-clock mode, burn a full
scheduler round per peek and starve rendering. Turn the empty peek into a short
timed wait (`wait_noresult`). The switch-count guard must then be raised by
orders of magnitude — real mode multiplies switches.

## Red flags

- A cross-thread "sometimes wrong" value → check `X86Context` actually saves
  that state; test with preemption off.
- A perf cliff absent on qemu → suspect the device-only cache-sync cost; measure
  it directly rather than inferring it.
- A wait returning a surprising value → EAX clobber, or a BOOL/wait-code
  mismatch.
- Editing preemption or block linking without re-running the byte-identity
  smoke — those two knobs change emulation semantics, not just speed.
