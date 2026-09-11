---
name: intermittent-corruption
description: Use when a runtime fault is non-deterministic — crashes on some runs not others, "works on the 2nd try", corrupt data with an off-by-one or shifted look, or a wild jump that only appears under the real clock. The elimination method that isolates whether the cause is input data, translation, control flow, interruption, or memory aliasing.
---

# Diagnosing intermittent corruption

Intermittent = timing-dependent = an ordering or state difference between runs.
The goal is to **narrow which layer differs** by proving the others identical,
until one suspect remains. Guessing costs days; proof by elimination has cracked
this class of bug in an afternoon.

## Get a fast, high-rate repro first

You cannot bisect a bug you hit 1-in-10. Compress the scenario until it fails
most runs — real clock plus a tight scripted drive reached ~90 % failure in
~2 minutes on the case this method was built for.

Keep a clean run and a crash run **from the same build** for side-by-side
diffing. Comparing across builds imports differences you did not intend to test.

## The elimination ladder (prove each rung identical, then climb)

Run the same operation in a crashing and a clean run and compare, cheapest
first:

1. **Input** — hash the exact bytes the code consumes (a cheap FNV over the
   buffer at the *real* source register, not what you assume the source is).
   Identical? The input is not corrupt; climb.
2. **Translation** — dump the translated ARM blocks over the code range: start
   address, size, hash, flags. Compare **intra-run only** — host literal
   addresses (ASLR) make the hash differ between processes. Within one run,
   "same blocks at the fault as at entry" proves the code did not self-modify or
   get retranslated.
3. **Control flow** — force the LinkNext funnel (`WX86_NOLINK=1`) and log every
   block target through the corruption window. An identical block sequence
   between clean and crash proves the code took the **same branches** — so the
   bad output is not a mispredicted branch or an extra loop iteration.
4. **Interruption** — trace scheduler switches and preemptions in the window.
   Zero switches between the clean input and the faulted output means nothing
   interrupted the computation.

If input, translation, control flow and interruption are all identical yet the
output differs, then the output was written to a location that **aliases another
live allocation**, or read from memory another actor changed — a **memory or
allocator problem**, not a logic bug.

That is the pivotal deduction. It tells you to stop reverse-engineering the
algorithm and start looking at allocation lifetimes, which is a completely
different search.

## Reading the corruption shape

A field that reads `0xC00` where `0x0C` is expected (the value shifted one byte
up) is a **1-byte misalignment**, not a random smash.

Chained or variable-stride records amplify it: if record N's size is 1 too big,
record N+1 lands 1 byte off and everything after cascades. Record 0 intact plus
record 1 shifted points precisely at where the stride was computed — which is
usually a much smaller piece of code than the one you were about to read.

## "It's a latent bug in the guest itself" is a real answer

If input, translation, control flow and interruption are identical and the only
variable is allocation order, the defect is in the **guest program** — exposed by
our variable timing, dormant on the original platform because its allocation
order was stable.

Do not "fix" code that is provably running correctly: that invents a bug you now
own. Prove it, then either:

- **contain it natively** — turn a fatal validator into a failure return so the
  caller's own rebuild path re-runs with moved cursors; or
- **neutralize the allocator condition** (guard padding, zero-on-recommit), but
  **only if a battery proves it removes the crash**. An untested "fix" that
  changes behaviour is worse than an honest retry net, because it also removes
  the signal.

## Verifying a candidate fix

- Run the crash battery (6–8 runs) with the **containment net OFF**, so you
  measure the fix and not the net.
- Run the virtual-clock byte-identity smoke: the fix must not perturb it.
- **A fix that drops crashes from 90 % to 50 % is NOT a fix** — it moved the
  timing. Keep the net and keep looking.

## Red flags

- Patching an algorithm before proving its input or flow differ → you are
  probably inventing a defect. Climb the ladder first.
- Comparing translated-block hashes *between processes* → ASLR noise,
  meaningless. Compare intra-run.
- Declaring victory on one clean run → intermittent means run the battery.
