---
name: vita-perf-budget
description: Use when making performance or memory decisions on PS Vita — deciding what fits in RAM/VRAM, catching OOMs before they happen, understanding why the Vita is slower than benchmarks suggest, choosing texture formats and allocation strategies at 960×544.
---

# vita-perf-budget

## Overview

The Vita is not a small PS3 — it is a mid-2011 mobile SoC. The CPU is a quad
Cortex-A9 at 444 MHz, memory is split across banks with different speeds, and
the GPU has a small dedicated pool. Nearly every fps regression traces back to
violating one of the constraints below. Treat this as a **budget sheet**, not
"optimization tips".

## The budget sheet

### CPU

| Item | Value | Notes |
|---|---|---|
| Cores | 4× Cortex-A9 | one is typically reserved by the OS; **plan for 3 usable** |
| Clock | 444 MHz default / 500 MHz peak | set it explicitly; some firmware clocks lower |
| L1 / L2 | 32 KB I + 32 KB D / 512 KB shared | cache-friendly loops matter more than raw MHz |
| NEON | yes (128-bit SIMD) | worth it for palette conversion, blits, mixdown |
| VFPv3 | yes, but weak | prefer integer math in hot loops |

Measured on a real port: the workload was **bound by computation, not memory**
(elasticity 0,95 against clock) — a dependent chain, not a bandwidth wall. Do
not assume "it's memory" without the scaling test.

### RAM

| Region | Size | Available | Notes |
|---|---|---|---|
| System (LPDDR2) | 512 MB | ~256 MB in user mode | ~200 MB/s realistic |
| PhyCont | +109 MB | via the PHYCONT memblock type | contiguous — required for GPU-mapped buffers |
| Extended mode | +256 MB | only if the app requests it | adds allocation latency |
| Stack | 1 MB per thread by default | grow at thread creation | deep recursion crashes hard |

**Assume ~256 MB usable**, not 512.

### VRAM

CDRAM is 128 MB, fast, dedicated. A 960×544 RGBA8 framebuffer is ~2 MB; depth
another ~2 MB; double-buffered, budget ~10 MB just for the swap chain.

### Storage / I/O

| Device | Real read speed | Notes |
|---|---|---|
| Memory card | 15–20 MB/s | the bottleneck |
| SD2Vita adapters | varies wildly | do not generalise from one card |
| Open latency | ~5–20 ms per file | batch reads; never stat a thousand files at boot |

**Cold-start budget**: ~2 s of I/O before the user sees something, or they
assume it hung. Show a loader.

!!! warning "A polluted writable directory can dominate the frame time"
    A path-resolution helper that is O(n) over the contents of the write
    directory turns a directory full of dumps into a per-call cost. One
    "mysterious console slowdown" was entirely this, and nothing to do with the
    code under investigation.

### GPU (SGX543MP4+)

| Metric | Realistic |
|---|---|
| Fill rate | ~300–500 Mpix/s in practice at 30 FPS |
| Triangles | ~30M tris/s peak — rarely the limit for 2D work |
| Texture units | 4 samplers per shader stage |
| Draw calls | ~1000/frame fine; ~5000/frame kills it — **batch** |

!!! danger "Never filter an index texture"
    If you upload palette-indexed pixels as a texture and do the palette lookup
    in the shader, the index texture must use **nearest** sampling. Bilinear
    filtering interpolates *indices*, which produces noise that looks like a
    decode bug — with every counter reading zero and nothing else wrong.

## Practical rules

1. **Stream large archives, do not preload.** Use a read-through LRU cache
   rather than holding a decompressed archive in RAM.
2. **Palette-indexed data stays palette-indexed on the GPU.** Store decoded
   frames as `R8` (1 byte/pixel) and do the lookup in the shader — 4× the VRAM
   of RGBA8 saved. See the filtering warning above.
3. **One atlas per scene's worth of sprites.** Per-frame uploads are a draw-call
   disaster; pack at load time.
4. **Preallocate pools.** Per-frame `malloc`/`new` tanks frame time and
   fragments the heap fast.
5. **Two worker threads, not five.** More just fights the scheduler — and on a
   GIL-serialised runtime, a second worker can be worth exactly zero if both
   land on the same core. Pin deliberately.
6. **Cap logic to a fixed timestep** and decouple presentation. Chasing 60 FPS
   for a 25 Hz-designed guest is a trap: if the guest itself is clocked at
   25 Hz, that is your ceiling regardless of host speed — and every
   "optimisation measured zero" needs re-testing with that cap removed.
7. **Log free memory at each major load.** If free drops under ~20 MB you are
   one spike from an OOM.

## Diagnosing "fast in the emulator, slow on hardware"

The emulator is usually 5–20× the real thing on cache- and memory-bound code.
Suspects, in order:

- O(n²) over a large collection — cache-miss cost is invisible in emulation
- Uncompressed textures — VRAM pressure is not RAM pressure in an emulator
- Log spam — printing can dominate frame time on real hardware
- Per-frame allocations
- Floating-point in hot loops (VFP on A9 is slow next to integer)
- Cache-maintenance calls that are nearly free on desktop and expensive here

## Common mistakes

- **Assuming 512 MB is usable.** Plan around ~256 MB.
- **Loading a whole scene at scene start.** Fine for short scenes, catastrophic
  when the user moves between areas constantly. Stream per region.
- **Ignoring PhyCont vs paged.** The GPU wants physically contiguous memory;
  allocating GPU buffers from the paged pool sometimes works and sometimes hangs.
- **Trusting emulator benchmarks.** Always profile on hardware before optimising.
- **Forgetting the power tick** — power management can downclock hard on idle.
- **Believing a `printf` proves anything on device** — an unarmed code path and a
  zero gain produce the same silent log. Route diagnostics to the on-device
  progress log instead.

## References

- VitaSDK docs: https://docs.vitasdk.org/
- vitaGL source — a good reference for layering OpenGL over sceGxm
