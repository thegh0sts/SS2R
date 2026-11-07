#!/usr/bin/env bash
# SS2R: builds dist/SS2R, a folder that runs on any Windows PC (no MSYS2 needed):
#
#   SS2R-Launcher.exe      the launcher (choose the game folder, Prepare, Play)
#   bin\                   SS2R.exe, bb-gpu-capabilities.exe and every DLL they load. Prepare writes
#                          boot-linked.bin and the other prepared files here, and the game keeps its
#                          settings (bbport.ini) and saves (user\) here.
#   python\                embeddable Python for Prepare
#   scripts\  tools\       the Prepare scripts
#   share\aerolib.inl      shadPS4's NID name list (GPL-2.0-or-later), so Prepare needs no download
#   licenses\              licences of the port and its components
#   README-Windows.md, VERSION
#
# NO game files: no eboot.bin, PRX, boot.bin, boot-linked.bin or anything else made from the game.
# Every player prepares their own copy from their own PS4 dump.
#
# Run from the project root after ./build.sh:   ./package.sh
# Python: copied from PYTHON_DIR (an embeddable Python for Windows; by default the copy in your
# UFC 3 package). Licences: the project's own, plus LICENSES_DIR if set.
set -euo pipefail
cd "$(dirname "$0")"

DIST=dist/SS2R
PYTHON_DIR="${PYTHON_DIR:-/f/Retail Games/EA SPORTS UFC 3/python}"
LICENSES_DIR="${LICENSES_DIR:-/f/Retail Games/EA SPORTS UFC 3/licenses}"

for f in out/SS2R.exe out/SS2R-Launcher.exe out/bb-gpu-capabilities.exe; do
    [ -f "$f" ] || { echo "missing $f: run ./build.sh first"; exit 1; }
done

rm -rf "$DIST"
mkdir -p "$DIST"/{bin,scripts,tools,share,licenses}

# Executables.
cp out/SS2R-Launcher.exe "$DIST/"
cp out/SS2R.exe out/bb-gpu-capabilities.exe "$DIST/bin/"

# Every DLL the executables load that is not part of Windows (MSYS2's clang64 runtime, FFmpeg,
# SDL3, Vulkan loader, ...), found with ldd.
for exe in out/SS2R.exe out/bb-gpu-capabilities.exe out/SS2R-Launcher.exe; do
    ldd "$exe" | awk '$2 == "=>" && $3 ~ /^\// {print $3}'
done | { grep -viE '^/c/windows/' || true; } | sort -u | while read -r dll; do
    cp -n "$dll" "$DIST/bin/"
done
dlls=$(find "$DIST/bin" -maxdepth 1 -iname "*.dll" | wc -l)
# The Vulkan loader is normally installed with the graphics driver; ship ours only as a fallback.
[ -f "$DIST/bin/vulkan-1.dll" ] || { [ -f out/vulkan-1.dll ] && cp out/vulkan-1.dll "$DIST/bin/"; } || true

