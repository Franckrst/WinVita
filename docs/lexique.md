# Lexicon

Generic-engine terms. For terms specific to a particular port (Diablo II,
MPQ, Battle.net...), see that port's own lexicon — for instance
[d2vita's](https://franckrst.github.io/D2Vita/lexique/).

**PE32** — the binary format of 32-bit Windows executables and DLLs
(*Portable Executable*). What `PeImage` loads.

**RVA** — *Relative Virtual Address*, an address relative to a PE module's
load base. Most addresses cited in a native hook are RVAs
(`module_base + 0x...`), not absolute addresses.

**Dynarec** — *dynamic recompiler/translator*: translates x86 machine code
to ARM code at execution time, block by block, caching already-translated
blocks (unlike an interpreter, which reinterprets every instruction on
every pass, or full ahead-of-time static compilation).

**Shim** — a native implementation provided in place of an imported
Windows function (e.g. `KERNEL32.dll!GetVersion`), registered via
`Bridge::register_shim`.

**Trap** — the mechanism by which the dynarec stops and hands control back
to native host code at a given address, instead of continuing to
translate/execute guest code. `Bridge::shim_trap` gets the trap address
associated with a registered shim.

**`set_alternate`** — see [Extension point](extension.md): redirects
execution from a guest address (always a function entry point) to
another.

**Native hot-path hook** — a shim placed via `set_alternate` on a game
function (not a real Windows API) for performance or diagnostic purposes,
usually with a faithful fallback to the original code.

**Faithful fallback** — in a hook, manually replaying the effect of the
intercepted function's original prologue (e.g. `push ebp`) before
resuming execution, so the hook is transparent when it has nothing to
change about the behavior.

**GIL** — *Global Interpreter Lock*, a global lock serializing access to
the dynarec across multiple native threads (CPython-GIL style) —
necessary because the dynarec isn't thread-safe by construction.

**Self-modifying code (SMC)** — code that writes into its own code memory
during execution. Requires invalidating or destroying the already-
translated blocks for that address range (`Cpu::invalidate_code`/
`discard_code`).

**COM (vtable)** — the Windows component model where an object is exposed
through a table of function pointers (*vtable*). `src/runtime/ds_emul.cpp`
(in the engine) builds a COM DirectSound vtable entirely from a shim
table, with no real underlying C++ class.
