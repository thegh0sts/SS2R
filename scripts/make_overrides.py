#!/usr/bin/env python3
"""SS2R: build bin/ss2r-overrides.kpf from the player's own base.kpf (run by the launcher's Prepare).

The engine loads its .kpf archives (zip files) in order, later ones replacing files of earlier ones,
and asks for an optional sshock2ee-patch1.kpf that update 1.04 doesn't have. The runtime answers that
request with this archive, which holds the player's own multiplayer menus with two buttons hidden the
way the game hides Switch-only ones (class "switch_only"):
  * menu/multi_driver.rml: "Online" (PlayFab: needs Nightdive's services); LAN gets the focus
  * menu/multi_main.rml:   "Join game by code" (PlayFab room codes)
Nothing from the game is distributed: the archive is made on the player's PC, like boot-linked.bin.

    python scripts/make_overrides.py <game folder> [--out <folder>]
"""
import argparse
import re
import sys
import zipfile
from pathlib import Path


def hide(rml: str, pattern: str, what: str) -> str:
    """Adds switch_only to the class of the kex_button whose attributes match `pattern`."""
    for m in re.finditer(r'<kex_button\b[^>]*>', rml, re.S):
        tag = m.group(0)
        if re.search(pattern, tag):
            if 'switch_only' in tag:
                return rml
            new = re.sub(r'class="([^"]*)"', lambda c: f'class="{c.group(1)} switch_only"', tag, count=1)
            if new == tag:
                new = tag.replace('<kex_button', '<kex_button class="switch_only"', 1)
            return rml[:m.start()] + new + rml[m.end():]
    raise SystemExit(f'make_overrides: the {what} button was not found (game version not supported?)')


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('game')
    ap.add_argument('--out', default='out')
    a = ap.parse_args()
    base = Path(a.game) / 'base.kpf'
    if not base.is_file():
        print(f'make_overrides: {base} not found')
        return 1
    with zipfile.ZipFile(base) as z:
        driver = z.read('menu/multi_driver.rml').decode('utf-8')
        main_menu = z.read('menu/multi_main.rml').decode('utf-8')

    # Multiplayer > connection type: hide Online, focus LAN.
    driver = hide(driver, r'kex-shock2-multi-driver="playfab"', 'Online')
    driver = re.sub(r'(kex-shock2-multi-driver="playfab")\s+autofocus', r'\1', driver)
    driver = re.sub(r'(kex-shock2-multi-driver="lan")(\s*>)', r'\1 autofocus\2', driver, count=1)
    # Multiplayer > main: hide "Join game by code".
    main_menu = hide(main_menu, r'id="multi_join_code"', 'Join by code')

    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    target = out / 'ss2r-overrides.kpf'
    tmp = target.with_suffix('.tmp')
    with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('menu/multi_driver.rml', driver)
        z.writestr('menu/multi_main.rml', main_menu)
    tmp.replace(target)
    print(f'make_overrides: wrote {target} (Online and Join-by-code hidden in the multiplayer menus)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
