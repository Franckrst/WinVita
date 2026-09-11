---
name: hardware-ab-bench
description: Use when measuring a performance change on real hardware — A/B-ing a native port, shim or runtime change, deciding whether an optimisation is worth keeping, or when two runs of the same build disagree. Covers both benches (the deterministic one, cooperative-scheduler only, and the real-load A/B required under the native scheduler), the self-normalising metric, the validity guard, and the traps that make hardware numbers lie.
---

# hardware-ab-bench

## Why this exists

Measuring perf on a device is not "run it twice and compare". Out of the box a
console produces a **12 % noise floor** — large enough to make a 7 % regression
look like a 3 % win. That exact inversion happened once and had to be retracted
the next morning.

The fix is **not more repetitions**. It is removing the sources of randomness,
after which two runs of the same build agree to **0,1 %** and a 1 % effect is
decisive on two runs per flavour.

**Never quote a hardware perf number taken without a bench that removes the
randomness.** Which bench, though, depends on the scheduler backend — and the
deterministic one does not exist for the mode that usually ships. Read §0 first.

## 0. Which bench applies — read this before choosing a protocol

| backend | deterministic bench | what to use |
|---|---|---|
| cooperative | available | §1 — the real thing: 0,1 % noise |
| **native** | **impossible** | §2 — native is real-clock by construction |

The native scheduler is typically the backend the product actually runs on. So
the mode you most need to measure is precisely the one §1 cannot measure. Do not
try to force it: a port should make the combination *native + virtual clock* a
hard, loud error rather than silently ignoring one of the two.

Two benches remain usable under the native scheduler, and they answer different
questions:

- **A light/menu bench** — cheap, reproducible (intra-leg dispersion 0,17 %
  measured over four interleaved legs). But it can run an order of magnitude
  fewer traps per frame than real load, so it **under-reports** anything that
  scales with trap density. Use it as a cheap gate, never as the published
  number.
- **A real-load A/B** — §2. Slower, less pretty, and the only one that answers
  « what does the user actually get ».

Measured spread between the two on the same change: **+3,03 % on the light
bench, +8,6 % under real load.** Publishing the light figure would have
understated the work by 3×.

## 1. The deterministic bench (cooperative only)

Two independent randomness leaks, two fixes:

| leak | why it bites | fix |
|---|---|---|
| **Real clock** | Guest logic ages in real time, so how much work happens between two frames depends on how fast you render. Measured: a work counter at 237 568 vs 303 104 on two *identical* runs (21,6 %). | force the **virtual clock** — the tick advances in fixed steps |
| **Seeded state** | A driver that starts from a clean state re-draws any clock-seeded value, producing a **different world** each run. | **freeze the wall clock** to a fixed value — same seed, same world |

### Proof it worked

| divergence between 2 identical runs | real clock | virtual clock |
|---|---|---|
| collision counter | 21,62 % | **0,00 %** |
| decode counter | 2,92 % | 0,01 % |
| explored counter | 0,52 % | **0,00 %** |
| **fps** | **12 %** | **0,1 %** |

Residual 0,01–0,03 % is fully explained by the runs stopping 2 frames apart.

### Two traps

- **Absolute fps changes between clocks.** The virtual clock ran ~18,9 fps where
  the real clock gave ~20,6. Those numbers compare **only to each other** — never
  across clock modes.
- **Restore the play configuration afterwards.** The virtual clock is a
  measurement mode, not a play mode.

## 2. The real-load A/B protocol (native scheduler)

This replaces §1 when the deterministic bench is unavailable.

**1 — Drive by script, to the second.** The same inputs at the same `T+Ns`, from
launch through to the measured scene. Not negotiable:

> The **same binary** gave **+20 %** hand-driven and **+8,6 %** script-driven.
> The honest number is the scripted one. A hand-driven leg lands in a different
> scene, and the delta then measures the scene, not the change.

