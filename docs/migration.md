# Migrating an existing port onto the engine

[Extension point](extension.md) explains how to start a **new** port on
winx86. This document covers the opposite, more frequent case: a port
that already exists, that has its own complete Win32 runtime, and that
needs to **stop duplicating** what the engine now owns.

This is the case for any port born by copying another — the fastest way
to start one, and the one that produces two codebases that then diverge.

!!! abstract "The guiding principle"
    **Every place where a second port switches over to the engine's code
    is proof of genericity. Every place where it can't is a design flaw
    found for free.**

    An engine isn't proven generic because it was designed that way. It is
    when a second consumer actually uses it. Until that happens, the
    engine holds a *copy* of code that also lives in both ports: three
    copies instead of two, which is worse than the starting point.

## State of play, with numbers

Every number below is measured on the tree, not estimated.

### What the engine owns

| | |
|---|---:|
| Engine sources (`src/`) | 17,723 lines |
| Win32 shim registrations | 362 |
| DLLs covered (at least partially) | 14 |
| Install units (`win32_shims_*_install`) | 14 |

Beyond the shims, the engine owns the cross-cutting building blocks a port
used to write for itself:

| block | file | what it replaces on the port side |
|---|---|---|
| guest scratch allocator | `runtime/guest_scratch` | a local `misc()` / `put_cstr` |
| region allocator | `runtime/guest_region` | a local `RegionAlloc` |
| current thread context | `runtime/guest_thread_ctx` | TIB access and last-error |
| guest atomics | `runtime/guest_atomics` | the `Interlocked*` family and their SMC contract |
| kernel objects + handle table | `runtime/guest_sync` | `KEvent`/`KSemaphore`/`KThread`/`KCrit`, `g_handles` |
| socket layer + observer + route | `runtime/win32_shims_wsock32` | the network stack and its instrumentation |
| graphics glue | `render/render.h`, `render/render_null.cpp` | the bridge to the GPU and the counting backend |
| presentation scaling | `platform/present_scale` | the `scale+flip` and proportional framing |
| audio sink | `runtime/audio_sink`, `platform/vita_audio` | the null / WAV / console trio |

### What a port keeps today

Measured on the first port, whose shim list is [generated from the
sources](shims.md) on both sides:

| | Keys | Share |
|---|---:|---:|
| served by the engine | 362 | 55% |
| served by the port | 294 | 45% |

The breakdown by DLL shows where the remaining work sits:

| DLL | total | engine | port |
|---|---:|---:|---:|
| `KERNEL32.dll` | 252 | 137 | 115 |
| `USER32.dll` | 92 | 56 | 36 |
| `ADVAPI32.dll` | 67 | 37 | 30 |
| `GDI32.dll` | 41 | 36 | 5 |
| `WSOCK32.dll` / `WS2_32.dll` | 62 | 58 | 4 |

DLLs absent from this table (`glide3x`, `native.hook`, `CRYPT32`, and
game-specific DLLs) are **entirely** on the port side, and that's
expected: they carry the game's own API, not Windows'.

## The concrete case: the second port

This base's second port — a 3D game, where the first renders in paletted
2D — descends from the same foundation: its history shows commits that
*remove* the first game rather than write a new runtime. That's what makes
the comparison so informative: **someone has already sorted "what's
specific to the first game?" and removed it. What survived on both sides
is not the game.**

### What it duplicates

| file | lines | registrations |
|---|---:|---:|
| `src/win32/shims_misc.cpp` | 1,471 | 252 |
| `src/win32/shims_kernel32.cpp` | 1,549 | 176 |
| `src/win32/shims_user32.cpp` | 730 | 94 |
| `src/win32/shims_gdi32.cpp` | 468 | 37 |
| `src/win32/shims_audio.cpp` | 384 | 10 |
| `src/win32/shims_carnivores.cpp` | 146 | 7 |

That's roughly **576 registrations**, against the 362 the engine
provides.

Plus, outside shims: its own `RegionAlloc` (`src/win32/rt_internals.h`),
its own `misc()` and `put_cstr` (`tools/rt_boot.cpp`), its own kernel
objects and its own handle table, its own `do_scale_and_flip`, its own
counting backend, its own audio sink layer.

### How much of it is the same thing

Measured, normalizing whitespace and comments:

