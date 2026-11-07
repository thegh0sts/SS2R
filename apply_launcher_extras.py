#!/usr/bin/env python3
r"""SS2R launcher: shader cache, About, and merging the base game with the update. Run once from the
project root, after apply_saves_tab.py:

    python apply_launcher_extras.py

Changes to launcher/windows/ss2_launcher.cpp:
  Tools tab   "Shader cache": its size and a Clear button (asks first; off while the game runs).
              "About": version (VERSION file), credits, GPL notice, Open licenses / Open README, and the
              source link once you set kSourceUrl near the top of the About code.
  Play tab    "No merged folder yet? Merge the base game and the update": choose the two dumps (also
              PS4 PKG Tool's Image0 + Sc0 layout), check they match, and make <base's folder>\<title>-<version>
              with hard links (or copies on another drive). The new folder becomes the game folder.
Then ./build.sh.
"""
import sys
from pathlib import Path

EXTRAS = r'''// ---------------------------------------------------------------------------------------------
// Tools tab extras: clearing the shader cache, and About (credits, licences, source).

/// The source code of this release (GPL): set it before publishing, e.g. "https://github.com/<you>/SS2R".
constexpr const char* kSourceUrl = "";

bool g_confirm_clear_cache = false;

/// The game's pipeline cache: <game folder>\user\cache\<title ID> (and UNKNOWN, before the title is known).
std::vector<fs::path> ShaderCacheDirs() {
    const fs::path base = g_app.out / L"user" / L"cache";
    std::vector<fs::path> dirs;
    const std::wstring title = SaveTitleId();
    if (!title.empty()) dirs.push_back(base / title);
    dirs.push_back(base / L"UNKNOWN");
    return dirs;
}

void ClearShaderCache() {
    std::uintmax_t removed = 0;
    std::string error;
    for (const fs::path& dir : ShaderCacheDirs()) {
        std::error_code ec;
        if (!fs::exists(dir, ec)) continue;
        const auto n = fs::remove_all(dir, ec);
        if (ec) error = PathText(dir) + ": " + ec.message();
        else removed += n;
    }
    g_app.notice = !error.empty() ? "Could not clear the cache: " + error
                                  : "Shader cache cleared (" + std::to_string(removed) + " files).";
}

std::string BuildVersion() {
    for (const fs::path& p : {g_app.root / L"VERSION", g_app.out / L"VERSION"}) {
        const std::string v = Trim(ReadText(p));
        if (!v.empty()) return v.substr(0, v.find('\n'));
    }
    return {};
}

void ToolsExtras() {
    App& a = g_app;
    Section("Shader cache");
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(kGrey, "%s", "Shaders are kept so later sessions run smoothly. Clear the cache only after a GPU driver "
                                    "update, a new build, or if you see graphics glitches. The next session rebuilds it "
                                    "(expect some stutter the first time each effect appears).");
    ImGui::PopTextWrapPos();
    {
        static std::uintmax_t bytes = 0;  // measured every few seconds, not every frame
        static double next = 0.0;
        if (ImGui::GetTime() >= next) {
            next = ImGui::GetTime() + 3.0;
            bytes = 0;
            for (const fs::path& dir : ShaderCacheDirs()) bytes += FolderBytes(dir);
        }
        ImGui::TextColored(kGrey, "Size: %s", FormatSize(bytes).c_str());
    }
    ImGui::BeginDisabled(a.game_running);
    if (ImGui::Button("Clear the shader cache")) g_confirm_clear_cache = true;
    ImGui::EndDisabled();
    if (g_confirm_clear_cache) {
        ImGui::OpenPopup("clear_cache");
        g_confirm_clear_cache = false;
    }
    if (ImGui::BeginPopupModal("clear_cache", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextUnformatted("Clear the shader cache? Saves and settings are not touched.");
        if (ImGui::Button("Clear")) {
            ClearShaderCache();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    Section("About");
    const std::string version = BuildVersion();
    ImGui::Text("SS2R %s", version.c_str());
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted("Runs the PS4 version of System Shock 2: 25th Anniversary Remaster natively on Windows. "
                           "You need your own copy of the game: none of its files are included.");
    ImGui::Spacing();
    ImGui::TextUnformatted("Built on SacredFire (T3CHNOLOG1C) and its CTR-NitroFueled-PC Windows fork, bbport "
                           "(deadinside28), Supermedo's Windows port and shadPS4. Licensed under the GNU GPL v2 or later.");
    ImGui::Spacing();
    ImGui::TextUnformatted("Frame generation: AMD FidelityFX FSR 3.1 through FireBurn/FSR-Vulkan (MIT). Also uses SDL3, "
                           "Dear ImGui, FFmpeg, Vulkan, Zydis, xxHash, fmt and others: their licences are in the licenses "
                           "folder.");
    ImGui::Spacing();
    ImGui::TextColored(kGrey, "%s", "Not affiliated with or endorsed by Nightdive Studios, Sony Interactive Entertainment or "
                                    "the System Shock rights holders. All trademarks belong to their owners.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (kSourceUrl[0]) {
        ImGui::TextUnformatted("Source code:");
        ImGui::SameLine();
        ImGui::TextLinkOpenURL(kSourceUrl, kSourceUrl);
    }
    {
        std::error_code ec;
        fs::path licenses = a.root / L"licenses";
        if (!fs::is_directory(licenses, ec)) licenses.clear();
        ImGui::BeginDisabled(licenses.empty());
        if (ImGui::Button("Open licenses folder")) OpenInShell(licenses);
        ImGui::EndDisabled();
        if (fs::is_regular_file(a.root / L"README-Windows.md", ec)) {
            ImGui::SameLine();
            if (ImGui::Button("Open README")) OpenInShell(a.root / L"README-Windows.md");
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Play tab: merge the base game and the update into one folder (the game folder this port needs).
// Files are hard-linked (instant, no extra space) when the new folder is on the same drive as the
// sources, and copied otherwise. The update's files replace the base game's. Also reads PS4 PKG
// Tool's split layout (Image0\ with the game files, Sc0\ with the sce_sys files).

struct DumpInfo {
    bool ok = false;
    std::string title, version, category, problem;
    fs::path files, sys;  // the game files, and the folder holding param.sfo
};

DumpInfo AnalyzeDump(const fs::path& dir) {
    DumpInfo d;
    std::error_code ec;
    if (dir.empty()) return d;
    if (!fs::is_directory(dir, ec)) {
        d.problem = "the folder does not exist";
        return d;
    }
    if (fs::is_regular_file(dir / L"sce_sys" / L"param.sfo", ec)) {
        d.files = dir;
        d.sys = dir / L"sce_sys";
    } else if (fs::is_directory(dir / L"Image0", ec) && fs::is_regular_file(dir / L"Sc0" / L"param.sfo", ec)) {
        d.files = dir / L"Image0";
        d.sys = dir / L"Sc0";
    } else {
        d.problem = "not a PS4 game dump (no sce_sys\\param.sfo)";
        return d;
    }
    const auto sfo = ReadSfo(d.sys / L"param.sfo");
    auto get = [&](const char* k) {
        const auto it = sfo.find(k);
        return it == sfo.end() ? std::string() : it->second;
    };
    d.title = get("TITLE_ID");
    d.version = get("APP_VER");
    d.category = get("CATEGORY");
    d.ok = !d.title.empty();
    if (!d.ok) d.problem = "param.sfo has no title ID";
    return d;
}

std::string DescribeDump(const DumpInfo& d) {
    if (!d.problem.empty()) return d.problem;
    if (!d.ok) return "not chosen";
    const std::string kind = d.category == "gd" ? "base game" : d.category == "gp" ? "update" : "category " + d.category;
    return kind + " " + d.title + " v" + d.version;
}

struct MergeState {
    fs::path base, update;
    DumpInfo base_info, update_info;
    bool loaded = false;
    std::atomic<bool> running{false}, finished{false};
    std::atomic<size_t> done{0}, total{0};
    std::mutex mutex;
    std::string error, result;
    fs::path target;
};
MergeState g_merge;

fs::path MergeTarget() {
    const MergeState& m = g_merge;
    const fs::path parent = m.base.parent_path();
    const std::string name = m.base_info.title + "-" + (m.update_info.version.empty() ? "merged" : m.update_info.version);
    fs::path target = parent / Widen(name);
    if (target == m.base || target == m.update) target = parent / Widen(name + "-merged");
    return target;
}

/// The files of a dump as (source, path in the merged folder).
void DumpFiles(const DumpInfo& d, std::vector<std::pair<fs::path, fs::path>>& files, std::error_code& ec) {
    auto add = [&](const fs::path& from, const fs::path& prefix) {
        for (fs::recursive_directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (it->is_regular_file(e2)) files.emplace_back(it->path(), prefix / fs::relative(it->path(), from, e2));
        }
    };
    add(d.files, {});
    if (!ec && d.sys != d.files / L"sce_sys") add(d.sys, L"sce_sys");
}

void StartMerge() {
    MergeState& m = g_merge;
    if (m.running) return;
    m.target = MergeTarget();
    m.done = 0;
    m.total = 0;
    m.finished = false;
    m.running = true;
    {
        std::scoped_lock lock{m.mutex};
        m.error.clear();
        m.result.clear();
    }
    std::thread([] {
        MergeState& st = g_merge;
        std::vector<std::pair<fs::path, fs::path>> files;
        std::error_code ec;
        DumpFiles(st.base_info, files, ec);
        if (!ec) DumpFiles(st.update_info, files, ec);  // after the base: the update's files win
        std::string error = ec ? "Cannot read the game folders: " + ec.message() : std::string();
        st.total = files.size();
        size_t copied = 0;
        for (size_t i = 0; error.empty() && i < files.size(); ++i) {
            const fs::path dest = st.target / files[i].second;
            std::error_code e2;
            fs::create_directories(dest.parent_path(), e2);
            if (fs::exists(dest, e2)) fs::remove(dest, e2);
            if (!CreateHardLinkW(dest.c_str(), files[i].first.c_str(), nullptr)) {
                if (!CopyFileW(files[i].first.c_str(), dest.c_str(), FALSE)) {
                    error = "Cannot create " + PathText(dest) + " (error " + std::to_string(GetLastError()) + ")";
                    break;
                }
                ++copied;
            }
            st.done = i + 1;
        }
        {
            std::scoped_lock lock{st.mutex};
            st.error = error;
            if (error.empty())
                st.result = "Merged " + std::to_string(files.size()) + " files into " + PathText(st.target) +
                            (copied ? " (" + std::to_string(copied) + " copied: different drive)." : " (hard links).");
        }
        st.running = false;
        st.finished = true;
    }).detach();  // reports through finished; nothing to join at exit
}

void MergeSection() {
    App& a = g_app;
    MergeState& m = g_merge;
    if (!m.loaded) {
        m.loaded = true;
        m.base = fs::path(Widen(a.launcher.Get("base_dir")));
        m.update = fs::path(Widen(a.launcher.Get("update_dir")));
        m.base_info = AnalyzeDump(m.base);
        m.update_info = AnalyzeDump(m.update);
    }
    if (m.finished) {  // the merge thread is done: use the new folder
        m.finished = false;
        std::scoped_lock lock{m.mutex};
        if (m.error.empty()) {
            a.game_dir = m.target;
            SaveLauncher();
            a.notice = m.result + "\nIt is now the game folder: press Prepare.";
        } else {
            a.notice = m.error;
        }
    }
    if (!ImGui::CollapsingHeader("No merged folder yet? Merge the base game and the update")) return;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(kGrey, "%s", "Choose your base game dump and your update 1.04 dump. A new folder is made next to the "
                                    "base game, with the update's files replacing the base game's. On the same drive it "
                                    "takes no extra space (hard links); on another drive the files are copied.");
    ImGui::PopTextWrapPos();
    auto row = [&](const char* label, fs::path& dir, DumpInfo& info, const char* key, const char* want) {
        ImGui::PushID(key);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(ImGui::GetFontSize() * 7.0f);
        ImGui::BeginDisabled(m.running || a.busy);
        if (ImGui::Button("Choose...")) {
            const std::wstring title = std::wstring(L"Choose the ") + Widen(label);
            if (auto picked = PickFolder(a.hwnd, title.c_str(), dir.empty() ? a.game_dir : dir)) {
                dir = *picked;
                info = AnalyzeDump(dir);
                a.launcher.Set(key, PathText(dir));
                SaveLauncher();
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        const bool right = info.ok && info.category == want;
        ImGui::TextColored(right ? kGreen : (dir.empty() ? kGrey : kRed), "%s", DescribeDump(info).c_str());
        if (!dir.empty()) ImGui::TextColored(kGrey, "    %s", PathText(dir).c_str());
        ImGui::PopID();
        return right;
    };
    const bool base_ok = row("Base game", m.base, m.base_info, "base_dir", "gd");
    const bool update_ok = row("Update", m.update, m.update_info, "update_dir", "gp");
    const bool same_title = base_ok && update_ok && m.base_info.title == m.update_info.title;
    if (base_ok && update_ok && !same_title)
        ImGui::TextColored(kRed, "The base game (%s) and the update (%s) are for different editions.", m.base_info.title.c_str(),
                           m.update_info.title.c_str());
    if (m.running) {
        const float progress = m.total ? float(m.done) / float(m.total) : 0.0f;
        ImGui::ProgressBar(progress, ImVec2(-FLT_MIN, 0.0f),
                           (std::to_string(size_t(m.done)) + " / " + std::to_string(size_t(m.total)) + " files").c_str());
    } else {
        ImGui::BeginDisabled(!same_title || a.busy);
        if (ImGui::Button("Merge")) StartMerge();
        ImGui::EndDisabled();
        if (same_title) {
            ImGui::SameLine();
            ImGui::TextColored(kGrey, "into %s", PathText(MergeTarget()).c_str());
        }
    }
}

'''

