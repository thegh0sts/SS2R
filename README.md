# SS2R: System Shock 2 25th Anniversary Remaster on Windows

SS2R runs the **PS4 version** of *System Shock 2: 25th Anniversary Remaster* natively on Windows.
It isn't an emulator running a whole console. It loads the game's own x86-64 code directly, gives it
Windows versions of the PS4 system libraries it calls, and translates its graphics to Vulkan.

> [!IMPORTANT]
> This project contains **no game files**. You need your own PS4 copy of the game (**CUSA51878**,
> base game and update 1.04), dumped from your own console. Game files, and files made from them such as
> `boot-linked.bin`, must not be shared.

> [!WARNING]
> **SS2R is still a work in progress.** Expect bugs, crashes and missing features, and back up your
> saves often (launcher → **Saves** → **Back up now**). If you hit a bug, please report it **with your
> exported saves** so it can be reproduced: see [Reporting problems](#reporting-problems).

<!-- Add a screenshot here, e.g. ![SS2R in game](docs/screenshot.png) -->

## Status

The game is playable from start to finish in testing so far: splash videos, menus, gameplay, saving and loading,
controller and keyboard input all work, at 60 FPS on a modern PC.

| Area | State |
|---|---|
| Boot, menus, videos | Working |
| Gameplay and scripts | Working |
| Saves | Working (save icons are not written yet) |
| Controller and keyboard | Working, remappable |
| Trophies | Recorded locally; names need your PS4 trophy key |
| Native 4K rendering | Working (Render resolution 1080p or 4K; no 1440p mode) |
| Co-op, LAN (up to 4 players) | Working: tested host + joiner, lobby and gameplay |
| Co-op over the internet | Experimental: virtual LAN (ZeroTier, Tailscale, Radmin) or port forwarding (see [Co-op multiplayer](#co-op-multiplayer)) |
| The game's "Online" option | Hidden (needs Nightdive's PlayFab services): LAN is used instead |
| Upscaling (FSR / DLSS) | Not offered yet: use 4K for the sharpest picture |
| Frame generation | Experimental, mostly untested |

Found a problem? See [Reporting problems](#reporting-problems).

## Features

- **Launcher** (`SS2R-Launcher.exe`) that sets everything up and starts the game:
  - checks your game folder and prepares it in one click
  - merges separate base game and update dumps for you (hard links, no extra space)
  - Settings: render resolution (1080p or native 4K), monitor, fullscreen, presentation mode, FPS
    limit, game language
  - Controls: controllers per player, button and keyboard remapping, stick dead zones
  - Saves: back up, export, import (including decrypted PS4 saves) and restore
  - Trophies: earned trophies with their pictures and unlock dates
  - Tools: shader cache, logs, individual setup steps
- **Co-op for up to 4 players** through the game's own LAN mode, on a home network or over the internet
  (Settings → Co-op: Off / Host / Join, player name, addresses)
- **In-game overlay** (Insert, or L3+R3 on a controller) to change settings while you play
- Portable: settings and saves stay in the program's folder

## Requirements

- Windows 10 or 11, 64-bit
- A GPU with Vulkan 1.3 support and an up-to-date driver
- Your PS4 dumps of *System Shock 2: 25th Anniversary Remaster*: the base game and update **1.04**

### Supported version

| Serial | Region | Update | Status |
|---|---|---|---|
| **CUSA51878** | Europe | 01.04 | Tested |

Other regions and updates may work but haven't been tested; reports are welcome. You can find a
dump's serial in its `sce_sys\param.sfo` (`TITLE_ID`), or in the launcher once the folder is chosen.

## Getting started

1. Download the latest release from the [Releases](../../releases) page and unzip it anywhere.
2. Run `SS2R-Launcher.exe`.
3. Choose your game folder:
   - **Already merged** (update 1.04 copied over the base game): click **Choose...** and pick it.
   - **Separate dumps:** open *"No merged folder yet? Merge the base game and the update"*, pick
     both dumps and click **Merge**. Both the usual layout and PS4 PKG Tool's `Image0` + `Sc0`
     layout are accepted.
4. Click **Prepare**. This builds the files the loader needs from *your* game, in `bin\`. You only
   need to do it again after changing the game folder or updating SS2R.
5. Click **PLAY**.

### Updating

Your settings (`bin\bbport.ini`, `bin\ss2_launcher.ini`), saves (`bin\user\`) and backups
(`backups\`) live in the program's folder. Copy them into the new version's folder, then press
**Prepare** once.

## Co-op multiplayer

> [!NOTE]
> Co-op works but is new: expect rough edges, and back up your saves first.

SS2R uses the game's own **LAN** co-op mode: one player hosts and up to three others join. The game's
**Online** option needs Nightdive's PlayFab services, so SS2R hides it: the Multiplayer menu offers
LAN only.

### Launcher settings (Settings → Co-op)

| Setting | What it does |
|---|---|
| **Mode** | **Off** (the game sees no network), **Host** or **Join** |
| **Player name** | The name the other players see, up to 16 characters (empty: "Player 1") |
| **Your address** (Host) | This PC's addresses on every connected network, VPNs included, with **Rescan** and **Copy IP** to send one to your friends |
| **Host's address** (Join) | The address the host sent you. Only needed when it's an internet address |

### Playing

1. **Everyone:** choose **Host** or **Join** in Settings → Co-op, set a player name, and press **PLAY**.
2. **Host:** Multiplayer → **LAN** → **Host game** → **New session**, then wait in the lobby.
3. **Joiners:** Multiplayer → **LAN** → **Join game** (sessions on your network are listed), or
   **Join game by IP** with the host's address.
4. **Host:** press **Start Game** once everyone is in.

Allow SS2R through Windows Firewall when asked (private networks).

### Over the internet

LAN search only works on a local network: joiners use **Join game by IP**.

- **Easiest: a virtual LAN** such as [ZeroTier](https://www.zerotier.com/), [Tailscale](https://tailscale.com/)
  or Radmin VPN. Everyone joins the same virtual network; the host picks that network's address in
  **Your address** and shares it. Nothing else to set up.
- **Direct:** the host forwards **UDP port 5069** on their router to their PC and shares their public IP
  (search "what is my IP"). Joiners enter it as **Host's address**. This doesn't work if your internet
  provider uses CGNAT (common on some NBN, 5G and satellite plans): use a virtual LAN instead, or ask
  your provider to opt you out.

The host never needs the joiners' addresses: SS2R answers whoever contacts it. All players need the
same SS2R version and game version. Co-op with the official PC or console versions isn't possible.

## Release layout

```
SS2R\
  SS2R-Launcher.exe     start here
  bin\                  SS2R.exe (the loader) and its DLLs
                        + made by Prepare: boot-linked.bin and friends, ss2r-overrides.kpf
                          (all from your game)
                        + your settings and saves
  python\               embedded Python, used by Prepare
  scripts\  tools\      the Prepare scripts
  share\                shadPS4's import name list (aerolib.inl)
  licenses\             licences of SS2R and its components
  VERSION               the SS2R version
```

## Building from source

SS2R builds with [MSYS2](https://www.msys2.org/)'s **CLANG64** environment.

1. Install MSYS2, open the **MSYS2 CLANG64** shell and install the toolchain and libraries:
   ```sh
   pacman -S --needed git python \
     mingw-w64-clang-x86_64-toolchain mingw-w64-clang-x86_64-cmake mingw-w64-clang-x86_64-ninja \
     mingw-w64-clang-x86_64-sdl3 mingw-w64-clang-x86_64-ffmpeg mingw-w64-clang-x86_64-vulkan-devel \
     mingw-w64-clang-x86_64-fmt mingw-w64-clang-x86_64-xxhash mingw-w64-clang-x86_64-zydis
   ```
2. Clone and build:
   ```sh
   git clone --recursive https://github.com/<your-username>/SS2R.git
   cd SS2R
   ./build.sh
   ```
   This produces `out/SS2R.exe`, `out/SS2R-Launcher.exe` and `out/bb-gpu-capabilities.exe`.
3. Run `out/SS2R-Launcher.exe`, choose your game folder and press **Prepare**, as above.

To make a release folder and zip (`dist/SS2R`, `dist/SS2R-<version>.zip`):

```sh
PYTHON_DIR=/path/to/python-3.13-embed-amd64 ./package.sh
```

`PYTHON_DIR` is an unzipped [embeddable Python for Windows](https://www.python.org/downloads/windows/).
The script copies every DLL the programs need, and refuses to package if any game file slipped in.

### Versions and releases

The version number lives in the `VERSION` file at the project root (for example `0.2.0`). The build
puts it in the launcher's title bar, Tools → About and the first line of every `run.txt`, and
`package.sh` names the zip after it. It's updated by hand: edit `VERSION` for each release.

GitHub tracks releases by **git tag**, not by file name, so tag each release to match. With the
[GitHub CLI](https://cli.github.com/):

```sh
./build.sh && ./package.sh
v=$(cat VERSION)
gh release create "v$v" "dist/SS2R-$v.zip" --title "SS2R $v" --notes "What's new..."
```

### Running the loader directly

```sh
cd out
./SS2R.exe boot-linked.bin --app0 ../game --import-names import_names.txt --content-profile content.bin
```

Useful environment variables:

| Variable | Effect |
|---|---|
| `BB_FRAME_STATS=1` | Frame timing in the log |
| `BB_LOG_UNRESOLVED=1` | Logs every unhandled system call (very large logs) |
| `BB_VIDEO_RES=3840x2160` | The "TV" size the game sees (4K needs `gnm_force4k`, which the launcher sets) |
| `BB_NEO=1` | Reports a PS4 Pro (not needed for 4K) |
| `BB_ONLINE_SERVER`, `BB_ONLINE_ID`, `BB_ONLINE_ALLOW` | Co-op networking (set by Settings → Co-op) |
| `BB_USER_NAME` | The player name the game shows (set by Settings → Co-op) |
| `BB_NO_OVERRIDES=1` | Turns off the menu overrides (the Online option shows again) |
| `BB_DISPLAY`, `BB_PRESENT_MODE`, `BB_LANGUAGE`, `BB_GAMEPAD` | Set by the launcher's settings |

## How it works

1. **Prepare** (`scripts/prepare.py`, `link_modules.py`) reads the game's `eboot.bin` and links it
   with the PS4 libraries the game ships (`libc.prx`, FMOD, FIOS2, and the script modules `allobjs`,
   `nvscript` and `squirrel`) into one image, `boot-linked.bin`. `gen_import_names.py` names the
   system functions the game imports, and `content_profile.py` records the game's content details.
2. **The loader** (`SS2R.exe`, `src/`) maps that image into memory and runs the game's code
   natively. Each call into a PS4 system library (kernel, threads, files, saves, pads, audio, video
   output, and so on) goes to a Windows implementation in `src/runtime_*.c`.
3. **Graphics** go through a Vulkan backend based on [shadPS4](https://github.com/shadps4-emu/shadPS4)'s
   `video_core` (`gpu/`), which translates the game's GNM command buffers and shaders to Vulkan.
4. **Co-op** uses the game engine's built-in LAN lobby (UDP, port 5069), with the PS4's socket
   functions mapped to Windows sockets (`src/runtime_net.c`). The PlayFab libraries the game ships
   for its Online mode are not used. Prepare also builds `ss2r-overrides.kpf` from the player's own
   `base.kpf`: the multiplayer menus with the Online and room-code buttons hidden. The runtime serves it
   in place of the optional `sshock2ee-patch1.kpf` archive the engine loads after its others.

## Reporting problems

SS2R is still being worked on, and bug reports are the fastest way to get problems fixed. Open an
issue with:

- your GPU and driver version, and Windows version
- what happened, where in the game (deck and area), and what you were doing
- `bin\run.txt` from that session (the launcher's **Copy last STOP** button copies the key error line)
- **your exported saves**, so the bug can be reproduced at the same point in the game:
  1. Close the game.
  2. In the launcher, open the **Saves** tab and click **Export...**.
  3. Choose a folder. The launcher creates `SS2R-saves-<date>` there.
  4. Zip that folder (right-click → *Send to* → *Compressed (zipped) folder*) and attach the zip
     to the issue.

  If the bug happens right after loading a particular save, say which slot it is (shown in the
  Saves tab).

Please don't attach game files or `boot-linked.bin`: they're made from the game and can't be shared.
Exported saves are fine to share: they hold only your progress.

## Version history

### v0.1.0 (2026-11-07)

First release.

**Game**
- Runs the PS4 version (CUSA51878, update 1.04) natively on Windows: splash videos, menus, the full game
  with its scripts, saving and loading
- Controller and keyboard input, remappable
- Native 4K rendering through the engine's own 4K mode, or 1080p
- Co-op for up to 4 players through the game's LAN mode, on a local network or over the internet
  (virtual LAN or port forwarding); the PlayFab-only Online option is hidden
- Trophies recorded locally, with an in-game notice when one unlocks

**Launcher**
- Game folder check, one-click **Prepare**, and merging of separate base game and update dumps
- Settings: render resolution, monitor, fullscreen, presentation mode, FPS limit, game language,
  frame generation (experimental)
- Settings → Co-op: Off / Host / Join, player name, this PC's addresses (Copy IP), host's address
- Controls: controllers per player, button and keyboard remapping, stick dead zones
- Saves: back up, export, import (including decrypted PS4 saves) and restore
- Trophies: pictures, unlock dates and progress; names with your own PS4 trophy key
- Tools: shader cache, logs, individual setup steps, About

**Known issues**
- Save icons are not written
- No 1440p mode, and no upscaling (FSR / DLSS) yet
- Frame generation is mostly untested
- The in-game console can't be opened

## Credits

SS2R is built on the work of:

- **SacredFire** (T3CHNOLOG1C) and its **CTR-NitroFueled-PC** Windows fork: the loader and launcher this
  port started from
- **bbport** (deadinside28): the original native PS4 loader approach
- **Supermedo**'s Windows port
- **[shadPS4](https://github.com/shadps4-emu/shadPS4)**: the Vulkan graphics backend and much PS4 knowledge
- **AMD FidelityFX FSR 3.1**, through FireBurn/FSR-Vulkan (MIT), for frame generation
- SDL3, Dear ImGui, FFmpeg, Vulkan, Zydis, xxHash, fmt, sirit, LibAtrac9, miniz and others: see
  [`licenses/`](licenses/)

## Licence

SS2R is free software, licensed under the **GNU General Public License v2.0 or later**: see
[`LICENSE`](LICENSE). Third-party components keep their own licences, listed in [`licenses/`](licenses/).

## Legal

SS2R is a fan project and is not affiliated with, endorsed by or connected to Nightdive Studios,
Sony Interactive Entertainment or the owners of *System Shock*. *System Shock* and *PlayStation* are
trademarks of their respective owners. SS2R contains no copyrighted game code or assets; it only
runs a copy of the game you dumped yourself from a console you own.
