#!/usr/bin/env python3
# tools/gen_shim_list.py — GENERE docs/shims.md a partir des sources.
#
# POURQUOI CET OUTIL EXISTE
# -------------------------
# La liste des shims que le moteur fournit est exactement le genre de page qui
# pourrit : ecrite a la main, elle est fausse au deuxieme commit, et une liste
# fausse est PIRE que pas de liste (on croit savoir ce que le moteur couvre).
# Elle est donc generee depuis les sources, et la CI refuse un commit ou la
# page et les sources ont diverge (`--check`).
#
# CE QU'IL FAIT
# -------------
# Il lit chaque `src/runtime/win32_shims_*.cpp`, y trouve les fonctions
# d'installation `win32_shims_<groupe>_install(...)`, et en extrait la sequence
# des inscriptions via le parseur DEJA EPROUVE de tools/shim_seq.py -- le meme
# qui sert de filet aux deplacements de shims. Pas de deuxieme analyseur a
# maintenir : si shim_seq.py sait lire une inscription, cette page la connait.
#
# LES DOUBLONS SONT RENDUS VISIBLES, PAS MASQUES
# ----------------------------------------------
# `Bridge::register_shim` ECRASE la cle : inscrire deux fois le meme nom est
# silencieux et c'est la DERNIERE inscription qui gagne. Une page qui
# dedoublonnerait en silence cacherait precisement ce qu'on veut voir. Les
# clefs inscrites plusieurs fois sont donc comptees ET signalees.
#
# USAGE
#   tools/gen_shim_list.py            # (re)ecrit docs/shims.md
#   tools/gen_shim_list.py --check    # ne rien ecrire ; sortie 1 si divergence
#   tools/gen_shim_list.py --stdout   # imprime sans ecrire
import argparse, collections, glob, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import shim_seq as S                                        # noqa: E402

OUT = os.path.join(ROOT, 'docs', 'shims.md')
RE_INSTALL = re.compile(r'\bvoid\s+(win32_shims_\w+_install)\s*\(')

# Rappel de ce que chaque DLL couvre, par NOM DE DLL (plusieurs DLL peuvent
# vivre dans un meme fichier). Purement redactionnel : une DLL absente d'ici
# est quand meme listee, sans phrase d'introduction.
BLURB = {
    'ADVAPI32.dll': "Registre, ACL et descripteurs de sécurité, jetons, gestionnaire de services — en « succès neutre » : le moteur n'a aucun modèle de sécurité à faire respecter, et un invité qui interroge ces API attend surtout qu'elles n'échouent pas.",
    'GDI32.dll':    "Objets GDI simulés par des poignées factices : palettes, polices, pinceaux, contextes de périphérique.",
    'USER32.dll':   "Géométrie de rectangles, chaînes, métriques d'écran, sémantique de fenêtre générique (peinture, contexte de périphérique, curseur) — la part de USER32 qui ne dépend ni d'une vraie fenêtre système ni de l'état clavier.",
    'DDRAW.dll':    "DirectDraw réduit à ce qu'un invité doit voir pour ne pas s'arrêter : le moteur ne dessine pas via DirectDraw.",
    'binkw32.dll':  "Lecteur vidéo Bink — répond « pas de vidéo » de façon cohérente plutôt que d'échouer.",
    'smackw32.dll': "Lecteur vidéo Smacker, même principe que Bink.",
    'ijl11.dll':    "Décodeur JPEG Intel.",
    'IMM32.dll':    "Éditeur de méthode d'entrée (IME) — toujours rapporté absent, ce qui est la vérité sur les plateformes visées.",
    'PSAPI.DLL':    "Énumération des modules du processus.",
    'KERNEL32.dll': "Les quelques entrées d'énumération de modules que KERNEL32 expose en doublon de PSAPI (`K32*`).",
    'SHELL32.dll':  "Deux appels shell sans effet observable pour l'invité.",
    'VERSION.dll':  "Ressources de version d'un fichier (`VS_FIXEDFILEINFO`).",
    'WSOCK32.dll':  "Couche socket complète, traduite vers POSIX : table de poignées, primitives BSD, résolution de noms, ordre des octets. Aucune logique propre à un jeu — ce qu'un portage veut observer ou dévier passe par l'observateur et la route (`wx86_net_set_observer`, `wx86_net_set_redirect`).",
    'WS2_32.dll':   "Les mêmes fonctions que WSOCK32.dll, sous des ordinaux différents — les deux DLL ne s'accordent pas sur la numérotation, d'où deux entrées par fonction.",
}


