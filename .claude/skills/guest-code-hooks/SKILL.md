---
name: guest-code-hooks
description: Use when intercepting, probing, or patching genuine x86 guest code at runtime — installing a native shim at a guest address, dumping registers/memory mid-function, replacing a hot function with a native one, forcing a branch outcome, or resuming into displaced bytes. Covers the 5-byte jmp seam, the bridge's retaddr accounting, and the re-entrancy hazards.
---

# Guest code hooks

The `Bridge` lets you divert any guest address to a C++ shim. This is how a port
probes, patches and natively-replaces code it has no source for and cannot
recompile.

Two primitives, both in `src/runtime/` :

- `Cpu::set_alternate(from_va, to_va)` — redirect execution of a guest address.
- `Bridge::register_shim(dll, name, shim)` / `Bridge::shim_trap(dll, name)` —
  register a native body under a key, and get the trap address to jump to.

## Install a hook (the jmp5 seam)

```cpp
Shim s; s.argc=0; s.tag="diag!my_probe";
s.fn=[&br](Cpu& c)->uint32_t{ /* ... */ return c.reg(R_EAX); };
br.register_shim("diag.hook","my_probe",s);
uint32_t trap = br.shim_trap("diag.hook","my_probe");
uint32_t at = MOD_BASE + 0xRVA;              // must be an instruction START
uint8_t j[5]={0xE9,0,0,0,0};
uint32_t rel = trap-(at+5); std::memcpy(j+1,&rel,4);
cpu->write(at,j,5);
```

The trap fires when the guest reaches `at`. Pick a site whose instruction is
**≥ 5 bytes** (the jmp overwrites 5). A shorter instruction gets its tail
clobbered — safe only if nothing ever branches into those bytes, but prefer a
≥ 5-byte site rather than reasoning about it.

!!! danger "Always hook a function ENTRY, never an internal jump target"
    The dynarec fuses contiguous x86 blocks into one translated block. A hook
    placed on a jump target *inside* an already-fused block is never revisited —
    it goes **mute, with no error**. Document the address you hook as a real
    entry point (typically a recognizable `push ebp` / `push ebx` prologue),
    verified in the disassembly.

## The golden rule: account for the bridge's retaddr pop

`Bridge::trap_handler` always simulates a `ret`: it pops 4 from ESP (reading
`[ESP]` as the return address) and jumps there — unless overridden by
`redirect_next`.

- Hook reached via a **`call`** (the bridge's default argc/stdcall math): the pop
  is correct — `[ESP]` really is a return address.
- Hook reached via a **`jmp`** (your 5-byte seam replaced a non-call
  instruction): `[ESP]` is frame data, **not** a return address. The pop corrupts
  the frame. **Undo it**: `c.set_reg(R_ESP, c.reg(R_ESP)-4)` before returning, so
  the bridge's `+4` nets to zero.

## Resume into displaced bytes

If your 5-byte jmp overwrote a real instruction the function still needs,
**emulate it in the shim** and `redirect_next` to the address after it:

```cpp
// replaced `mov eax,[esp+0x490]` (7 bytes) at BASE+0x1417a:
c.set_reg(R_EAX, c.read_u32(c.reg(R_ESP)+0x490));  // emulate it
c.set_reg(R_ESP, c.reg(R_ESP)-4);                  // undo bridge pop (jmp seam)
br.redirect_next(BASE+0x14181);                    // = 0x1417a + 7
```

## Re-emit a call you displaced (two-seam pattern)

To wrap a `call` and observe its result: replace the call with seam A, re-emit
it by hand, and put seam B at the instruction after the call.

```cpp
// seam A at the 5-byte `call`: push the original retaddr, jump to the target.
// The bridge will esp+=4, so pre-bias by 8:
c.write_u32(esp-4, RETADDR); c.set_reg(R_ESP, esp-8);
br.redirect_next(CALL_TARGET);
```

Seam B (at `RETADDR`) then sees the callee's result in EAX.

**Key each seam's saved state by thread id.** A callee that takes a critical
section can yield the scheduler, so two guest threads may interleave between A
and B. A single C++ `static` would be clobbered — this is not theoretical, it is
the normal case under the cooperative scheduler.

## Native replacement (hot function → C++)

For a hot, pure function (a blit, a decompressor): relocate its first 5 bytes
into a trampoline (`orig5 + jmp back`), overwrite the entry with a jmp to a shim
that does the work at ARM speed, and on any case the shim does not handle,
`redirect_next` into the trampoline so the real code runs instead.

Enforce byte-exactness of the output against a deterministic replay before
trusting it. A native replacement that is *nearly* right is worse than none: it
produces plausible output and moves the bug somewhere else.

## Forcing a branch / tolerating a fault

You can make a validator "succeed" or "fail" by rebuilding its epilogue inside
the shim: restore the callee-saved registers from the frame, set EAX, move ESP
past the locals, then `redirect_next` to the return address. Restore **exactly**
what the real epilogue would (`pop ebp/edi/esi/ebx`, in the right order) or the
caller runs on a corrupt frame — a fault that then appears far from its cause.

## Hazards

- The hook site must be an instruction boundary — verify with `objdump`, never
  infer it from a hex offset.
- Allocator and critical-section shims can switch threads mid-hook: never hold
  hook state in a bare `static` across a guest call; key it by thread id.
- A shim's return value becomes EAX. If the scheduler later wakes this thread
  from a wait, that wake can overwrite EAX — see the `wake_writes_eax` flag in
  `dynarec-internals`.
- Env-gate every diagnostic probe and cap its output (`static int n; if(n<N)`),
  or a probe that was fine in a 200-frame test drowns a real run.
