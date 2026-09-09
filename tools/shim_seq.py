#!/usr/bin/env python3
# tools/shim_seq.py — reconstruit la SEQUENCE ORDONNEE des shims Win32 inscrits.
# Porte depuis carn-vita (meme auteur, meme motif de code, 2026-09-09) en vue de
# l'extraction des hooks natifs (D2_CELLOPT, DCC, lightgrid, RLE, collision...)
# hors de tools/rt_boot.cpp — c'est le filet qui doit couvrir CETTE tache-la,
# comme il a couvert la fusion wt-d3d dans carn-vita.
#
# POURQUOI CET OUTIL EXISTE
# -------------------------
# Bridge::register_shim ECRASE la cle : quand un nom est inscrit deux fois, c'est
# la DERNIERE inscription qui gagne. Deplacer un bloc de shims — ou un hook natif
# pose via cpu->set_alternate()+br.shim_trap() — est donc une operation qui peut
# changer la table effective SANS casser la compilation et SANS que rien ne le
# dise. Ce script est le controle qui le voit.
#
# CE QU'IL FAIT
# -------------
# Il lit les sources et rend, dans l'ordre d'appel reel, une ligne par
# inscription :   <cle>\t<empreinte du corps>
# La cle est "DLL.dll!Nom" ou "DLL.dll!#ordinal". L'empreinte est un sha1 du
# texte de l'appel, commentaires et espaces retires : deux corps identiques ont
# la meme empreinte, un corps modifie ne peut pas passer inapercu.
#
# La correspondance helper -> DLL n'est PAS codee en dur : elle est DEDUITE du
# corps de chaque lambda helper (`auto K=[&](...){ ... br.register_shim(...) }`).
# Un helper qui inscrit dans deux DLL — c'est le cas de W(), qui sert WSOCK32 ET
# WS2_32 — rend donc bien DEUX cles. `--explain` imprime la table deduite.
#
# USAGE
#   tools/shim_seq.py                      # arbre courant
#   tools/shim_seq.py --ref <git-ref>      # etat a cette revision
#   tools/shim_seq.py --explain            # + table helper -> DLL deduite
# La comparaison de deux etats se fait par tools/shim_seq.sh.
import argparse, hashlib, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Unites de shims, dans l'ordre ou rt_boot.cpp les appelle. L'ordre REEL est
# celui des appels releves dans rt_boot.cpp, pas celui de cette liste : la liste
# ne sert qu'a savoir quel fichier ouvrir pour quelle fonction.
# Peuplee au fil des extractions (meme mecanique que carn-vita) : cle = nom
# de la fonction d'installation appelee depuis main(), valeur = fichier ou
# elle vit. rt_boot.cpp reste sinon monolithique (K/U/GD/SH/A/W/Uc/REG/REGORD/
# GL sont des lambdas LOCALES a main()).
UNITS = {
    'native_hooks_codec_install_celwatch': 'src/runtime/native_hooks_codec.cpp',
    'native_hooks_codec_install_codecs':   'src/runtime/native_hooks_codec.cpp',
    'native_hooks_codec_install_post':     'src/runtime/native_hooks_codec.cpp',
    'native_hooks_cellengine_install_blit': 'src/runtime/native_hooks_cellengine.cpp',
    'native_hooks_cellengine_install_rest': 'src/runtime/native_hooks_cellengine.cpp',
}

def strip_comments(s):
    """Retire // et /* */ sans toucher au contenu des chaines et des caracteres."""
    out=[]; i=0; n=len(s)
    while i<n:
        c=s[i]
        if c=='"' or c=="'":
            q=c; out.append(c); i+=1
            while i<n:
                if s[i]=='\\': out.append(s[i:i+2]); i+=2; continue
                out.append(s[i])
                if s[i]==q: i+=1; break
                i+=1
            continue
        if c=='/' and i+1<n and s[i+1]=='/':
            while i<n and s[i]!='\n': i+=1
            continue
        if c=='/' and i+1<n and s[i+1]=='*':
            i+=2
            while i+1<n and not (s[i]=='*' and s[i+1]=='/'): i+=1
            i+=2; continue
        out.append(c); i+=1
    return ''.join(out)

def match_paren(s, i):
    """i pointe sur '('. Rend l'index de la ')' correspondante (chaines respectees)."""
    d=0; n=len(s)
    while i<n:
        c=s[i]
        if c=='"' or c=="'":
            q=c; i+=1
            while i<n:
                if s[i]=='\\': i+=2; continue
                if s[i]==q: break
                i+=1
        elif c=='(': d+=1
        elif c==')':
            d-=1
            if d==0: return i
        i+=1
    raise ValueError('parenthese non fermee a %d'%i)

def match_brace(s, i):
    """i pointe sur '{'. Rend l'index de la '}' correspondante."""
    d=0; n=len(s)
    while i<n:
        c=s[i]
        if c=='"' or c=="'":
            q=c; i+=1
            while i<n:
                if s[i]=='\\': i+=2; continue
                if s[i]==q: break
                i+=1
        elif c=='{': d+=1
        elif c=='}':
            d-=1
            if d==0: return i
        i+=1
    raise ValueError('accolade non fermee a %d'%i)

