# winx86

**Generic x86 → ARMv7 execution engine for porting Windows games (PE32) to PS Vita.**

winx86 loads a Windows x86 (32-bit) executable or DLL, translates its code
on the fly to ARMv7 (dynarec derived from
[Box86](https://github.com/ptitSeb/box86)), provides a library of
[Win32 shims proven to be generic](shims.md), and offers the hook points
for a port to plug in its own and its own native optimizations — **without
winx86 ever needing to know the game**.

This repository is **private** for now.

## Who this is for

Anyone porting a Windows x86 (PE32) game to PS Vita who doesn't want to
start from zero on "run x86 code on ARM." winx86 loads, translates,
schedules, serves the Win32 calls proven to depend on no game, and offers
extension points for the rest. What belongs to the port itself: the shims
*its* binary expects that the engine doesn't provide, the functions worth
porting natively for performance, and the game's own install/configuration.

## Proof by example: d2vita

[d2vita](https://gitlab.com/claude5564407/d2-vita) is the port that gave
birth to winx86: a private, personal port of *Diablo II: Lord of
Destruction*. winx86 was extracted from it on 2026-09-09 by isolating
whatever had **zero** knowledge of the game.

The boundary has moved since, in the right direction: what started as "zero
shims in the engine" became a library of more than two hundred
registrations, DLL group by DLL group, each extracted after reading its
**body**, never its API name. What stays in d2vita stays there with a
written technical reason — not out of caution. d2vita consumes winx86 as a
git submodule.

## Architecture at a glance

```
Windows executable (.exe/.dll)
        │
   PE32 loader (src/runtime/pe_image.*)
        │
   Cpu interface (src/runtime/cpu.h) ──── CpuBox86 backend (ARMv7, dynarec)
        │
   Bridge (src/runtime/bridge.*) ─── extension point: Win32 shims + native hooks
        │
   Guest thread scheduler (cooperative or native + GIL)
```

## "Hybrid" architecture

winx86 is a **self-contained, independently buildable static library**
(`libwinx86.a`), not a fork to copy-paste. A port adds it as a git
submodule and links its own boot executable against this library.
Concretely, that means:

- winx86 evolves in its own git history, its own CI, its own verification
  suite (`tools/shim_seq.py`, the qemu-arm oracle).
- A port can pin an exact winx86 version (the submodule commit) and
  advance it at its own pace.
- A generic fix found by one port (e.g. a dynarec bug) gets upstreamed
  into winx86 and benefits every port that consumes it — the method has
  already paid off once between d2vita and its private sibling project
  *carn-vita* (a port of Carnivores 2, same underlying engine, forked from
  d2vita before this repository existed): three generic fixes found on the
  carn-vita side were upstreamed into d2vita in early September 2026,
  before winx86 even existed as a separate repository.

## Getting started

See [Quick start](demarrage.md) to build winx86 on its own, and
[Extension point](extension.md) to plug in your own game.
