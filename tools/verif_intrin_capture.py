#!/usr/bin/env python3
# tools/verif_intrin_capture.py — proves, by disassembly of the guest PE, that a
# dyn86_intrin target is legitimately CAPTURED at block start.
#
#   usage: verif_intrin_capture.py <PE> <va>:<end> [<va>:<end> ...]
#          verif_intrin_capture.py WoW.exe 0x6579b0:0x6579b8 0x657140:0x657163
#
# WHY THIS EXISTS (dyn86_intrin.h, "WHAT MAKES BLOCK-START RECOGNITION VALID"):
# the translator swaps a native helper in for a guest function only when a block
# BEGINS at that function's first byte. Two properties must hold, and neither is
# visible from the function's own code -- they are properties of every OTHER byte
# in .text:
#   1. no direct `jmp` to the entry. A `jmp` is a tail call: control arrives
#      WITHOUT the block restarting there, so the helper would be skipped on that
#      path while the caller's stack says a call happened.
#   2. no branch from OUTSIDE the function into its body (past the first byte).
#      Such an entry bypasses the point where the helper's preconditions hold.
# A direct `call` to the entry is exactly what we want, and an INDIRECT call
# (vtable slot, function pointer) also creates a block at the entry, so a target
# reached only indirectly is fine -- it is reported, not failed.
#
# Branches INSIDE the function (its own loops) are normal and are counted
# separately: they never let outside code in, they are the body's own control flow.
#
# METHOD, and why it is the conservative one. There is no capstone here, and a
# linear objdump sweep over 4 MiB of .text desynchronises on the first island of
# data and then reports fictional instructions -- which would make it MISS real
# branches, the one error this check must never make. So the scan below is a raw
# opcode sweep over every byte offset: every byte that COULD begin a relative
# branch is decoded as one. That over-approximates (an immediate or a string byte
# can look like `E9 xx xx xx xx`), so it can only ever report a conflict that
# does not exist -- never hide one. A target that passes this check is safe; a
# target that fails it gets the objdump second opinion printed below, because a
# false positive here costs an optimisation, and a false negative costs
# correctness.
import subprocess
import struct
import sys


def sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        sys.exit("pas un PE")
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsz = struct.unpack_from("<H", data, pe + 20)[0]
    base = struct.unpack_from("<I", data, pe + 24 + 28)[0]
    out, off = [], pe + 24 + optsz
    for _ in range(nsec):
        name = data[off:off + 8].split(b"\0")[0].decode("latin1")
        vsz, va, rsz, ro = struct.unpack_from("<IIII", data, off + 8)
        out.append((name, base + va, vsz, ro, rsz))
        off += 40
    return base, out


# Relative-branch opcodes, as (length, operand size, is_jmp, is_call).
# Only RELATIVE forms matter: an absolute/indirect branch cannot be resolved
# statically here, and -- see the header -- an indirect call is harmless anyway.
def scan_branches(text, text_va):
    """Yields (site_va, target_va, kind) for every byte offset that could start a
    relative branch. kind is 'jmp', 'jcc' or 'call'."""
    n = len(text)
    i = 0
    while i < n:
        b = text[i]
        kind = size = oplen = None
        if b == 0xE9:                                   # jmp rel32
            kind, size, oplen = "jmp", 4, 5
        elif b == 0xEB:                                 # jmp rel8
            kind, size, oplen = "jmp", 1, 2
        elif b == 0xE8:                                 # call rel32
            kind, size, oplen = "call", 4, 5
        elif 0x70 <= b <= 0x7F:                         # jcc rel8
            kind, size, oplen = "jcc", 1, 2
        elif b == 0xE3:                                 # jecxz rel8
            kind, size, oplen = "jcc", 1, 2
        elif 0xE0 <= b <= 0xE2:                         # loop/loopz/loopnz rel8
            kind, size, oplen = "jcc", 1, 2
        elif b == 0x0F and i + 1 < n and 0x80 <= text[i + 1] <= 0x8F:   # jcc rel32
            kind, size, oplen = "jcc", 4, 6
        if kind and i + oplen <= n:
            disp_at = i + oplen - size
            if size == 1:
                d = struct.unpack_from("<b", text, disp_at)[0]
            else:
                d = struct.unpack_from("<i", text, disp_at)[0]
            yield text_va + i, (text_va + i + oplen + d) & 0xFFFFFFFF, kind
        i += 1


