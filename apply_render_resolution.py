#!/usr/bin/env python3
r"""SS2R launcher: Render resolution (1080p / 4K) in Settings > Display. Run once from the project root,
after apply_video_res.py and apply_saves_tab.py:

    python apply_render_resolution.py

4K works through the engine's own switch, gnm_force4k (kexEngine's PS4 renderer: "Forces 4k resolution
upon startup"), saved in the game's settings file kexengine.cfg, plus a 4K "TV" from the loader
(BB_VIDEO_RES=3840x2160) so the two match. On PLAY the launcher:
  * sets BB_VIDEO_RES (3840x2160 for 4K, unset for 1080p);
  * sets  seta gnm_force4k "1"/"0"  in <game folder>\user\savedata\<user>\<title>\SAVEDATA\kexengine.cfg
    (the game makes that file on its first start; until then 4K applies from the second start).
Then ./build.sh.
"""
import sys
from pathlib import Path

p = Path('launcher/windows/ss2_launcher.cpp')
if not p.exists():
    sys.exit('run this from the project root (launcher/windows/ss2_launcher.cpp not found)')
s = p.read_text(encoding='utf-8')
if 'SetGameForce4k' in s:
    sys.exit('already applied')
if 'std::wstring SaveTitleId()' not in s:
    sys.exit('apply_saves_tab.py first (this uses its save folder lookup)')
probe = Path('src/probe.c')
if probe.exists() and 'BB_VIDEO_RES' not in probe.read_text(encoding='utf-8'):
    print('CHECK: src/probe.c has no BB_VIDEO_RES: run apply_video_res.py too, or 4K will not take effect')

HELPER = r'''// ---------------------------------------------------------------------------------------------
// Render resolution: kexEngine's own 4K switch (gnm_force4k) lives in the game's settings file,
// <save root>\SAVEDATA\kexengine.cfg, which the game writes on its first start.

/// Sets  seta gnm_force4k "1"/"0"  in kexengine.cfg; false when the game has not made the file yet.
bool SetGameForce4k(bool on) {
    const fs::path cfg = SaveRoot() / L"SAVEDATA" / L"kexengine.cfg";
    if (!IsFile(cfg)) return false;
    std::string text = ReadText(cfg);
    const std::string line = std::string("seta gnm_force4k \"") + (on ? "1" : "0") + "\"";
    size_t at = 0;
    bool found = false;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        if (text.compare(at, 17, "seta gnm_force4k ") == 0) {
            const size_t stop = (end > at && text[end - 1] == '\r') ? end - 1 : end;
            if (text.compare(at, stop - at, line) == 0) return true;  // already so
            text.replace(at, stop - at, line);
            found = true;
            break;
        }
        at = end + 1;
    }
    if (!found) {
        if (!text.empty() && text.back() != '\n') text += '\n';
        text += line + "\n";
    }
    return WriteTextAtomic(cfg, text);
}

'''

edits = [
    ('constexpr int kFpsLimits[] = {',
     'constexpr Choice kRenderResolutions[] = {\n'
     '    {"", "1080p", "1080"},\n'
     '    {"", "4K (2160p)", "2160"},\n'
     '};\n'
     'bool SetGameForce4k(bool on);  // with the Saves helpers, below\n'
     'constexpr int kFpsLimits[] = {'),
    ('    SetEnv(L"BB_LANGUAGE", a.launcher.Get("language", "1"));\n',
     '    SetEnv(L"BB_LANGUAGE", a.launcher.Get("language", "1"));\n'
     '    {  // Render resolution: the engine\'s 4K switch and a matching 4K "TV"\n'
     '        const bool four_k = a.launcher.Get("render_resolution", "1080") == "2160";\n'
     '        SetEnv(L"BB_VIDEO_RES", four_k ? "3840x2160" : "");\n'
     '        if (!SetGameForce4k(four_k) && four_k)\n'
     '            a.notice = "4K takes effect from the next start (the game creates its settings file on the first one).";\n'
     '    }\n'),
    ('    LauncherChoice("Presentation", "present_mode", kPresentModes, "Mailbox", 14.0f);',
     '    LauncherChoice("Render resolution", "render_resolution", kRenderResolutions, "1080", 14.0f);\n'
     '    HelpMarker("The resolution the game draws at. 4K uses the engine\'s own PS4 Pro 4K mode: much sharper, "\n'
     '               "and still worth it on a 1080p or 1440p monitor (it is scaled down, like supersampling). "\n'
     '               "Needs a stronger GPU.");\n'
     '    LauncherChoice("Presentation", "present_mode", kPresentModes, "Mailbox", 14.0f);'),
    ('void Frame() {', HELPER + 'void Frame() {'),
]
for old, new in edits:
    if s.count(old) != 1:
        sys.exit(f'expected exactly one match in ss2_launcher.cpp (nothing changed):\n{old}')
    s = s.replace(old, new)

old_note = ('    ImGui::TextColored(kGrey, "Upscaling (FSR/DLSS), internal resolution and ultrawide are tied to Crash Team "\n'
            '                              "Racing\'s shaders and patches in this loader, so they are not offered here yet.");')
if old_note in s:
    s = s.replace(old_note, '    ImGui::TextColored(kGrey, "Upscaling (FSR/DLSS) is not offered yet: for the sharpest picture, use "\n'
                            '                              "Render resolution 4K.");')
p.write_text(s, encoding='utf-8', newline='\n')
print('changed: launcher/windows/ss2_launcher.cpp (Render resolution)')
