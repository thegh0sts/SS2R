#!/usr/bin/env python3
"""SS2R: link the Dark Engine script modules (allobjs, nvscript, squirrel) like the other PRXs.

Run once from the project root:  python apply_script_modules.py
It edits, and reports each change (a file already changed is left as it is):
  scripts/link_modules.py       also finds PRXs in the game folder's root; writes out/module_exports.txt
  launcher/windows/ss2_launcher.cpp   the module list gains the three script modules
  src/probe.c                   room for more linked modules and TLS module IDs up to 15
  src/runtime.c                 TLS_MODULES 8 -> 16
"""
import re
import sys
from pathlib import Path

SCRIPT_MODULES = 'allobjs-orbis-x86_64.prx nvscript-orbis-x86_64.prx squirrel-orbis-x86_64.prx'
changes, problems = [], []


def edit(path, fn):
    p = Path(path)
    if not p.exists():
        problems.append(f'{path}: not found')
        return
    text = p.read_text(encoding='utf-8')
    new = fn(text)
    if new != text:
        p.write_text(new, encoding='utf-8', newline='\n')
        changes.append(path)


def link_modules(text):
    # 1. PRX lookup: sce_module, then prx\, then the game folder's root (the script modules).
    if "game / filename" not in text:
        old = "            path = game / 'prx' / filename"
        if old not in text:
            problems.append("link_modules.py: the prx\\ fallback was not found (apply the earlier fix first)")
            return text
        indent = ' ' * 8
        start = text.index(old)
        end = text.find('\n', start)  # after the whole line, comment included
        end = len(text) if end < 0 else end
        text = (text[:end] + "\n" + indent + "if not path.exists():\n" + indent +
                "    path = game / filename   # script modules (allobjs, nvscript, squirrel) sit in the root" + text[end:])
    # 2. Every export, for sceKernelDlsym: out/module_exports.txt ("file NID image-offset").
    if 'module_exports.txt' not in text:
        anchor = "    with (out / 'boot-linked.bin').open('wb') as f:"
        if anchor not in text:
            problems.append('link_modules.py: the boot-linked.bin writer was not found')
            return text
        block = ("    with (out / 'module_exports.txt').open('w', encoding='utf-8', newline='\\n') as f:\n"
                 "        for identity, (address, kind, filename) in exports.items():\n"
                 "            f.write(f'{filename} {identity[0]} {address:#x}\\n')\n")
        text = text.replace(anchor, block + anchor, 1)
    return text


def launcher(text):
    if 'nvscript-orbis-x86_64.prx' in text:
        return text
    old = 'Party.prx libSceNpToolkit2.prx";'
    if old not in text:
        problems.append('ss2_launcher.cpp: the module list was not found')
        return text
    return text.replace(old, 'Party.prx libSceNpToolkit2.prx "\n    L"' + SCRIPT_MODULES + '";', 1)


def probe(text):
    text2 = text.replace('x->tls_module>7', 'x->tls_module>15')
    # The linked-module table: at least 16 entries.
    m = re.search(r'(LinkedModule\s+modules\s*\[\s*)(\d+)(\s*\])', text2)
    if m and int(m.group(2)) < 16:
        text2 = text2[:m.start(2)] + '16' + text2[m.end(2):]
    elif not m:
        problems.append('probe.c: "LinkedModule modules[N]" not found; check its size by hand (needs 9 or more)')
    return text2


def runtime(text):
    return re.sub(r'#define TLS_MODULES 8\b', '#define TLS_MODULES 16', text)


edit('scripts/link_modules.py', link_modules)
edit('launcher/windows/ss2_launcher.cpp', launcher)
edit('src/probe.c', probe)
edit('src/runtime.c', runtime)
for c in changes:
    print('changed:', c)
for p in problems:
    print('CHECK:', p)
if not changes and not problems:
    print('nothing to change (already applied)')
sys.exit(1 if problems else 0)
