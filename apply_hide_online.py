#!/usr/bin/env python3
r"""SS2R: hide the multiplayer menu's "Online" (PlayFab) and "Join game by code" buttons.
Put make_overrides.py in scripts\ first, then run once from the project root:

    python apply_hide_online.py

  src/runtime_file.c                  the engine's optional /app0/sshock2ee-patch1.kpf (absent in update
                                      1.04) is answered with ss2r-overrides.kpf from the folder the game
                                      runs in, when that file exists. A real sshock2ee-patch1.kpf in the
                                      game folder always wins. BB_NO_OVERRIDES=1 switches it off.
  launcher/windows/ss2_launcher.cpp   Prepare also runs scripts/make_overrides.py, which builds
                                      ss2r-overrides.kpf from the player's own base.kpf.
Then ./build.sh, and press Prepare once in the launcher.
"""
import re
import sys
from pathlib import Path

rf = Path('src/runtime_file.c')
lc = Path('launcher/windows/ss2_launcher.cpp')
if not rf.exists() or not lc.exists():
    sys.exit('run this from the project root (src/runtime_file.c or launcher/windows/ss2_launcher.cpp not found)')
if not Path('scripts/make_overrides.py').exists():
    sys.exit('put make_overrides.py in scripts\\ first')
changes = []

s = rf.read_text(encoding='utf-8')
if 'ss2r-overrides.kpf' not in s:
    anchor = ('    pthread_mutex_unlock(&lock);\n'
              '    if (result==ENOENT) fprintf(stderr,"Runtime: no mount for guest path %s\\n",guest);\n')
    if s.count(anchor) != 1:
        sys.exit('src/runtime_file.c: the end of translate() was not found as expected (nothing changed)')
    hook = ('    pthread_mutex_unlock(&lock);\n'
            '    /* SS2R: the engine loads an optional sshock2ee-patch1.kpf after its other archives (absent in\n'
            '     * update 1.04). ss2r-overrides.kpf, made by Prepare from the player\'s own base.kpf, stands in\n'
            '     * for it: the multiplayer menus without the Online (PlayFab) option. A real patch1 wins. */\n'
            '    if (!result && !strncmp(buffer,"/app0/",6)) {\n'
            '        static const char want[]="/sshock2ee-patch1.kpf";\n'
            '        const size_t have=strlen(buffer), wl=sizeof(want)-1;\n'
            '        if (have>=wl && !strcmp(buffer+have-wl,want) && access(out,F_OK) &&\n'
            '            !access("ss2r-overrides.kpf",F_OK) && !getenv("BB_NO_OVERRIDES")) {\n'
            '            snprintf(out,size,"%s","ss2r-overrides.kpf");\n'
            '            static int told;\n'
            '            if (!told) { told=1; printf("Runtime: %s -> ss2r-overrides.kpf (SS2R menu overrides)\\n",guest); }\n'
            '        }\n'
            '    }\n'
            '    if (result==ENOENT) fprintf(stderr,"Runtime: no mount for guest path %s\\n",guest);\n')
    s = s.replace(anchor, hook, 1)
    rf.write_text(s, encoding='utf-8', newline='\n')
    changes.append(str(rf))

t = lc.read_text(encoding='utf-8')
if 'make_overrides.py' not in t:
    pat = re.compile(r'(        PyStep\("Writing the content profile \(content_profile\.py\)", a\.root / L"scripts" / '
                     r'L"content_profile\.py", game[^\n]*\),\n)')
    m = pat.search(t)
    if not m:
        sys.exit('ss2_launcher.cpp: the content_profile step in PrepareSteps was not found (nothing changed)')
    step = ('        PyStep("Hiding the Online option in the menus (make_overrides.py)", '
            'a.root / L"scripts" / L"make_overrides.py",\n'
            '               game + L" --out " + Quote(a.out.wstring())),\n')
    t = t[:m.end()] + step + t[m.end():]
    lc.write_text(t, encoding='utf-8', newline='\n')
    changes.append(str(lc))

print('\n'.join('changed: ' + c for c in changes) if changes else 'already applied')
