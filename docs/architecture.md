# Architecture

## Overview

```
Windows executable (.exe/.dll)
        │
   PE32 loader (src/runtime/pe_image.h, .cpp)
        │
   Cpu interface (src/runtime/cpu.h)
        │
        ├── CpuBox86 (src/runtime/cpu_box86.cpp) — ARMv7 backend, dynarec
        │
   Bridge (src/runtime/bridge.h, .cpp) — extension point
        │
   Guest thread scheduler
        ├── sched_cooperative.* — cooperative (one active thread at a time)
        └── sched_native.* + gil.* — native threads + global lock (GIL)
```

## The PE32 loader

`PeImage` (`src/runtime/pe_image.*`) maps a 32-bit Windows executable or DLL
into memory: parses the PE/COFF headers, resolves the import table, applies
relocations if the binary isn't loaded at its preferred address. Generic
for any valid PE32 — no knowledge of the binary's content.

## The Cpu interface

`Cpu` (`src/runtime/cpu.h`) is the central abstraction: a set of x86
registers, addressable memory, and the execution/translation primitives.
The only backend provided today is `CpuBox86` (ARMv7 via the dynarec), but
the interface itself doesn't presuppose any particular backend.

Two primitives of the `Cpu` interface are **the** engine's extension point
— detailed in [Extension point](extension.md):

- `Cpu::set_alternate(from_va, to_va)` — redirects execution from one guest
  address to another.
- `Cpu::discard_code(va, size)` / `invalidate_code(va, size)` — the
  self-modifying-code (SMC) contract: the distinction between "mark dirty
  for re-verification" and "unconditionally destroy the translated blocks"
  at that address, needed by any port that patches guest code in memory.

## The dynarec (Box86)

`third_party/box86-dynarec/` is a **verbatim** extraction of the ARMv7 core
of [Box86](https://github.com/ptitSeb/box86) (ptitSeb, MIT), plus a set of
local patches applied on top and mechanically regenerated — never
hand-edited:

```bash
tools/regen_box86_patch.sh   # captures the current local delta -> tools/box86_local_patches.diff
tools/extract_box86.sh       # re-extracts a clean Box86 + reapplies the patch, refuses if the result diverges from the existing tree
```

This machinery exists for a precise reason: being able to resync against a
newer upstream Box86 without silently losing the local patches (fastmmu,
memintrin, emitprof, signtag, the scheduler's preemption hook, the SMC
write-protection switch). The patches are designed to be upstreamable to
Box86 itself.

`src/dynarec86/` is the integration layer between Box86 and the rest of
winx86 (the `Cpu` interface, native-intrinsic recognition at translation
time — see below).

## Recognizing a native intrinsic at translation time

`src/dynarec86/dyn86_intrin.h` provides a generic mechanism for
recognizing, **at translation time** (not via a runtime trap), that an x86
sequence matches a known function, and replacing it with a direct native
call. This is faster than a classic hook (`set_alternate` + trap), but
requires the replacement to be true equivalent native code, not just a
shortcut. A port uses it for its hottest functions (see how d2vita uses it
for its DCC sprite decoder, on the d2vita side).

## The JIT pool on PS Vita — 16 MiB segments

ARM code translated by the dynarec is stored in executable memory
(`sceKernelAllocMemBlockForVM`, kernel VM domain — distinct from the usual
RW blocks from `sceKernelAllocMemBlock`).
`src/dynarec86/shim/vita/mman_vita.c` implements this "JIT pool" on the
Vita port side.

Discovered on 2026-09-13 (never tested before on this project): the Vita
kernel caps **every VM block at 16 MiB (0x1000000)**, refused beyond that
with `SCE_KERNEL_ERROR_MEMBLOCK_OVERFLOW` (sce=0x80024B0B) — verified by
requesting 17 MiB then 29 MiB in one go, both refused. This is a kernel
limit, not a project sizing choice.

The pool therefore grows by **segments** of 16 MiB opened lazily
(`jitpool_grow()`), rather than as a single block that gets enlarged:
`WX86_JITPOOL_SEGS` (alias `D2_JITPOOL_SEGS`) sets the targeted number of
segments (2 by default → 32 MiB), `WX86_JITPOOL_MB`/`D2_JITPOOL_MB` the
size of each (capped at 16 MiB per segment). A failed open arms a single
global flag (`g_jitseg_refused`, set only after a REAL kernel refusal,
never preemptively) that stops any further attempt for the rest of the
session — no hammering the kernel on every subsequent `PROT_EXEC` block.
This flag only blocks **growth**: segments already open before the failure
stay fully usable, each being a separate entry in the segment table.

Splitting into segments costs nothing in performance: linking between
translated blocks is **always** an indirect branch to an absolute address
(`CreateJmpNext` in `box86-dynarec`: `LDR_literal`+`BX`; call return via
`BXcond`/`BX` after a stack check or jump table), never a range-limited
direct ARM `B`/`BL` — so the distance between segments has no addressing
impact whatsoever.

## Schedulers

Two strategies for guest threads (`CreateThread`, etc.):

- **Cooperative** (`sched_cooperative.*`) — only one game thread runs at a
  time, explicit switching. Simpler, more deterministic, slower under
  heavy contention.
- **Native** (`sched_native.*` + `gil.*`) — real OS threads, serialized by
  a global lock (GIL, Python-style) to protect the dynarec, which isn't
  thread-safe by construction. Allows real parallelism on native code
  sections that deliberately release the GIL.

The choice is made at the port level, not by winx86.

## The Bridge — Win32 import bridge

`Bridge` (`src/runtime/bridge.h`, `.cpp`) is the generic Win32
shim-registration mechanism: `register_shim(dll, name, shim)` associates a
DLL+symbol function name with a native implementation, `shim_trap(dll,
name)` gets the trap address to hand to `set_alternate`.

On top of this mechanism, `src/runtime/win32_shims_*.cpp` provides a
library of shims **proven, body by body**, to carry no game-specific
behavior — the exact list is [generated from the sources](shims.md). Each
one is installed by an explicit call from the port
(`win32_shims_<group>_install(br)`): the engine registers none of its own
initiative, and since the last registration wins, a port can always
substitute its own.

## Generic services exposed to the port

When a feature is generic but the policy isn't, the engine exposes a
neutral extension point rather than guessing:

- **`guest_scratch.{h,cpp}`** — the guest-memory scratch allocator, needed
  by any Win32 API that returns a *pointer* the caller will read. The
  engine allocates, the port grants it a range (the memory layout itself
  has nothing universal about it).
- **`win32_shims_wsock32.h`** — the socket handle table, the single
  Winsock last-error store, a passive observer of the socket layer (the
  engine reports, it never asks for an opinion), and a generic connection
  route.
- **`net_nonblock.{h,cpp}`**, **`poll_gil.{h,cpp}`** — name resolution and
  waiting compatible with the global lock.

See [Extension point](extension.md).
