#!/usr/bin/env python3
r"""SS2R test: let the game's "TV" be something other than 1920x1080. Run once from the project root:

    python apply_video_res.py

src/probe.c passes a fixed 1920x1080 to the graphics layer as the video-out size, which the game
renders for. With this change BB_VIDEO_RES=WxH (e.g. 2560x1440, 3840x2160) sets it instead; without
it nothing changes. For testing whether SS2 renders natively above 1080p (with or without BB_NEO=1).
Then ./build.sh.
"""
import sys
from pathlib import Path

p = Path('src/probe.c')
if not p.exists():
    sys.exit('run this from the project root (src/probe.c not found)')
s = p.read_text(encoding='utf-8')
if 'BB_VIDEO_RES' in s:
    sys.exit('already applied')
old = 'attributes,1920,1080,icon};'
if s.count(old) != 1:
    sys.exit(f'expected one "{old}" in src/probe.c (nothing changed)')
start = s.rfind('\n', 0, s.index(old)) + 1
indent = s[start:len(s) - len(s[start:].lstrip(' \t'))]
block = (
    f'{indent}/* BB_VIDEO_RES=WxH: the video-out ("TV") size the game renders for; 1920x1080 by default. */\n'
    f'{indent}uint32_t video_w=1920, video_h=1080;\n'
    f'{indent}{{ const char *res=getenv("BB_VIDEO_RES"); unsigned w=0, h=0;\n'
    f'{indent}  if (res && sscanf(res,"%ux%u",&w,&h)==2 && w>=640 && h>=360 && w<=7680 && h<=4320) {{ video_w=w; video_h=h; }}\n'
    f'{indent}  printf("Video out: %ux%u%s\\n",video_w,video_h,res ? " (BB_VIDEO_RES)" : ""); }}\n'
)
s = s[:start] + block + s[start:].replace(old, 'attributes,video_w,video_h,icon};', 1)
if '#include <stdio.h>' not in s:
    s = '#include <stdio.h>\n' + s
p.write_text(s, encoding='utf-8', newline='\n')
print('changed: src/probe.c (BB_VIDEO_RES)')