STR = r'"((?:[^"\\]|\\.)*)"'
RE_HELPER  = re.compile(r'\bauto\s+([A-Za-z_]\w*)\s*=\s*\[&?\]\s*\(')
RE_REGSHIM = re.compile(r'\bbr\.register_shim(_ordinal)?\s*\(')

def helper_targets(body, params):
    """Deduit ce qu'un helper inscrit : liste de (dll, keykind, keysrc).
    dll  : nom litteral, ou '$0' si le helper prend la DLL en 1er parametre.
    keykind : 'name' ou 'ord'. keysrc : index de l'argument porteur de la cle."""
    dll_is_param = params and params[0][1] == 'dll'
    tgts=[]
    for m in RE_REGSHIM.finditer(body):
        ordinal = bool(m.group(1))
        op = m.end()-1
        args = split_args(body[op+1:match_paren(body, op)])
        if not args: continue
        a0 = args[0].strip()
        lit = re.fullmatch(STR, a0)
        if lit:                       dll = lit.group(1)
        elif a0 == 'dll' and dll_is_param: dll = '$0'
        else:                         continue
        # l'argument porteur de la cle est le parametre `name`/`ord` du helper
        keyidx = 1 if dll_is_param else 0
        tgts.append((dll, 'ord' if ordinal else 'name', keyidx))
    # dedoublonne en gardant l'ordre (W() inscrit WSOCK32 puis WS2_32)
    seen=set(); out=[]
    for t in tgts:
        if t in seen: continue
        seen.add(t); out.append(t)
    return out

def split_args(s):
    args=[]; d=0; cur=''; i=0; n=len(s)
    while i<n:
        c=s[i]
        if c=='"' or c=="'":
            q=c; cur+=c; i+=1
            while i<n:
                if s[i]=='\\': cur+=s[i:i+2]; i+=2; continue
                cur+=s[i]
                if s[i]==q: i+=1; break
                i+=1
            continue
        if c in '([{<': d+=1
        if c in ')]}>': d-=1
        if c==',' and d==0: args.append(cur); cur=''; i+=1; continue
        cur+=c; i+=1
    if cur.strip(): args.append(cur)
    return args

def parse_params(sig):
    """['const char* name','uint32_t ac',...] -> [(type,nom),...]"""
    out=[]
    for a in split_args(sig):
        a=a.strip()
        m=re.search(r'([A-Za-z_]\w*)\s*$', a)
        out.append((a, m.group(1) if m else ''))
    return out

def collect_helpers(src):
    """Rend [(offset_def, nom, [cibles], fin_du_corps)] pour chaque helper."""
    hs=[]
    for m in RE_HELPER.finditer(src):
        name=m.group(1); op=m.end()-1
        try: cp=match_paren(src, op)
        except ValueError: continue
        params=parse_params(src[op+1:cp])
        bo=src.find('{', cp)
        if bo<0: continue
        try: bc=match_brace(src, bo)
        except ValueError: continue
        body=src[bo:bc+1]
        tg=helper_targets(body, params)
        if tg: hs.append((m.start(), name, tg, bc))
    return hs

def fingerprint(text):
    return hashlib.sha1(re.sub(r'\s+', '', text).encode('utf-8', 'surrogateescape')).hexdigest()[:12]

