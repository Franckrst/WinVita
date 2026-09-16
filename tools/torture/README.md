# Banc de fidelite x86 Win32

**Ce banc appartient au moteur.** Le meme PE 32 bits mesure la fidelite Win32 de
n'importe quel portage : `torture.c`, `bench_gtc.c` et `gil_ident.cpp` etaient
dupliques a l'octet pres dans deux portages avant d'arriver ici.

Le portage ne fournit que deux donnees, et c'est le seul point d'extension :

```sh
RT_BIN=build-arm/<son runtime ARM> REFDIR=<ses references> \
    third_party/winx86/tools/torture/run_torture.sh
```

A single 32-bit Win32 PE (`torture.c`) that runs **identically on real Windows, on
Wine, and inside the engine runtime** and emits a machine-comparable report, so the
three can be `diff`ed. It exercises the runtime-fidelity axes: modules, memory,
Toolhelp, threads, TLS, thread contexts, sync, dynamic code, exceptions, timing.

It is **not** a Warden and does not emulate one — it only reads real guest state
and reports what each API returned, the way any Win32 program would.

## Report format

One line per check:

```
CHECK|<api>|<args>|<ret 0x%08lx>|<GetLastError>|<effect>
```

`effect` is machine-comparable (a base/handle, an MBI `State=/Type=/Protect=`, a
`tid=/exit=`, or an `ok|MISS|STALE|FALSE|end-of-chain` token) — never free prose.

## Run

```sh
tools/torture/run_torture.sh                 # default 39-check report + diff
TORTURE_FAULT=1 tools/torture/run_torture.sh # + opt-in raw-SEH #DE probe
TORTURE_MT=1 tools/torture/run_torture.sh    # + opt-in multi-thread stress (mt/*)
```

Gotcha: `TORTURE_MT=0` still ENABLES the section — the `${TORTURE_MT:+...}`
pattern tests non-empty, not truth (same behavior as `TORTURE_FAULT`); unset
the variable to disable.

`TORTURE_MT=1` (compile-time `-DTORTURE_MT`, same pattern as `TORTURE_FAULT`) adds
a real-concurrency section: Interlocked storm (4×100k), auto-reset event
ping-pong ×1000, semaphore exactly-once delivery, `waitAll` partial-failure /
full-consume semantics, and concurrent SMC (patch + `FlushInstructionCache`
while another thread executes the block — the dynarec invalidation dance).
Effects record outcomes only (final counts, WAIT codes; a hung join emits
`HUNG`), never scheduler wake statistics. The section runs BEFORE the
`RtlCaptureContext` check because wine-9.0 wow64 crashes there (freestanding
EBP=0 entry), truncating the golden at 19 checks — anything after it would be
absent from the golden. Combine with `D2SCHED=native` to stress the native
scheduler with real OS-preempted threads.

It builds `torture.exe` with `i686-w64-mingw32-gcc`, runs it under Wine (golden),
runs it inside the engine (`qemu-arm $RT_BIN` via `D2_RUNEXE`), and diffs
the two reports. `OUT=<dir>` and `REFDIR=<1.14d install>` override the defaults.

Manual:

```sh
i686-w64-mingw32-gcc -m32 -O1 -nostartfiles -e _torture_entry -o torture.exe torture.c -lkernel32 -luser32
wine ./torture.exe                                  ;  cat torture_report.txt   # golden
D2WRITE=/tmp/t D2_RUNEXE=$PWD/torture.exe qemu-arm -B 0x10000 $RT_BIN <any-1.14d-dir>
                                                    ;  cat /tmp/t/torture_report.txt
```

`D2_RUNEXE=<path>` is a gated runtime entry point: it loads an arbitrary 32-bit EXE
at its ImageBase and drives its entry through the SAME shim + dynarec + scheduler
path as Game.exe (imports resolve via the K()/U() table; a missing one trips the
default-shim CONTROLLED STOP). Unset, the boot is byte-identical to before.

## Reading the diff

Lines that differ are either a fidelity gap or an **intended** divergence. Expected
divergences (not bugs):

- opaque values: handles (the engine uses `0x82…`/`0x7e…` pseudo-handles), allocation
  addresses, tick/QPC/filetime — inherently platform-specific.
- honest counts: `Module32Next count` and `Thread32` count are smaller on the engine
  because it lists only the modules it really mapped and its own real threads —
  never a synthesized "full" view.
- `ncpu=1`, `pid=1`, `tid=1`: the cooperative single-host-thread model, reported
  honestly.
- `GetLastError` stickiness: the engine leaves LastError untouched on success (Win32
  semantics); Wine resets it more aggressively on some calls.

The `effect` (semantic) field should match on essentially every line — a mismatch
there (not just in the opaque value) is a real gap to investigate.

`TORTURE_FAULT=1` adds a raw fs:[0] SEH probe (a hand-built EXCEPTION_REGISTRATION
plus an `idiv`-by-zero — mingw GCC does not emit working MSVC `__try/__except` on
i686). Wine runs the handler off the chain. This engine does not: it classifies the
fault as `0xC0000094` (INTEGER_DIVIDE_BY_ZERO), hands that code to whatever fault
dispatcher the consumer registered, and terminates the thread with it. **No guest
handler is ever called** — the engine dispatches no exception to guest code, and a
consumer that wants SEH has to implement it above this interface.
