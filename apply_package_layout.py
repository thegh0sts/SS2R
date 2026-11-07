#!/usr/bin/env python3
r"""SS2R: teach the launcher the release layout (run once from the project root).

Release layout (made by package.sh):
    SS2R-Launcher.exe          the launcher
    bin\                       SS2R.exe, its DLLs, and everything Prepare makes (boot-linked.bin, ...),
                               plus the player's settings (bbport.ini) and saves (user\)
    python\  scripts\  tools\  share\aerolib.inl  licenses\  README-Windows.md

Changes to launcher/windows/ss2_launcher.cpp:
  * the working folder is bin\ when bin\SS2R.exe exists (a release), otherwise out\ (the source tree);
  * every Prepare step gets --out <that folder>, and gen_import_names.py gets --aerolib share\aerolib.inl
    when the release ships it (no download needed).
"""
import sys
from pathlib import Path

p = Path('launcher/windows/ss2_launcher.cpp')
if not p.exists():
    sys.exit('run this from the project root (launcher/windows/ss2_launcher.cpp not found)')
s = p.read_text(encoding='utf-8')
if 'OutArg()' in s:
    sys.exit('already applied')

edits = [
    # 1. The working folder.
    ('    a.out = a.root / L"out";\n    if (!IsDir(a.out)) a.out = ExeDir();',
     '    // A release keeps the game executable and everything Prepare makes in bin\\; the source tree uses out\\.\n'
     '    if (IsFile(a.root / L"bin" / L"SS2R.exe")) a.out = a.root / L"bin";\n'
     '    else a.out = a.root / L"out";\n'
     '    if (!IsDir(a.out)) a.out = ExeDir();'),
    # 2. Helpers in front of PrepareSteps.
    ('std::vector<Step> PrepareSteps() {',
     '/// Where the Prepare scripts write (the folder the game runs in).\n'
     'std::wstring OutArg() { return L" --out " + Quote(g_app.out.wstring()); }\n'
     '/// gen_import_names.py: the release ships shadPS4\'s aerolib.inl in share\\ (no download).\n'
     'std::wstring NamesArgs() {\n'
     '    const fs::path aerolib = g_app.root / L"share" / L"aerolib.inl";\n'
     '    return OutArg() + (IsFile(aerolib) ? L" --aerolib " + Quote(aerolib.wstring()) : L"");\n'
     '}\n\n'
     'std::vector<Step> PrepareSteps() {'),
    ('a.root / L"scripts" / L"prepare.py", game),',
     'a.root / L"scripts" / L"prepare.py", game + OutArg()),'),
    ('               game + L" --modules " + kModules),',
     '               game + OutArg() + L" --modules " + kModules),'),
    ('a.root / L"scripts" / L"content_profile.py", game),',
     'a.root / L"scripts" / L"content_profile.py", game + OutArg()),'),
    # 3. Prepare and the Tools tab: gen_import_names.py.
    ('a.root / L"tools" / L"gen_import_names.py", L"")',
     'a.root / L"tools" / L"gen_import_names.py", NamesArgs())'),
    # 4. Tools tab: content_profile.py.
    ('                          Quote(a.game_dir.wstring()))},',
     '                          Quote(a.game_dir.wstring()) + OutArg())},'),
]
for old, new in edits:
    n = s.count(old)
    if n == 0:
        sys.exit(f'not found (nothing changed):\n{old}')
    s = s.replace(old, new)
p.write_text(s, encoding='utf-8', newline='\n')
print('changed: launcher/windows/ss2_launcher.cpp (release layout)')
