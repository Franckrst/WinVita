# History and provenance

## Box86

winx86 owes its x86 → ARMv7 translation core to
[Box86](https://github.com/ptitSeb/box86) (Sébastien Chevalier, aka
"ptitSeb", MIT license). `third_party/box86-dynarec/` is a verbatim
extraction of it, plus a set of local patches mechanically regenerated
(fastmmu, memintrin, emitprof, signtag, a scheduler-preemption hook, a
write-protection switch for self-modifying code). These patches are
designed to be upstreamable to Box86 itself. See
[Architecture](architecture.md) for the extraction/resync machinery.

## Extraction from d2vita (2026-09-09)

winx86 was extracted on 2026-09-09 from the
[d2vita](https://gitlab.com/claude5564407/d2-vita) project, a port of
Diablo II: Lord of Destruction to PS Vita, by isolating the subset of the
engine that had no knowledge of the game (PE32 loader, dynarec, `Cpu`/
`Bridge`, schedulers, generic tooling). The rest — the game's ~600 Win32
shims, its performance hooks on precise addresses of its own binary, its
Glide rendering — stayed in d2vita.

## The boundary moved (2026-09-10 / 09-11)

The initial cut was cautious: **no** Win32 shim had followed, an audit
having found game-specific content hidden behind perfectly generic API
names. That caution was justified on the evidence, but the conclusion "no
shim is generic by nature" turned out to be too strong.

By going back through it DLL group by DLL group, and classifying each
function by its **body** rather than its name, more than two hundred
registrations joined the engine: SHELL32, ADVAPI32, USER32,
DirectDraw/Bink/Smacker/ijl11, IMM32, GDI32, window and cursor, VERSION and
PSAPI, then the whole socket layer.

Three neutral extension points were born from this work, each carrying a
policy that had to be pulled out of a shim without losing it: the guest
scratch allocator, the passive socket-layer observer, and the connection
route. The detail is in [Extension point](extension.md), the exact list in
[Shims provided](shims.md).

## Why a fresh git history, not inherited from d2vita

winx86 starts with a **deliberately fresh** git history, not extracted by
rewriting d2vita's history (`git filter-repo` or equivalent). d2vita is
derivative work from decompiled proprietary Windows binaries — a legally
sensitive context detailed in d2vita's own documentation. Rewriting its
history into a new repository would risk resurfacing, via `git log`/`git
show` on old commits, content that later commits removed from the current
state but not from the history. Starting winx86 from a fresh history
avoids the question rather than managing it case by case.

## The JIT pool capped by the kernel, not by choice (2026-09-13)

An attempt to reserve the JIT pool *before* the guest arena (to absorb the
alignment waste, 16+13=29 MiB in a single block) revealed that the Vita
kernel refuses any `sceKernelAllocMemBlockForVM` block beyond 16 MiB
(`SCE_KERNEL_ERROR_MEMBLOCK_OVERFLOW`) — never tested before on this
project. The attempt was abandoned (code reverted to its prior state), and
the pool rewritten to grow by 16 MiB segments rather than a single block.
Technical detail in [Architecture](architecture.md).

Along the way, a preexisting latent bug was fixed: the old code set its
"already tried" flag **before** knowing the allocation's result — a single
failure (memory pressure, fragmentation) disarmed the pool for the rest of
the session, with no retry and no second signal beyond the "REFUSED" log
line. The new code sets this flag only **after** a real kernel refusal,
never preemptively — and since segments live in a separate array, a
segment already open before the failure stays fully usable: only future
growth is blocked, not what was already allocated.

## Evolution

See `git log` for the commit-by-commit detail. The main initial
milestones: extraction of the generic engine (2026-09-09), wiring d2vita
as the first consumer via a git submodule, a LIBTAG/EXTRA "flavor"
mechanism for A/B builds, a fix for a build-contamination bug (a header
specific to the real Vita console was polluting the qemu-arm/Linux test
target).
