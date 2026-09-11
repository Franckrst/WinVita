---
name: instrument-verification
description: Use before trusting ANY counter, profiler, histogram or diagnostic you just added — especially before spending device time on it. Covers proving an instrument reads what you think, the return-address traps (__builtin_return_address, LR clobber), load-bias between ELF and runtime addresses, counters hidden behind an instantaneous state, static sites vs executed calls, and writing the decision criterion before the measurement.
---

# instrument-verification

## Why this exists

An instrument that lies costs more than no instrument. It produces a **plausible
ranking**, you act on it, and you optimise the wrong function while believing you
have evidence.

Three paid-for examples:

| the instrument | what it said | truth |
|---|---|---|
| static count of TLS-resolution sites | « 8 per trap » | 16,81 — sites are not executions |
| a census read after shutdown | `max=100 %` | the `b > 0` guard fell at shutdown — a frozen number |
| a per-caller histogram | six credible addresses | **not one was a return address** |

The third is the model case: the addresses looked like code addresses, they were
stable across windows, they resolved to plausible function names. Everything
looked right. They were garbage.

**Rule: an instrument is not trusted until you have proven, statically, that it
reads what you think. Prove it BEFORE spending a measurement cycle on it.**

## When to use

- You just added a counter, histogram, profiler or diagnostic line
- You are about to spend device/hardware time collecting from it
- A measurement is about to decide a chantier (refactor, rewrite, abandon)
- **A number confirms what you expected** — that is when to test it, not publish it
- You are converting a count into a percentage or a time

## 1. The cheapest possible proof comes first

Ordering matters. A device run costs minutes; a disassembly check costs seconds.
**Never let an expensive cycle be the thing that discovers your instrument is
broken.**

```
    disassembly / static check        seconds   <- do this first, always
    qemu or desktop run               minutes
    platform emulator                 minutes
    hardware                          ~8 min per leg
```

Two broken versions of the same counter were caught at the disassembly step in
one evening. Each would otherwise have cost a full hardware run **and** produced
a credible, wrong ranking of where the time went.

## 2. Return addresses: two traps, one check

The check that catches both, applied mechanically:

> **A return address always points just after a `bl`/`blx`.**

If the address your histogram reports is preceded by `mov`, `str`, `add` — it is
not a return address, full stop. No amount of stable, repeatable,
plausible-looking data changes that.

### Trap A — `__builtin_return_address(0)` under a tail call

GCC turns `return __real_f(p);` into a `b` (sibling call). The builtin then no
longer designates the call site. Silent, no warning.

```c
__attribute__((noinline, optimize("no-optimize-sibling-calls")))
void* __wrap_f(void* p) {
    const unsigned ra = (unsigned)(uintptr_t)__builtin_return_address(0);
    ...
    return __real_f(p);
}
```

Both attributes are needed. Verify in the disassembly that LR is read before
anything clobbers it.

### Trap B — reading LR in asm does NOT save you

The obvious workaround is worse:

```c
unsigned ra; __asm__ volatile("mov %0, lr" : "=r"(ra));   /* WRONG */
```

GCC does not know LR is meaningful and will use it as a scratch register
**before** your asm. Observed, in the generated binary:

```
8103e3ac:  movw  lr, #12120      <- gcc parks a constant in LR
8103e3b0:  movt  lr, #33065
8103e3b4:  mov   r0, lr          <- your asm reads the constant
```

The correct version reads LR still intact:

```
8103e3ac:  movw  r4, #12120      <- gcc uses r4 instead
8103e3b4:  lsr   r2, lr, #2      <- LR is still the caller's return address
```

## 3. Runtime addresses are not ELF addresses

The image is relocated at load. One measured bias was **+0x29000**: every address
published by the running instrument sat 0x29000 above the address in the ELF.

**Do not assume the bias, prove it.** The proof is the `bl` rule from §2:
subtract your candidate bias, then check that *every* entry lands just after a
`bl <your instrument>`. 6/6 landing correctly is proof; 4/6 means the bias is
wrong or the data is.

## 4. A cumulative counter must not hide behind an instantaneous state

A GIL diagnostic (`sched_native.cpp`) returned early when the lock was not held
**at the sampling instant** — and the cumulative acquisition count disappeared
with it. On a light bench the lock is rarely held, so the field vanished; under
load it is nearly always held, so it was there. The counter was always valid; the
gate destroyed it exactly where it was hardest to obtain.

**When a diagnostic mixes a cumulative counter with an instantaneous state,
print the counter unconditionally.** Absence of a line must mean one thing only.

## 4 bis. An oracle must observe the OUTPUT of what you changed, not its input

Before trusting any comparator, ask where in the pipeline it samples. A hash
taken **upstream** of the code under test is blind to that code by construction:
it will report "identical" no matter what you break, and it will do so
confidently, forever.

> A port kept a framebuffer hash used as the pixel-exactness oracle for every
> A/B in the project. Validating a rewrite of the **presentation scaler**, the
> obvious move was to reuse it. But that hash fingerprints the source bitmap the
> game writes — the scaler's *input*. Every scaler bug, up to and including
> rendering pure black, would have passed.

The failure is silent and flattering, which is what makes it dangerous: a
comparator that always agrees feels like strong evidence. The project has been
bitten by comparators whose legs were both wrong; this is the same family.

