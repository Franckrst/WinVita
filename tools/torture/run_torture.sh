#!/bin/bash
# run_torture.sh — construit le test de fidelite x86 Win32, le fait tourner sous
# Wine (etalon) puis DANS le moteur (qemu-arm, via WX86_RUNEXE / D2_RUNEXE), et
# compare les deux rapports.
#
# Le banc appartient au MOTEUR : le meme PE 32 bits mesure la fidelite Win32 de
# n'importe quel portage. Le portage ne fournit que deux donnees — son binaire
# de runtime ARM et son dossier de references. C'est le seul point d'extension.
#
# D2_RUNEXE garde son nom d'origine : c'est la variable que les runtimes lisent
# REELLEMENT aujourd'hui (rt_boot.cpp). La renommer ici sans la renommer la-bas
# fabriquerait un banc qui ne charge rien en ayant l'air de marcher.
#
#   RT_BIN=build-arm/mon_rt_arm tools/torture/run_torture.sh
#   TORTURE_FAULT=1 ...   # + sonde SEH #PF reelle, optionnelle
#   TORTURE_MT=1 ...      # + section multi-fils, optionnelle
#     (-DTORTURE_MT a la compilation, comme TORTURE_FAULT : le MEME exe exerce
#      la section des DEUX cotes du diff — etalon Wine et moteur.)
#
# Requiert : i686-w64-mingw32-gcc, wine, qemu-arm, le binaire RT_BIN, et un
# dossier de references passe en argv[1] (inutilise au chargement quand
# l'execution directe d'un exe est demandee).
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"

# --- le point d'extension : ce que le PORTAGE doit fournir -------------------
RT_BIN="${RT_BIN:-}"
if [ -z "$RT_BIN" ] || [ ! -x "$RT_BIN" ]; then
    echo "run_torture.sh: RT_BIN doit designer le binaire de runtime ARM du portage." >&2
    echo "  ex. RT_BIN=build-arm/rt_boot_arm $0" >&2
    exit 2
fi
REFDIR="${REFDIR:-}"
if [ -z "$REFDIR" ]; then
    echo "run_torture.sh: REFDIR doit designer le dossier de references du portage." >&2
    exit 2
fi
# ---------------------------------------------------------------------------

OUT="${OUT:-/tmp/torture}"; mkdir -p "$OUT"
FAULT="${TORTURE_FAULT:+-DTORTURE_FAULT}"
MT="${TORTURE_MT:+-DTORTURE_MT}"
# MT ajoute des sections a fils avec des attentes genereuses (5-10 s) : on
# elargit les delais du harnais pour qu'une verification BLOQUEE soit RAPPORTEE
# au lieu de tuer la passe.
TMO_WINE="${TORTURE_MT:+120}"; TMO_WINE="${TMO_WINE:-40}"
TMO_QEMU="${TORTURE_MT:+400}"; TMO_QEMU="${TMO_QEMU:-120}"

echo "== build torture.exe (i686-w64-mingw32-gcc $FAULT $MT) =="
i686-w64-mingw32-gcc -m32 -O1 -msse2 -mstackrealign -nostartfiles -e _torture_entry \
    $FAULT $MT -o "$OUT/torture.exe" "$HERE/torture.c" -lkernel32 -luser32 || exit 1
file "$OUT/torture.exe"

echo "== run under WINE (etalon) =="
( cd "$OUT" && rm -f torture_report.txt && WINEDEBUG=-all timeout "$TMO_WINE" wine ./torture.exe >/dev/null 2>&1 )
cp -f "$OUT/torture_report.txt" "$OUT/wine.txt" 2>/dev/null && echo "  -> $OUT/wine.txt ($(wc -l < "$OUT/wine.txt") checks)"

echo "== run DANS le moteur (qemu-arm, execution directe de l'exe) =="
rm -f "$OUT/rt_report/torture_report.txt"; mkdir -p "$OUT/rt_report"
D2WRITE="$OUT/rt_report" D2_RUNEXE="$OUT/torture.exe" \
    timeout "$TMO_QEMU" qemu-arm -B 0x10000 "$RT_BIN" "$REFDIR" > "$OUT/rt.log" 2>&1
cp -f "$OUT/rt_report/torture_report.txt" "$OUT/rt.txt" 2>/dev/null && echo "  -> $OUT/rt.txt ($(wc -l < "$OUT/rt.txt") checks)"

echo "== DIFF (wine vs moteur) — toute ligne differente est un ecart de fidelite ou une divergence voulue =="
if [ -f "$OUT/wine.txt" ] && [ -f "$OUT/rt.txt" ]; then
    diff <(sort "$OUT/wine.txt") <(sort "$OUT/rt.txt") && echo "  IDENTICAL — parite parfaite"
else
    echo "  (un cote manque — voir $OUT/rt.log)"; tail -20 "$OUT/rt.log"
fi
