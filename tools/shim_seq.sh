#!/usr/bin/env bash
# tools/shim_seq.sh — compare la table de shims de l'arbre courant a celle d'une
# autre revision. C'est LE controle qui protege un deplacement de shims (Win32
# ET hooks natifs poses via set_alternate()+shim_trap(), meme table).
# Porte depuis carn-vita (meme auteur, 2026-09-09) en vue de l'extraction des
# hooks de tools/rt_boot.cpp (Phase 3 du decoupage winx86/d2vita).
#
#   tools/shim_seq.sh                # compare a HEAD (regression pure)
#   tools/shim_seq.sh <git-ref>      # compare a cette revision
#   ALLOW_REORDER=1 tools/shim_seq.sh <ref>   # tolere une permutation de blocs
#                                             # A CONDITION que la table
#                                             # effective soit identique
#   ALLOW_BODY=<fichier> ...         # excuse des corps modifies, mais SEULEMENT
#                                    # la transition exacte qui y est ecrite
#                                    # (cle, ancienne empreinte, nouvelle) —
#                                    # une derive ulterieure redevient fatale.
#
# Trois verdicts, du plus grave au moins grave :
#   ENSEMBLE  une cle apparait/disparait, ou change de nombre d'inscriptions.
#             Toujours fatal : la table n'a pas le meme contenu.
#   EFFECTIF  meme jeu de cles, mais pour au moins une cle c'est un AUTRE corps
#             qui gagne. Toujours fatal : register_shim ecrase la cle, donc
#             c'est exactement le bug qu'un deplacement de blocs fabrique, et
#             que tools/torture ne voit pas.
#   ORDRE     meme table effective, mais la sequence differe (permutation de
#             blocs). Fatal par defaut ; ALLOW_REORDER=1 l'accepte, ce qui est
#             legitime pour un refactor qui deplace des blocs entiers.
#
# Sortie non nulle des qu'un verdict n'est pas au vert, avec la CLE fautive
# nommee — jamais un simple "ca a change".
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REF="${1:-HEAD}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

python3 "$ROOT/tools/shim_seq.py"            > "$TMP/cur.txt" 2>"$TMP/cur.err" || {
    echo "FAIL: reconstruction de l'arbre courant impossible"; cat "$TMP/cur.err"; exit 2; }
python3 "$ROOT/tools/shim_seq.py" --ref "$REF" > "$TMP/ref.txt" 2>"$TMP/ref.err" || {
    echo "FAIL: reconstruction de $REF impossible"; cat "$TMP/ref.err"; exit 2; }

ncur=$(wc -l < "$TMP/cur.txt"); nref=$(wc -l < "$TMP/ref.txt")
echo "== shim_seq : $REF ($nref inscriptions) -> arbre courant ($ncur) =="
# Un rapport vide passerait tous les tests suivants : on le refuse d'emblee.
if [ "$ncur" -lt 100 ] || [ "$nref" -lt 100 ]; then
    echo "FAIL: sequence invraisemblablement courte — le parseur n'a rien vu"; exit 2
fi

rc=0
# --- ENSEMBLE ---
cut -f1 "$TMP/ref.txt" | sort > "$TMP/ref.keys"
cut -f1 "$TMP/cur.txt" | sort > "$TMP/cur.keys"
if ! diff -q "$TMP/ref.keys" "$TMP/cur.keys" >/dev/null; then
    echo "ENSEMBLE: DIVERGENT"
    diff "$TMP/ref.keys" "$TMP/cur.keys" | grep -E '^[<>]' | sed 's/^</  disparue: /;s/^>/  apparue : /' | head -40
    rc=2
else
    echo "ENSEMBLE: identique ($ncur inscriptions, cles et multiplicites)"
fi