**2 — Use a self-normalising metric.** Publish a **ratio**, not a raw rate —
e.g. µs per lock acquisition (`Δt / Δacq`, both from the same periodic line),
rather than ms per frame. A denser scene raises both terms together, so the
ratio divides out scene variation instead of suffering it. The denominator must
be time-proportional in steady state; verify that, don't assume it.

**3 — Publish the validity guard, every time.** A per-frame count of that same
denominator says whether the two legs did the **same guest work per frame**:

| campaign | witness | candidate | verdict |
|---|---|---|---|
| A | 2880 | 2884 | +0,1 % — comparison valid |
| B | 2865 | 2913 | +1,7 % — the new build did *more* guest work, so the win is not from doing less |

**If this number differs materially between legs, the comparison is void.** No
amount of repetition fixes it.

**4 — Back to back, same slot, same session. Absolute values do not travel.**

> The **same binary** measured **31,17 µs/acq** in the morning on one slot and
> **34,44 µs/acq** in the afternoon on another. The known cause was checked and
> ruled out. Cause still unidentified.

Consequence: **never compare against a journal from another session, another
slot, or another binary.** Doing exactly that once made a real gain look
*negative* before the true witness leg was run.

**5 — Put the new build SECOND.** Thermal drift penalises the second leg by
1,0–1,5 %. Running the candidate last makes the measured gain conservative
rather than flattering.

**6 — No polling during the measurement window.** Launch, drive, then a silent
window, then a single read. Polling the device perturbs what you are measuring.

**7 — Take a median over windows past the warm-up**, discarding boot and load.

### Per-leg signature

Distinct binary **sizes** are the cheap per-leg signature: verify the uploaded
size in a separate session before launching, so a failed transfer cannot
silently measure the previous binary twice. This is not paranoia — it has
happened.

## 3. Always gate the campaign on a determinism proof

Start every campaign with **two runs of the same flavour** and compare the work
counters before measuring anything. If they do not match, stop: measuring
different worlds is how you get a retraction.

**Read the numbers, not the script's verdict.** A strict string comparison of
counters will scream NON-DETERMINISTIC over a 0,03 % difference caused by a
2-frame stop offset. Compare field by field, in percent.

## 4. Multi-flavour A/B without reinstalling

Do **not** install one package per flavour. Install one, then swap the
executable between runs. Give each flavour a build tag so its log and its
writable directory are separate — no cross-contamination even though they share
one installation.

**Knobs without a rebuild**: a config file read *after* the baked-in defaults
lets you A/B a single knob on the same binary — one variable moves, nothing
else. Verify it landed by finding it echoed in the log; a knob that silently did
not apply produces a perfectly clean "no difference" result.

## 5. Measure movement, not a still frame

A workload sitting still measures almost nothing: a static scene held 23 fps
where a moving one dropped to 11–17. Drive a **repeatable moving workload** and
measure over a fixed frame window.

Do not use "time to reach frame N" as the metric for a real-time guest — the
guest advances on its own clock, so that measures the clock, not the work.

> Any such driving harness must be **compile-gated out of a shipping build**. An
> unconditional one takes over the first thousands of frames of a real user's
> session. That regression is not hypothetical; it shipped once.

## 6. Scripted-boot fragility

A single scripted key can land on a different screen than intended and exit the
app. Two mitigations, both needed:

1. Repeat the input, spaced, so it is harmless if the screen is not there yet.
2. **Retry inside the driver**: on an early exit, relaunch the same flavour under
   identical conditions rather than recording a dead run. Roughly 2 runs in 8
   needed it.

Also beware stray files in a state directory (a metadata file from another OS
counted as a real entry) hanging a scripted selection.

## Red flags

- A hardware number with no determinism gate → not a number, an anecdote.
- A ratio published without its validity guard → unverifiable.
- Comparing against yesterday's log → absolute values do not travel.
- The candidate measured first → the thermal drift flatters it.
- A gain that only exists on the light bench → say so explicitly.
