---
name: vita3k-testing
description: Use when validating a Vita homebrew VPK locally with Vita3K — booting, capturing screenshots, reading Vita3K logs, comparing against a reference run. Handles the headless / no-hardware case. Covers the AppRun.wrapped no-op trap, the Xvfb-has-no-OpenGL trap, and the console-mode-does-not-skip-Qt trap.
---

# vita3k-testing

## Overview

Vita3K is the smoke test between "it compiles" and "it works on hardware" — the
second rung of the 3-level ladder in `runtime-debugging`. Getting it to run a VPK
without user interaction takes a handful of non-obvious steps, each of which
costs a round of confusion the first time.

## When to use

- Confirming a new subsystem boots and reaches its expected diagnostic output
- Comparing behaviour cross-arch (x86 desktop vs ARM) on checksums or seeds
- Suspecting a platform-specific bug (endianness, alignment, newlib gap, a
  missing `SceXxx` import)
- Reviewing what the log actually says after a boot that "seemed to work"

## Not for

- Real hardware testing — that needs USB/VitaShell and a human
- Performance claims — an emulator is 5–20× the real thing on memory-bound code
- Reversing guest code to place a hook — see `guest-code-hooks`

## The moving parts

1. **A real X display, not Xvfb.** Xvfb does not expose OpenGL ≥ 4.4 — Vita3K
   logs `Failed to create OpenGL context (need at least ver 4.4)` and hangs on
   the Qt frontend. Use a real X server (llvmpipe there gives GL 4.5, enough).
2. **`AppRun.wrapped` is a no-op unless `$APPIMAGE` is set.** Invoked directly it
   exits 0 without launching anything. Either export `APPIMAGE=fake`, or call the
   real binary at `squashfs-root/usr/bin/Vita3K`.
3. **`--console 1` does not skip Qt** — the app selector still opens. Use
   `-r <TITLE_ID>` and let it auto-launch.
4. **VPK install = zip extract.** `7z x -o ux0/app/<TITLE_ID>/ path/to/foo.vpk`.
   There is no installer to run.
5. **`initial-setup: false` *and* `show-welcome: false`** in
   `~/.config/Vita3K/config.yml` — they are two different dialogs.

## Reading the log

`~/.cache/Vita3K/vita3k.log` fills only during a **graceful** shutdown. `pkill -9`
truncates it. Prefer SIGTERM with a short wait, or let the app exit itself.

Sequence expected in a healthy trace-level run:

1. `[main]: Vita3K v… `
2. `[create_gl_context]: Created OpenGL 4.6 context` — anything lower means the
   display does not cut it
3. `[create]: GL_VERSION = …`
4. `[load_app_impl]: Title: <expected>, Serial: <TITLE_ID>`
5. `[load_module]: Loading module "app0:eboot.bin"` + `Loaded module segment`
6. `[operator()]: Module … loaded`
7. system modules (`os0:kd/…`)
8. `[export_sceIoOpen]: Opening file: …` — where the app's own I/O starts

**Usually harmless**: ALSA/PipeWire/PulseAudio errors (no audio device in a
sandbox); a `stat_file: Missing file` for data you have not installed.

**Stop and dig**: `Failed to create OpenGL context`; `Failed to link module` or
`Unresolved import: SceXxx_yyy` (the eboot references a symbol Vita3K cannot
resolve); `EXC_ACCESS_VIOLATION` in the `app0:eboot.bin` range (ARM code
segfaulted — capture the last diagnostic output before the crash and diff it
against the desktop run).

## Screenshots as evidence

`DISPLAY=:0 import -window root /tmp/shot.png` grabs the whole desktop; the
Vita3K window sits somewhere in it. When confirming a run, **the screenshot is
the evidence**, not a nice-to-have — a log line saying "started" and a window
that never painted look identical otherwise.

## Cross-arch parity

If a pipeline emits both a **decoded** and a **raw** checksum, the pair localises
a divergence immediately:

- **raw differs** ⇒ the I/O layer returned different bytes on the two
  architectures. Trust nothing downstream until that is resolved.
- **decoded differs, raw matches** ⇒ the parser has an arch-specific bug
  (endianness, unsigned overflow, alignment).
- **both match** ⇒ the pipeline is deterministic across architectures; any visual
  difference is further downstream.

Before flagging a divergence, **verify you are comparing the same input**. A
probe that walks a candidate list and takes the first hit will happily compare
two different inputs if the list order differs on one side — and you cry wolf.

## Red flags

- "Ran it, log is empty, exit 0" ⇒ `AppRun.wrapped` without `$APPIMAGE`.
- "The screenshot is all black and tiny" ⇒ it exited before creating a window;
  check the log for the GL error.
- "The welcome dialog keeps coming back" ⇒ you set one of the two flags.
- "The log stops mid-load with no crash message" ⇒ you `pkill -9`ed it.
