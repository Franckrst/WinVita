# AI tooling / reusable agents

This page lists the development tooling designed to be reusable by any
port built on winx86 — not just d2vita — and suggests a few directions for
later.

## `.claude/skills/` and `.claude/agents/`

The engine's know-how is versioned with it, and loaded automatically by
Claude Code when working in this repository.

**Skills** describe a domain: the dynarec and scheduler internals, hooks
on guest code, the elimination method for an intermittent corruption, how
to verify an instrument before trusting it, the measurement protocol on
real hardware, and the Vita platform basics.

**Agents** describe a complete, replayable procedure:

- `shim-split` — moving shims between the engine and a port: classify by
  **body**, not API name, the `shim_seq` safety net, the "one registration
  per key" rule, and the threshold past which "too entangled" is an
  acceptable conclusion.
- `doc-update` — bring the docs back in line with the code, by checking
  the code rather than a summary.

The boundary is the same as for code: whatever only serves a single
consumer (reading *its* archives, driving *its* game loop) stays with it.

## `tools/gen_shim_list.py` — the shims page is generated

[The list of shims provided by the engine](shims.md) isn't hand-written:
it's produced from `src/runtime/win32_shims_*.cpp`, reusing the
already-proven parser from `shim_seq.py` rather than maintaining a second
parallel analyzer. CI regenerates it and **fails if it has drifted**
(`gen_shim_list.py --check`).

A hand-written list would be wrong by the second commit, and a wrong list
is worse than no list: it makes you believe you know what the engine
covers.

Two deliberate choices:

- **duplicate registrations are flagged**, not silently deduplicated — a
  page that hid them would hide exactly what you want to see;
- Winsock ordinals are **named from the source** (the `connect_fn` lambda
  yields `connect`), never from a hand-copied table. That's what makes the
  real divergence between `WSOCK32.dll` and `WS2_32.dll` on ordinals
  10/11/12 visible.

## `tools/shim_seq.py` / `.sh`

Reconstructs the **ordered sequence** of Win32 shims (and native hooks)
registered in a boot binary, with a body fingerprint for each
registration, and compares two states (current tree vs. a git revision).
Three verdicts, worst to least severe:

- **SET** — a key appears/disappears, or its registration count changes.
  Always fatal.
- **EFFECTIVE** — same key set, but a different body wins for at least one
  key (reminder: `register_shim` overwrites, the last registration wins).
  Always fatal — this is exactly the bug a botched block move produces.
- **ORDER** — same effective table, but the sequence differs (block
  permutation). Fatal by default; `ALLOW_REORDER=1` accepts it for a
  refactor that moves whole blocks without changing the effective table.

**When to use it**: before any refactor that moves shim-registration code
around — typically when pulling a piece out of a monolithic file into its
own, exactly the kind of operation this repository has performed on
itself multiple times while splitting off from d2vita. Run
`tools/shim_seq.sh` before and after; `OK` = nothing changed on the
effective-table side.

A file passed via `ALLOW_BODY=<file>` documents *explicitly* expected body
transitions (key, fingerprint before, fingerprint after) when a behavior
change is intentional — never a blank check, a later drift of the same key
becomes fatal again. Nothing mandates its name or location: it isn't a
fixed `.allow` file versioned in the repository.

## `tools/extract_box86.sh` / `tools/regen_box86_patch.sh`

Let you resync `third_party/box86-dynarec/` against a newer upstream Box86
without losing the local patches: `regen_box86_patch.sh` captures the
current local delta into a mechanically-validated patch file
(self-checked by reapplying it and comparing the result bit for bit);
`extract_box86.sh` re-extracts a clean Box86, reapplies the patch, and
**refuses to touch the existing tree** if the result diverges — rather
than silently overwriting uncaptured work.

## `tools/pe_analyze.cpp`

Inspects a PE32 binary's import table — useful for empirically checking
which Win32 functions a binary actually imports before writing a shim for
a function that will never be called.

## The safe extraction methodology (demonstrated, not just documented)

winx86 itself was extracted from code entangled in a 12,000+ line
monolithic boot file, over several passes. The method that worked, every
time:

1. **Map read-only first** — identify the exact boundaries (often
   non-contiguous: related code can be interleaved with completely
   unrelated code) and the full inventory of symbols crossing the
   boundary, before touching a single file.
2. **Never trust an estimated boundary** — re-verify with tooling
   (`grep -n`, `sed -n`) at extraction time, not just at mapping time; the
   code moves.
3. **Externalize mechanically, without changing the logic** — every
   symbol crossing the boundary becomes an `extern`, nothing else changes.
4. **Validate with a sequence/fingerprint diff** (`shim_seq.sh` here)
   **and a deterministic oracle replayed twice** (see d2vita's own
   documentation for the qemu-arm oracle) — commit only if both are
   genuinely green, never on compile success alone.
5. **Stop and report honestly if it isn't a clean move** — several
   attempts during winx86's extraction were deliberately aborted midway
   (a move of whole Win32 shims was abandoned upon discovering
   game-specific content hidden in generic-looking function names). A
   sloppy extraction that "looks like it works" is worse than no
   extraction at all.

## A named technique: genericity by comparison

When two ports exist (here: d2vita and its sibling project carn-vita,
both derived from the same engine), you can *prove* a piece of code is
generic rather than assume it: if both ports have the same body for the
same function, that's strong proof of genericity; if they've diverged,
that's proof that customization was needed — even when the function's
name (`GetVersion`, `CreateProcessA`...) gives no hint of it. This
technique made it possible to find, upstream, and cross-check several
generic fixes between the two projects before winx86 existed as a
separate repository.

## Proposals (not yet built)

- **A `CLAUDE.md` template** for any new port built on winx86, capturing
  the rules that made this work reliable: never claim a test/build
  succeeded without actually running it; private repository by default
  until a port's legality from decompiled proprietary binaries has been
  settled; never fake behavior marked as complete.
- **A three-tier CI template** (fast qemu-arm → Vita3K → real hardware)
  packaged as a reusable GitHub Actions workflow, so a new port doesn't have to
  rediscover the distinction between "it compiles," "it boots under
  emulation," and "it's actually faster on real console." The current CI
  only publishes the site and checks the generated page is fresh.
- **A `shim_seq` generalized beyond shims** — the same pattern
  (sequence + fingerprint + diff between two revisions) would apply to
  any "last registration wins" registration table, not just
  `register_shim`. Not yet extracted into a generic library for lack of a
  second concrete use case to justify it.