p = Path('launcher/windows/ss2_launcher.cpp')
if not p.exists():
    sys.exit('run this from the project root (launcher/windows/ss2_launcher.cpp not found)')
s = p.read_text(encoding='utf-8')
if 'void ToolsExtras()' in s:
    sys.exit('already applied')
if 'std::wstring SaveTitleId()' not in s:
    sys.exit('apply_saves_tab.py first (this uses its helpers)')
edits = [
    ('void PlayTab() {', 'void MergeSection();  // below, with the Saves helpers it uses\nvoid ToolsExtras();\n\nvoid PlayTab() {'),
    ('    Section("Prepared files");', '    MergeSection();\n    Section("Prepared files");'),
    ('    ImGui::TextColored(kGrey, "Modules linked: %s", Narrow(kModules).c_str());\n}',
     '    ImGui::TextColored(kGrey, "Modules linked: %s", Narrow(kModules).c_str());\n    ToolsExtras();\n}'),
    ('void Frame() {', EXTRAS + 'void Frame() {'),
]
for old, new in edits:
    if s.count(old) != 1:
        sys.exit(f'expected exactly one match (nothing changed):\n{old}')
    s = s.replace(old, new)
p.write_text(s, encoding='utf-8', newline='\n')
print('changed: launcher/windows/ss2_launcher.cpp (shader cache, About, merge)')