# --- EFFECTIF : pour chaque cle, le corps de la DERNIERE inscription ---
awk -F'\t' '{ w[$1]=$2 } END { for (k in w) print k"\t"w[k] }' "$TMP/ref.txt" | sort > "$TMP/ref.win"
awk -F'\t' '{ w[$1]=$2 } END { for (k in w) print k"\t"w[k] }' "$TMP/cur.txt" | sort > "$TMP/cur.win"
# Transitions de corps explicitement documentees : cle + empreinte AVANT +
# empreinte APRES. Rien d'autre n'est excuse.
# Sentinelle : un fichier VIDE en premier argument d'awk ferait passer NR==FNR
# pour vrai sur tout le second fichier — toutes les divergences seraient alors
# silencieusement "autorisees". C'est exactement le controle qui rend PASS sans
# rien mesurer ; la sentinelle l'interdit.
printf '\t\t\n' > "$TMP/allow"
if [ -n "${ALLOW_BODY:-}" ]; then
    [ -f "$ALLOW_BODY" ] || { echo "FAIL: ALLOW_BODY=$ALLOW_BODY introuvable"; exit 2; }
    grep -vE '^\s*(#|$)' "$ALLOW_BODY" | awk '{print $1"\t"$2"\t"$3}' >> "$TMP/allow"
fi
join -t $'\t' -j1 "$TMP/ref.win" "$TMP/cur.win" > "$TMP/pairs" 2>/dev/null
awk -F'\t' 'NR==FNR{a[$1"\t"$2"\t"$3]=1;next} $2!=$3 && !($1"\t"$2"\t"$3 in a){print}' \
    "$TMP/allow" "$TMP/pairs" > "$TMP/badbody"
awk -F'\t' 'NR==FNR{a[$1"\t"$2"\t"$3]=1;next} $2!=$3 && ($1"\t"$2"\t"$3 in a){print}' \
    "$TMP/allow" "$TMP/pairs" > "$TMP/okbody"
comm -3 <(cut -f1 "$TMP/ref.win") <(cut -f1 "$TMP/cur.win") > "$TMP/onlyone"
if [ -s "$TMP/badbody" ] || [ -s "$TMP/onlyone" ]; then
    echo "EFFECTIF: DIVERGENT — le corps gagnant a change pour :"
    awk -F'\t' '{print "  "$1"  ("$2" -> "$3")"}' "$TMP/badbody" | head -40
    sed 's/^/  cle absente d un cote: /' "$TMP/onlyone" | head -10
    rc=2
else
    n=$(wc -l < "$TMP/cur.win"); k=$(wc -l < "$TMP/okbody")
    if [ "$k" -gt 0 ]; then
        echo "EFFECTIF: identique sur $((n-k)) cles ; $k corps modifies, tous documentes dans $ALLOW_BODY :"
        awk -F'\t' '{print "  "$1}' "$TMP/okbody"
    else
        echo "EFFECTIF: identique (meme corps gagnant pour les $n cles)"
    fi
fi

# --- ORDRE ---
if diff -q "$TMP/ref.txt" "$TMP/cur.txt" >/dev/null; then
    echo "ORDRE: identique"
else
    first=$(diff <(cut -f1 "$TMP/ref.txt") <(cut -f1 "$TMP/cur.txt") | grep -E '^[<>]' | head -1 | sed 's/^[<>] //')
    nblk=$(diff <(cut -f1 "$TMP/ref.txt") <(cut -f1 "$TMP/cur.txt") | grep -cE '^[0-9]')   # zones de diff, pas blocs source
    if [ "${ALLOW_REORDER:-0}" = "1" ] && [ "$rc" -eq 0 ]; then
        echo "ORDRE: $nblk zones deplacees, table effective inchangee (ALLOW_REORDER=1)"
        echo "       premiere cle deplacee : $first"
    else
        echo "ORDRE: DIVERGENT ($nblk zones), premiere cle deplacee : $first"
        rc=1
    fi
fi
[ "$rc" -eq 0 ] && echo "OK" || echo "ECHEC (rc=$rc)"
exit $rc
