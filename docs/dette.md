# Known limitations / technical debt

This page honestly lists what isn't done, rather than leaving it to be
discovered in silence.

!!! note "It shrinks too"
    A debt page only grows if nobody removes the entries once paid off.
    Every finished piece of work should remove one — a settled debt left
    lingering makes it look like work still remains.

## Environment variables: two `D2_*` names with no equivalent

Most of the renaming is done (`1e003e2`, completed by `bc41daf` for the
guest memory layout): the generic settings are named `WX86_*`, and the old
`D2_*` names (including `D2ARENA`, `D2HI`, `D2LAYOUT`, `D2MEMBASE`, now
`WX86_ARENA`/`WX86_HI`/`WX86_LAYOUT`/`WX86_MEMBASE`) still work as a
fallback so as not to break existing scripts.

Two variables read by the engine have **no** `WX86_` twin: `D2_EIPTRAP_N`,
`D2_XFERTRACE` — both in `third_party/box86-dynarec/dynarec/dynarec.c`,
vendored, mechanically-patched Box86 code (see
[Architecture](architecture.md)), deliberately left untouched. Two others
have no prefix at all (`THREADLOG`, `TRAPTAG`), which is a collision risk
with a consumer's own environment.

This is a **naming** problem, not a coupling one: their behavior is
entirely generic. But it can mislead someone looking for a tie to the
original game where none exists.

## On-screen presentation — never audited

`vita_present.cpp` (on-screen presentation) has never been reviewed to
extract a possibly-generic part. It's the last big chunk nobody has looked
at — so the unknown isn't "how much is generic," it's "we don't know."

## Hardcoded personal path in two scripts

`tools/regen_box86_patch.sh` and `tools/extract_box86.sh` default to
`/home/doudou/repos/box86` for the upstream Box86 repository. It's
overridable as the first argument, so there's no functional consequence,
but it's a personal path in a repository meant to be readable by others.

## A single proof of use

winx86 has only one real consumer so far. Everything declared "generic" is
so by code audit, not by the proof of a second port actually using it —
with one exception, the cross-port comparison technique described in [AI
tooling](outils-ia.md).

This is the repository's most honest limit: the boundary was drawn with
care, but it hasn't yet been pulled on by a second use.
