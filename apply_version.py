#!/usr/bin/env python3
r"""SS2R: one version number, set in the VERSION file at the project root. Run once from the project root:

    python apply_version.py [0.1.0]

  VERSION                             created with the given number (default 0.1.0) if it doesn't exist
  gpu/CMakeLists.txt                  reads VERSION at configure time (and again whenever it changes):
                                      SS2R_VERSION for the launcher and SS2R.exe, and out/VERSION
                                      (which package.sh copies into the release and uses for the zip name)
  launcher/windows/ss2_launcher.cpp   the window title ("... - SS2R 0.1.0") and Tools > About show it
  src/probe.c                         the first line of every run.txt says "SS2R 0.1.0"
To release a new version: edit VERSION, then ./build.sh and ./package.sh.
"""
import re
import sys
from pathlib import Path

version = sys.argv[1] if len(sys.argv) > 1 else '0.1.0'
if not re.fullmatch(r'[0-9A-Za-z._+-]{1,32}', version):
    sys.exit(f'not a version number: {version}')
cm = Path('gpu/CMakeLists.txt')
lp = Path('launcher/windows/ss2_launcher.cpp')
probe = Path('src/probe.c')
for f in (cm, lp, probe):
    if not f.exists():
        sys.exit(f'run this from the project root ({f} not found)')
changes = []

vf = Path('VERSION')
if not vf.exists():
    vf.write_text(version + '\n', encoding='utf-8', newline='\n')
    changes.append(f'VERSION ({version})')

c = cm.read_text(encoding='utf-8')
if 'SS2R_VERSION' not in c:
    c = c.rstrip('\n') + '''

# SS2R version: the VERSION file at the project root (reconfigures when it changes).
set(SS2R_VERSION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SS2R_VERSION_FILE}")
if(EXISTS "${SS2R_VERSION_FILE}")
    file(STRINGS "${SS2R_VERSION_FILE}" SS2R_VERSION LIMIT_COUNT 1)
    string(STRIP "${SS2R_VERSION}" SS2R_VERSION)
endif()
if(NOT SS2R_VERSION)
    set(SS2R_VERSION "dev")
endif()
foreach(target sacredfire ss2-launcher)
    if(TARGET ${target})
        target_compile_definitions(${target} PRIVATE SS2R_VERSION="${SS2R_VERSION}")
    endif()
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/../VERSION" "${SS2R_VERSION}\\n")  # out/VERSION, for package.sh
message(STATUS "SS2R version ${SS2R_VERSION}")
'''
    cm.write_text(c, encoding='utf-8', newline='\n')
    changes.append(str(cm))

s = lp.read_text(encoding='utf-8')
if 'SS2R_VERSION' not in s:
    m = re.search(r'constexpr const wchar_t\* kWindowTitle = L"([^"]*)";', s)
    if not m:
        sys.exit('ss2_launcher.cpp: kWindowTitle not found (nothing changed)')
    s = s.replace(m.group(0),
                  '#ifndef SS2R_VERSION\n#define SS2R_VERSION "dev"  // set by gpu/CMakeLists.txt from VERSION\n#endif\n'
                  f'constexpr const wchar_t* kWindowTitle = L"{m.group(1)} " SS2R_VERSION;', 1)
    old = ('std::string BuildVersion() {\n'
           '    for (const fs::path& p : {g_app.root / L"VERSION", g_app.out / L"VERSION"}) {')
    if old in s:  # Tools > About: the built-in number first
        s = s.replace(old, 'std::string BuildVersion() {\n'
                           '    if (std::strcmp(SS2R_VERSION, "dev") != 0) return SS2R_VERSION;\n'
                           '    for (const fs::path& p : {g_app.root / L"VERSION", g_app.out / L"VERSION"}) {', 1)
    lp.write_text(s, encoding='utf-8', newline='\n')
    changes.append(str(lp))

p = probe.read_text(encoding='utf-8')
if 'SS2R_VERSION' not in p:
    m = re.search(r'\n([ \t]*)puts\("BUILD MARKER[^"]*"\);', p)
    if m:
        p = p.replace(m.group(0), f'\n{m.group(1)}printf("SS2R %s\\n", SS2R_VERSION);' + m.group(0), 1)
        p = '#ifndef SS2R_VERSION\n#define SS2R_VERSION "dev"\n#endif\n' + p
        probe.write_text(p, encoding='utf-8', newline='\n')
        changes.append(str(probe))
    else:
        print('CHECK: src/probe.c has no BUILD MARKER line; the version is not printed in run.txt')

print('\n'.join('changed: ' + c for c in changes) if changes else 'already applied')
print(f'version: {vf.read_text(encoding="utf-8").strip()}  (edit VERSION to change it)')