# Un ordinal nu (`#4`) n'apprend rien a un lecteur. Les fonctions inscrites par
# ordinal le sont via une lambda NOMMEE (`connect_fn`, `htons_fn`...) : le nom
# vient donc de la source elle-meme, pas d'une table recopiee a la main qui
# pourrait mentir. Un ordinal dont la lambda n'est pas nommee ainsi reste
# affiche nu — mieux vaut un trou visible qu'un nom invente.
RE_ORD_NAMED = re.compile(
    r'\b\w+\s*\(\s*"([A-Za-z0-9_.]+)"\s*,\s*(\d+)\s*,\s*\d+\s*,\s*([A-Za-z_]\w*)_fn\s*\)')


def ordinal_names(src_raw):
    """Rend {(dll, ordinal): nom} deduit des lambdas nommees de la source."""
    out = {}
    for dll, ord_, name in RE_ORD_NAMED.findall(src_raw):
        out[(dll, int(ord_))] = name
    return out


def collect():
    """Rend les inscriptions dans l'ordre + l'inventaire des unites."""
    regs = []                                   # (dll, entree, fichier) dans l'ordre
    units = []                                  # (fichier, fonction, n)
    names = {}                                  # (dll, ordinal) -> nom
    for path in sorted(glob.glob(os.path.join(ROOT, 'src/runtime/win32_shims_*.cpp'))):
        rel = os.path.relpath(path, ROOT)
        raw = open(path, encoding='utf-8', errors='surrogateescape').read()
        names.update(ordinal_names(raw))
        src = S.strip_comments(raw)
        for fn in RE_INSTALL.findall(src):
            body = S.unit_body(src, fn)
            if body is None:
                sys.exit('%s: %s introuvable' % (rel, fn))
            keys = S.keys_of(body, rel)
            units.append((rel, fn, len(keys)))
            for key, _fp, _label in keys:
                dll, entry = key.split('!', 1)
                regs.append((dll, entry, rel))
    return regs, units, names


def sort_key(entry):
    """Les ordinaux (#12) se trient en nombre, les noms en alphabetique."""
    if entry.startswith('#'):
        return (0, int(entry[1:]), '')
    return (1, 0, entry.lower())


