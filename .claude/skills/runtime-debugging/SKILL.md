---
name: runtime-debugging
description: Use when debugging a winx86-based port — a crash/hang/wrong behaviour seen on hardware or in an emulator, a freeze or fast-forward, an intermittent fault, or any "it works on qemu but not on device" gap. Covers the desktop-first reproduction rule, the 3-level validation ladder, watchdog attribution, resolving a guest EIP to a module+offset, and the byte-identity gate.
---

# Runtime debugging

A port runs **genuine third-party binaries** on an x86→ARMv7 dynarec. There is
no source for the guest code. Every bug is diagnosed by **observing the guest
from the outside** — shims, hooks and counters — never by editing the guest.

## The one rule: reproduce on the desktop first

A hardware-only bug is a slow bug: minutes per iteration, no debugger. Almost
every device bug reproduces under **qemu-arm with the real clock forced on**,
which puts the desktop binary on the same timing regime the device uses.
Iteration typically drops from ~10 min to ~2 min.

Drive the guest **deterministically** (a scripted input sequence keyed on frame
numbers, not on wall-clock). A compressed script that reaches the interesting
state in ~2 minutes reproduces most load-path bugs and is the difference between
bisecting and guessing.

## The 3-level validation ladder (never skip a level)

1. **qemu-arm** — determinism and counters are exact; wall-clock is meaningless.
2. **Emulator on the target platform** (Vita3K for Vita) — catches packaging,
   module-loading and import errors that qemu cannot see.
3. **Hardware** — the only place performance and real-clock timing are true.

Never conclude a perf or timing claim from qemu alone. Never sign off a
milestone on qemu + emulator without a hardware pass. See `hardware-ab-bench`
for what a hardware measurement actually requires.

## The byte-identity gate

The **virtual-clock** path must stay byte-for-byte deterministic: same switch
count, same counters at the same points, identical frame output. Run it after
every runtime change.

If a change perturbs the virtual smoke, it changed **emulation semantics** — not
just speed. Stop and understand why before shipping, even if the change looks
like a pure optimisation. This gate is the reason the engine can be refactored
at all.

## Changing the shared `Cpu` interface without breaking the oracle

If you change the `Cpu` interface, its semantics must not move by one bit, or
the byte-identity gate above stops meaning anything.

**The pattern that makes such a change safe: give the new method a default
implementation in `cpu.h` that IS the old code, effect for effect.** Then
override it only in the backend you are optimising. A backend that does not
override stays correct without being touched, and the gate is untouched by
construction.

Two details that had to be checked before one such rewrite, and that generalise:

- **A hoisted call with a side effect must keep being called exactly once.**
  Moving a consuming call (`take_redirect()`) out of a conditional changed
  *where* the final EIP was arbitrated, not *how many times* the redirect was
  consumed — that had to be established, not assumed.
- **Check the locals you are eliminating are not read later.** One looked live;
  it was only mentioned in comments after the epilogue.

## Watchdog attribution (finding WHERE time or faults go)

A port should log a periodic line carrying cumulative counters — frames, guest
reads, current EIP, scheduler switches, I/O, JIT translations, cache syncs,
failures. **Deltas between lines localize a freeze** far faster than a debugger:

- I/O climbing → storage reads. JIT/sync climbing → translation.
- A cache-sync counter climbing was a real freeze culprit once (whole-block
  domain sync per translated block, ~27 ms each).
- A failure counter above zero → JIT map exhaustion; blocks get retranslated on
  every visit, which reads as a mysterious slowdown.
- EIP frozen across lines → hung; resolve it (below) to see where.
- Frames frozen but switches climbing → spinning in the scheduler, not rendering.

## Resolving a guest EIP / crash address

A port that packs modules contiguously can reproduce its own map with a short
script over each module's `SizeOfImage`, turning a raw `eip=` into
`Module+0x…`.

**Watch the ImageBase.** A DLL's preferred base is often nothing like what the
section table's load address suggests, and getting this wrong produces a
plausible, wrong attribution. Then
`objdump -D -b binary -m i386 --adjust-vma=<imagebase>` around the offset reads
the actual instruction.

## Hooking the guest to see inside

To dump state at a chosen guest address, install a native hook — see
`guest-code-hooks`. Env-gate every probe and cap its output. A config file read
at startup on the device lets you flip probes without recompiling, which on a
platform with a ~10-minute build-deploy cycle is the difference between one
evening and three.

## Gotchas that cost a round each

- **Fast-forward** = the virtual clock advancing at emulation speed. The fix is
  real-clock mode; the tick APIs must then read real time.
- **A vendored library's own logging may be compiled out** — use a bare `printf`
  in vendored translation units when adding a probe, not the library's macro.
- **qemu stdout is fully buffered under redirection** — a killed run loses its
  log. Set line buffering in `main` and keep it.
- **Some progress markers only exist on one platform.** Know which marker means
  "reached the interesting state" on *each* level of the ladder, or a scripted
  battery silently measures the wrong thing.
- **A missing data file can read as an infinite retry loop**, not an error — a
  hang with a low read count, not a crash. Check inputs exist before debugging
  the engine.

## Red flags

- "It only fails on hardware" → you have not tried the desktop real-clock
  reproduction yet.
- "The counters look fine so it's fine" → check the byte-identity gate too.
- Concluding a perf number from qemu → invalid; measure on device.