def keys_of(src, label, explain=None):
    """Sequence ordonnee des inscriptions d'une unite deja debarrassee de ses
    commentaires. Rend [(cle, empreinte, label)]."""
    helpers = collect_helpers(src)
    if explain is not None:
        for off, name, tg, _ in helpers:
            explain.append('  %-28s %-6s -> %s' % (label, name,
                ', '.join('%s (%s, arg%d)'%(d,k,i) for d,k,i in tg)))
    # zones a ignorer : l'interieur du corps d'un helper (ses register_shim y sont
    # generiques, ils ne nomment aucune cle concrete)
    holes = [(off, end) for off, _, _, end in helpers]
    def in_hole(p): return any(a <= p <= b for a, b in holes)

    hnames = {}
    for off, name, tg, _ in helpers: hnames.setdefault(name, []).append((off, tg))

    out=[]
    # 1) appels de helper
    for name, defs in hnames.items():
        for m in re.finditer(r'(?<![\w:.>])%s\s*\(' % re.escape(name), src):
            p=m.start()
            if in_hole(p): continue
            op=m.end()-1
            try: cp=match_paren(src, op)
            except ValueError: continue
            args=split_args(src[op+1:cp])
            cand=[d for d in defs if d[0] < p]
            if not cand: continue
            tg=max(cand, key=lambda d: d[0])[1]
            fp=fingerprint(src[p:cp+1])
            for dll, kind, keyidx in tg:
                if dll=='$0':
                    lit=re.fullmatch(STR, args[0].strip())
                    if not lit: continue
                    dllv=lit.group(1)
                else:
                    dllv=dll
                if keyidx>=len(args): continue
                a=args[keyidx].strip()
                if kind=='ord':
                    mm=re.fullmatch(r'(?:0[xX])?[0-9a-fA-F]+[uU]?', a)
                    if not mm: continue
                    key='%s!#%d' % (dllv, int(a.rstrip('uU'), 0))
                else:
                    lit=re.fullmatch(STR, a)
                    if not lit: continue
                    key='%s!%s' % (dllv, lit.group(1))
                out.append((p, key, fp, label))
    # 2) inscriptions ecrites a la main (DSOUND, wsprintfA, WINMM...)
    for m in RE_REGSHIM.finditer(src):
        p=m.start()
        if in_hole(p): continue
        ordinal=bool(m.group(1)); op=m.end()-1
        try: cp=match_paren(src, op)
        except ValueError: continue
        args=split_args(src[op+1:cp])
        if len(args)<2: continue
        d=re.fullmatch(STR, args[0].strip())
        if not d: continue
        a=args[1].strip()
        if ordinal:
            if not re.fullmatch(r'(?:0[xX])?[0-9a-fA-F]+[uU]?', a): continue
            key='%s!#%d' % (d.group(1), int(a.rstrip('uU'), 0))
        else:
            lit=re.fullmatch(STR, a)
            if not lit: continue
            key='%s!%s' % (d.group(1), lit.group(1))
        # l'empreinte d'une inscription manuelle est celle de l'appel lui-meme
        out.append((p, key, fingerprint(src[p:cp+1]), label))
    out.sort(key=lambda t: t[0])
    return [(k, f, l) for _, k, f, l in out]

def read(path, ref):
    if ref:
        r = subprocess.run(['git', '-C', ROOT, 'show', '%s:%s' % (ref, path)],
                           capture_output=True)
        if r.returncode != 0: return None
        return r.stdout.decode('utf-8', 'surrogateescape')
    p = os.path.join(ROOT, path)
    if not os.path.exists(p): return None
    return open(p, encoding='utf-8', errors='surrogateescape').read()

def unit_body(src, fn):
    # NOTE 2026-09-09 : la signature n'est plus figee a `ShimCtx&` (carn-vita) --
    # d2-vita extrait des install(Cpu*,Bridge&[,...]) ; seul le NOM de la
    # fonction identifie l'unite, la liste de parametres est libre.
    m = re.search(r'\bvoid\s+%s\s*\(' % re.escape(fn), src)
    if not m: return None
    bo = src.find('{', m.end())
    return src[bo:match_brace(src, bo)+1]

def build(ref, explain=None):
    rt = read('tools/rt_boot.cpp', ref)
    if rt is None: sys.exit('tools/rt_boot.cpp introuvable (ref=%s)' % ref)
    rt = strip_comments(rt)
    cache = {}
    seq = []
    # decoupe rt_boot.cpp aux appels register_*, et intercale la sequence de
    # l'unite appelee A CET ENDROIT-LA : register_misc et register_coverage sont
    # appelees a deux endroits distincts, avec DSOUND/GDI32/USER32 entre les deux.
    # NOTE 2026-09-09 : accepte une liste d'arguments arbitraire (pas de
    # parentheses imbriquees dans les appels reels), pas seulement un
    # identifiant unique -- d2-vita appelle install(cpu,br[,&g_frame]).
    calls = ([(m.start(), m.group(1)) for m in
              re.finditer(r'\b(%s)\s*\([^()]*\)\s*;' % '|'.join(UNITS), rt)]
             if UNITS else [])
    prev = 0
    for off, fn in calls + [(len(rt), None)]:
        chunk = rt[prev:off]
        seq += keys_of(chunk, 'tools/rt_boot.cpp', explain if prev == 0 else None)
        prev = off
        if fn is None: break
        if fn not in cache:
            u = read(UNITS[fn], ref)
            cache[UNITS[fn]] = strip_comments(u) if u is not None else None
        u = cache[UNITS[fn]]
        if u is None:
            sys.exit('%s appelle %s mais %s est absent (ref=%s)'
                     % ('rt_boot.cpp', fn, UNITS[fn], ref))
        body = unit_body(u, fn)
        if body is None: sys.exit('%s introuvable dans %s' % (fn, UNITS[fn]))
        seq += keys_of(body, UNITS[fn] + ':' + fn, explain)
    return seq

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ref', default=None, help='revision git a lire (defaut: arbre courant)')
    ap.add_argument('--explain', action='store_true', help='imprime la table helper -> DLL deduite')
    a = ap.parse_args()
    ex = [] if a.explain else None
    seq = build(a.ref, ex)
    if ex:
        sys.stderr.write('# table helper -> DLL, DEDUITE des sources :\n')
        sys.stderr.write('\n'.join(ex) + '\n#\n')
    for k, f, _ in seq: print('%s\t%s' % (k, f))
    sys.stderr.write('# %d inscriptions\n' % len(seq))

if __name__ == '__main__':
    main()