**Checks, in order:**
1. **Locate the sampling point** relative to your change. Upstream = useless.
2. **Prove it bites**: inject a deliberate, minimal fault (one bit, one pixel)
   and confirm the oracle fails. Then remove the fault and confirm it passes.
   Do this with the **same binary** that will produce the real verdict.
3. **Prove it ran**: a comparator reporting "0 differences" and one that never
   executed look identical in a log. Publish the count of comparisons made, and
   check it is non-zero and plausible.

When the natural oracle samples in the wrong place, the strongest replacement is
to run **both implementations on the same input inside the same call** and
compare their outputs directly. That removes run-to-run non-determinism from the
question entirely — no two runs need to reach the same state.

## 5. Static sites are not executed calls

Counting occurrences in the source — or even in the disassembly — answers a
different question from counting executions, and the two differ by a large
factor whenever branches are mutually exclusive or loops are involved.

- « 8 sites » → really 16,81 executions per trap.
- 4 static sites inside one accessor → 1 execution per call (three branches are
  exclusive), but ~4 **calls** per lock acquisition.

**To count executions, intercept the call.** `-Wl,--wrap=<symbol>` redirects
every undefined reference to `__wrap_<symbol>`, from which you call
`__real_<symbol>`:

```sh
# build knob: define AND link flag together — separating them gives a
# counter that is always zero, i.e. a mute diagnostic
EXTRA_DEFS="$EXTRA_DEFS -DMY_WRAP"
WRAP_LD="-Wl,--wrap=__emutls_get_address"
```

Then verify the wrap actually took, in the disassembly:

```sh
objdump -d out.elf | grep -c 'wrap___emutls_get_address'   # 48
objdump -d out.elf | grep -c '<__emutls_get_address>'      # 1 (the wrapper itself)
```

Per-caller attribution — which is what tells you *where to work* — needs the
return address; see §2.

## 6. Write the decision criterion BEFORE the measurement

A number is much easier to rationalise once you have seen it. Fix the thresholds
first, in writing:

| remaining chains/acq | ceiling on the gain | decision |
|---|---|---|
| ≤ 3 | ≤ 5 % | take the cheap fix, close the file |
| 4 – 8 | 7 – 14 % | open the mechanical refactor |
| > 8 | > 14 % | the heavier design becomes justifiable |

Measured 9,35 → the heavy chantier was justified, and the justification was not
retrofitted.

## 7. Turn a decision into arithmetic: price × frequency

Do not argue about whether an optimisation is worth it. Measure the two factors
separately, then multiply:

- **price** — what one occurrence costs (0,55 µs per TLS chain, on hardware)
- **frequency** — how many occurrences per unit of work (9,35 per lock
  acquisition, measured under real load)

The unknown is usually the frequency, and it is usually the cheaper one to get.

**Cross-check by convergence.** That per-chain price was obtained three
independent ways — 0,43–0,58 µs, 0,44 µs, 0,55 µs — across two different scenes.
Three methods, two scenes, agreement. *That* is when a unit is safe to multiply
by. A single measurement multiplied by a big number is how a recensement goes
wrong by a factor of 50.

## 8. Honest attribution when the gain exceeds the model

One batched path saved 4,37 µs/acq, which at 0,55 µs/chain implies ~8 chains —
more than theory predicted (~3–4). The surplus almost certainly came from three
virtual dispatches removed at the same time.

**Report what you measured, not what you modelled**: « +13,2 % measured », not
« 8 chains removed ». An over-attributed win poisons the next estimate, because
the inflated unit price gets reused.

## Red flags

- « The number is stable across windows, so it's right » → stability is not
  correctness; the broken histogram was perfectly stable.
- « The addresses resolve to plausible function names » → check the `bl` rule.
- « I'll verify it on the hardware run » → you just made the expensive step the
  verification step.
- « The counter confirms what I expected » → test it now, publish later.
- Comparing to a run from another binary, another slot, or another session.
- Quoting a percentage without saying what the denominator's scene was.

## Related

- `hardware-ab-bench` — the hardware A/B protocol these instruments feed
- `runtime-debugging` — the 3-level validation ladder

## 4 ter. Vérifie OÙ ton oracle écrit, pas seulement ce qu'il dit

Un banc scripté redirige souvent la sortie de l'invité vers un fichier à lui,
et n'imprime qu'un verdict sur sa propre sortie. Grepper la sortie du script
revient alors à grepper le mauvais fichier — et une absence y est totalement
muette, parce que ce que tu cherches n'y a jamais été écrit.

> 2026-09-11. Question posée : le chemin rapide du dynarec s'arme-t-il ?
> Deux journaux consultés, aucun message d'armement. J'ai failli en conclure
> que le chemin était mort — la même conclusion négative, tirée d'un silence,
> qui avait déjà fait mentir deux runs témoins le matin même.
> En réalité le banc écrit la sortie de l'invité dans un fichier temporaire à
> lui et n'expose que son verdict. En relançant le binaire directement :
> `csintrin: ... ARME`, puis `enter=7468 (replis=0) leave=7468`. Le chemin
> s'armait depuis le début et s'exécutait 7468 fois par run.

Deux silences différents se ressemblent : « le code ne s'exécute pas » et
« son journal ne va pas là où je regarde ». Avant toute conclusion négative,
**produis un témoin positif dans le fichier que tu grepes** — une ligne dont
tu sais qu'elle doit s'y trouver. Si elle n'y est pas, c'est ton fichier qui
est faux, pas le code.