# Prepare scripts (no __pycache__) and the NID name list.
cp scripts/*.py "$DIST/scripts/"
cp tools/*.py "$DIST/tools/" 2>/dev/null || true
if [ -f out/aerolib.inl ]; then
    cp out/aerolib.inl "$DIST/share/"
else
    echo "note: out/aerolib.inl not found; Prepare will download it on first use"
fi

# Python.
if [ -f "$PYTHON_DIR/python.exe" ]; then
    cp -r "$PYTHON_DIR" "$DIST/python"
    rm -rf "$DIST/python/__pycache__"
else
    echo "WARNING: no embeddable Python at '$PYTHON_DIR'."
    echo "         Set PYTHON_DIR=<folder with python.exe> or unzip python-3.x-embed-amd64.zip into $DIST/python"
fi

# Licences: the project's, then any extra folder of third-party licences.
for f in LICENSE LICENSE.txt LICENSE.md COPYING THIRD-PARTY.md; do
    [ -f "$f" ] && cp "$f" "$DIST/licenses/"
done
[ -d licenses ] && cp -r licenses/. "$DIST/licenses/"
[ -d "$LICENSES_DIR" ] && cp -rn "$LICENSES_DIR"/. "$DIST/licenses/"
[ -f "$DIST/python/LICENSE.txt" ] && cp -n "$DIST/python/LICENSE.txt" "$DIST/licenses/Python-LICENSE.txt"
[ -f LICENSE ] || [ -f LICENSE.txt ] || [ -f LICENSE.md ] || echo "WARNING: no LICENSE file in the project root"

[ -f out/VERSION ] && cp out/VERSION "$DIST/"

cat > "$DIST/README-Windows.md" <<'EOF'
# SS2R: System Shock 2 25th Anniversary Remaster on Windows

Runs the PS4 version of System Shock 2: 25th Anniversary Remaster natively on Windows.

This package does **not** contain the game. You need your own PS4 copy (base game and update 1.04),
dumped from your own console.

## Requirements

- Windows 10 or 11, 64-bit
- A GPU with Vulkan 1.3 and an up-to-date driver
- Your PS4 dumps of the base game and update 1.04

## Setup (once)

1. Run `SS2R-Launcher.exe`.
2. Game folder, either:
   - **Already merged** (update 1.04 copied over the base game): click **Choose...** and pick it, or
   - **Separate dumps:** open *"No merged folder yet? Merge the base game and the update"*, choose
     both and click **Merge**. The merged folder is made next to the base game (no extra space on
     the same drive) and chosen for you.
3. Click **Prepare**. It builds `bin\boot-linked.bin` and the other prepared files from your game.
   Run it again only after choosing a different game folder or updating SS2R.

## Playing

Click **PLAY**. Graphics and controls are in the Settings and Controls tabs; the in-game overlay
(Insert, or L3+R3 on a controller) changes the same settings while you play.

- **Saves** tab: back up, export, import and restore your saves.
- **Trophies** tab: your trophies as you earn them. Their names are encrypted in the game; paste
  your PS4 trophy key (or import it from shadPS4) to see them.

## Updating SS2R

Your settings (`bin\bbport.ini`, `bin\ss2_launcher.ini`), saves (`bin\user\`) and backups
(`backups\`) live in this folder. To update, copy those into the new version's folder, then press
**Prepare** once.

## Problems

Each run's output is in `bin\run.txt`. The launcher shows the last error in red. Clearing the shader
cache (Tools tab) helps after a GPU driver update or with graphics glitches.

## Licence

SS2R is free software under the GNU GPL v2 or later; see `licenses\`. Not affiliated with Nightdive
Studios or Sony Interactive Entertainment.
EOF

size=$(du -sh "$DIST" | cut -f1)
echo "Packaged $PWD/$DIST ($size, $dlls DLLs in bin)"
echo "Contents check: no game files"
if find "$DIST" -iname 'eboot.bin' -o -iname '*.prx' -o -iname 'boot*.bin' -o -iname 'image.bin' | grep -q .; then
    echo "ERROR: game-derived files found in $DIST"; exit 1
fi

# A zip for sharing, named after the version when there is one.
ver=$(head -n1 "$DIST/VERSION" 2>/dev/null | tr -cd 'A-Za-z0-9._-')
zipname="SS2R${ver:+-$ver}.zip"
rm -f "dist/$zipname"
if command -v zip >/dev/null; then
    (cd dist && zip -qr "$zipname" SS2R) && echo "Zip: $PWD/dist/$zipname"
elif command -v powershell.exe >/dev/null; then
    powershell.exe -NoProfile -Command "Compress-Archive -Path '$(cygpath -w "$DIST")' -DestinationPath '$(cygpath -w "dist/$zipname")'" \
        && echo "Zip: $PWD/dist/$zipname"
else
    echo "No zip tool: right-click dist\\SS2R > Send to > Compressed (zipped) folder"
fi
