#!/usr/bin/env bash
# tools/selftest.sh — les auto-controles du moteur qui tournent SUR BUREAU.
#
# POURQUOI CE SCRIPT EXISTE
# -------------------------
# tools/present_scale_selftest.cpp est le seul filet qui couvre la couche de
# presentation, laquelle n'est compilee NI sur bureau NI sous qemu chez les
# portages (elle vit sous #ifdef __vita__). Ce filet existait mais sa ligne de
# compilation n'etait ecrite nulle part : il fallait la retrouver a chaque fois,
# donc en pratique personne ne le rejouait. Un filet qu'on ne relance pas ne
# protege rien.
#
#   bash tools/selftest.sh
#
# CODE DE SORTIE : 0 = tout passe. Toute autre valeur = un echec reel ; le
# script n'imprime JAMAIS « ok » sans avoir vu le programme rendre 0.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${OUT:-$ROOT/build-selftest}"
CXX="${CXX:-g++}"
mkdir -p "$OUT" || exit 2
fail=0

run() { # $1 = nom, $2... = sources
  local nom="$1"; shift
  echo "== $nom =="
  if ! $CXX -std=c++17 -O2 -Wall -I"$ROOT/src" -o "$OUT/$nom" "$@" 2>"$OUT/$nom.build.log"; then
    echo "ECHEC de compilation :"; cat "$OUT/$nom.build.log"; fail=1; return
  fi
  if "$OUT/$nom"; then :; else echo "ECHEC a l'execution (rc=$?)"; fail=1; fi
}

run present_scale "$ROOT/tools/present_scale_selftest.cpp" "$ROOT/src/platform/present_scale.cpp"

if [ "$fail" -eq 0 ]; then echo "SELFTEST: PASS"; else echo "SELFTEST: FAIL"; fi
exit "$fail"