def objdump_around(path, ro, va, site_va, data):
    """Second opinion on ONE reported conflict.

    The raw scan reports a byte OFFSET, not an instruction. To tell a genuine
    branch from bytes inside somebody's operand, the only sound test available
    without a full disassembler is: does ANY instruction stream reaching this
    point actually start an instruction here? So the site is disassembled from
    several preceding anchors; if no anchor ever puts an instruction boundary on
    the site, the "branch" lives inside another instruction's bytes and the
    conflict is a false positive -- which this scan is designed to produce rather
    than risk the opposite error."""
    raw = data[ro + (site_va - va): ro + (site_va - va) + 6]
    tmp = "/tmp/_verif_intrin_chunk.bin"
    for back in range(1, 25):
        start = site_va - back
        blob = data[ro + (start - va): ro + (start - va) + back + 16]
        with open(tmp, "wb") as f:
            f.write(blob)
        out = subprocess.run(
            ["objdump", "-D", "-b", "binary", "-m", "i386", "-M", "intel",
             "--adjust-vma=0x%x" % start, tmp],
            capture_output=True, text=True).stdout
        for line in out.splitlines():
            if line.strip().startswith("%x:" % site_va):
                return f"octets {raw.hex()} | ancrage -{back}: {line.strip()}"
    return (f"octets {raw.hex()} | AUCUN ancrage (jusqu'a -24) ne place une instruction "
            f"ici -> FAUX POSITIF probable (octets d'un operande)")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__ or "usage: verif_intrin_capture.py <PE> <va>:<end> ...")
    path = sys.argv[1]
    targets = []
    for a in sys.argv[2:]:
        lo, _, hi = a.partition(":")
        if not hi:
            sys.exit("chaque cible doit etre <va>:<fin> (fin exclue)")
        targets.append((int(lo, 16), int(hi, 16)))

    data = open(path, "rb").read()
    base, secs = sections(data)
    text = next((s for s in secs if s[0] == ".text"), None)
    if not text:
        sys.exit("pas de section .text")
    _, tva, tvsz, tro, trsz = text
    body = data[tro:tro + min(trsz, tvsz)]
    print(f"{path}: .text VA 0x{tva:08x} taille 0x{len(body):x} (base image 0x{base:08x})")

    hits = list(scan_branches(body, tva))
    print(f"balayage conservateur : {len(hits)} sites de branchement relatif possibles\n")

    bad = 0
    for lo, hi in targets:
        calls = jmps = inner = 0
        conflicts = []
        for site, tgt, kind in hits:
            inside_target = lo < tgt < hi
            at_entry = tgt == lo
            if not (inside_target or at_entry):
                continue
            site_is_internal = lo <= site < hi
            if at_entry:
                if kind == "call":
                    calls += 1
                else:
                    conflicts.append((site, tgt, kind, "TAIL CALL vers l'entree"))
            elif site_is_internal:
                inner += 1          # the function's own loops: normal
            else:
                conflicts.append((site, tgt, kind, "entree EXTERNE dans le corps"))

        ok = not conflicts
        print(f"cible 0x{lo:08x}..0x{hi:08x} ({hi - lo} octets)")
        print(f"   call directs vers l'entree : {calls}")
        print(f"   branchements internes (boucles propres) : {inner}")
        if ok:
            if calls == 0:
                print("   RESULTAT : CAPTURABLE (aucun appel direct -- atteinte indirecte "
                      "uniquement, ce qui cree aussi un bloc a l'entree)")
            else:
                print("   RESULTAT : CAPTURABLE")
        else:
            bad += 1
            print(f"   RESULTAT : NON CAPTURABLE -- {len(conflicts)} conflit(s)")
            for site, tgt, kind, why in conflicts[:8]:
                print(f"     {why}: {kind} en 0x{site:08x} -> 0x{tgt:08x}")
                print(f"       {objdump_around(path, tro, tva, site, data)}")
            if len(conflicts) > 8:
                print(f"     ... et {len(conflicts) - 8} autre(s)")
        print()

    print("VERDICT :", "TOUTES LES CIBLES SONT CAPTURABLES" if not bad
          else f"{bad} cible(s) REFUSEE(S)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
