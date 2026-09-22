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
run authenticode "$ROOT/tools/authenticode_selftest.cpp" "$ROOT/src/runtime/authenticode.cpp"
run prof_map "$ROOT/tools/prof_map_selftest.cpp"

# La frequence du melangeur : elle vient de l'appelant, plus d'une constante.
# Le puits WAV et le socle DirectSound se compilent sur bureau, donc ce filet
# fabrique deux fichiers a deux frequences et relit leurs en-tetes.
# -Wno-* : ds_emul.cpp et audio_sink_host.cpp portent des avertissements de
# STYLE anterieurs (indentation trompeuse, aide inutilisee). Les corriger ici
# melangerait un nettoyage a un filet ; on les tait pour ce filet-la seulement.
EXTRA="-Wno-misleading-indentation -Wno-unused-function" \
run ds_rate "$ROOT/tools/ds_rate_selftest.cpp" \
            "$ROOT/src/runtime/ds_emul.cpp" \
            "$ROOT/src/runtime/audio_sink_host.cpp" \
            "$ROOT/src/runtime/guest_scratch.cpp" \
            "$ROOT/src/runtime/host_clock.cpp" \
            "$ROOT/src/runtime/gil.cpp" \
            "$ROOT/src/platform/vita_host.cpp"

# La table des alternates : unite FEUILLE (src/dynarec86/alt_table.c), donc
# compilable ici. -Dmalloc=wx86_selftest_malloc n'est pose QUE sur elle :
# l'injection de faute porte sur l'allocateur, le corps teste est celui de la
# bibliotheque. Voir l'en-tete de tools/alt_table_selftest.cpp.
# Compilee en C (comme dans la bibliotheque) : en C++, -Dmalloc casserait le
# `using std::malloc` de <stdlib.h>.
echo "== alt_table =="
if ! ${CC:-gcc} -std=gnu17 -O2 -Wall -Wextra -Werror -I"$ROOT/src/dynarec86/shim" \
      -Dmalloc=wx86_selftest_malloc -c "$ROOT/src/dynarec86/alt_table.c" \
      -o "$OUT/alt_table.o" 2>"$OUT/alt_table.build.log"; then
  echo "ECHEC de compilation :"; cat "$OUT/alt_table.build.log"; fail=1
elif ! $CXX -std=gnu++17 -O2 -Wall -Wextra -Werror \
      "$ROOT/tools/alt_table_selftest.cpp" "$OUT/alt_table.o" -o "$OUT/alt_table" \
      2>>"$OUT/alt_table.build.log"; then
  echo "ECHEC de compilation :"; cat "$OUT/alt_table.build.log"; fail=1
elif ! "$OUT/alt_table"; then
  echo "ECHEC a l'execution"; fail=1
fi

# La politique de croissance de la PISCINE JIT (mman_vita.c). Ce fichier ne se
# compile QUE pour la console : ni le CMake bureau ni l'oracle qemu-arm ne le
# voient (qemu lie le mmap de Linux). La seule verification qu'il ait jamais
# eue, c'etait de livrer et de lire les rapports de plantage — et ils ont
# parle : sur 8 consoles 0.1.6, le 2e segment de piscine n'a ete demande
# qu'une fois, a 2690 s, et refuse ; une traduction sans memoire tue le fil
# invite (pas d'interpreteur dans ce build).
# Ce filet compile le VRAI mman_vita.c contre un faux noyau
# (tools/tests/fake_psp2/) qui modelise le budget user ET le retrecissement de
# l'espace d'adressage VM — c'est ce second cadran qui reproduit le defaut :
# 2 Mo refuses alors que 5120 Ko sont annonces libres.
# En C, comme dans la bibliotheque (le fichier ne compile pas en C++).
echo "== jitpool =="
if ! ${CC:-gcc} -std=gnu11 -O1 -g -Wall -Wextra \
      -I"$ROOT/tools/tests/fake_psp2" -I"$ROOT/src/dynarec86/shim/vita" \
      -o "$OUT/jitpool" "$ROOT/tools/jitpool_selftest.c" \
      "$ROOT/src/dynarec86/shim/vita/mman_vita.c" -lpthread \
      2>"$OUT/jitpool.build.log"; then
  echo "ECHEC de compilation :"; cat "$OUT/jitpool.build.log"; fail=1
elif ! "$OUT/jitpool" >"$OUT/jitpool.run.log" 2>&1; then
  echo "ECHEC a l'execution :"; cat "$OUT/jitpool.run.log"; fail=1
else
  echo "   OK: 9 scenarios (6 piscine JIT, 3 piscine RW des metadonnees)"
fi

if [ "$fail" -eq 0 ]; then echo "SELFTEST: PASS"; else echo "SELFTEST: FAIL"; fi
exit "$fail"
