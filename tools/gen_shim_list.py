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
    'ADVAPI32.dll': "Registry, ACLs and security descriptors, tokens, service manager — in \"neutral success\" mode: the engine has no security model to enforce, and a guest querying these APIs mostly expects them not to fail.",
    'GDI32.dll':    "GDI objects simulated with fake handles: palettes, fonts, brushes, device contexts.",
    'USER32.dll':   "Rectangle geometry, strings, screen metrics, generic window semantics (painting, device context, cursor) — the part of USER32 that depends on neither a real system window nor keyboard state.",
    'DDRAW.dll':    "DirectDraw reduced to what a guest needs to see to not stop: the engine doesn't draw via DirectDraw.",
    'binkw32.dll':  "Bink video player — answers \"no video\" consistently instead of failing.",
    'smackw32.dll': "Smacker video player, same principle as Bink.",
    'ijl11.dll':    "Intel JPEG decoder.",
    'IMM32.dll':    "Input Method Editor (IME) — always reported absent, which is the truth on the targeted platforms.",
    'PSAPI.DLL':    "Process module enumeration.",
    'KERNEL32.dll': "The few module-enumeration entries KERNEL32 exposes as duplicates of PSAPI (`K32*`).",
    'SHELL32.dll':  "Two shell calls with no observable effect for the guest.",
    'VERSION.dll':  "A file's version resources (`VS_FIXEDFILEINFO`).",
    'WSOCK32.dll':  "Complete socket layer, translated to POSIX: handle table, BSD primitives, name resolution, byte order. No game-specific logic — whatever a port wants to observe or divert goes through the observer and the route (`wx86_net_set_observer`, `wx86_net_set_redirect`).",
    'WS2_32.dll':   "The same functions as WSOCK32.dll, under different ordinals — the two DLLs disagree on numbering, hence two entries per function.",
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
    L.append('<!-- PAGE GENERATED BY tools/gen_shim_list.py — DO NOT HAND-EDIT. -->')
    L.append('<!-- Regenerate: tools/gen_shim_list.py   /   Check: tools/gen_shim_list.py --check -->')
    L.append('')
    L.append('# Win32 shims provided by the engine')
    L.append('')
    L.append('This page is **generated from the sources** by '
             '`tools/gen_shim_list.py`, and CI fails if it drifts from '
             '`src/runtime/win32_shims_*.cpp`. It states what the engine '
             'covers **today** — not what we\'d like it to cover.')
    L.append('')
    L.append('!!! warning "A DLL listed here isn\'t a *complete* DLL"')
    L.append('    The engine only provides functions proven to carry '
             '**no** game-specific behavior. The same DLL can therefore be '
             'served half by the engine and half by the port, function by '
             'function. See [Extension point](extension.md).')
    L.append('')

    total = len(regs)
    uniq = len({(d, e) for d, e, _ in regs})
    L.append('| | |')
    L.append('|---|---|')
    L.append('| Registrations | %d |' % total)
    L.append('| Distinct keys | %d |' % uniq)
    L.append('| DLLs covered | %d |' % len(by_dll))
    L.append('| Install units | %d |' % len(units))
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
        L.append('%d registration%s, %d distinct key%s — `%s`'
                 % (len(entries), 's' if len(entries) > 1 else '',
                    len(counts), 's' if len(counts) > 1 else '',
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
            L.append('!!! danger "Key%s registered multiple times (⚠️)"'
                     % ('s' if len(dups) > 1 else ''))
            L.append('    %s. `Bridge::register_shim` **overwrites**: it\'s '
                     'the **last** registration that wins, silently. These '
                     'duplicates are inherited and deliberately kept in '
                     'their original order so the winning body stays the '
                     'same — see the file\'s own header.'
                     % ', '.join('`%s`' % e for e in sorted(dups)))
            L.append('')

    L.append('## Install units')
    L.append('')
    L.append('Each group exposes an `install` function that the port calls '
             'from its own boot binary. None is called automatically: the '
             'engine doesn\'t decide on the port\'s behalf which shims it '
             'wants.')
    L.append('')
    L.append('| Function | File | Registrations |')
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