def render():
    regs, units, names = collect()
    by_dll = collections.OrderedDict()
    for dll, entry, rel in regs:
        by_dll.setdefault(dll, []).append((entry, rel))

    dup_total = 0
    L = []
    L.append('<!-- PAGE GENEREE PAR tools/gen_shim_list.py — NE PAS EDITER A LA MAIN. -->')
    L.append('<!-- Regenerer : tools/gen_shim_list.py   /   Verifier : tools/gen_shim_list.py --check -->')
    L.append('')
    L.append('# Shims Win32 fournis par le moteur')
    L.append('')
    L.append('Cette page est **générée depuis les sources** par '
             '`tools/gen_shim_list.py`, et la CI échoue si elle diverge de '
             '`src/runtime/win32_shims_*.cpp`. Elle dit ce que le moteur '
             'couvre **aujourd\'hui** — pas ce qu\'on aimerait qu\'il couvre.')
    L.append('')
    L.append('!!! warning "Une DLL listée ici n\'est pas une DLL *complète*"')
    L.append('    Le moteur ne fournit que les fonctions dont il a été prouvé '
             'qu\'elles ne portent **aucun** comportement propre à un jeu. Une '
             'même DLL peut donc être servie pour moitié par le moteur et pour '
             'moitié par le portage, fonction par fonction. Voir '
             '[Point d\'extension](extension.md).')
    L.append('')

    total = len(regs)
    uniq = len({(d, e) for d, e, _ in regs})
    L.append('| | |')
    L.append('|---|---|')
    L.append('| Inscriptions | %d |' % total)
    L.append('| Clés distinctes | %d |' % uniq)
    L.append('| DLL couvertes | %d |' % len(by_dll))
    L.append('| Unités d\'installation | %d |' % len(units))
    L.append('')

    for dll in sorted(by_dll, key=str.lower):
        entries = by_dll[dll]
        counts = collections.Counter(e for e, _ in entries)
        dups = {e for e, n in counts.items() if n > 1}
        dup_total += len(dups)
        L.append('## %s' % dll)
        L.append('')
        groups = sorted({rel for _, rel in entries})
        if dll in BLURB:
            L.append(BLURB[dll])
            L.append('')
        L.append('%d inscription%s, %d clé%s distincte%s — `%s`'
                 % (len(entries), 's' if len(entries) > 1 else '',
                    len(counts), 's' if len(counts) > 1 else '',
                    's' if len(counts) > 1 else '',
                    '`, `'.join(groups)))
        L.append('')
        seen = set()
        cells = []
        for e, _rel in sorted(entries, key=lambda t: sort_key(t[0])):
            if e in seen:
                continue
            seen.add(e)
            label = '`%s`' % e
            if e.startswith('#'):
                nm = names.get((dll, int(e[1:])))
                if nm:
                    label = '`%s` %s' % (e, nm)
            cells.append('%s%s' % (label, ' ⚠️' if e in dups else ''))
        # 3 colonnes, lisible sans defilement horizontal
        L.append('| | | |')
        L.append('|---|---|---|')
        for i in range(0, len(cells), 3):
            row = cells[i:i + 3] + [''] * (3 - len(cells[i:i + 3]))
            L.append('| %s |' % ' | '.join(row))
        L.append('')
        if dups:
            L.append('!!! danger "Clé%s inscrite%s plusieurs fois (⚠️)"'
                     % ('s' if len(dups) > 1 else '', 's' if len(dups) > 1 else ''))
            L.append('    %s. `Bridge::register_shim` **écrase** : c\'est la '
                     '**dernière** inscription qui gagne, silencieusement. Ces '
                     'doublons sont hérités et volontairement conservés dans '
                     'leur ordre d\'origine pour que le corps gagnant reste le '
                     'même — voir l\'en-tête du fichier concerné.'
                     % ', '.join('`%s`' % e for e in sorted(dups)))
            L.append('')

    L.append('## Unités d\'installation')
    L.append('')
    L.append('Chaque groupe expose une fonction `install` que le portage appelle '
             'depuis son propre binaire de boot. Aucune n\'est appelée '
             'automatiquement : le moteur ne décide pas à la place du portage '
             'quels shims celui-ci veut.')
    L.append('')
    L.append('| Fonction | Fichier | Inscriptions |')
    L.append('|---|---|---|')
    for rel, fn, n in sorted(units, key=lambda t: t[1]):
        L.append('| `%s` | `%s` | %d |' % (fn, rel, n))
    L.append('')
    return '\n'.join(L) + '\n', total, uniq, dup_total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true',
                    help='ne rien ecrire ; sortie 1 si docs/shims.md a diverge')
    ap.add_argument('--stdout', action='store_true', help='imprimer sans ecrire')
    a = ap.parse_args()
    text, total, uniq, dups = render()

    if a.stdout:
        sys.stdout.write(text)
        return 0
    if a.check:
        cur = open(OUT, encoding='utf-8').read() if os.path.exists(OUT) else None
        if cur == text:
            sys.stderr.write('docs/shims.md a jour (%d inscriptions, %d cles)\n' % (total, uniq))
            return 0
        sys.stderr.write('docs/shims.md est PERIME : les sources ont change.\n'
                         'Regenerer avec  tools/gen_shim_list.py  et commiter le resultat.\n')
        return 1
    open(OUT, 'w', encoding='utf-8').write(text)
    sys.stderr.write('docs/shims.md ecrit : %d inscriptions, %d cles, %d doublon(s)\n'
                     % (total, uniq, dups))
    return 0


if __name__ == '__main__':
    sys.exit(main())
