# Extension point — how to port your game

winx86 contains **no game hook** and **no knowledge of any particular
binary**. It provides two hook primitives, a library of Win32 shims
**proven** to carry no game-specific behavior, and a handful of neutral
extension points through which a port injects its own policy.

## The two primitives

### `Cpu::set_alternate(from_va, to_va)`

`src/runtime/cpu.h`. Redirects execution from one guest address to
another. This is the "hook at a function's entry" mechanism.

!!! warning "Always a function ENTRY address"
    Never an internal jump target. The dynarec merges contiguous x86 code
    blocks into a single translated block; a hook placed on a jump target
    internal to an already-merged block will never be revisited — the
    hook becomes **silently mute**. Document this address as a real entry
    point (usually a `push ebp` / `push ebx` prologue recognizable in the
    disassembly).

### `Bridge::register_shim(dll, name, shim)` / `Bridge::shim_trap(dll, name)`

`src/runtime/bridge.h`. `register_shim` registers a native implementation
under a `"DLL.dll"` + `"FunctionName"` key; `shim_trap` gets the
corresponding trap address, to pass to `set_alternate`.

!!! danger "`register_shim` overwrites the key"
    Registering the same key twice produces no error: **the last
    registration wins**, silently. This is a feature — it lets a port
    substitute its own version for an engine default without winx86
    knowing — but it's also the classic trap when reorganizing code that
    registers a lot of shims. A duplicate of this kind has already broken
    a port's network connection, with nothing flagging it.

    `tools/shim_seq.py` / `.sh` exist exactly for this — see [AI
    tooling](outils-ia.md).

## The shims the engine provides

The exact list is [generated from the sources](shims.md) and checked by
CI. In short: registry and security (ADVAPI32), fake GDI objects, window
geometry and semantics, module enumeration, version resources, video
players and IME reported as absent, and a complete socket layer.

**None of these install functions is called automatically.** The port
calls whichever it wants, from its own boot binary:

```cpp
#include "runtime/win32_shims_advapi32.h"
#include "runtime/win32_shims_wsock32.h"

win32_shims_advapi32_install(br);
win32_shims_wsock32_install(br);
// ... then your own shims, which can overwrite any of the keys above if
//     your game needs different behavior.
```

An engine that registered shims with authority would decide for its
consumer instead. The order is yours, and the last registration wins.

## The neutral extension points

When a feature is generic but the **policy** isn't, the engine exposes an
extension point rather than guessing. Three examples, all from real cases.

### The guest scratch allocator

`src/runtime/guest_scratch.h`. Many Win32 APIs don't return a value but a
**pointer** to memory the caller will read — `gethostbyname` returns a
`hostent*`, `inet_ntoa` a `char*`, `GetCommandLineA` a string. That
pointer must be a **guest** address, so the result has to be written into
guest memory space.

The engine owns the allocator; the port grants it a range, because the
memory layout itself has nothing universal about it:

```cpp
wx86_scratch_init(base, size);        // the port declares the range
wx86_scratch_set_oom_handler(&my_logger);
```

This is a bump allocator that **never frees**: it's reserved for
process-lifetime return values. A call site that allocates on **every
call** on a hot path will eventually exhaust it — that has happened, and
the fix is to cache the result, not enlarge the range.

### The network observer

`src/runtime/win32_shims_wsock32.h`. **A single** passive observation
point over the whole socket layer: `connect`, `connect-done`, `send`,
`recv`, `close`, `resolve`. The engine **reports** what happens; it never
asks for an opinion and never changes behavior based on the answer.

Everything about protocol, packet framing, logging, or instrumentation
belongs to the port, which registers **one** observer and dispatches
internally. Nothing in this interface names a protocol, a port, or a
product.

```cpp
wx86_net_set_observer(&my_observer);   // exactly one, the second replaces it
```

### The connection route

`wx86_net_set_redirect(ip)` rewrites every non-local connection to a given
address, keeping the port — the moral equivalent of a hosts-file entry.

!!! note "A safety net, not the normal method"
    To point a client at a private server, the faithful way is the same
    as a real PC: configure the **client's own server list** (its
    registry keys, its `.ini`) so it asks for your host from the start.
    Measured on one port: once the list was trimmed down to the local
    address, the redirect became entirely unnecessary for the whole
    application path. It stays useful only for a probe going out to a
    literal address, without going through a name.

## The general shape of a port

