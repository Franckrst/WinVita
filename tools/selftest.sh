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

# EXTRA : drapeaux supplementaires pour un controle donne (assainisseurs).
run() { # $1 = nom, $2... = sources
  local nom="$1"; shift
  echo "== $nom =="
  # shellcheck disable=SC2086
  if ! $CXX -std=gnu++17 -O2 -Wall -Wextra -Werror ${EXTRA:-} -I"$ROOT/src" -o "$OUT/$nom" "$@" 2>"$OUT/$nom.build.log"; then
    echo "ECHEC de compilation :"; cat "$OUT/$nom.build.log"; fail=1; return
  fi
  if "$OUT/$nom"; then :; else echo "ECHEC a l'execution (rc=$?)"; fail=1; fi
}

EXTRA="-Wno-unused-parameter" \
  run present_scale "$ROOT/tools/present_scale_selftest.cpp" "$ROOT/src/platform/present_scale.cpp"

# Le clavier virtuel : en-tete AUTONOME, donc entierement prouvable ici.
# 1 297 verifications — les 95 ASCII imprimables, le dessin borne au panneau,
# tactile == dessin, MAJ a trois etats, mode masque, bornes de l'echo, et un
# etat volontairement incoherent qui ne doit ni planter ni deborder.
EXTRA="-O1 -g -fsanitize=address,undefined" \
  run kb "$ROOT/tools/tests/kb_test.cpp"

# Le verrou de sortie couvre-t-il la RESOLUTION DE NOM ? Une requete DNS est
# deja un depart, et le verrou ne s'appliquait qu'apres la resolution. Ce
# filet n'interroge aucun hote public : sa seule resolution reelle porte sur
# « localhost ».
run net_resolve_guard "$ROOT/tools/net_resolve_guard_selftest.cpp" \
                      "$ROOT/src/runtime/net_nonblock.cpp" \
                      "$ROOT/src/runtime/net_guard.cpp"

# GARDE DE SECRET : le texte tape ne doit atteindre AUCUNE sortie.
echo "== garde de secret du clavier =="
if grep -nE '\b(printf|fprintf|sprintf|snprintf|puts|fputs|fwrite|d2vita_progress|wx86_vita_progress|jpline)\b' \
        "$ROOT/src/platform/vita_kb.h"; then
  echo "   ECHEC: vita_kb.h contient un appel de sortie"; fail=1
else echo "   OK: vita_kb.h sans appel de sortie"; fi

# La carte des familles du profil : en-tete AUTONOME, donc prouvable ici. Le
# classement vivait dans cpu_box86.cpp, qui ne se compile que pour ARM/Vita —
# aucun oracle ne pouvait l'exercer.
run prof_map "$ROOT/tools/prof_map_selftest.cpp"

if [ "$fail" -eq 0 ]; then echo "SELFTEST: PASS"; else echo "SELFTEST: FAIL"; fi
exit "$fail"