| object | verdict |
|---|---|
| `KEvent` ↔ `WxEvent` | **identical** (245 characters on both sides) |
| `KSemaphore` ↔ `WxSemaphore` | **identical** (219 characters) |
| `KThread` ↔ `WxThread` | **identical** (259 characters) |
| `KCrit` ↔ `WxCrit` | identical **except for declaration style** (`uint32_t va=0; uint32_t owner=0;` vs. `uint32_t va=0, owner=0;`) |
| `KMultiWait` ↔ `WxMultiWait` | **identical line for line** (28 lines on both sides) |
| `KIocp` ↔ `WxIocp` | **identical line for line** (16 lines) |

Six kernel objects, five of them rigorously identical and the sixth
separated by a semicolon. This isn't a happy convergence: it's the same
code, copied and then left to drift cosmetically.

!!! danger "Citing evidence doesn't replace shipping the object"
    The engine long provided only **4** of these 6 types — while
    `guest_sync.h`'s own header already cited `WxMultiWait`'s atomicity
    invariant as *proof* that these structures are generic. The proof was
    there, the object wasn't, and both ports kept writing it.

    Found while migrating the second port, fixed since. This is exactly
    what the guiding principle predicts: **what a second consumer can't
    reuse names an engine flaw.**

### What's already done

The first wave has already happened, and it validates the method.

The engine had deduplicated KERNEL32 **against** the second port, on a
two-condition criterion: identical body once comments and whitespace are
stripped, **and** no external dependency (no shared state, no scheduler,
no clock). 46 shims satisfied both. The second port then re-verified those
46 one by one on its own side, then removed them in favor of
`win32_shims_kernel32_install(br)`.

The ordering detail is worth copying:

```cpp
win32_shims_kernel32_install(br);   // engine first
register_kernel32(br);              // then its own, which win
```

The ~15 KERNEL32 shims the engine provides **on top** (the `Interlocked*`,
`GetLastError`/`SetLastError`) depend on an extension point — current TIB
and scheduler — that this port doesn't wire up yet. So they stay on its
side, and since its own registration comes **after**, it overwrites them
with no side effect. The "last registration wins" rule isn't just a trap:
used deliberately, it's the mechanism that makes a gradual migration safe.

## The split rule: three categories, not two

This is where almost everyone gets it wrong, and the mistake is expensive
because it leaves generic code on the wrong side forever.

| category | criterion | destination |
|---|---|---|
| **generic** | no literal, branch, or state dependency specific to a game | the **engine** |
| **game-specific** | a hardcoded literal, a dependency on game state, a workaround, an address from its own binary | the **port** |
| **console-specific** | depends on the target hardware, not the game | the **engine** |

!!! warning "The third category is the one that gets misfiled"
    The engine targets PS Vita. So the console's GPU, its audio, its
    cores, its monotonic clock, its boot log, reading its pad: **all of
    that belongs to the engine**, even though it doesn't look like Win32.

    Filed under "game-specific" because it doesn't look like emulation,
    this code stays on the port side and every new port rewrites it.

The counter-example that fixes the boundary, drawn from inputs:

- the table mapping *this button* to *this game action* (a key, a click,
  an inventory shortcut) is **game-specific** — those codes mean nothing
  for another title, and both ports do have two different tables;
- **reading** the pad, the virtual cursor, the orbit, the sensitivity, the
  dead zone, the short-press thresholds are **console-specific**: both
  ports need them identically.

So the right split is: the engine reads the pad and manages the cursor,
the port provides the mapping table.

## The migration order, wave by wave

Safest to riskiest. Each wave states what it replaces, what it **proves**,
and how it's validated.

### Wave 1 — dependency-free shims *(done on the second port)*

**Replaces**: shims whose body is identical and that depend on no external
symbol.
**Proves**: that the two-condition criterion is operational, and that
"engine first, port second" ordering is safe.
**Validates**: recount registrations, build both targets, a real game run
under ARM emulation.

Starting here isn't excessive caution: it's the wave that installs the
mechanism. Everything else depends on it.

### Wave 2 — the allocators