1. Build your `Cpu` (`CpuBox86`) and your `Bridge`.
2. Load your PE32 via `PeImage`.
3. Declare the guest scratch range (`wx86_scratch_init`).
4. Call the `win32_shims_*_install()` you want, **then** register your own
   shims — yours win, since the last registration wins.
5. Wire your policies onto the neutral extension points (network observer,
   route, etc.).
6. Register your native performance hooks on *your* binary's hot addresses
   via `set_alternate` + `shim_trap`.
7. Start the scheduler (cooperative or native).

### What a port must provide for a console target

Exactly one thing, and it's **mandatory** in a `__vita__` build:

```cpp
// file scope, CONSTANT initializer — not a setter
extern "C" const char* const wx86_vita_progress_path = "ux0:data/<your-game>/boot.txt";
```

This is the **path** of the durable progress log, the only window into a
boot with no screen. Everything else — the log itself, the lock that makes
it thread-safe, the distribution of threads across user cores,
auto-pinning, and the inventory published in the `cores:` line — belongs
to the engine (`platform/vita_host.h`).

!!! danger "Don't declare it weak, and don't invent a default value"
    Without this definition, the link **fails** on target — and that's
    intentional. The engine long called its log through a **weak**
    reference to a symbol prefixed for its first consumer: a port that
    provided nothing got a **mute** log and **unpinned** threads, with not
    a single link error (`docs/migration.md`, pitfall 9). A link that
    breaks early is worth a thousand times more than a service that stays
    silent.

    Off console, there's **nothing** to provide: the log there is a no-op
    that doesn't even read this path.

## Full example: an observation hook with a faithful fallback

This hook intercepts a game function's entry at a precise address, counts
events, then **faithfully replays** the original prologue before letting
the dynarec continue — a fallback that changes nothing about behavior,
only instrumented:

```cpp
void install_watch(Cpu* cpu, Bridge& br){
    static uint32_t s_entry = MOD_BASE + 0x2092d0;   // ENTRY address

    Shim s; s.argc = 0; s.stdcall_cleanup = false; s.tag = "native!watch";
    s.fn = [&br](Cpu& c) -> uint32_t {
        const uint32_t E = c.reg(R_ESP);
        ++g_count;
        // Faithful fallback: replay the original prologue (`push ebx`)
        // then resume right after, as if the hook had never existed.
        c.write_u32(E - 4, c.reg(R_EBX));
        c.set_reg(R_ESP, E - 8);
        br.redirect_next(s_entry + 1);
        return c.reg(R_EAX);
    };

    br.register_shim("native.hook", "watch", s);
    cpu->set_alternate(s_entry, br.shim_trap("native.hook", "watch"));
}
```

Worth remembering:

- The `"native.hook"` DLL key is a **port convention** for hooks that
  don't correspond to any real Windows DLL, not a winx86 requirement.
- A hook can be conditional (armed by an environment variable) for
  optional diagnostics or performance A/B testing.
- The "faithful fallback" is the safest pattern for a hook that only
  observes: no risk of divergence, just one trap per call.

## Why some shims stay with the port {#frontiere}

An audit run while trying to extract a port's Win32 shims found
game-specific content hidden behind perfectly generic names: an install
path hardcoded into an `SHGetFolderPathA`, an authentication emulation
specific to the game's own protocol inside a `CryptoAPI` shim. Nothing in
the signature gave it away.

The conclusion at the time — "no Win32 shim is generic by nature" — was
right about the evidence and **too pessimistic about what followed**.
What worked afterward, and produced the library listed in [Shims
provided](shims.md):

**classify every function by its BODY, never by its API name.**

| class | criterion | destination |
|---|---|---|
| generic | no literal, branch, or state dependency specific to a game; no dependency on a port helper | the engine |
| specific | a hardcoded literal, a dependency on game state, a workaround | the port |
| at risk | an unresolved dependency, risk of duplicate registration | stays in place, **with the reason written in the code** |

Two cutting criteria, learned the hard way:

- **Does the game inspect the *content* of the returned value, or only
  its success?** A shim where only success matters is generic; a shim
  whose content is examined carries game knowledge. That's decided by
  disassembly, not by signature.
- **"Too entangled" is only an acceptable conclusion on concrete evidence
  of a danger** — not on difficulty. Two efforts concluded "not worth it"
  were reversed by redoing the design work properly, and both times the
  decoupling held.

The full procedure is in `.claude/agents/shim-split.md`.
