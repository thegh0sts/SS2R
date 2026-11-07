#!/usr/bin/env python3
r"""SS2R: fix "duplicate resource: type ICON" after apply_icons.py. Run once from the project root:

    python fix_icons.py

SS2R.exe already has a resource file (packaging/windows/sacredfire.rc) with icon 1. So:
  gpu/CMakeLists.txt               the extra icon resource is added to the launcher only
  packaging/windows/sacredfire.rc  its icon now points at res/SS2R.ico (the old line is kept as a comment)
Then ./build.sh.
"""
import re
import sys
from pathlib import Path

cm = Path('gpu/CMakeLists.txt')
rc = Path('packaging/windows/sacredfire.rc')
if not cm.exists():
    sys.exit('run this from the project root (gpu/CMakeLists.txt not found)')

# 1. CMake: only the launcher gets the generated icon resource.
s = cm.read_text(encoding='utf-8')
old = 'foreach(pair "sacredfire|SS2R" "ss2-launcher|SS2R-Launcher")'
if old in s:
    s = s.replace(old, 'foreach(pair "ss2-launcher|SS2R-Launcher")  # SS2R.exe: packaging/windows/sacredfire.rc')
    cm.write_text(s, encoding='utf-8', newline='\n')
    print('changed:', cm)
elif 'foreach(pair "ss2-launcher|SS2R-Launcher")' in s:
    print('ok:', cm, '(already fixed)')
else:
    print('CHECK:', cm, ': the icon block from apply_icons.py was not found')

# 2. sacredfire.rc: its ICON line points at res/SS2R.ico (path relative to the .rc file).
if not rc.exists():
    sys.exit(f'CHECK: {rc} not found')
text = rc.read_text(encoding='utf-8', errors='replace')
if 'res/SS2R.ico' in text:
    print('ok:', rc, '(already points at res/SS2R.ico)')
else:
    pattern = re.compile(r'^(?P<head>\s*\w+\s+ICON\s+(?:\w+\s+)*)"(?P<path>[^"]+)"', re.M)
    m = pattern.search(text)
    if not m:
        print(f'CHECK: no ICON line found in {rc}. Its contents:\n')
        print(text)
        sys.exit(1)
    line_start = text.rfind('\n', 0, m.start()) + 1
    line_end = text.find('\n', m.end())
    line_end = len(text) if line_end < 0 else line_end
    old_line = text[line_start:line_end]
    new_line = m.group('head') + '"../../res/SS2R.ico"' + text[m.end():line_end]
    text = text[:line_start] + '// was: ' + old_line.strip() + '\n' + new_line + text[line_end:]
    rc.write_text(text, encoding='utf-8', newline='\n')
    print('changed:', rc)
    print('   old:', old_line.strip())
    print('   new:', new_line.strip())

if not Path('res/SS2R.ico').exists():
    print('CHECK: res/SS2R.ico is missing: put your icon there before ./build.sh')