**Replaces**: `misc()` / `put_cstr` with `runtime/guest_scratch`, the local
`RegionAlloc` with `runtime/guest_region`.
**Proves**: that the engine can own **state** from the port, not just pure
functions. This is the first real transfer of ownership.
**Validates**: no twin should remain (see pitfall #1); exhaustion must be
reported by the port through the callback the engine exposes.

The engine owns the allocator; the port grants it a range, because the
memory layout itself has nothing universal about it. Note that the engine
**generalized** rather than copied: the `GuestRegion` class is the port's
own algorithm with the failure report turned into a callback. So this
isn't an `#include` in place of a definition, it's a small adaptation of
callers.

### Wave 3 — kernel objects and the handle table

**Replaces**: `KEvent`/`KSemaphore`/`KThread`/`KCrit`, `g_handles`, the ID
counter, the critical-section map, with `runtime/guest_sync`.
**Proves**: that the subtlest Win32 semantics — the kind that cost a real
deadlock to debug — is genuinely shared.
**Validates**: **mandatorily under real load**, not at startup. A
synchronization regression is starvation or a deadlock, and a green
startup proves nothing.

A good *positive* witness: by the end of the pass, some threads should be
**blocked on kernel objects**. A thread can only block if its handle
lookup succeeded — when a handle is unknown, the code returns immediately.
Blocked threads therefore prove the table resolves correctly, where "no
error" proves nothing.

The port keeps its instrumentation: it registers it on the observer
(`wx86_sync_set_observer`), a single point, and dispatches internally. The
engine reports, it never asks for an opinion.

!!! warning "Check first that the game actually uses these objects"
    On the second port, this wave was done and **the repository's oracle
    doesn't see it** — not because the oracle is flawed, but because the
    game never takes this path: its renderer imports **no**
    synchronization function, and its main executable only one
    (`WaitForSingleObject`).

    Proven in three steps, not assumed: a fault injected into
    `wx86_handle_find` changes nothing about the verdict; probes on
    `handle_add`/`handle_find`/`crit_for` never fire; and their presence
    in the binary is verified with `strings`, so this silence isn't that
    of a missing probe.

    Practical consequence: for such a port, these structures are
    **inherited dead weight**, not a live need. The migration is still
    good (it removes a duplication and aligns with code validated
    elsewhere), but it must be reported as *not validated by this port*
    — and the real load requested above should come from a port that
    genuinely exercises synchronization. Don't dress up an unexercised
    path as a green validation.

### Wave 4 — presentation and graphics glue

**Replaces**: the local `scale+flip` with `platform/present_scale`
(`scale_blit` + `fit_rect`), the local counting backend with
`render/render_null.cpp`, its own vertex and batch types with those from
`render/render.h`.
**Proves**: that the most performance-sensitive layer holds with no cost.
**Validates**: pixel identity **on hardware**, and a measured cost.

!!! danger "This wave's trap"
    The presentation file is often compiled **only on console** — neither
    on desktop nor under emulation. The usual pixel-identity oracle can't
    exercise it, and that's exactly why this code never gets touched.

    The workaround used: a self-check that embeds a faithful transcript
    of the original loop and compares outputs **byte for byte**, runnable
    on desktop (`tools/present_scale_selftest.cpp`). Then prove the oracle
    **cuts**, by injecting a single-bit fault.

    Reference measurement on console: identical image over 400 frames,
    cost **+0.12%** — within the noise.

The second port has **no** direct replacement here: its counting backend
is an empty stub (35 lines) in its own namespace, with its own types,
where the engine's actually counts (82 lines). The migration is a type
adaptation, not a removal. Its resizer, on the other hand, is the
**superset** the engine's `fit_rect` was drawn from: its proportional
framing is already what the generic function implements.

#### What wave 4 actually delivered on the second port (2026-09-12)

Presentation migrated; graphics glue did not. Both halves are equally
instructive.

**Three engine flaws, all fixed upstream, none visible from the first
consumer alone:**

1. **`present_scale` only knew two pixel formats** — and its header
   claimed these were "the only two DIB formats ports present today." The
   second port presents **16-bit 5-5-5**, on its **default** path. A list
   written from a single caller describes that caller, not the domain.
2. **`platform/vita_audio.cpp` called three WEAK symbols from the first
   port** (`d2vita_progress_c`, `d2vita_pin_self_c`,
   `d2vita_core_register_c`) for three services `platform/vita_host.h`
   now owns. A port whose symbols don't carry that prefix got a mute log
   and an unpinned thread, **with no link error**. *The fix only covered
   audio: **six other units** carried the same flaw — see pitfall 9.*
3. **`VitaSink::open(freq, …)` received the frequency and ignored it** in
   favor of a hardcoded 22050 Hz — the first game's own number. A stream
   at another frequency would have played at the wrong pitch, silently.

**And two proofs that had drifted from their subject:**

4. **`vita_kb.h` cited `tools/tests/kb_test.cpp`** as justification for
   being provable on the host, and that file existed **only for the first
   consumer**. This is exactly wave 3's flaw, repeated: the object moves,
   the proof stays behind. The oracle now lives here (1,297 checks).
5. **`present_scale_selftest.cpp` had no command line written for it**,
   and its sub-rectangle case was only checked by **bound invariants** —
   never compared against a reference output, even though it's the second
   port's default framing. `tools/selftest.sh` replays both, 320 cases.

!!! warning "Migrating a DEAD path validates nothing"
    The second port presents **through the GPU**: `do_scale_and_flip` and
    its presentation thread have, on its side, **no caller**. Wiring
    `scale_blit` there removes a real duplication and proves strictly
    nothing — no run exercises it.

    Checking WHO CALLS before migrating changes what you're entitled to
    claim, and sometimes what you choose to migrate: it's the 1:1
    conversion of the GPU path (destination = source, a degenerate case
    of `scale_blit`) that actually brings the engine into this game's
    picture.

!!! tip "The sub-rectangle is where the two ports diverge"
    The first consumer only calls `scale_blit` in **fullscreen**
    (`DstRect{0,0,SCR_W,SCR_H}`). The second calls it with a
    **sub-rectangle** (letterboxing) and at 1:1. Three uses, one body —
    and it's by writing the reference for the other two that you discover
    what the first never exercised.

**Graphics glue, on the other hand, stays with the port, and with a
proof:** the engine has **no vocabulary for 2D presentation**, while the
second port has two pipelines on **a single GL context** — triangles, and
a path that presents a host image as a fullscreen quad with its text runs
and overlays. Wiring the engine in for the 3D path alone would split
ownership of the context between two interfaces. A concrete danger, not
just difficulty.

And the finding that puts the glue back in its place: **`render/render.h`
currently has NO consumer** — verified by `grep` in both ports. It was
inferred from two existing backends and adopted by zero. Under this
document's guiding principle, that's speculative genericity until a real
port uses it; the missing 2D presentation is what would pull it out of
that state.

### Wave 5 — extension points to be designed

Everything else. And it must be said plainly: **the mechanical vein is
running out.**

On the first port, after the previous waves, applying the two-condition
criterion left only **8** functions movable as-is — and the promising
hypothesis ("if we also moved the two obvious helpers, a lot more would
unblock") was tested and **refuted**: zero more.

The remaining functions aren't pending moves. They are:

- **transfers of ownership of entire states** (file table, memory layout,
  wait profiling), each its own wave like wave 2;
- functions that **genuinely diverge** between the two ports, so each is
  an extension point to be designed — data or a callback supplied by the
  consumer, never two implementations inside the engine.

This is a design phase, not a moving job, and it's done one function at a
time.

## The pitfalls

Each comes from a real incident, which is why they're worth more than an
abstract recommendation.

### 1. Move ownership, never just the type

**Three incidents in two days, all the same.** The *type* moves to the
engine, the *table* stays with the port, and the engine gets a second one,
empty: the critical-section map, the handle table, the ID counter. The
shim then operates on one table, the shared core on the other.
**Invisible at compile time, payable as a deadlock.**

One case was even sneakier: the ID counter isn't only used by waitable
objects — a process snapshot isn't waitable but still consumes a number.
Without a shared counter, the two series overlap and two distinct objects
carry the same number.

**The check**: after every wave, explicitly verify no twin remains on the
port side — zero local type definition, zero local table, zero local
counter. That's a `grep` you run and write into the report, not an
intention.

### 2. One key, one registration

`register_shim` overwrites silently: the last one wins. That's the
feature that makes a gradual migration possible (§ wave 1), and it's the
classic trap when moving blocks around. A duplicate of this kind has
already broken a port's network connection with nothing flagging it.

**And when a duplicate's two bodies differ**: keep the **winner**, i.e.
the one that was already effective — even if it looks worse. It's the
only one execution and tests have ever validated; the other never ran.
On one real duplicate-removal pass, two out of five keys had different
bodies and the **dead** body looked better (it carried a null-pointer
guard absent from the live one). Substituting it along the way would turn
a behavior-neutral removal into an unvalidated behavior change disguised
as cleanup. If the dead body really is better: note the flaw on the spot,
and handle the improvement in a separate commit that announces itself as
such.

`tools/shim_seq.py` / `.sh` exist for this — and they have their own blind
spot: an **undeclared unit** silently disappears from the count. A count
that drops by exactly the size of your wave is more likely the tool than
your code.

### 3. "The game never calls it" isn't proof of uselessness

This pattern has come up **twice**. Network functions left as stubs on
the grounds that the client never listens turned out to be perfectly
implementable, and now are for real. A stub justified by the *first*
game's needs becomes a hole for the second.

Before copying a stub into the engine, ask whether a real generic
implementation actually costs more.

### 4. An oracle must observe the OUTPUT of the code under test

A project's pixel-identity oracle can very well fingerprint the
**input** of the function being rewritten — in which case it will sign
off on any bug, black screen included. That was the obvious reflex during
wave 4, and it was nearly followed.

Three checks before trusting a green verdict: locate the sampling point
relative to the code under test; **prove the oracle cuts with the same
binary** (inject a fault, verify it's caught, remove it); publish the
number of comparisons made.

### 5. Silence proves nothing

**Three negative conclusions drawn from silence turned out to be wrong in
two days.** A message missing from a log can mean "the code doesn't arm"…
or "the log starts too late," or "the bench redirects output to another
file."

Before concluding something doesn't happen: produce a **positive witness**
in the file you're querying. If the witness doesn't show up either, it's
the observation that's at fault, not the code.

### 6. Enough measurement passes

On wave 4, at **4 passes per leg** the measurement gave **-1.42%** with an
apparently significant gap, and a regression was about to be reported. The
next two passes **flipped the sign**: the final result is **+0.12%**.

Stopping at 4 would have published a nonexistent regression **and**
blocked a free improvement. Group the passes, or take a slope — never
conclude on a gap smaller than its own dispersion.

### 7. Prove the reference BEFORE using it

A regression was diagnosed and then bisected across four commits before a
reproducibility check showed that **the reference itself wasn't
reproducible**: the reused write directory held a five-day-old profile
file, absent from later runs.

There was no regression. **Replay the reference twice and compare
fingerprints before comparing anything else** — it's the cheapest check
in the set, and the one whose absence costs the most.

### 8. A negative control can target a fault the game never sees

To prove an oracle cuts, a fault was injected into an allocator: shift
every block by 16 bytes. **Verdict and counters unchanged.** The oracle
wasn't blind — the fault was genuinely benign at that scale (the frees
failed silently, which leaks without breaking anything).

A negative control placed there would have "proven" the oracle works
while demonstrating nothing. If your fault doesn't move anything, **look
for a different one** before concluding anything, either way.

### 9. A WEAK link named after one consumer is a SILENT flaw

This is pitfall 1 in its most discreet form, and it survived its own
fix.

Fixing `platform/vita_audio.cpp` (§ wave 4, point 2) settled **one** unit.
An `nm` sweep over the engine's Vita objects showed **seven**:

| unit | weak symbols prefixed for the first port |
|---|---|
| `runtime/bridge.cpp` | `d2vita_progress_c` |
| `runtime/cpu_box86.cpp` | `d2vita_progress_c` |
| `runtime/sched_cooperative.cpp` | `d2vita_progress_c` |
| `runtime/sched_native.cpp` | `d2vita_progress_c`, `d2vita_core_mask_c`, `d2vita_pin_self_c`, `d2vita_core_register_c` |
| `dynarec86/shim/vita/mman_vita.c` | `d2vita_progress_c` |
| `third_party/box86-dynarec/dynarec/dynarec.c` | `d2vita_progress_c` |
| ~~`platform/vita_audio.cpp`~~ | fixed in wave 4 |

`sched_native.cpp` was the worst case: it's the **default** scheduler, it
pins and registers worker threads, and a comment there named
`vita_present.cpp` — a file belonging to the first consumer.

!!! danger "What 'it compiles' doesn't prove"
    An unresolved weak link resolves to `NULL`. No error, no warning,
    **nothing**. All four targets stay green while the log stays mute and
    threads stay unpinned. The only possible proof is at the **symbol
    table**: a `w` in `nm`'s output on an engine object is a flaw, not a
    detail.

**The hard part: portability off console.** Three of these units also
compile for the qemu/desktop harness, where there is no console. That's
what made the weak link the original choice. The solution is more
tedious and strictly better: outside `__vita__`, `platform/vita_host.cpp`
defines the log as a **no-op**. The generic body therefore links
**STRONG** everywhere, with no null check, and off-console behavior is
identical to what the unresolved weak reference produced — silence. A
port has **nothing** to provide off console: the no-op doesn't even read
`wx86_vita_progress_path`.

The core services (`wx86_vita_core_mask`, `wx86_vita_pin_self`,
`wx86_vita_core_register`) stay **console-only**: all their call sites
already live under `#ifdef __vita__`. Only make portable what needs to
be.

!!! tip "The alternative considered, and why it was dropped"
    Renaming the weak symbol to `wx86_vita_progress_c` while keeping it
    weak would have made the wrong prefix disappear **without** removing
    the failure mode: a port providing nothing would have stayed mute
    without knowing it. A rename isn't a fix.

!!! warning "The second port didn't prove the flaw, it masked it"
    The second port **defined** all four `d2vita_*_c` wrappers: it
    descends from the first by copy and inherited the names. So its log
    was **not** mute, and its threads **were** pinned. The flaw was real
    and yet to come — it was waiting for the third port, the one that
    wouldn't have copied the prefix. A genericity flaw can stay invisible
    with *two* consumers when the second was born from the first.

## What isn't in the engine yet

An honest guide about its own gaps is usable; a guide that overpromises
costs a day to whoever follows it.

| missing | where it lives today | why |
|---|---|---|
| ~~DirectSound emulation~~ | **in the engine** (`runtime/ds_emul`) | shipped since. A port whose game goes through a different audio library won't consume it, but the **sink** (null / WAV / console) and the host clock are shared. |
| the console's GPU backend | port side | deliberately scoped to the states the first game emits. The generic glue exists (`render/render.h`), the backend that implements it doesn't. |
| pad reading and the cursor | port side | identified as "console-specific" but not yet moved. The defaults (orbit, sensitivity, dead zone) are **identical across two unrelated games**: they're about stick ergonomics, not the game. Work in progress. |
| ~115 `KERNEL32` functions | port side | files and paths, memory layout, wait timing, clock — see wave 5. |
| scheduling | **both** exist | the engine provides cooperative **and** native; a port chooses. The first port now only targets native, the second still starts cooperative. This isn't debt, it's a per-consumer choice. |

### An asymmetry worth knowing before starting

The engine provides **2** registrations for `DDRAW.dll` — enough to
answer "no DirectDraw" cleanly. The second port registers **128**: a full
COM emulation, with the vtables for `IDirectDraw` and its interfaces.

This isn't a simple coverage gap. It's the engine **shaped by the first
consumer's needs**: the first game doesn't draw through DirectDraw, so two
functions were enough, so the question never came up. Nobody had seen it,
because measuring the boundary counts what the engine *owns*, never what
a second consumer *would need* it to own.

This is precisely the flaw a generic engine has to avoid, and exactly the
kind of thing only a real migration reveals. Take it as good news: found
now, it costs one wave; found at the third port, it costs a redesign.

Likewise, the second port's audio layer is written in **C** where the
engine's is C++, yet with the same shape (the null / WAV / console trio,
the same `write` and `close`). Reusing one for the other is an
adaptation, not an inclusion.

## Operational summary

1. Compare the two codebases function by function, on the normalized
   **body** — never the name.
2. Only move first what satisfies **both** conditions: identical body
   **and** no external dependency.
3. Register the engine **before** its own shims, so its own win, and
   migrate domain by domain.
4. For anything holding state: transfer **ownership**, verify no twin
   remains.
5. Validate each wave to the height of its risk class — and for
   synchronization, **under real load**, never at startup.
6. Treat what diverges as an **extension point to be designed**, not as a
   move running late.
