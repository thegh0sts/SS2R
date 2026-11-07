// SPDX-License-Identifier: GPL-2.0-or-later
// SS2R launcher (Windows): sets up and starts System Shock 2: 25th Anniversary Remaster with the
// sacredfire loader. "Prepare" runs prepare.py, link_modules.py (SS2's module list),
// gen_import_names.py and content_profile.py; "Play" starts out\SS2R.exe with the prepared
// files. Output appears in the Log panel and is also written to out\run.txt.
// Settings and Controls edit the same bbport.ini keys as the in-game menu (Insert or L3+R3); the
// Controls tab and its keyboard page come from the CTR-NitroFueled-PC launcher (launcher.cpp).
// Dear ImGui on Win32 + Direct3D 11. This launcher's own settings: out\ss2_launcher.ini.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>  // before windows.h
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "gamepads.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

namespace fs = std::filesystem;

namespace {

// The sce_module / prx PRXs linked natively, in this order. Party.prx stays module 4: probe.c
// skips its initializer by index.
constexpr const wchar_t* kModules =
    L"libc.prx libSceFios2.prx libfmod.prx libPlayFabMultiplayer.prx Party.prx libSceNpToolkit2.prx "
    L"allobjs-orbis-x86_64.prx nvscript-orbis-x86_64.prx squirrel-orbis-x86_64.prx";
#ifndef SS2R_VERSION
#define SS2R_VERSION "dev"  // set by gpu/CMakeLists.txt from VERSION
#endif
constexpr const wchar_t* kWindowTitle = L"System Shock 2 Remastered - SS2R " SS2R_VERSION;

// ---------------------------------------------------------------------------------------------
// Text and file helpers

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::string PathText(const fs::path& p) { return Narrow(p.wstring()); }
std::wstring Quote(const std::wstring& s) { return L"\"" + s + L"\""; }
std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}
std::string ReadText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
bool WriteTextAtomic(const fs::path& p, const std::string& text) {
    fs::path temp = p;
    temp += L".tmp";
    {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << text;
        if (!f) return false;
    }
    return MoveFileExW(temp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
bool IsFile(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}
bool IsDir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

// The copied CTR code is bilingual; this launcher is English.
bool g_spanish = false;
const char* T(const char* es, const char* en) { return g_spanish ? es : en; }

// ---------------------------------------------------------------------------------------------
// key=value files, edited in place: comments, order and unknown keys are kept (from launcher.cpp).

struct Ini {
    std::vector<std::string> lines;
    std::map<std::string, std::string> values;

    void Load(const fs::path& path) {
        lines.clear();
        values.clear();
        std::stringstream ss(ReadText(path));
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
            const auto t = Trim(line);
            if (t.empty() || t[0] == '#' || t[0] == ';') continue;
            const auto eq = t.find('=');
            if (eq == std::string::npos) continue;
            values[Trim(t.substr(0, eq))] = Trim(t.substr(eq + 1));
        }
    }
    std::string Get(const std::string& key, const std::string& fallback = "") const {
        const auto it = values.find(key);
        return it == values.end() ? fallback : it->second;
    }
    bool Flag(const std::string& key, bool fallback) const {
        const auto v = Get(key);
        return v.empty() ? fallback : v != "0";
    }
    int Int(const std::string& key, int fallback) const {
        const auto v = Get(key);
        if (v.empty()) return fallback;
        char* end = nullptr;
        const long n = std::strtol(v.c_str(), &end, 10);
        return end && *end == '\0' ? int(n) : fallback;
    }
    void Set(const std::string& key, const std::string& value) {
        values[key] = value;
        erased.erase(key);
    }
    // Removes the key: its line is dropped on Save.
    void Erase(const std::string& key) {
        values.erase(key);
        erased.insert(key);
    }
    std::set<std::string> erased;
    bool Save(const fs::path& path, const char* header) {
        std::vector<std::string> out;
        std::map<std::string, bool> written;
        for (const auto& line : lines) {
            const auto t = Trim(line);
            const auto eq = t.find('=');
            if (!t.empty() && t[0] != '#' && t[0] != ';' && eq != std::string::npos) {
                const auto key = Trim(t.substr(0, eq));
                if (erased.count(key)) continue;
                const auto it = values.find(key);
                if (it != values.end()) {
                    if (!written[key]) out.push_back(key + "=" + it->second);
                    written[key] = true;
                    continue;
                }
            }
            out.push_back(line);
        }
        if (lines.empty() && header) out.push_back(header);
        for (const auto& [key, value] : values) {
            if (!written[key]) out.push_back(key + "=" + value);
        }
        std::string text;
        for (const auto& line : out) text += line + "\n";
        if (!WriteTextAtomic(path, text)) return false;
        lines = out;
        erased.clear();
        return true;
    }
};

// ---------------------------------------------------------------------------------------------
// Processes, folders, shell

fs::path ExeDir() {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    return fs::path(std::wstring(buf, n)).parent_path();
}

/// The project root: the first folder at or above the exe that has scripts\link_modules.py.
fs::path FindRoot() {
    fs::path dir = ExeDir();
    for (int i = 0; i < 4 && !dir.empty(); ++i) {
        if (IsFile(dir / L"scripts" / L"link_modules.py")) return dir;
        dir = dir.parent_path();
    }
    return ExeDir().parent_path();
}

struct Python {
    std::wstring exe;
    std::wstring args;  // before the script, e.g. L"-3 "
    std::string description;
};

std::optional<Python> FindPython(const fs::path& root) {
    const fs::path bundled = root / L"python" / L"python.exe";
    if (IsFile(bundled)) return Python{bundled.wstring(), L"", "python\\ (bundled)"};
    const fs::path msys = L"C:\\msys64\\clang64\\bin\\python.exe";
    if (IsFile(msys)) return Python{msys.wstring(), L"", PathText(msys)};
    wchar_t found[MAX_PATH * 4];
    if (SearchPathW(nullptr, L"py.exe", nullptr, DWORD(std::size(found)), found, nullptr))
        return Python{found, L"-3 ", Narrow(found)};
    if (SearchPathW(nullptr, L"python.exe", nullptr, DWORD(std::size(found)), found, nullptr)) {
        // Windows' "App execution alias" opens the Microsoft Store instead of running Python.
        if (std::wstring(found).find(L"WindowsApps") == std::wstring::npos) return Python{found, L"", Narrow(found)};
    }
    return std::nullopt;
}

struct Captured {
    bool started = false;
    DWORD code = 1;
    std::string output;
};

/// Runs a console program without a window and returns its output (stdout + stderr).
Captured RunCaptured(const std::wstring& exe, const std::wstring& command_line, const fs::path& cwd,
                     DWORD timeout_ms) {
    Captured result;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &sa, 0)) return result;
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = write;
    si.hStdError = write;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = command_line;
    const BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                   cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    CloseHandle(write);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(read);
        return result;
    }
    result.started = true;
    std::thread reader([&] {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(read, buf, sizeof buf, &n, nullptr) && n) result.output.append(buf, n);
    });
    if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
    reader.join();
    GetExitCodeProcess(pi.hProcess, &result.code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(read);
    return result;
}

std::optional<fs::path> PickFolder(HWND owner, const wchar_t* title, const fs::path& start) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return std::nullopt;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(title);
    if (!start.empty() && IsDir(start)) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }
    std::optional<fs::path> result;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = fs::path(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

void OpenInShell(const fs::path& target) {
    ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------------------------------------
// State

enum LineKind { kNormal, kGameLog, kProblem, kRuntime, kCommand };

struct LogLine {
    std::string text;
    LineKind kind;
};

struct Step {
    std::string description;
    std::wstring exe, command_line;
    fs::path cwd;
    bool tee_run_txt = false;
};

struct App {
    HWND hwnd = nullptr;
    fs::path root, out, bin;  // bin: where SDL3.dll and bb-gpu-capabilities.exe are (out\)
    std::optional<Python> python;
    Ini launcher;  // out\ss2_launcher.ini
    Ini game;      // bbport.ini (GameIniPath)
    fs::path game_dir;
    char extra_args[512] = "";

    std::mutex mutex;  // guards the log, the strings below and displays/gamepads
    std::deque<LogLine> log;
    std::string last_stop, last_api;
    std::string status = "Ready.";

    std::thread worker;
    std::atomic<bool> busy{false}, stop_requested{false}, game_running{false};
    std::atomic<HANDLE> job{nullptr};
    bool auto_scroll = true;
    bool scroll_to_end = false;
    std::string notice;

    // Monitors and controllers (bb-gpu-capabilities).
    std::vector<std::pair<std::string, std::string>> displays;  // value (number), label
    std::vector<std::pair<std::string, std::string>> gamepads;  // GUID, name
    std::thread probe;
    std::atomic<bool> probe_done{false};
};

App g_app;

// Colours: SHODAN's cyan on a dark steel background. The copied CTR code uses these names.
const ImVec4 kText{0.90f, 0.92f, 0.93f, 1.0f};
const ImVec4 kGrey{0.55f, 0.59f, 0.63f, 1.0f};
const ImVec4 kGreen{0.40f, 0.85f, 0.55f, 1.0f};
const ImVec4 kRed{1.0f, 0.40f, 0.38f, 1.0f};
const ImVec4 kYellow{1.0f, 0.80f, 0.32f, 1.0f};
const ImVec4 kAccent{0.36f, 0.82f, 0.86f, 1.0f};
const ImVec4 kAccentHover{0.48f, 0.90f, 0.93f, 1.0f};
const ImVec4 kAccentActive{0.26f, 0.68f, 0.72f, 1.0f};
const ImVec4 kBg{0.059f, 0.063f, 0.075f, 1.0f};
const ImVec4 kPanel{0.090f, 0.098f, 0.118f, 1.0f};
const ImVec4 kCard{0.110f, 0.120f, 0.143f, 1.0f};
const ImVec4 kFrame{0.145f, 0.158f, 0.188f, 1.0f};
const ImVec4 kFrameHover{0.185f, 0.205f, 0.245f, 1.0f};
const ImVec4 kFrameActive{0.225f, 0.250f, 0.300f, 1.0f};
const ImVec4 kBorder{0.190f, 0.210f, 0.250f, 1.0f};

// Segoe Fluent Icons / Segoe MDL2 Assets code points, as UTF-8.
#define ICON_PLAY "\xEE\x9D\xA8"      // E768
#define ICON_SETTINGS "\xEE\x9C\x93"  // E713
#define ICON_GAMEPAD "\xEE\x9F\xBC"   // E7FC
#define ICON_KEYBOARD "\xEE\x9D\xA5"  // E765
#define ICON_TOOLS "\xEE\xA4\x8F"     // E90F
#define ICON_SAVE "\xEE\x9D\x8E"      // E74E
#define ICON_TROPHY "\xEE\x9C\xB4"    // E734 (star)
#define ICON_FOLDER "\xEE\xA2\xB7"    // E8B7
#define ICON_CHECK "\xEE\x9C\xBE"     // E73E
#define ICON_WARN "\xEE\x9E\xBA"      // E7BA
#define ICON_INFO "\xEE\xA5\x86"      // E946
#define ICON_BLUETOOTH "\xEE\x9C\x82" // E702
#define ICON_USB "\xEE\xA2\x8E"       // E88E
#define ICON_UNDO "\xEE\x9E\xA7"      // E7A7
#define ICON_VIBRATE "\xEE\xA1\xB7"   // E877

ImFont* g_font_heading = nullptr;
ImFont* g_font_title = nullptr;
float g_ui_scale = 1.0f;

fs::path LauncherIniPath() { return g_app.out / L"ss2_launcher.ini"; }

/// bbport.ini: the game's settings (read by the GPU layer and src/runtime_pad.c at start, and
/// written by the in-game menu). Next to SS2R.exe (out\), where the game runs.
/// If your copy keeps it elsewhere, change this one line.
fs::path GameIniPath() { return g_app.out / L"bbport.ini"; }

void SaveLauncher() {
    App& a = g_app;
    a.launcher.Set("game_dir", PathText(a.game_dir));
    a.launcher.Set("extra_args", a.extra_args);
    if (!a.launcher.Save(LauncherIniPath(), "# SS2R launcher settings")) a.notice = "Could not save ss2_launcher.ini.";
}

void SaveGameIni() {
    if (!g_app.game.Save(GameIniPath(), "# Game settings (also changed by the in-game menu: Insert or L3+R3)"))
        g_app.notice = "Could not save bbport.ini.";
}

void LoadSettings() {
    App& a = g_app;
    a.launcher.Load(LauncherIniPath());
    a.game.Load(GameIniPath());
    const std::string dir = a.launcher.Get("game_dir");
    if (!dir.empty()) a.game_dir = Widen(dir);
    else if (IsFile(a.root / L"game" / L"eboot.bin")) a.game_dir = a.root / L"game";
    std::snprintf(a.extra_args, sizeof a.extra_args, "%s", a.launcher.Get("extra_args").c_str());
}

// ---------------------------------------------------------------------------------------------
// Log and background tasks

LineKind Classify(const std::string& line) {
    if (line.rfind("STOP:", 0) == 0 || line.rfind("Fault:", 0) == 0 || line.rfind("ERROR", 0) == 0 ||
        line.find("Traceback") != std::string::npos)
        return kProblem;
    if (line.rfind("> ", 0) == 0) return kCommand;
    if (line.size() > 9 && isdigit((unsigned char)line[0]) && isdigit((unsigned char)line[1]) && line[2] == ':')
        return kGameLog;  // the engine's own "hh:mm:ss: ..." lines
    if (line.rfind("Runtime:", 0) == 0 || line.rfind("GPU", 0) == 0 || line.rfind("Module ", 0) == 0) return kRuntime;
    return kNormal;
}

void AddLog(const std::string& raw) {
    std::string line = raw;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    std::scoped_lock lock{g_app.mutex};
    App& a = g_app;
    if (line.rfind("STOP:", 0) == 0 || line.rfind("Fault:", 0) == 0) {
        a.last_stop = line;
        a.last_api.clear();
    }
    if (line.rfind("API:", 0) == 0 && !a.last_stop.empty() && a.last_api.empty()) a.last_api = line;
    a.log.push_back({line, Classify(line)});
    while (a.log.size() > 30000) a.log.pop_front();
}

std::wstring PythonCommand(const std::wstring& script, const std::wstring& args) {
    const Python& p = *g_app.python;
    return Quote(p.exe) + L" " + p.args + L"-X utf8 " + Quote(script) + (args.empty() ? L"" : L" " + args);
}

/// Runs one program, streaming its output (stdout + stderr) into the log. Returns its exit code.
DWORD RunStep(const Step& step) {
    App& a = g_app;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &sa, 0)) return 1;
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = write;
    si.hStdError = write;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = step.command_line;
    const BOOL ok = CreateProcessW(step.exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                                   step.cwd.empty() ? nullptr : step.cwd.c_str(), &si, &pi);
    CloseHandle(write);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(read);
        AddLog("ERROR: could not start " + PathText(step.exe) + " (Windows error " + std::to_string(GetLastError()) + ")");
        return 1;
    }
    // A job object, so Stop ends the program and anything it started.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    a.job = job;
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    std::ofstream tee;
    if (step.tee_run_txt) tee.open(a.out / L"run.txt", std::ios::binary | std::ios::trunc);
    std::string pending;
    char buf[8192];
    DWORD n = 0;
    while (ReadFile(read, buf, sizeof buf, &n, nullptr) && n) {
        if (tee) tee.write(buf, n);
        pending.append(buf, n);
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            AddLog(pending.substr(0, nl));
            pending.erase(0, nl + 1);
        }
    }
    if (!pending.empty()) AddLog(pending);
    CloseHandle(read);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    a.job = nullptr;
    if (job) CloseHandle(job);
    return code;
}

/// Runs the steps in order on a background thread. A non-zero exit code ends a setup task;
/// the game's own exit code is only reported.
void StartTask(const std::string& name, std::vector<Step> steps, bool is_game) {
    App& a = g_app;
    if (a.busy || steps.empty()) return;
    if (a.worker.joinable()) a.worker.join();
    {
        std::scoped_lock lock{a.mutex};
        a.log.clear();
        a.last_stop.clear();
        a.last_api.clear();
        a.status = name + "...";
    }
    a.busy = true;
    a.game_running = is_game;
    a.stop_requested = false;
    a.scroll_to_end = true;
    a.worker = std::thread([name, steps = std::move(steps), is_game] {
        App& app = g_app;
        std::string result = name + " finished.";
        for (const Step& step : steps) {
            AddLog("> " + step.description);
            const DWORD code = RunStep(step);
            if (app.stop_requested) {
                result = name + " stopped.";
                break;
            }
            if (is_game) {
                result = "Game exited with code " + std::to_string(int(code)) +
                         (code == 0xC0000135 ? " (a DLL was not found)." : ".");
                break;
            }
            if (code != 0) {
                result = name + " failed at: " + step.description + " (exit code " + std::to_string(code) + ").";
                AddLog("ERROR: " + result);
                break;
            }
        }
        {
            std::scoped_lock lock{app.mutex};
            app.status = result;
        }
        app.game_running = false;
        app.busy = false;
    });
}

void StopTask() {
    App& a = g_app;
    a.stop_requested = true;
    if (HANDLE job = a.job.load()) TerminateJobObject(job, 1);
}

// Monitors and controllers, as the game's own tools see them (bb-gpu-capabilities.exe).
void StartProbe() {
    App& a = g_app;
    a.probe_done = false;
    a.probe = std::thread([] {
        App& app = g_app;
        const fs::path tool = app.bin / L"bb-gpu-capabilities.exe";
        std::vector<std::pair<std::string, std::string>> displays, pads;
        const Captured d = RunCaptured(tool.wstring(), Quote(tool.wstring()) + L" --displays", app.bin, 10000);
        std::stringstream ds(d.output);
        std::string line;
        int number = 0;
        while (std::getline(ds, line)) {
            line = Trim(line);
            const auto t1 = line.find('\t');
            if (t1 == std::string::npos) continue;
            const auto t2 = line.find('\t', t1 + 1);
            ++number;
            std::string label = std::to_string(number) + ": " + line.substr(0, t1) + " (" +
                                line.substr(t1 + 1, t2 == std::string::npos ? std::string::npos : t2 - t1 - 1) + ")";
            if (t2 != std::string::npos && line.substr(t2 + 1) == "1") label += " - primary";
            displays.emplace_back(std::to_string(number), label);
        }
        const Captured p = RunCaptured(tool.wstring(), Quote(tool.wstring()) + L" --gamepads", app.bin, 10000);
        std::stringstream ps(p.output);
        while (std::getline(ps, line)) {
            line = Trim(line);
            const auto t = line.find('\t');
            if (t != std::string::npos) pads.emplace_back(line.substr(0, t), line.substr(t + 1));
        }
        std::scoped_lock lock{app.mutex};
        app.displays = std::move(displays);
        app.gamepads = std::move(pads);
        app.probe_done = true;
    });
}

// ---------------------------------------------------------------------------------------------
// Checks and tasks

struct GameCheck {
    bool eboot = false, sys = false, libc = false, data = false;
    bool ok() const { return eboot && sys && libc && data; }
};

GameCheck CheckGame(const fs::path& dir) {
    GameCheck c;
    if (dir.empty()) return c;
    c.eboot = IsFile(dir / L"eboot.bin");
    c.sys = IsFile(dir / L"sce_sys" / L"param.sfo");
    c.libc = IsFile(dir / L"sce_module" / L"libc.prx");
    c.data = IsFile(dir / L"base.kpf");
    return c;
}

struct PrepCheck {
    bool loader = false, linked = false, names = false, content = false;
    bool ok() const { return loader && linked && names && content; }
};

PrepCheck CheckPrepared() {
    const fs::path& o = g_app.out;
    return {IsFile(o / L"SS2R.exe"), IsFile(o / L"boot-linked.bin"), IsFile(o / L"import_names.txt"),
            IsFile(o / L"content.bin")};
}

Step PyStep(const std::string& description, const fs::path& script, const std::wstring& args) {
    App& a = g_app;
    return {description, a.python->exe, PythonCommand(script.wstring(), args), a.root, false};
}

/// Where the Prepare scripts write (the folder the game runs in).
std::wstring OutArg() { return L" --out " + Quote(g_app.out.wstring()); }
/// gen_import_names.py: the release ships shadPS4's aerolib.inl in share\ (no download).
std::wstring NamesArgs() {
    const fs::path aerolib = g_app.root / L"share" / L"aerolib.inl";
    return OutArg() + (IsFile(aerolib) ? L" --aerolib " + Quote(aerolib.wstring()) : L"");
}

std::vector<Step> PrepareSteps() {
    App& a = g_app;
    const std::wstring game = Quote(a.game_dir.wstring());
    return {
        PyStep("Preparing the executable (prepare.py)", a.root / L"scripts" / L"prepare.py", game + OutArg()),
        PyStep("Linking the game's modules (link_modules.py)", a.root / L"scripts" / L"link_modules.py",
               game + OutArg() + L" --modules " + kModules),
        PyStep("Naming the remaining imports (gen_import_names.py)", a.root / L"tools" / L"gen_import_names.py", NamesArgs()),
        PyStep("Writing the content profile (content_profile.py)", a.root / L"scripts" / L"content_profile.py", game + OutArg()),
        PyStep("Hiding the Online option in the menus (make_overrides.py)", a.root / L"scripts" / L"make_overrides.py",
               game + L" --out " + Quote(a.out.wstring())),
    };
}

std::wstring LoaderArgs() {
    App& a = g_app;
    std::wstring args = L" boot-linked.bin --app0 " + Quote(a.game_dir.wstring()) +
                        L" --import-names import_names.txt --content-profile content.bin";
    if (a.extra_args[0]) args += L" " + Widen(a.extra_args);
    return args;
}

void SetEnv(const wchar_t* name, const std::string& value) {
    SetEnvironmentVariableW(name, value.empty() ? nullptr : Widen(value).c_str());
}

// The game's language (BB_LANGUAGE): PS4 system language codes. A language the game's files do
// not have shows English.
struct Choice {
    const char* es;
    const char* en;
    const char* value;
};
constexpr Choice kGameLanguages[] = {
    {"", "English (US)", "1"},     {"", "English (UK)", "18"},   {"", "French", "2"},
    {"", "German", "4"},           {"", "Italian", "5"},         {"", "Spanish (Spain)", "3"},
    {"", "Spanish (Latin America)", "20"}, {"", "Russian", "8"}, {"", "Polish", "16"},
    {"", "Portuguese (Brazil)", "17"},     {"", "Japanese", "0"},
};
constexpr Choice kPresentModes[] = {
    {"", "Mailbox (recommended)", "Mailbox"},
    {"", "FIFO (VSync)", "Fifo"},
    {"", "FIFO Relaxed", "FifoRelaxed"},
    {"", "Immediate (no VSync)", "Immediate"},
};
constexpr Choice kRenderResolutions[] = {
    {"", "1080p", "1080"},
    {"", "4K (2160p)", "2160"},
};
bool SetGameForce4k(bool on);  // with the Saves helpers, below
constexpr int kFpsLimits[] = {0, 30, 60, 72, 90, 120, 144, 240};

std::string CoopMode();  // Co-op settings, below
std::string CoopDisplayName();
std::string CoopPsnId();
bool SetGameCvar(const std::string& name, std::string value);

void StartGame() {
    App& a = g_app;
    SaveLauncher();
    SaveGameIni();
    Pads::Stop();  // the game takes the controllers
    SetEnv(L"BB_DISPLAY", a.launcher.Get("display"));
    SetEnv(L"BB_PRESENT_MODE", a.launcher.Get("present_mode", "Mailbox"));
    SetEnv(L"BB_LANGUAGE", a.launcher.Get("language", "1"));
    {  // Render resolution: the engine's 4K switch and a matching 4K "TV"
        const bool four_k = a.launcher.Get("render_resolution", "1080") == "2160";
        SetEnv(L"BB_VIDEO_RES", four_k ? "3840x2160" : "");
        if (!SetGameForce4k(four_k) && four_k)
            a.notice = "4K takes effect from the next start (the game creates its settings file on the first one).";
    }
    {   // Co-op (Settings > Co-op): the runtime's network mode, the player name
        const std::string mode = CoopMode();
        const bool coop = mode != "off";
        const std::string shown = CoopDisplayName();
        SetEnv(L"BB_USER_NAME", shown);
        SetEnv(L"BB_ONLINE_SERVER", coop ? "127.0.0.1" : "");
        SetEnv(L"BB_ONLINE_ID", coop ? CoopPsnId() : "");
        SetEnv(L"BB_ONLINE_ALLOW", mode == "join" ? a.launcher.Get("coop_join") : "");
        if (!shown.empty()) SetGameCvar("net_player_name", shown);
    }
    SetEnv(L"BB_MENU_LANGUAGE", "en");
    SetEnv(L"BB_GAMEPAD", a.launcher.Get("gamepad"));
    SetEnv(L"BB_PAD_DEADZONE", a.launcher.Get("pad_deadzone"));
    SetEnv(L"BB_PAD_DEADZONE_OUTER", a.launcher.Get("pad_deadzone_outer"));
    SetEnv(L"BB_PAD_CENTER_CAL", a.launcher.Flag("pad_center_cal", true) ? "" : "0");
    // The loader's DLLs: next to it, in out\bin, and MSYS2's clang64 runtime (libc++, FFmpeg...).
    {
        static std::wstring original;
        static bool saved = false;
        if (!saved) {
            wchar_t current[32768];
            const DWORD n = GetEnvironmentVariableW(L"PATH", current, DWORD(std::size(current)));
            if (n && n < std::size(current)) original.assign(current, n);
            saved = true;
        }
        std::wstring path = a.out.wstring() + L";" + (a.out / L"bin").wstring() + L";C:\\msys64\\clang64\\bin";
        if (!original.empty()) path += L";" + original;
        SetEnvironmentVariableW(L"PATH", path.c_str());
    }
    const fs::path exe = a.out / L"SS2R.exe";
    StartTask("Game", {{"Starting SS2R.exe", exe.wstring(), Quote(exe.wstring()) + LoaderArgs(), a.out, true}}, true);
}

void StartImportSummary() {
    App& a = g_app;
    const fs::path exe = a.out / L"SS2R.exe";
    StartTask("Import list",
              {{"Listing how every import is resolved (--list-imports)", exe.wstring(),
                Quote(exe.wstring()) + LoaderArgs() + L" --list-imports", a.out, false}},
              false);
}

// ---------------------------------------------------------------------------------------------
// UI helpers (the copied CTR code uses Section, BeginCard/EndCard, HelpMarker, LauncherFlag)

void Section(const char* label) {
    ImGui::Spacing();
    if (g_font_heading) ImGui::PushFont(g_font_heading, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::SeparatorText(label);
    ImGui::PopStyleColor();
    if (g_font_heading) ImGui::PopFont();
}

void BeginCard(const char* id) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
    ImGui::PushStyleColor(ImGuiCol_Border, kBorder);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f * g_ui_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * g_ui_scale, 12.0f * g_ui_scale));
    ImGui::BeginChild(id, ImVec2(0, 0),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
}
void EndCard() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

void HelpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void GameFlag(const char* key, const char* label, bool fallback, const char* help = nullptr) {
    bool v = g_app.game.Flag(key, fallback);
    if (ImGui::Checkbox(label, &v)) {
        g_app.game.Set(key, v ? "1" : "0");
        SaveGameIni();
    }
    if (help) HelpMarker(help);
}

void LauncherFlag(const char* key, const char* label, bool fallback, const char* help = nullptr) {
    bool v = g_app.launcher.Flag(key, fallback);
    if (ImGui::Checkbox(label, &v)) {
        g_app.launcher.Set(key, v ? "1" : "0");
        SaveLauncher();
    }
    if (help) HelpMarker(help);
}

template <size_t N>
void LauncherChoice(const char* label, const char* key, const Choice (&choices)[N], const char* fallback, float width_em) {
    const std::string current = g_app.launcher.Get(key, fallback);
    const Choice* selected = &choices[0];
    for (const auto& c : choices)
        if (current == c.value) selected = &c;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * width_em);
    if (ImGui::BeginCombo(label, selected->en)) {
        for (const auto& c : choices) {
            if (ImGui::Selectable(c.en, &c == selected)) {
                g_app.launcher.Set(key, c.value);
                SaveLauncher();
            }
        }
        ImGui::EndCombo();
    }
}

template <size_t N>
void GameChoice(const char* label, const char* key, const Choice (&choices)[N], const char* fallback, float width_em) {
    const std::string current = g_app.game.Get(key, fallback);
    const Choice* selected = &choices[0];
    for (const auto& c : choices)
        if (current == c.value) selected = &c;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * width_em);
    if (ImGui::BeginCombo(label, selected->en)) {
        for (const auto& c : choices) {
            if (ImGui::Selectable(c.en, &c == selected)) {
                g_app.game.Set(key, c.value);
                SaveGameIni();
            }
        }
        ImGui::EndCombo();
    }
}

void CheckLine(bool ok, const char* label, const char* missing_hint) {
    ImGui::TextColored(ok ? kGreen : kRed, ok ? "  OK  " : "  --  ");
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
    if (!ok && missing_hint && *missing_hint) {
        ImGui::SameLine();
        ImGui::TextColored(kGrey, "(%s)", missing_hint);
    }
}

bool AccentButton(const char* label, const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentActive);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.03f, 0.06f, 0.07f, 1.0f));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}

// ---------------------------------------------------------------------------------------------
// Controls tab and its Keyboard page (from launcher.cpp, CTR-NitroFueled-PC)

// Keyboard tab: bbport.ini key.<input>=<SDL key names> (src/runtime_pad.c reads them at start;
// a missing line keeps the runtime's default). Keys are captured by their scan code (the key's
// place on the keyboard, as SDL's scancodes), so the binding holds whatever the layout.

struct KeyInput {
    const char* id;      // runtime_pad.c input name
    const char* es;
    const char* en;
    const char* fallback;  // runtime_pad.c bind_defaults()
};
constexpr KeyInput kKeyInputs[] = {
    {"move_up", "Stick izquierdo arriba", "Left stick up", "W"},
    {"move_down", "Stick izquierdo abajo", "Left stick down", "S"},
    {"move_left", "Stick izquierdo izquierda (girar)", "Left stick left", "A"},
    {"move_right", "Stick izquierdo derecha (girar)", "Left stick right", "D"},
    {"cross", "X (acelerar)", "Cross", "Space"},
    {"square", "Cuadrado (frenar)", "Square", "E"},
    {"circle", "Círculo", "Circle", "Left Shift"},
    {"triangle", "Triángulo", "Triangle", "Q"},
    {"r1", "R1", "R1", "3"},
    {"l1", "L1", "L1", "1"},
    {"r2", "R2", "R2", "F"},
    {"l2", "L2", "L2", "R"},
    {"l3", "L3", "L3", "Z"},
    {"r3", "R3", "R3", "C"},
    {"options", "Options (pausa)", "Options", "Return"},
    {"touchpad", "Panel táctil", "Touchpad", "Tab"},
    {"touchpad_right", "Panel táctil (derecha)", "Touchpad (right)", "Backspace"},
    {"up", "Cruceta arriba", "D-pad up", "I"},
    {"down", "Cruceta abajo", "D-pad down", "K"},
    {"left", "Cruceta izquierda", "D-pad left", "J"},
    {"right", "Cruceta derecha", "D-pad right", "L"},
    {"look_up", "Stick derecho arriba", "Right stick up", "Up"},
    {"look_down", "Stick derecho abajo", "Right stick down", "Down"},
    {"look_left", "Stick derecho izquierda", "Right stick left", "Left"},
    {"look_right", "Stick derecho derecha", "Right stick right", "Right"},
};

// Scan code set 1 (WM_KEYDOWN lParam bits 16-23, + 0x100 for E0-extended) -> SDL scancode name.
const char* SdlKeyName(unsigned code) {
    static const std::map<unsigned, const char*> names = {
        {0x02, "1"}, {0x03, "2"}, {0x04, "3"}, {0x05, "4"}, {0x06, "5"}, {0x07, "6"}, {0x08, "7"},
        {0x09, "8"}, {0x0A, "9"}, {0x0B, "0"}, {0x0C, "-"}, {0x0D, "="}, {0x0E, "Backspace"},
        {0x0F, "Tab"}, {0x10, "Q"}, {0x11, "W"}, {0x12, "E"}, {0x13, "R"}, {0x14, "T"}, {0x15, "Y"},
        {0x16, "U"}, {0x17, "I"}, {0x18, "O"}, {0x19, "P"}, {0x1A, "["}, {0x1B, "]"},
        {0x1C, "Return"}, {0x1D, "Left Ctrl"}, {0x1E, "A"}, {0x1F, "S"}, {0x20, "D"}, {0x21, "F"},
        {0x22, "G"}, {0x23, "H"}, {0x24, "J"}, {0x25, "K"}, {0x26, "L"}, {0x27, ";"}, {0x28, "'"},
        {0x29, "`"}, {0x2A, "Left Shift"}, {0x2B, "\\"}, {0x2C, "Z"}, {0x2D, "X"}, {0x2E, "C"},
        {0x2F, "V"}, {0x30, "B"}, {0x31, "N"}, {0x32, "M"}, {0x33, ","}, {0x34, "."}, {0x35, "/"},
        {0x36, "Right Shift"}, {0x37, "Keypad *"}, {0x38, "Left Alt"}, {0x39, "Space"},
        {0x3A, "CapsLock"}, {0x3B, "F1"}, {0x3C, "F2"}, {0x3D, "F3"}, {0x3E, "F4"}, {0x3F, "F5"},
        {0x40, "F6"}, {0x41, "F7"}, {0x42, "F8"}, {0x43, "F9"}, {0x44, "F10"}, {0x57, "F11"},
        {0x58, "F12"}, {0x47, "Keypad 7"}, {0x48, "Keypad 8"}, {0x49, "Keypad 9"},
        {0x4A, "Keypad -"}, {0x4B, "Keypad 4"}, {0x4C, "Keypad 5"}, {0x4D, "Keypad 6"},
        {0x4E, "Keypad +"}, {0x4F, "Keypad 1"}, {0x50, "Keypad 2"}, {0x51, "Keypad 3"},
        {0x52, "Keypad 0"}, {0x53, "Keypad ."}, {0x56, "NonUSBackslash"},
        {0x11C, "Keypad Enter"}, {0x11D, "Right Ctrl"}, {0x135, "Keypad /"}, {0x138, "Right Alt"},
        {0x147, "Home"}, {0x148, "Up"}, {0x149, "PageUp"}, {0x14B, "Left"}, {0x14D, "Right"},
        {0x14F, "End"}, {0x150, "Down"}, {0x151, "PageDown"}, {0x153, "Delete"},
    };
    const auto it = names.find(code);
    return it == names.end() ? nullptr : it->second;
}

// How a key name reads in the launcher.
std::string KeyLabel(const std::string& name) {
    static const std::map<std::string, std::pair<const char*, const char*>> labels = {
        {"Space", {"Espacio", "Space"}}, {"Return", {"Enter", "Enter"}},
        {"Backspace", {"Retroceso", "Backspace"}}, {"Left Shift", {"Mayús izq.", "Left Shift"}},
        {"Right Shift", {"Mayús der.", "Right Shift"}}, {"Left Ctrl", {"Ctrl izq.", "Left Ctrl"}},
        {"Right Ctrl", {"Ctrl der.", "Right Ctrl"}}, {"Left Alt", {"Alt", "Left Alt"}},
        {"Right Alt", {"Alt Gr", "Right Alt"}}, {"Up", {"Flecha arriba", "Up arrow"}},
        {"Down", {"Flecha abajo", "Down arrow"}}, {"Left", {"Flecha izquierda", "Left arrow"}},
        {"Right", {"Flecha derecha", "Right arrow"}}, {"CapsLock", {"Bloq Mayús", "Caps Lock"}},
        {"Delete", {"Supr", "Delete"}}, {"Home", {"Inicio", "Home"}}, {"End", {"Fin", "End"}},
        {"PageUp", {"Re Pág", "Page Up"}}, {"PageDown", {"Av Pág", "Page Down"}},
        {"`", {"Tecla a la izquierda del 1", "` key"}},
        {"NonUSBackslash", {"Tecla < >", "< > key"}}, {"Keypad Enter", {"Enter (teclado num.)", "Keypad Enter"}},
    };
    const auto it = labels.find(name);
    if (it != labels.end()) return T(it->second.first, it->second.second);
    if (name.rfind("Keypad ", 0) == 0) return std::string(T("Num ", "Num ")) + name.substr(7);
    return name;
}

std::atomic<int> g_key_capture{-1};      // the input waiting for a key, -1 none
std::atomic<unsigned> g_key_captured{0};  // scan code + 1 (0: nothing yet)
bool g_key_replace = false;                 // the captured key replaces the default binding

std::vector<std::string> SplitKeys(const std::string& value) {
    std::vector<std::string> keys;
    std::stringstream ss(value);
    std::string key;
    while (std::getline(ss, key, ',')) {
        key = Trim(key);
        if (!key.empty()) keys.push_back(key);
    }
    return keys;
}

void KeyboardTab() {
    App& a = g_app;
    ImGui::TextWrapped("%s", T("Teclas del jugador 1. El teclado funciona a la vez que el mando. Pulsa \"Cambiar\" y luego la "
                               "tecla; cada acción admite hasta 4 teclas. Se guarda en bbport.ini y vale desde el próximo arranque.",
                               "Player 1's keys. The keyboard works together with the controller. Press \"Change\", then the key; "
                               "each action takes up to 4 keys. Saved in bbport.ini, used from the next start."));
    ImGui::TextColored(kGrey, "%s", T("Insert abre el menú del juego y Esc cancela, por eso no se pueden asignar.",
                                      "Insert opens the in-game menu and Esc cancels, so they cannot be bound."));
    const int capturing = g_key_capture;
    if (capturing >= 0) {
        const unsigned got = g_key_captured.exchange(0);
        if (got) {
            const unsigned code = got - 1;
            if (code == 0x01) {
                g_key_capture = -1;  // Esc: cancel
            } else if (const char* name = SdlKeyName(code); name && code != 0x152) {
                const KeyInput& in = kKeyInputs[capturing];
                const std::string key = std::string("key.") + in.id;
                auto keys = g_key_replace ? std::vector<std::string>{} : SplitKeys(a.game.Get(key));
                if (std::find(keys.begin(), keys.end(), name) == keys.end()) {
                    if (keys.size() >= 4) keys.erase(keys.begin());
                    keys.push_back(name);
                }
                std::string value;
                for (const auto& k : keys) value += (value.empty() ? "" : ",") + k;
                a.game.Set(key, value);
                SaveGameIni();
                g_key_capture = -1;
            } else {
                a.notice = T("Esa tecla no se puede asignar.", "That key cannot be bound.");
                g_key_capture = -1;
            }
        }
    }

    Section(T("Controles", "Controls"));
    if (ImGui::BeginTable("keys", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(T("Acción", "Action"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn(T("Teclas", "Keys"), ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 14.0f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < int(std::size(kKeyInputs)); ++i) {
            const KeyInput& in = kKeyInputs[i];
            const std::string key = std::string("key.") + in.id;
            const bool custom = a.game.values.count(key) > 0;
            const auto keys = SplitKeys(custom ? a.game.Get(key) : in.fallback);
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(T(in.es, in.en));
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (capturing == i) {
                ImGui::TextColored(kYellow, "%s", T("Pulsa una tecla... (Esc cancela)", "Press a key... (Esc cancels)"));
            } else if (keys.empty()) {
                ImGui::TextColored(kGrey, "%s", T("(sin tecla)", "(none)"));
            } else {
                std::string text;
                for (const auto& k : keys) text += (text.empty() ? "" : "  /  ") + KeyLabel(k);
                if (custom) ImGui::TextUnformatted(text.c_str());
                else ImGui::TextColored(kGrey, "%s", text.c_str());
            }
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(capturing >= 0);
            if (ImGui::SmallButton(T("Cambiar", "Change"))) {
                // A key replaces the default binding; on a custom one it is added (up to 4).
                g_key_replace = !custom;
                g_key_captured = 0;
                g_key_capture = i;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(T("Quitar", "Clear"))) {
                a.game.Set(key, "");
                SaveGameIni();
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!custom);
            if (ImGui::SmallButton(T("Original", "Default"))) {
                a.game.Erase(key);
                SaveGameIni();
            }
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(capturing >= 0);
    if (ImGui::Button(T("Restaurar todas las teclas", "Restore all keys"))) {
        for (const auto& in : kKeyInputs) a.game.Erase(std::string("key.") + in.id);
        SaveGameIni();
    }
    ImGui::EndDisabled();
    ImGui::TextColored(kGrey, "%s", T("Gris: tecla original. Las teclas van por su posición en el teclado.",
                                      "Grey: default key. Keys go by their place on the keyboard."));
}

// ---------------------------------------------------------------------------------------------
// Controls tab (v0.4). The connected controllers through the game's own SDL3 (gamepads.cpp), so
// names, GUIDs and buttons are what src/runtime_pad.c sees: a live test, the players'
// controllers (bbport.ini port.1-4, as the in-game menu writes them; player 1 also launcher.ini
// gamepad = BB_GAMEPAD), the gamepad buttons (bbport.ini pad.<input>) and the sticks' dead zone
// (launcher.ini pad_deadzone*, passed as BB_PAD_DEADZONE*). The keyboard is its second page.

uint32_t g_pad_selected = 0;
int g_pad_capture = -1;  // kKeyInputs index waiting for a gamepad button, -1 none
bool g_pad_replace = false;
double g_pads_retry = 0.0;

// runtime_pad.c bind_defaults() per kKeyInputs id; nullptr: not a gamepad binding (the sticks).
const char* PadFallback(const std::string& id) {
    static const std::map<std::string, const char*> defaults = {
        {"cross", "a"},           {"circle", "b"},         {"square", "x"},           {"triangle", "y"},
        {"l1", "leftshoulder"},   {"r1", "rightshoulder"}, {"l2", "lefttrigger"},     {"r2", "righttrigger"},
        {"l3", "leftstick"},      {"r3", "rightstick"},    {"options", "start"},      {"touchpad", "back,touchpad"},
        {"touchpad_right", ""},   {"up", "dpup"},          {"down", "dpdown"},        {"left", "dpleft"},
        {"right", "dpright"},
    };
    const auto it = defaults.find(id);
    return it == defaults.end() ? nullptr : it->second;
}

void LeaveControls() {
    if (g_key_capture >= 0) g_key_capture = -1;
    if (g_pad_capture >= 0) {
        g_pad_capture = -1;
        Pads::CancelCapture();
    }
}

/// SDL's gamepads while the launcher shows them; never while the game runs (it has them).
bool EnsurePads() {
    App& a = g_app;
    if (a.game_running) {
        Pads::Stop();
        return false;
    }
    if (Pads::Running()) return true;
    const double now = ImGui::GetTime();
    if (now < g_pads_retry) return false;
    if (!Pads::Start((a.bin / L"SDL3.dll").wstring())) {
        g_pads_retry = now + 3.0;
        return false;
    }
    return true;
}

/// The controller of player p (1..4): a GUID, or "" for automatic (p = 1) / nobody.
std::string PlayerPad(int p) {
    std::string v = g_app.game.Get("port." + std::to_string(p));
    if (p == 1 && v.empty()) v = g_app.launcher.Get("gamepad");
    return v;
}

void SetPlayerPad(int p, const Pads::Info* pad) {
    App& a = g_app;
    a.game.Set("port." + std::to_string(p), pad ? pad->guid : "");
    SaveGameIni();
    if (p == 1) {
        a.launcher.Set("gamepad", pad ? pad->guid : "");
        a.launcher.Set("gamepad_name", pad ? pad->name : "");
    }
    a.launcher.Set("port_name." + std::to_string(p), pad ? pad->name : "");
    SaveLauncher();
}

ImU32 Col(const ImVec4& c, float alpha = 1.0f) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha)); }

/// A drawing of the controller with what is pressed now (the raw input, before any remapping).
void DrawPad(const Pads::Info& info, const Pads::State& st, float dz_inner, float dz_outer) {
    using namespace Pads;
    const bool ps = info.family == Family::PlayStation;
    const float W = std::min(ImGui::GetContentRegionAvail().x, 430.0f * g_ui_scale);
    const float H = W * 0.56f;
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto P = [&](float x, float y) { return ImVec2(o.x + x * W, o.y + y * W); };
    auto R = [&](float r) { return r * W; };
    const ImU32 body = Col(kFrame), edge = Col(kBorder), idle = Col(ImVec4(0.24f, 0.27f, 0.37f, 1.0f)),
                on = Col(kAccent), text = Col(kText), dim = Col(kGrey);
    auto centered = [&](ImVec2 c, const std::string& label, ImU32 color) {
        const ImVec2 t = ImGui::CalcTextSize(label.c_str());
        dl->AddText(ImVec2(c.x - t.x * 0.5f, c.y - t.y * 0.5f), color, label.c_str());
    };
    // Body and grips.
    dl->AddCircleFilled(P(0.235f, 0.405f), R(0.125f), body, 48);
    dl->AddCircleFilled(P(0.765f, 0.405f), R(0.125f), body, 48);
    dl->AddRectFilled(P(0.11f, 0.115f), P(0.89f, 0.43f), body, R(0.095f));
    // Triggers (analog fill) and shoulders.
    auto trigger = [&](float x0, float x1, int axis, int button) {
        const float v = std::clamp(st.axes[axis] / 32767.0f, 0.0f, 1.0f);
        dl->AddRectFilled(P(x0, 0.0f), P(x1, 0.04f), idle, R(0.012f));
        if (v > 0.01f) dl->AddRectFilled(P(x0, 0.0f), P(x0 + (x1 - x0) * v, 0.04f), on, R(0.012f));
        centered(P((x0 + x1) * 0.5f, 0.02f), Label(button, info.family, info.type, g_spanish), text);
    };
    trigger(0.14f, 0.34f, AxTriggerLeft, BtnLeftTrigger);
    trigger(0.66f, 0.86f, AxTriggerRight, BtnRightTrigger);
    auto shoulder = [&](float x0, float x1, int button) {
        dl->AddRectFilled(P(x0, 0.055f), P(x1, 0.095f), st.buttons[button] ? on : idle, R(0.012f));
        centered(P((x0 + x1) * 0.5f, 0.075f), Label(button, info.family, info.type, g_spanish), text);
    };
    shoulder(0.14f, 0.34f, BtnLeftShoulder);
    shoulder(0.66f, 0.86f, BtnRightShoulder);
    // D-pad and left stick: PlayStation pads have the d-pad on top, the others the stick.
    const ImVec2 dpad = ps ? ImVec2(0.25f, 0.25f) : ImVec2(0.37f, 0.37f);
    const ImVec2 lstick = ps ? ImVec2(0.37f, 0.37f) : ImVec2(0.25f, 0.25f);
    {
        const float a = 0.027f;
        const struct { float dx, dy; int b; } arms[] = {
            {0, -2 * a, BtnDpadUp}, {0, 2 * a, BtnDpadDown}, {-2 * a, 0, BtnDpadLeft}, {2 * a, 0, BtnDpadRight}};
        dl->AddRectFilled(P(dpad.x - a, dpad.y - a), P(dpad.x + a, dpad.y + a), idle);
        for (const auto& arm : arms)
            dl->AddRectFilled(P(dpad.x + arm.dx - a, dpad.y + arm.dy - a), P(dpad.x + arm.dx + a, dpad.y + arm.dy + a),
                              st.buttons[arm.b] ? on : idle, R(0.006f));
    }
    // Face buttons, with the shapes or letters of this controller.
    {
        const ImVec2 c(0.75f, 0.25f);
        const float d = 0.058f, r = 0.028f;
        const struct { float dx, dy; int b; } face[] = {
            {0, -d, BtnNorth}, {0, d, BtnSouth}, {-d, 0, BtnWest}, {d, 0, BtnEast}};
        for (const auto& f : face) {
            const ImVec2 at = P(c.x + f.dx, c.y + f.dy);
            const bool down = st.buttons[f.b];
            dl->AddCircleFilled(at, R(r), down ? on : idle, 32);
            const float k = R(r) * 0.48f;
            const ImU32 ink = down ? Col(kBg) : text;
            if (ps) {
                const float th = 1.8f * g_ui_scale;
                if (f.b == BtnNorth) {
                    dl->AddTriangle(ImVec2(at.x, at.y - k), ImVec2(at.x - k, at.y + k * 0.75f), ImVec2(at.x + k, at.y + k * 0.75f),
                                    down ? ink : Col(ImVec4(0.35f, 0.88f, 0.66f, 1.0f)), th);
                } else if (f.b == BtnEast) {
                    dl->AddCircle(at, k, down ? ink : Col(ImVec4(1.0f, 0.45f, 0.45f, 1.0f)), 24, th);
                } else if (f.b == BtnSouth) {
                    const ImU32 blue = down ? ink : Col(ImVec4(0.45f, 0.66f, 1.0f, 1.0f));
                    dl->AddLine(ImVec2(at.x - k, at.y - k), ImVec2(at.x + k, at.y + k), blue, th);
                    dl->AddLine(ImVec2(at.x - k, at.y + k), ImVec2(at.x + k, at.y - k), blue, th);
                } else {
                    dl->AddRect(ImVec2(at.x - k * 0.85f, at.y - k * 0.85f), ImVec2(at.x + k * 0.85f, at.y + k * 0.85f),
                                down ? ink : Col(ImVec4(0.96f, 0.60f, 0.88f, 1.0f)), 0.0f, 0, th);
                }
            } else {
                std::string l = Label(f.b, info.family, info.type, g_spanish);
                if (l.size() > 1) l = l.substr(0, 1);
                centered(at, l, ink);
            }
        }
    }
    // Centre buttons: the touchpad on PlayStation pads.
    if (ps) {
        dl->AddRectFilled(P(0.39f, 0.12f), P(0.61f, 0.27f), st.buttons[BtnTouchpad] ? on : idle, R(0.02f));
        dl->AddRectFilled(P(0.335f, 0.13f), P(0.365f, 0.17f), st.buttons[BtnBack] ? on : idle, R(0.01f));
        dl->AddRectFilled(P(0.635f, 0.13f), P(0.665f, 0.17f), st.buttons[BtnStart] ? on : idle, R(0.01f));
        dl->AddCircleFilled(P(0.5f, 0.32f), R(0.02f), st.buttons[BtnGuide] ? on : idle, 24);
    } else {
        dl->AddCircleFilled(P(0.44f, 0.25f), R(0.017f), st.buttons[BtnBack] ? on : idle, 24);
        dl->AddCircleFilled(P(0.56f, 0.25f), R(0.017f), st.buttons[BtnStart] ? on : idle, 24);
        dl->AddCircleFilled(P(0.5f, 0.16f), R(0.028f), st.buttons[BtnGuide] ? on : idle, 32);
    }
    // Sticks: the dead zone (inner ring) and the full-travel ring, the stick's position.
    auto stick = [&](ImVec2 c, int ax, int ay, int button) {
        const ImVec2 at = P(c.x, c.y);
        const float rad = R(0.068f);
        dl->AddCircleFilled(at, rad, Col(kBg), 40);
        dl->AddCircle(at, rad, edge, 40, 1.5f * g_ui_scale);
        if (dz_inner > 0.0f) dl->AddCircle(at, rad * dz_inner, dim, 32, 1.0f * g_ui_scale);
        if (dz_outer < 0.999f) dl->AddCircle(at, rad * dz_outer, Col(kAccent, 0.45f), 40, 1.0f * g_ui_scale);
        const float x = std::clamp(st.axes[ax] / 32767.0f, -1.0f, 1.0f), y = std::clamp(st.axes[ay] / 32767.0f, -1.0f, 1.0f);
        const ImVec2 dot(at.x + x * rad, at.y + y * rad);
        dl->AddLine(at, dot, Col(kAccent, 0.6f), 2.0f * g_ui_scale);
        dl->AddCircleFilled(dot, R(0.026f), st.buttons[button] ? on : text, 24);
    };
    stick(lstick, AxLeftX, AxLeftY, BtnLeftStick);
    stick(ImVec2(0.63f, 0.37f), AxRightX, AxRightY, BtnRightStick);
    ImGui::Dummy(ImVec2(W, H));
}

void ControllersPage() {
    App& a = g_app;
    const bool sdl = EnsurePads();
    if (sdl) Pads::Update();
    const auto& pads = Pads::List();
    // The same model twice: "#2" tells them apart.
    auto display = [&](const Pads::Info& p) {
        int same = 0, index = 0;
        for (const auto& q : pads) {
            if (q.name == p.name) ++same;
            if (q.id == p.id) index = same;
        }
        return same > 1 ? p.name + " #" + std::to_string(index) : p.name;
    };
    auto players_of = [&](const Pads::Info& p) {
        std::string out;
        for (int n = 1; n <= 4; ++n)
            if (PlayerPad(n) == p.guid) out += (out.empty() ? "" : " ") + std::string(T("J", "P")) + std::to_string(n);
        return out;
    };

    Section(T("Mandos conectados", "Connected controllers"));
    if (a.game_running) {
        ImGui::TextColored(kYellow, "%s  %s", ICON_INFO,
                           T("El juego está abierto y usa los mandos. Ciérralo para probarlos aquí; dentro del juego, "
                             "Insert › Controllers asigna los puertos.",
                             "The game is running and has the controllers. Close it to test them here; in the game, "
                             "Insert › Controllers assigns the ports."));
    } else if (!sdl) {
        ImGui::TextColored(kRed, "%s  %s: %s", ICON_WARN, T("No se pueden leer los mandos", "Controllers cannot be read"),
                           Pads::Error().c_str());
    } else if (pads.empty()) {
        BeginCard("nopads");
        ImGui::TextColored(kGrey, "%s", ICON_GAMEPAD);
        ImGui::SameLine();
        ImGui::TextWrapped("%s", T("No hay mandos conectados. Conecta uno por USB o Bluetooth: aparece aquí al momento. "
                                   "El teclado funciona siempre (página Teclado).",
                                   "No controllers connected. Plug one in over USB or pair it over Bluetooth: it shows "
                                   "up here at once. The keyboard always works (Keyboard page)."));
        EndCard();
    } else {
        bool found = false;
        for (const auto& p : pads) found |= p.id == g_pad_selected;
        if (!found) g_pad_selected = pads.front().id;
        if (ImGui::BeginTable("padlayout", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("list", ImGuiTableColumnFlags_WidthStretch, 0.9f);
            ImGui::TableSetupColumn("test", ImGuiTableColumnFlags_WidthStretch, 1.1f);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const float row = ImGui::GetTextLineHeightWithSpacing() * 2.4f;
            for (const auto& p : pads) {
                ImGui::PushID(int(p.id));
                const bool selected = p.id == g_pad_selected;
                const ImVec2 at = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##pad", selected, 0, ImVec2(0, row))) g_pad_selected = p.id;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const float pad_x = 10.0f * g_ui_scale, line = ImGui::GetTextLineHeight();
                dl->AddText(ImVec2(at.x + pad_x, at.y + row * 0.5f - line * 0.5f), Col(selected ? kAccent : kGrey),
                            ICON_GAMEPAD);
                const float tx = at.x + pad_x + ImGui::GetFontSize() * 2.0f;
                dl->AddText(g_font_heading ? g_font_heading : ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(tx, at.y + row * 0.12f), Col(kText),
                            display(p).c_str());
                std::string sub = Pads::TypeName(p.type, g_spanish);
                if (p.wireless) sub += std::string(" · ") + ICON_BLUETOOTH + T(" inalámbrico", " wireless");
                else if (p.wired) sub += std::string(" · ") + ICON_USB + " USB";
                if (p.battery >= 0) sub += " · " + std::to_string(p.battery) + "%" + (p.charging ? T(" cargando", " charging") : "");
                dl->AddText(ImVec2(tx, at.y + row * 0.12f + line * 1.15f), Col(kGrey), sub.c_str());
                const std::string who = players_of(p);
                if (!who.empty()) {
                    const ImVec2 t = ImGui::CalcTextSize(who.c_str());
                    const float right = at.x + ImGui::GetContentRegionAvail().x - pad_x;
                    dl->AddRectFilled(ImVec2(right - t.x - 12 * g_ui_scale, at.y + 6 * g_ui_scale),
                                      ImVec2(right, at.y + 6 * g_ui_scale + t.y + 4 * g_ui_scale), Col(kAccent, 0.25f), 99.0f);
                    dl->AddText(ImVec2(right - t.x - 6 * g_ui_scale, at.y + 8 * g_ui_scale), Col(kAccent), who.c_str());
                }
                ImGui::PopID();
            }
            ImGui::TableNextColumn();
            for (const auto& p : pads) {
                if (p.id != g_pad_selected) continue;
                Pads::State st;
                Pads::Read(p.id, st);
                const int inner = std::clamp(a.launcher.Int("pad_deadzone", 5), 0, 126);
                const int outer = std::clamp(a.launcher.Int("pad_deadzone_outer", 127), inner + 1, 127);
                DrawPad(p, st, inner / 127.0f, outer / 127.0f);
                ImGui::TextColored(kGrey, T("Stick izq. %+.2f %+.2f   Stick der. %+.2f %+.2f", "Left %+.2f %+.2f   Right %+.2f %+.2f"),
                                   st.axes[Pads::AxLeftX] / 32767.0f, st.axes[Pads::AxLeftY] / 32767.0f,
                                   st.axes[Pads::AxRightX] / 32767.0f, st.axes[Pads::AxRightY] / 32767.0f);
                if (ImGui::Button((std::string(ICON_VIBRATE "  ") + T("Vibrar", "Rumble")).c_str())) {
                    if (!Pads::Rumble(p.id, 450)) a.notice = T("Este mando no admite vibración.", "This controller has no rumble.");
                }
                ImGui::SameLine();
                ImGui::TextColored(kGrey, "%s", T("Pulsa botones y mueve los sticks para probarlo.",
                                                  "Press buttons and move the sticks to test it."));
            }
            ImGui::EndTable();
        }
    }

    Section(T("Jugadores", "Players"));
    for (int n = 1; n <= 4; ++n) {
        ImGui::PushID(n);
        const std::string current = PlayerPad(n);
        std::string preview = n == 1 ? T("Automático: el último mando que uses", "Automatic: the last controller used")
                                     : T("Nadie", "Nobody");
        if (!current.empty()) {
            preview = a.launcher.Get("port_name." + std::to_string(n));
            if (preview.empty() && n == 1) preview = a.launcher.Get("gamepad_name");
            if (preview.empty()) preview = current;
            bool connected = false;
            for (const auto& p : pads)
                if (p.guid == current && !connected) {
                    preview = display(p);
                    connected = true;
                }
            if (!connected && sdl) preview += T(" (no conectado)", " (not connected)");
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted((std::string(T("Jugador ", "Player ")) + std::to_string(n)).c_str());
        ImGui::SameLine(ImGui::GetFontSize() * 6.0f);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22.0f);
        if (ImGui::BeginCombo("##player", preview.c_str())) {
            if (ImGui::Selectable(n == 1 ? T("Automático: el último mando que uses", "Automatic: the last controller used")
                                         : T("Nadie", "Nobody"),
                                  current.empty()))
                SetPlayerPad(n, nullptr);
            for (const auto& p : pads) {
                ImGui::PushID(int(p.id));
                const std::string label = display(p) + "  (" + Pads::TypeName(p.type, g_spanish) + ")";
                if (ImGui::Selectable(label.c_str(), p.guid == current)) SetPlayerPad(n, &p);
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (n == 1)
            HelpMarker(T("El jugador 1 también usa el teclado. En automático juega el mando que toques: coge cualquiera y pulsa un botón.",
                         "Player 1 also uses the keyboard. On automatic, the controller you touch plays: pick any and press a button."));
        if (n == 2)
            HelpMarker(T("Jugadores 2 a 4: pantalla dividida. Entran al juego mientras su mando está conectado. También se cambia "
                         "dentro del juego (Insert › Controllers).",
                         "Players 2 to 4 (split screen, in games that have it). They join the game while their controller is connected. Also set in "
                         "the game (Insert › Controllers)."));
        ImGui::PopID();
    }

    // Gamepad buttons: pad.<input> in bbport.ini.
    Section(T("Botones del mando", "Controller buttons"));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(kGrey, "%s", T("Lo que hace cada botón en el juego, para todos los mandos. Pulsa \"Cambiar\" y luego el botón "
                                      "en el mando; cada acción admite hasta 4. Se guarda en bbport.ini y vale desde el próximo arranque.",
                                      "What each button does in the game, for every controller. Press \"Change\", then the button on "
                                      "the controller; each action takes up to 4. Saved in bbport.ini, used from the next start."));
    ImGui::PopTextWrapPos();
    const Pads::Info* shown = nullptr;
    for (const auto& p : pads)
        if (p.id == g_pad_selected) shown = &p;
    const Pads::Family family = shown ? shown->family : Pads::Family::Xbox;
    const int type = shown ? shown->type : 3;  // SDL_GAMEPAD_TYPE_XBOXONE
    if (g_pad_capture >= 0) {
        std::string name;
        uint32_t from = 0;
        if (Pads::TakeCapture(name, from) && !name.empty()) {
            const KeyInput& in = kKeyInputs[g_pad_capture];
            const std::string key = std::string("pad.") + in.id;
            auto names = g_pad_replace ? std::vector<std::string>{} : SplitKeys(a.game.Get(key));
            if (std::find(names.begin(), names.end(), name) == names.end()) {
                if (names.size() >= 4) names.erase(names.begin());
                names.push_back(name);
            }
            std::string value;
            for (const auto& n : names) value += (value.empty() ? "" : ",") + n;
            a.game.Set(key, value);
            SaveGameIni();
            g_pad_capture = -1;
        } else if (!Pads::Capturing()) {
            g_pad_capture = -1;  // the controllers went away
        }
    }
    if (ImGui::BeginTable("padbuttons", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(T("Acción", "Action"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn(T("Botones", "Buttons"), ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 14.0f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < int(std::size(kKeyInputs)); ++i) {
            const KeyInput& in = kKeyInputs[i];
            const char* fallback = PadFallback(in.id);
            if (!fallback) continue;
            const std::string key = std::string("pad.") + in.id;
            const bool custom = a.game.values.count(key) > 0;
            const auto names = SplitKeys(custom ? a.game.Get(key) : fallback);
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(T(in.es, in.en));
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (g_pad_capture == i) {
                ImGui::TextColored(kYellow, "%s", T("Pulsa un botón del mando...", "Press a controller button..."));
            } else if (names.empty()) {
                ImGui::TextColored(kGrey, "%s", T("(sin botón)", "(none)"));
            } else {
                std::string text;
                for (const auto& n : names) {
                    const int input = Pads::InputFromName(n);
                    text += (text.empty() ? "" : "  /  ") + (input >= 0 ? Pads::Label(input, family, type, g_spanish) : n);
                }
                ImGui::TextColored(custom ? kText : kGrey, "%s", text.c_str());
            }
            ImGui::TableNextColumn();
            if (g_pad_capture == i) {
                if (ImGui::SmallButton(T("Cancelar", "Cancel"))) {
                    g_pad_capture = -1;
                    Pads::CancelCapture();
                }
            } else {
                ImGui::BeginDisabled(g_pad_capture >= 0 || !sdl || pads.empty());
                if (ImGui::SmallButton(T("Cambiar", "Change"))) {
                    g_pad_replace = !custom;
                    g_pad_capture = i;
                    Pads::ArmCapture();
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(g_pad_capture >= 0);
                if (ImGui::SmallButton(T("Quitar", "Clear"))) {
                    a.game.Set(key, "");
                    SaveGameIni();
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(!custom);
                if (ImGui::SmallButton(T("Original", "Default"))) {
                    a.game.Erase(key);
                    SaveGameIni();
                }
                ImGui::EndDisabled();
                ImGui::EndDisabled();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(g_pad_capture >= 0);
    if (ImGui::Button((std::string(ICON_UNDO "  ") + T("Restaurar todos los botones", "Restore all buttons")).c_str())) {
        for (const auto& in : kKeyInputs)
            if (PadFallback(in.id)) a.game.Erase(std::string("pad.") + in.id);
        SaveGameIni();
    }
    ImGui::EndDisabled();
    ImGui::TextColored(kGrey, "%s", T("Gris: botón original. Los nombres siguen al mando elegido arriba.",
                                      "Grey: default button. Names follow the controller chosen above."));

    Section(T("Sticks", "Sticks"));
    {
        int inner = std::clamp(a.launcher.Int("pad_deadzone", 5), 0, 126);
        int outer = std::clamp(a.launcher.Int("pad_deadzone_outer", 127), inner + 1, 127);
        int inner_pct = int(std::lround(inner * 100.0 / 127.0)), outer_pct = int(std::lround(outer * 100.0 / 127.0));
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
        if (ImGui::SliderInt(T("Zona muerta", "Dead zone"), &inner_pct, 0, 40, "%d %%")) {
            a.launcher.Set("pad_deadzone", std::to_string(int(std::lround(inner_pct * 127.0 / 100.0))));
            SaveLauncher();
        }
        HelpMarker(T("Cuánto hay que mover el stick antes de que el juego lo note. Súbela si el kart gira solo (stick con deriva). "
                     "Por defecto 4 %. Se ve como el anillo gris en la prueba de arriba.",
                     "How far the stick moves before the game notices. Raise it if the view drifts by itself (stick drift). "
                     "Default 4 %. It is the grey ring in the test above."));
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
        if (ImGui::SliderInt(T("Giro completo al", "Full turn at"), &outer_pct, 60, 100, "%d %%")) {
            a.launcher.Set("pad_deadzone_outer", std::to_string(int(std::lround(outer_pct * 127.0 / 100.0))));
            SaveLauncher();
        }
        HelpMarker(T("Con menos del 100 % el giro llega al máximo antes del tope del stick: útil con sticks gastados o mandos "
                     "que no llegan al borde. Es el anillo naranja.",
                     "Below 100 % the turn reaches its maximum before the stick's edge: useful for worn sticks or "
                     "controllers that do not reach the edge. It is the orange ring."));
        LauncherFlag("pad_center_cal", T("Corregir el centro de mandos genéricos", "Correct the centre of generic controllers"), true,
                     T("Algunos mandos clónicos dicen estar inclinados con el stick suelto; el port mide su centro al conectarlos. "
                       "Desactívalo solo si un mando original se comporta raro.",
                       "Some clone controllers report a tilt with the stick at rest; the port measures their centre when they "
                       "connect. Turn it off only if a genuine controller behaves oddly."));
        if (ImGui::Button((std::string(ICON_UNDO "  ") + T("Valores por defecto", "Defaults")).c_str())) {
            a.launcher.Erase("pad_deadzone");
            a.launcher.Erase("pad_deadzone_outer");
            a.launcher.Erase("pad_center_cal");
            SaveLauncher();
        }
    }
}

void ControlsTab() {
    if (ImGui::BeginTabBar("controls")) {
        if (ImGui::BeginTabItem((std::string(ICON_GAMEPAD "  ") + T("Mandos", "Controllers") + "###pads").c_str())) {
            if (g_key_capture >= 0) g_key_capture = -1;
            ImGui::BeginChild("pads");
            ControllersPage();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem((std::string(ICON_KEYBOARD "  ") + T("Teclado", "Keyboard") + "###keys").c_str())) {
            if (g_pad_capture >= 0) {
                g_pad_capture = -1;
                Pads::CancelCapture();
            }
            ImGui::BeginChild("keyboard");
            KeyboardTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

// ---------------------------------------------------------------------------------------------
// Play, Settings and Tools tabs

void DrawHeader() {
    if (g_font_title) ImGui::PushFont(g_font_title, ImGui::GetStyle().FontSizeBase * 1.8f);
    ImGui::TextColored(kAccent, "SYSTEM SHOCK 2");
    if (g_font_title) ImGui::PopFont();
    ImGui::SameLine();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10.0f * g_ui_scale);
    ImGui::TextColored(kGrey, "25th Anniversary Remaster  -  native loader (sacredfire)");
    ImGui::Separator();
}

void DrawStatusBox() {
    App& a = g_app;
    std::string stop, api, status;
    {
        std::scoped_lock lock{a.mutex};
        stop = a.last_stop;
        api = a.last_api;
        status = a.status;
    }
    ImGui::TextColored(a.busy ? kYellow : kGrey, "%s", status.c_str());
    if (!stop.empty()) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.20f, 0.08f, 0.08f, 1.0f));
        const float h = ImGui::GetTextLineHeightWithSpacing() * (api.empty() ? 1.6f : 2.6f);
        ImGui::BeginChild("stopbox", ImVec2(0, h), ImGuiChildFlags_Borders);
        ImGui::TextColored(kRed, "%s", stop.c_str());
        if (!api.empty()) ImGui::TextColored(kYellow, "%s", api.c_str());
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
}

void DrawLog() {
    App& a = g_app;
    ImGui::Checkbox("Follow output", &a.auto_scroll);
    ImGui::SameLine();
    if (ImGui::Button("Copy log")) {
        std::string all;
        {
            std::scoped_lock lock{a.mutex};
            for (const auto& l : a.log) all += l.text + "\n";
        }
        ImGui::SetClipboardText(all.c_str());
        a.notice = "Log copied to the clipboard.";
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy last STOP")) {
        std::string text;
        {
            std::scoped_lock lock{a.mutex};
            text = a.last_stop + (a.last_api.empty() ? "" : "\n" + a.last_api);
        }
        ImGui::SetClipboardText(text.c_str());
        a.notice = "STOP lines copied to the clipboard.";
    }
    ImGui::SameLine();
    if (ImGui::Button("Open run.txt")) OpenInShell(a.out / L"run.txt");

    ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
    ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::scoped_lock lock{a.mutex};
        ImGuiListClipper clipper;
        clipper.Begin(int(a.log.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const LogLine& l = a.log[size_t(i)];
                ImVec4 color = kText;
                switch (l.kind) {
                case kProblem: color = kRed; break;
                case kGameLog: color = kAccent; break;
                case kRuntime: color = kGrey; break;
                case kCommand: color = kYellow; break;
                default: break;
                }
                ImGui::TextColored(color, "%s", l.text.c_str());
            }
        }
    }
    if ((a.auto_scroll && a.busy) || a.scroll_to_end) {
        ImGui::SetScrollHereY(1.0f);
        a.scroll_to_end = false;
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void MergeSection();  // below, with the Saves helpers it uses
void ToolsExtras();

void PlayTab() {
    App& a = g_app;
    const GameCheck game = CheckGame(a.game_dir);
    const PrepCheck prep = CheckPrepared();
    const bool busy = a.busy;

    BeginCard("setup");
    Section("Game folder");
    ImGui::TextColored(kGrey, "The merged game: base game with update 1.04 copied over it (eboot.bin, base.kpf, sce_module, prx...).");
    ImGui::TextWrapped("%s", a.game_dir.empty() ? "(not chosen)" : PathText(a.game_dir).c_str());
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Choose...", ImVec2(110.0f * g_ui_scale, 0))) {
        if (auto picked = PickFolder(a.hwnd, L"Choose the System Shock 2 game folder", a.game_dir)) {
            a.game_dir = *picked;
            SaveLauncher();
        }
    }
    ImGui::EndDisabled();
    if (!a.game_dir.empty()) {
        CheckLine(game.eboot, "eboot.bin", "");
        CheckLine(game.sys, "sce_sys\\param.sfo", "");
        CheckLine(game.libc, "sce_module\\libc.prx", "");
        CheckLine(game.data, "base.kpf (game data)", "copy the base game's files into this folder");
    }
    MergeSection();
    Section("Prepared files");
    CheckLine(prep.loader, "out\\SS2R.exe", "run build.sh");
    CheckLine(prep.linked, "out\\boot-linked.bin", "press Prepare");
    CheckLine(prep.names, "out\\import_names.txt", "press Prepare");
    CheckLine(prep.content, "out\\content.bin", "press Prepare");
    if (!a.python) ImGui::TextColored(kRed, "Python was not found: install it or MSYS2's python, then restart the launcher.");
    else ImGui::TextColored(kGrey, "Python: %s", a.python->description.c_str());
    EndCard();

    ImGui::Spacing();
    const float w = ImGui::GetContentRegionAvail().x;
    const float btn_h = 44.0f * g_ui_scale;
    ImGui::BeginDisabled(busy || !a.python || !game.ok());
    if (ImGui::Button("Prepare", ImVec2(w * 0.22f, btn_h))) StartTask("Prepare", PrepareSteps(), false);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Runs prepare.py, link_modules.py, gen_import_names.py and content_profile.py.\n"
                          "Needed once per game folder, and after updating the scripts or the module list.");
    ImGui::SameLine();
    if (busy) {
        if (ImGui::Button("STOP", ImVec2(-FLT_MIN, btn_h))) StopTask();
    } else {
        ImGui::BeginDisabled(!game.ok() || !prep.ok());
        if (g_font_title) ImGui::PushFont(g_font_title, ImGui::GetStyle().FontSizeBase * 1.3f);
        if (AccentButton(ICON_PLAY "  PLAY", ImVec2(-FLT_MIN, btn_h))) StartGame();
        if (g_font_title) ImGui::PopFont();
        ImGui::EndDisabled();
    }
    ImGui::Spacing();
    DrawStatusBox();
    ImGui::Spacing();
    DrawLog();
}

void CoopSettings();  // below

void SettingsTab() {
    App& a = g_app;
    ImGui::TextColored(kGrey, "Applied when you press Play. The in-game menu (Insert or L3+R3) changes the same settings.");

    Section("Performance");
    {
        int limit = a.game.Int("fps_limit", 60);
        auto name = [](int v) -> std::string { return v == 0 ? std::string("No limit") : std::to_string(v) + " FPS"; };
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        if (ImGui::BeginCombo("FPS limit", name(limit).c_str())) {
            for (int v : kFpsLimits) {
                if (ImGui::Selectable(name(v).c_str(), v == limit)) {
                    a.game.Set("fps_limit", std::to_string(v));
                    SaveGameIni();
                }
            }
            ImGui::EndCombo();
        }
        HelpMarker("The loader's frame limiter. System Shock 2 Remastered runs at 60 on PS4; other values may "
                   "change the game's speed if it ties its timing to the frame rate.");
    }
    GameFlag("async_pipelines", "Compile shaders in the background (experimental)", false,
             "Fewer hitches when an effect is first seen; an effect may show up a frame late.");

    Section("Display");
    GameFlag("fullscreen", "Fullscreen", false);
    GameFlag("show_fps", "FPS counter in the corner", false);
    {
        std::scoped_lock lock{a.mutex};
        const std::string current = a.launcher.Get("display");
        std::string preview = "Primary";
        for (const auto& [value, label] : a.displays)
            if (value == current) preview = label;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22.0f);
        if (ImGui::BeginCombo("Monitor", preview.c_str())) {
            if (ImGui::Selectable("Primary", current.empty())) {
                a.launcher.Set("display", "");
                SaveLauncher();
            }
            for (const auto& [value, label] : a.displays) {
                if (ImGui::Selectable(label.c_str(), value == current)) {
                    a.launcher.Set("display", value);
                    SaveLauncher();
                }
            }
            ImGui::EndCombo();
        }
    }
    LauncherChoice("Render resolution", "render_resolution", kRenderResolutions, "1080", 14.0f);
    HelpMarker("The resolution the game draws at. 4K uses the engine's own PS4 Pro 4K mode: much sharper, "
               "and still worth it on a 1080p or 1440p monitor (it is scaled down, like supersampling). "
               "Needs a stronger GPU.");
    LauncherChoice("Presentation", "present_mode", kPresentModes, "Mailbox", 14.0f);
    HelpMarker("Mailbox: smooth without tearing. FIFO: classic VSync. Immediate: lowest latency, may tear.");
    {
        static constexpr Choice kFramegen[] = {{"", "Off", "off"}, {"", "AMD FSR 3 (experimental)", "fsr3"}};
        GameChoice("Frame generation", "framegen", kFramegen, "off", 14.0f);
        HelpMarker("Shows an interpolated frame between each pair of game frames. Experimental, and untested "
                   "with System Shock 2: turn it off if you see artefacts.");
    }
    ImGui::TextColored(kGrey, "Upscaling (FSR/DLSS) is not offered yet: for the sharpest picture, use "
                              "Render resolution 4K.");

    Section("Controller");
    ImGui::TextColored(kGrey, "%s  Controllers, players and buttons are in the Controls tab.", ICON_GAMEPAD);

    CoopSettings();

    Section("Console");
    LauncherChoice("Game language", "language", kGameLanguages, "1", 14.0f);
    HelpMarker("The PS4 system language the game sees. A language the game's files do not have shows English.");
}

void ToolsTab() {
    App& a = g_app;
    const bool busy = a.busy;
    Section("Launch options");
    ImGui::SetNextItemWidth(420.0f * g_ui_scale);
    if (ImGui::InputText("Extra loader arguments", a.extra_args, sizeof a.extra_args)) SaveLauncher();
    ImGui::TextColored(kGrey, "Added to SS2R.exe's command line, e.g. --timeout 120");

    Section("Individual steps");
    ImGui::BeginDisabled(busy || !a.python);
    if (ImGui::Button("Regenerate import names")) {
        StartTask("Import names",
                  {PyStep("Naming the remaining imports (gen_import_names.py)", a.root / L"tools" / L"gen_import_names.py", NamesArgs())},
                  false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Rewrite content profile") && !a.game_dir.empty()) {
        StartTask("Content profile",
                  {PyStep("Writing the content profile (content_profile.py)", a.root / L"scripts" / L"content_profile.py",
                          Quote(a.game_dir.wstring()) + OutArg())},
                  false);
    }
    ImGui::EndDisabled();
    ImGui::BeginDisabled(busy || !CheckPrepared().ok());
    ImGui::SameLine();
    if (ImGui::Button("List imports")) StartImportSummary();
    ImGui::EndDisabled();

    Section("Folders");
    if (ImGui::Button("Open out folder")) OpenInShell(a.out);
    ImGui::SameLine();
    ImGui::BeginDisabled(a.game_dir.empty());
    if (ImGui::Button("Open game folder")) OpenInShell(a.game_dir);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Open project folder")) OpenInShell(a.root);
    ImGui::SameLine();
    if (ImGui::Button("Open bbport.ini")) OpenInShell(GameIniPath());

    ImGui::Spacing();
    ImGui::TextColored(kGrey, "Project: %s", PathText(a.root).c_str());
    ImGui::TextColored(kGrey, "Game settings: %s", PathText(GameIniPath()).c_str());
    ImGui::TextColored(kGrey, "Modules linked: %s", Narrow(kModules).c_str());
    ToolsExtras();
}

// ---------------------------------------------------------------------------------------------
// Saves: <game folder>\user\savedata\<user>\<title ID>\<dir> holds a save's files and <dir>.sce_sys its
// param.bin (title, subtitle, detail) and icon0.png, as src/runtime_savedata.c keeps them.
// The game folder is the one SS2R.exe runs in (g_app.out: out\ or a release's bin\).

struct SaveParam {  // OrbisSaveDataParam
    char title[128], subtitle[128], detail[1024];
    uint32_t user_param;
    int32_t pad;
    int64_t mtime;
    uint8_t reserved[32];
};
static_assert(sizeof(SaveParam) == 1328, "OrbisSaveDataParam layout");

struct SaveSlot {
    std::string dir, title, subtitle, detail, modified;
    std::uintmax_t bytes = 0;
};

struct Backup {
    fs::path path;
    std::string name, modified;
    std::uintmax_t bytes = 0;
};

struct SavesState {
    std::vector<SaveSlot> saves;
    std::vector<Backup> backups;
    bool dirty = true;
    fs::path pending_path;
    int pending_action = 0, confirm_action = 0;  // 1 restore, 2 delete a backup
};
SavesState g_saves;

std::map<std::string, std::string> ParseSfo(const std::string& data) {
    std::map<std::string, std::string> out;
    auto u32 = [&](size_t at) -> uint32_t {
        if (at + 4 > data.size()) return 0;
        return uint32_t(uint8_t(data[at])) | uint32_t(uint8_t(data[at + 1])) << 8 |
               uint32_t(uint8_t(data[at + 2])) << 16 | uint32_t(uint8_t(data[at + 3])) << 24;
    };
    auto u16 = [&](size_t at) -> uint32_t {
        if (at + 2 > data.size()) return 0;
        return uint32_t(uint8_t(data[at])) | uint32_t(uint8_t(data[at + 1])) << 8;
    };
    if (data.size() < 20 || data.compare(0, 4, std::string("\0PSF", 4)) != 0) return out;
    const uint32_t keys = u32(8), values = u32(12), count = u32(16);
    for (uint32_t i = 0; i < count && 20 + i * 16 + 16 <= data.size(); ++i) {
        const size_t entry = 20 + size_t(i) * 16;
        const uint32_t key_off = u16(entry), fmt = u16(entry + 2), len = u32(entry + 4), off = u32(entry + 12);
        const size_t key_at = size_t(keys) + key_off;
        if (key_at >= data.size()) continue;
        const size_t key_end = data.find('\0', key_at);
        if (key_end == std::string::npos) continue;
        const std::string key = data.substr(key_at, key_end - key_at);
        const size_t value_at = size_t(values) + off;
        if (value_at + len > data.size()) continue;
        if (fmt == 0x0404) {
            out[key] = std::to_string(u32(value_at));
        } else {
            std::string v = data.substr(value_at, len);
            while (!v.empty() && v.back() == '\0') v.pop_back();
            out[key] = v;
        }
    }
    return out;
}

std::map<std::string, std::string> ReadSfo(const fs::path& file) { return ParseSfo(ReadText(file)); }

std::uintmax_t FolderBytes(const fs::path& dir) {
    std::uintmax_t total = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2)) total += it->file_size(e2);
    }
    return total;
}

std::string FileTimeText(const fs::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &data)) return {};
    FILETIME local;
    SYSTEMTIME st;
    FileTimeToLocalFileTime(&data.ftLastWriteTime, &local);
    FileTimeToSystemTime(&local, &st);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

std::string Stamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::string FixedText(const char* s, size_t n) { return std::string(s, strnlen(s, n)); }

std::string FormatSize(std::uintmax_t bytes) {
    char buf[64];
    if (bytes >= (1ull << 20)) std::snprintf(buf, sizeof buf, "%.1f MB", double(bytes) / double(1ull << 20));
    else std::snprintf(buf, sizeof buf, "%.0f KB", double(bytes) / 1024.0);
    return buf;
}

/// The game's title ID, from the chosen game folder's sce_sys\param.sfo.
std::wstring SaveTitleId() {
    const auto sfo = ReadSfo(g_app.game_dir / L"sce_sys" / L"param.sfo");
    const auto it = sfo.find("TITLE_ID");
    return it == sfo.end() || it->second.empty() ? std::wstring() : Widen(it->second);
}

/// user\savedata\<user>\<title ID>: the game's title ID under any user, else the first title folder
/// there is (the game folder may not be chosen yet), else user 1 with the game's title ID.
fs::path SaveRoot() {
    const fs::path base = g_app.out / L"user" / L"savedata";
    const std::wstring title = SaveTitleId();
    std::error_code ec;
    fs::path first;
    for (fs::directory_iterator user(base, ec), end; !ec && user != end; user.increment(ec)) {
        std::error_code e2;
        if (!user->is_directory(e2)) continue;
        if (!title.empty() && fs::is_directory(user->path() / title, e2)) return user->path() / title;
        for (fs::directory_iterator t(user->path(), e2), tend; !e2 && t != tend; t.increment(e2)) {
            std::error_code e3;
            if (first.empty() && t->is_directory(e3)) first = t->path();
        }
    }
    if (!first.empty()) return first;
    return base / L"1" / (title.empty() ? std::wstring(L"title") : title);
}

fs::path BackupRoot() { return g_app.root / L"backups"; }

bool IsMetaName(const std::wstring& name) {
    auto ends = [&](const wchar_t* suffix) {
        const size_t n = wcslen(suffix);
        return name.size() > n && name.compare(name.size() - n, n, suffix) == 0;
    };
    return ends(L".sce_sys") || ends(L".memory");
}

std::vector<SaveSlot> ListSaves() {
    std::vector<SaveSlot> slots;
    const fs::path root = SaveRoot();
    std::error_code ec;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_directory(e2) || IsMetaName(it->path().filename().wstring())) continue;
        SaveSlot s;
        s.dir = PathText(it->path().filename());
        s.bytes = FolderBytes(it->path());
        fs::path meta = it->path();
        meta += L".sce_sys";
        const std::string raw = ReadText(meta / L"param.bin");
        if (raw.size() >= sizeof(SaveParam)) {
            SaveParam p;
            std::memcpy(&p, raw.data(), sizeof p);
            s.title = FixedText(p.title, sizeof p.title);
            s.subtitle = FixedText(p.subtitle, sizeof p.subtitle);
            s.detail = FixedText(p.detail, sizeof p.detail);
        }
        s.modified = FileTimeText(it->path());
        for (fs::recursive_directory_iterator f(it->path(), e2), fend; !e2 && f != fend; f.increment(e2)) {
            std::error_code e3;
            if (f->is_regular_file(e3)) {
                const std::string t = FileTimeText(f->path());
                if (t > s.modified) s.modified = t;
            }
        }
        slots.push_back(std::move(s));
    }
    std::sort(slots.begin(), slots.end(), [](const SaveSlot& a, const SaveSlot& b) { return a.dir < b.dir; });
    return slots;
}

std::vector<Backup> ListBackups() {
    std::vector<Backup> list;
    std::error_code ec;
    for (fs::directory_iterator it(BackupRoot(), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        const std::string name = PathText(it->path().filename());
        if (!it->is_directory(e2) || name.rfind("saves-", 0) != 0) continue;
        list.push_back({it->path(), name, FileTimeText(it->path()), FolderBytes(it->path())});
    }
    std::sort(list.begin(), list.end(), [](const Backup& a, const Backup& b) { return a.name > b.name; });
    return list;
}

bool HasSaves() {
    std::error_code ec;
    return fs::is_directory(SaveRoot(), ec) && !fs::is_empty(SaveRoot(), ec);
}

std::string CopyTree(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to, ec);
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    return ec ? PathText(from) + " -> " + PathText(to) + ": " + ec.message() : std::string();
}

/// A copy of all saves in backups\saves-<date>[-label]; empty error when there is nothing to copy.
std::string BackupSaves(const char* label, fs::path* made = nullptr) {
    if (!HasSaves()) return {};
    fs::path target = BackupRoot() / Widen("saves-" + Stamp() + (label ? std::string("-") + label : ""));
    const std::string error = CopyTree(SaveRoot(), target);
    if (error.empty() && made) *made = target;
    return error;
}

std::string RestoreBackup(const fs::path& backup) {
    const fs::path root = SaveRoot();
    std::string error = BackupSaves("auto");
    if (!error.empty()) return error;
    std::error_code ec;
    fs::remove_all(root, ec);
    if (ec) return PathText(root) + ": " + ec.message();
    return CopyTree(backup, root);
}

/// A save folder from elsewhere: this launcher's backup or another PC's saves folder (copied as it is),
/// or a decrypted PS4 save (Apollo Save Tool, Save Wizard: its files plus sce_sys\param.sfo), converted
/// to the runtime's layout.
std::string ImportSaves(const fs::path& from) {
    std::error_code ec;
    const fs::path root = SaveRoot();
    const fs::path sfo_path = from / L"sce_sys" / L"param.sfo";
    if (fs::is_regular_file(sfo_path, ec)) {
        const auto sfo = ReadSfo(sfo_path);
        auto get = [&](const char* k) {
            const auto it = sfo.find(k);
            return it == sfo.end() ? std::string() : it->second;
        };
        std::string dir = get("SAVEDATA_DIRECTORY");
        if (dir.empty() || dir.size() >= 32 || dir.find_first_of("\\/:") != std::string::npos)
            return T("param.sfo no dice en qué ranura va la partida (SAVEDATA_DIRECTORY).",
                     "param.sfo does not name the save slot (SAVEDATA_DIRECTORY).");
        std::string error = BackupSaves("before-import");
        if (!error.empty()) return error;
        const fs::path target = root / Widen(dir);
        fs::path meta = target;
        meta += L".sce_sys";
        fs::remove_all(target, ec);
        fs::create_directories(target, ec);
        fs::create_directories(meta, ec);
        for (fs::directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->path().filename() == L"sce_sys") continue;
            std::error_code e2;
            fs::copy(it->path(), target / it->path().filename(),
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing, e2);
            if (e2) return PathText(it->path()) + ": " + e2.message();
        }
        SaveParam p{};
        std::snprintf(p.title, sizeof p.title, "%s", get("MAINTITLE").c_str());
        std::snprintf(p.subtitle, sizeof p.subtitle, "%s", get("SUBTITLE").c_str());
        std::snprintf(p.detail, sizeof p.detail, "%s", get("DETAIL").c_str());
        p.mtime = int64_t(time(nullptr));
        if (!WriteTextAtomic(meta / L"param.bin", std::string(reinterpret_cast<const char*>(&p), sizeof p)))
            return T("No se pudo escribir param.bin.", "Could not write param.bin.");
        fs::copy_file(from / L"sce_sys" / L"icon0.png", meta / L"icon0.png", fs::copy_options::overwrite_existing, ec);
        return {};
    }
    // The runtime's layout: <dir> next to <dir>.sce_sys.
    bool ours = false;
    for (fs::directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        fs::path meta = it->path();
        meta += L".sce_sys";
        std::error_code e2;
        if (!IsMetaName(it->path().filename().wstring()) && fs::is_directory(meta, e2)) ours = true;
    }
    if (!ours)
        return T("No reconozco esa carpeta: elige una copia de seguridad, la carpeta de partidas de otro PC o una "
                 "partida de PS4 descifrada (con sce_sys\\param.sfo).",
                 "This folder is not recognized: choose a backup, the saves folder from another PC or a "
                 "decrypted PS4 save (with sce_sys\\param.sfo).");
    std::string error = BackupSaves("before-import");
    if (!error.empty()) return error;
    return CopyTree(from, root);
}

void SavesTab() {
    App& a = g_app;
    SavesState& sv = g_saves;
    if (sv.dirty) {
        sv.saves = ListSaves();
        sv.backups = ListBackups();
        sv.dirty = false;
    }
    const bool locked = a.game_running;
    static bool was_running = false;  // saves made in the game show up once it closes
    if (was_running && !locked) sv.dirty = true;
    was_running = locked;
    if (locked)
        ImGui::TextColored(kYellow, "%s", T("Cierra el juego para copiar, importar o restaurar partidas.",
                                            "Close the game to back up, import or restore saves."));
    Section(T("Partidas", "Saves"));
    ImGui::TextColored(kGrey, "%s", PathText(SaveRoot()).c_str());
    if (sv.saves.empty()) {
        ImGui::TextUnformatted(T("Todavía no hay partidas guardadas.", "No saves yet."));
    } else if (ImGui::BeginTable("saves", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(T("Ranura", "Slot"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn(T("Descripción", "Description"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(T("Tamaño", "Size"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn(T("Modificada", "Modified"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (const auto& s : sv.saves) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(s.dir.c_str());
            ImGui::TableNextColumn();
            std::string text = s.title;
            if (!s.subtitle.empty()) text += (text.empty() ? "" : " - ") + s.subtitle;
            if (!s.detail.empty()) text += (text.empty() ? "" : " - ") + s.detail;
            ImGui::TextUnformatted(text.empty() ? "-" : text.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(FormatSize(s.bytes).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(s.modified.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(locked);
    ImGui::BeginDisabled(sv.saves.empty());
    if (ImGui::Button(T("Hacer copia de seguridad", "Back up now"))) {
        fs::path made;
        const std::string error = BackupSaves(nullptr, &made);
        a.notice = error.empty() ? std::string(T("Copia creada: ", "Backup created: ")) + PathText(made.filename()) : error;
        sv.dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Exportar...", "Export..."))) {
        if (const auto dest = PickFolder(a.hwnd, L"Export saves", fs::path())) {
            const fs::path target = *dest / Widen("SS2R-saves-" + Stamp());
            const std::string error = CopyTree(SaveRoot(), target);
            a.notice = error.empty() ? std::string(T("Partidas exportadas a ", "Saves exported to ")) + PathText(target) : error;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(T("Importar...", "Import..."))) {
        if (const auto from = PickFolder(a.hwnd, L"Import saves", fs::path())) {
            const std::string error = ImportSaves(*from);
            a.notice = error.empty() ? std::string(T("Partida importada. La anterior quedó en una copia de seguridad.",
                                                     "Save imported. The previous saves were backed up."))
                                     : error;
            sv.dirty = true;
        }
    }
    HelpMarker(T("Acepta una copia de seguridad de este launcher, la carpeta de partidas de otro PC, o una partida de "
                 "PS4 descifrada (Apollo Save Tool, Save Wizard: una carpeta con sce_sys\\param.sfo). Antes de importar "
                 "se hace una copia de seguridad de tus partidas.",
                 "Takes a backup made by this launcher, the saves folder from another PC, or a decrypted PS4 save "
                 "(Apollo Save Tool, Save Wizard: a folder with sce_sys\\param.sfo). Your saves are backed up "
                 "before importing."));
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(T("Abrir carpeta", "Open folder"))) {
        std::error_code ec;
        fs::create_directories(SaveRoot(), ec);
        OpenInShell(SaveRoot());
    }

    Section(T("Copias de seguridad", "Backups"));
    if (sv.backups.empty()) {
        ImGui::TextColored(kGrey, "%s", T("Todavía no hay copias.", "No backups yet."));
    } else if (ImGui::BeginTable("backups", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(T("Copia", "Backup"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(T("Tamaño", "Size"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        for (const auto& b : sv.backups) {
            ImGui::PushID(b.name.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(b.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(FormatSize(b.bytes).c_str());
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(locked);
            if (ImGui::SmallButton(T("Restaurar", "Restore"))) {
                sv.pending_path = b.path;
                sv.pending_action = 1;
            }
            ImGui::EndDisabled();
            ImGui::TableNextColumn();
            if (ImGui::SmallButton(T("Borrar", "Delete"))) {
                sv.pending_path = b.path;
                sv.pending_action = 2;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button(T("Abrir carpeta de copias", "Open backups folder"))) {
        std::error_code ec;
        fs::create_directories(BackupRoot(), ec);
        OpenInShell(BackupRoot());
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Actualizar", "Refresh"))) sv.dirty = true;
}

/// The Restore / Delete confirmation (drawn every frame, outside the tab bar).
void SaveConfirmations() {
    App& a = g_app;
    SavesState& sv = g_saves;
    if (sv.pending_action) {
        ImGui::OpenPopup("save_confirm");
        sv.confirm_action = sv.pending_action;
        sv.pending_action = 0;
    }
    if (ImGui::BeginPopupModal("save_confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        const std::string name = PathText(sv.pending_path.filename());
        if (sv.confirm_action == 1) {
            ImGui::Text(T("¿Restaurar %s?", "Restore %s?"), name.c_str());
            ImGui::TextColored(kGrey, "%s", T("Tus partidas actuales se guardan antes en una copia automática.",
                                              "Your current saves are backed up automatically first."));
        } else {
            ImGui::Text(T("¿Borrar la copia %s? No se puede deshacer.", "Delete the backup %s? This cannot be undone."), name.c_str());
        }
        if (ImGui::Button(sv.confirm_action == 1 ? T("Restaurar", "Restore") : T("Borrar", "Delete"))) {
            std::string error;
            if (sv.confirm_action == 1) {
                error = RestoreBackup(sv.pending_path);
                if (error.empty()) a.notice = std::string(T("Restaurada: ", "Restored: ")) + name;
            } else {
                std::error_code ec;
                fs::remove_all(sv.pending_path, ec);
                if (ec) error = name + ": " + ec.message();
            }
            if (!error.empty()) a.notice = error;
            sv.dirty = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancelar", "Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------------------------
// Trophies tab (ported from the CTR launcher's Achievements tab): the game's trophies
// (sce_sys/trophy/trophy00.trp) with the ones unlocked while playing, which the runtime records in
// <game folder>\user\trophies\<TITLE_ID>\unlocked.txt (src/runtime_services.c). Icons are plain PNGs
// in the TRP. Names, descriptions and grades are in its ESFM files, AES-encrypted with the console's
// trophy key, which is not part of this port: with the player's own key (the one shadPS4 uses: entered
// here or imported from shadPS4's keys.json) they are decrypted as shadPS4 does and written to
// trophies.tsv, which the runtime reads for its in-game notice and to find the platinum.

ID3D11ShaderResourceView* MakeTexture(const uint32_t* bgra, int width, int height);  // after g_device

/// The game language (launcher setting, PS4 code): picks TROP_<nn>.ESFM.
std::string TrophyLanguage() { return g_app.launcher.Get("language", "1"); }
/// The title ID the runtime files trophies under (param.sfo TITLE_ID; "UNKNOWN", as the runtime, when missing).
std::string TrophyTitleId() {
    const std::string id = Narrow(SaveTitleId());
    return id.empty() ? std::string("UNKNOWN") : id;
}

std::string XmlUnescape(std::string s) {
    const std::pair<const char*, const char*> entities[] = {
        {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}};
    for (const auto& [from, to] : entities) {
        for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += std::strlen(to))
            s.replace(at, std::strlen(from), to);
    }
    return s;
}

std::map<std::string, std::string> XmlAttributes(const std::string& tag) {
    std::map<std::string, std::string> out;
    size_t i = 0;
    while (i < tag.size()) {
        const size_t eq = tag.find('=', i);
        if (eq == std::string::npos) break;
        size_t name_end = eq;
        while (name_end > i && (tag[name_end - 1] == ' ' || tag[name_end - 1] == '\n' || tag[name_end - 1] == '\r' || tag[name_end - 1] == '\t')) --name_end;
        size_t name_start = name_end;
        while (name_start > i && !std::isspace(static_cast<unsigned char>(tag[name_start - 1])) && tag[name_start - 1] != '<') --name_start;
        size_t q = eq + 1;
        while (q < tag.size() && std::isspace(static_cast<unsigned char>(tag[q]))) ++q;
        if (q >= tag.size() || (tag[q] != '"' && tag[q] != '\'')) break;
        const char quote = tag[q];
        const size_t close = tag.find(quote, q + 1);
        if (close == std::string::npos) break;
        out[tag.substr(name_start, name_end - name_start)] = XmlUnescape(tag.substr(q + 1, close - q - 1));
        i = close + 1;
    }
    return out;
}



struct Trophy {
    int id = 0;
    char grade = 0;  // 'P' 'G' 'S' 'B', 0 unknown
    bool hidden = false;
    std::string name, detail;
    ID3D11ShaderResourceView* icon = nullptr;
    uint64_t unlocked = 0;  // unix time, 0 locked
};

struct TrophySet {
    fs::path game;              // the game folder it was read from
    std::string language;       // the game language its names are in
    std::string title_id, np_comm_id, title_name;
    std::vector<Trophy> list;
    ID3D11ShaderResourceView* icon = nullptr;
    bool names = false;         // names and grades known
    bool builtin = false;       // a built-in list, not the game's own (encrypted) texts (none for SS2)
    bool spanish = false;       // the launcher language its built-in texts are in
    std::string error, note;
    fs::file_time_type unlocked_time{};
    double next_poll = 0.0;
};
TrophySet g_trophies;
bool g_trophies_reload = true;
int g_trophy_filter = 0;  // 0 all, 1 unlocked, 2 locked
bool g_trophy_reset_confirm = false;
char g_trophy_key_input[64] = "";

/// Where the runtime keeps them: <game folder>\user\trophies\<title ID> (src/runtime_services.c trophy_path).
fs::path TrophyDir(const std::string& title) { return g_app.out / L"user" / L"trophies" / Widen(title); }

void ReleaseTrophies(TrophySet& t) {
    for (auto& tr : t.list)
        if (tr.icon) tr.icon->Release();
    if (t.icon) t.icon->Release();
    t = TrophySet{};
}

/// A PNG to a texture, through WIC.
ID3D11ShaderResourceView* PngTexture(const std::string& png) {
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    UINT w = 0, h = 0;
    if (SUCCEEDED(wic->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(png.data())), DWORD(png.size()))) &&
        SUCCEEDED(wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                        WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(converter->GetSize(&w, &h)) && w && h && w <= 4096 && h <= 4096) {
        std::vector<uint32_t> pixels(size_t(w) * h);
        if (SUCCEEDED(converter->CopyPixels(nullptr, w * 4, UINT(pixels.size() * 4), reinterpret_cast<BYTE*>(pixels.data()))))
            view = MakeTexture(pixels.data(), int(w), int(h));
    }
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    wic->Release();
    return view;
}

/// The 16-byte trophy key from hex ("" when it is not 32 hex digits).
std::string TrophyKeyBytes(const std::string& hex) {
    std::string clean;
    for (char c : hex)
        if (std::isxdigit(static_cast<unsigned char>(c))) clean += c;
    if (clean.size() != 32) return {};
    std::string out;
    for (size_t i = 0; i < 32; i += 2) out += char(std::stoi(clean.substr(i, 2), nullptr, 16));
    return out;
}

/// AES-128-CBC without padding (Windows CNG). encrypt: true to encrypt.
bool AesCbc(const std::string& key, const unsigned char iv_in[16], const unsigned char* in, size_t size, unsigned char* out,
            bool encrypt) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE handle = nullptr;
    bool ok = false;
    unsigned char iv[16];
    std::memcpy(iv, iv_in, 16);
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC,
                                         sizeof(BCRYPT_CHAIN_MODE_CBC), 0)) &&
        BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(alg, &handle, nullptr, 0, (PUCHAR)key.data(), ULONG(key.size()), 0))) {
        ULONG done = 0;
        ok = BCRYPT_SUCCESS(encrypt ? BCryptEncrypt(handle, (PUCHAR)in, ULONG(size), nullptr, iv, 16, out, ULONG(size), &done, 0)
                                    : BCryptDecrypt(handle, (PUCHAR)in, ULONG(size), nullptr, iv, 16, out, ULONG(size), &done, 0)) &&
             done == size;
    }
    if (handle) BCryptDestroyKey(handle);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

/// One ESFM entry decrypted to its XML (shadPS4's DecryptEFSM): the key for the file is the
/// NP communication ID (16 bytes, zero-padded) encrypted with the trophy key (CBC, zero IV); the
/// entry is IV (16 bytes) + data, CBC-decrypted with that key and IV.
std::string DecryptEsfm(const std::string& trophy_key, const std::string& np_comm_id, const std::string& entry) {
    if (entry.size() <= 32 || (entry.size() - 16) % 16) return {};
    unsigned char comm[16]{}, zero[16]{}, file_key[16];
    std::memcpy(comm, np_comm_id.data(), std::min<size_t>(np_comm_id.size(), 16));
    if (!AesCbc(trophy_key, zero, comm, 16, file_key, true)) return {};
    std::string xml(entry.size() - 16, '\0');
    if (!AesCbc(std::string(reinterpret_cast<char*>(file_key), 16), reinterpret_cast<const unsigned char*>(entry.data()),
                reinterpret_cast<const unsigned char*>(entry.data()) + 16, entry.size() - 16,
                reinterpret_cast<unsigned char*>(xml.data()), false))
        return {};
    const size_t end = xml.rfind('>');
    if (end == std::string::npos) return {};
    xml.resize(end + 1);
    return xml.rfind("<?xml", 0) == 0 || xml.find("<trophyconf") != std::string::npos ? xml : std::string{};
}

std::string XmlTag(const std::string& xml, const std::string& tag, size_t from = 0, size_t until = std::string::npos) {
    const size_t a = xml.find("<" + tag + ">", from);
    if (a == std::string::npos || a >= until) return {};
    const size_t b = xml.find("</" + tag + ">", a);
    if (b == std::string::npos || b > until) return {};
    return XmlUnescape(xml.substr(a + tag.size() + 2, b - a - tag.size() - 2));
}

/// The trophies in a decrypted TROP*.XML: id -> (grade, hidden, name, detail).
void ParseTrophyXml(const std::string& xml, TrophySet& t) {
    t.title_name = XmlTag(xml, "title-name");
    for (size_t at = 0; (at = xml.find("<trophy ", at)) != std::string::npos;) {
        const size_t close = xml.find('>', at), end = xml.find("</trophy>", at);
        if (close == std::string::npos || end == std::string::npos) break;
        const auto attr = XmlAttributes(xml.substr(at + 8, close - at - 8));
        auto get = [&](const char* k) {
            const auto it = attr.find(k);
            return it == attr.end() ? std::string() : it->second;
        };
        const int id = std::atoi(get("id").c_str());
        for (auto& tr : t.list) {
            if (tr.id != id) continue;
            const std::string type = get("ttype");
            tr.grade = type.empty() ? 0 : type[0];
            tr.hidden = get("hidden") == "yes";
            tr.name = XmlTag(xml, "name", close, end);
            tr.detail = XmlTag(xml, "detail", close, end);
        }
        at = end;
    }
}

std::string Sanitize(std::string s) {
    for (char& c : s)
        if (c == '\t' || c == '\r' || c == '\n') c = ' ';
    return s;
}

/// trophies.tsv: "#source<TAB>key|builtin", "#lang<TAB>code", then id, grade, hidden, name,
/// detail per line. True for the game's own texts (decrypted with the key) in this language.
bool ReadTrophyTsv(const fs::path& file, const std::string& language, TrophySet& t, std::string* source = nullptr) {
    std::stringstream ss(ReadText(file));
    std::string line;
    bool right_language = false, any = false;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> f;
        std::stringstream ls(line);
        std::string field;
        while (std::getline(ls, field, '\t')) f.push_back(field);
        if (f.size() >= 2 && f[0] == "#lang") right_language = f[1] == language;
        if (f.size() >= 2 && f[0] == "#source" && source) *source = f[1];
        if (f.size() >= 2 && f[0] == "#title") t.title_name = f[1];
        if (f.size() < 5 || f[0].empty() || !std::isdigit(static_cast<unsigned char>(f[0][0]))) continue;
        for (auto& tr : t.list)
            if (tr.id == std::atoi(f[0].c_str())) {
                tr.grade = f[1].empty() ? 0 : f[1][0];
                tr.hidden = f[2] == "1";
                tr.name = f[3];
                tr.detail = f[4];
                any = true;
            }
    }
    return any && right_language && (!source || *source != "builtin");
}

void WriteTrophyTsv(const TrophySet& t) {
    std::string out = std::string("#source\t") + (t.builtin ? "builtin" : "key") + "\n#lang\t" + t.language + "\n#title\t" +
                      Sanitize(t.title_name) + "\n";
    for (const auto& tr : t.list)
        out += std::to_string(tr.id) + "\t" + std::string(1, tr.grade ? tr.grade : '?') + "\t" + (tr.hidden ? "1" : "0") + "\t" +
               Sanitize(tr.name) + "\t" + Sanitize(tr.detail) + "\n";
    std::error_code ec;
    fs::create_directories(TrophyDir(t.title_id), ec);
    WriteTextAtomic(TrophyDir(t.title_id) / L"trophies.tsv", out);
}

void ReadUnlocked(TrophySet& t) {
    for (auto& tr : t.list) tr.unlocked = 0;
    std::stringstream ss(ReadText(TrophyDir(t.title_id) / L"unlocked.txt"));
    std::string line;
    while (std::getline(ss, line)) {
        int id = -1;
        unsigned long long at = 0;
        if (std::sscanf(line.c_str(), "%d %llu", &id, &at) != 2) continue;
        for (auto& tr : t.list)
            if (tr.id == id && !tr.unlocked) tr.unlocked = at ? at : 1;
    }
    std::error_code ec;
    t.unlocked_time = fs::last_write_time(TrophyDir(t.title_id) / L"unlocked.txt", ec);
}

/// The trophy key the launcher keeps, else shadPS4's (keys.json "ReleaseTrophyKey").
std::string ShadPs4TrophyKey() {
    const wchar_t* appdata = _wgetenv(L"APPDATA");
    if (!appdata) return {};
    const std::string json = ReadText(fs::path(appdata) / L"shadPS4" / L"keys.json");
    const size_t at = json.find("\"ReleaseTrophyKey\"");
    if (at == std::string::npos) return {};
    const size_t colon = json.find(':', at);
    if (colon == std::string::npos) return {};
    size_t i = colon + 1;
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
    if (i < json.size() && json[i] == '"') {  // a hex string
        const size_t end = json.find('"', i + 1);
        return end == std::string::npos ? std::string() : json.substr(i + 1, end - i - 1);
    }
    if (i < json.size() && json[i] == '[') {  // a byte array
        const size_t end = json.find(']', i);
        std::stringstream ss(json.substr(i + 1, end == std::string::npos ? std::string::npos : end - i - 1));
        std::string hex, n;
        char buf[3];
        while (std::getline(ss, n, ',')) {
            n = Trim(n);
            if (n.empty()) continue;
            std::snprintf(buf, sizeof buf, "%02x", std::atoi(n.c_str()) & 0xff);
            hex += buf;
        }
        return hex;
    }
    return {};
}

void LoadTrophies() {
    App& a = g_app;
    ReleaseTrophies(g_trophies);
    TrophySet& t = g_trophies;
    t.game = a.game_dir;
    t.language = TrophyLanguage();
    t.spanish = g_spanish;
    t.title_id = TrophyTitleId();
    const fs::path trp = a.game_dir / L"sce_sys" / L"trophy" / L"trophy00.trp";
    const std::string data = ReadText(trp);
    auto be32 = [&](size_t o) {
        return uint32_t(uint8_t(data[o])) << 24 | uint32_t(uint8_t(data[o + 1])) << 16 | uint32_t(uint8_t(data[o + 2])) << 8 |
               uint32_t(uint8_t(data[o + 3]));
    };
    auto be64 = [&](size_t o) { return uint64_t(be32(o)) << 32 | be32(o + 4); };
    if (data.size() < 0x60 || be32(0) != 0xDCA24D00) {
        t.error = a.game_dir.empty() ? T("Elige primero el juego en la pestaña Jugar.", "Choose the game in the Play tab first.")
                                     : T("Este juego no trae el archivo de trofeos (sce_sys\\trophy\\trophy00.trp).",
                                         "This game has no trophy file (sce_sys\\trophy\\trophy00.trp).");
        return;
    }
    const uint32_t count = be32(0x10), entry_size = be32(0x14);
    std::map<std::string, std::string> entries;
    for (uint32_t i = 0; i < count && i < 1024; ++i) {
        const size_t e = 0x60 + size_t(i) * entry_size;
        if (e + 48 > data.size()) break;
        const std::string name(data.c_str() + e, strnlen(data.c_str() + e, 32));
        const uint64_t pos = be64(e + 32), size = be64(e + 40);
        if (pos + size <= data.size()) entries[name] = data.substr(size_t(pos), size_t(size));
    }
    for (const auto& [name, bytes] : entries) {
        int id = -1;
        if (name.size() == 11 && name.rfind("TROP", 0) == 0 && name.substr(7) == ".PNG" && std::sscanf(name.c_str() + 4, "%3d", &id) == 1) {
            Trophy tr;
            tr.id = id;
            tr.icon = PngTexture(bytes);
            t.list.push_back(std::move(tr));
        }
    }
    std::sort(t.list.begin(), t.list.end(), [](const Trophy& x, const Trophy& y) { return x.id < y.id; });
    if (entries.count("ICON0.PNG")) t.icon = PngTexture(entries["ICON0.PNG"]);
    // The NP communication ID (npbind.dat), needed to decrypt.
    const std::string npbind = ReadText(a.game_dir / L"sce_sys" / L"npbind.dat");
    for (size_t i = 0; i + 12 <= npbind.size(); ++i)
        if (npbind.compare(i, 4, "NPWR") == 0 && npbind[i + 9] == '_') {
            t.np_comm_id = npbind.substr(i, 12);
            break;
        }
    // Names: the game's own texts (trophies.tsv in this language, else decrypted with the key),
    // else the built-in list.
    std::string source;
    TrophySet probe;
    for (const auto& tr : t.list) probe.list.push_back(Trophy{tr.id});
    if (ReadTrophyTsv(TrophyDir(t.title_id) / L"trophies.tsv", t.language, probe, &source)) {
        ReadTrophyTsv(TrophyDir(t.title_id) / L"trophies.tsv", t.language, t);
        t.names = true;
    } else {
        const std::string key = TrophyKeyBytes(a.launcher.Get("trophy_key"));
        if (!key.empty() && !t.np_comm_id.empty()) {
            char localized[32];
            std::snprintf(localized, sizeof localized, "TROP_%02d.ESFM", std::atoi(t.language.c_str()));
            const std::string file = entries.count(localized) ? localized : "TROP.ESFM";
            const std::string xml = entries.count(file) ? DecryptEsfm(key, t.np_comm_id, entries[file]) : std::string{};
            if (!xml.empty()) {
                ParseTrophyXml(xml, t);
                t.names = true;
                WriteTrophyTsv(t);
            } else {
                t.note = T("La clave de trofeos no descifra este juego: revisa que sea la de tu PS4 (32 cifras hexadecimales).",
                           "The trophy key does not decrypt this game: check it is your PS4's (32 hex digits).");
            }
        }
    }
    ReadUnlocked(t);
}

void TrophiesTab() {
    App& a = g_app;
    const std::string language = TrophyLanguage();
    if (g_trophies_reload || g_trophies.game != a.game_dir || g_trophies.language != language ||
        (g_trophies.builtin && g_trophies.spanish != g_spanish)) {
        g_trophies_reload = false;
        LoadTrophies();
    }
    TrophySet& t = g_trophies;
    // The game writes unlocked.txt while it runs: re-read it when it changes.
    if (ImGui::GetTime() > t.next_poll && !t.title_id.empty()) {
        t.next_poll = ImGui::GetTime() + 1.0;
        std::error_code ec;
        const auto time = fs::last_write_time(TrophyDir(t.title_id) / L"unlocked.txt", ec);
        if (!ec && time != t.unlocked_time) ReadUnlocked(t);
    }
    if (!t.error.empty()) {
        BeginCard("trophy_error");
        ImGui::TextColored(kYellow, "%s  %s", ICON_INFO, t.error.c_str());
        EndCard();
        return;
    }
    auto points = [](char g) { return g == 'P' ? 180 : g == 'G' ? 90 : g == 'S' ? 30 : g == 'B' ? 15 : 0; };
    int unlocked = 0, earned = 0, total_points = 0;
    int by_grade[4][2]{};  // B S G P: unlocked, total
    for (const auto& tr : t.list) {
        unlocked += tr.unlocked != 0;
        total_points += points(tr.grade);
        if (tr.unlocked) earned += points(tr.grade);
        const int g = tr.grade == 'B' ? 0 : tr.grade == 'S' ? 1 : tr.grade == 'G' ? 2 : tr.grade == 'P' ? 3 : -1;
        if (g >= 0) {
            by_grade[g][1]++;
            if (tr.unlocked) by_grade[g][0]++;
        }
    }
    const int total = int(t.list.size());
    const float progress = total_points ? float(earned) / float(total_points) : total ? float(unlocked) / float(total) : 0.0f;
    static const ImVec4 grade_colors[] = {ImVec4(0.82f, 0.54f, 0.32f, 1.0f), ImVec4(0.80f, 0.83f, 0.88f, 1.0f),
                                          ImVec4(1.0f, 0.80f, 0.29f, 1.0f), ImVec4(0.55f, 0.83f, 1.0f, 1.0f)};
    auto grade_index = [](char g) { return g == 'B' ? 0 : g == 'S' ? 1 : g == 'G' ? 2 : g == 'P' ? 3 : -1; };
    auto grade_name = [](int g) {
        static const char* es[] = {"Bronce", "Plata", "Oro", "Platino"};
        static const char* en[] = {"Bronze", "Silver", "Gold", "Platinum"};
        return g_spanish ? es[g] : en[g];
    };

    // Summary.
    BeginCard("trophy_summary");
    const float big = 72.0f * g_ui_scale;
    if (t.icon) {
        ImGui::Image(ImTextureRef(ImTextureID(reinterpret_cast<intptr_t>(t.icon))), ImVec2(big * 1.78f, big));
        ImGui::SameLine(0.0f, 16.0f * g_ui_scale);
    }
    ImGui::BeginGroup();
    if (g_font_heading) ImGui::PushFont(g_font_heading, ImGui::GetStyle().FontSizeBase * 1.25f);
    ImGui::TextUnformatted(t.title_name.empty() ? "System Shock 2: 25th Anniversary Remaster" : t.title_name.c_str());
    if (g_font_heading) ImGui::PopFont();
    ImGui::Text(T("%d de %d logros  ·  %d %%", "%d of %d trophies  ·  %d %%"), unlocked, total, int(progress * 100.0f + 0.5f));
    ImGui::ProgressBar(progress, ImVec2(-FLT_MIN, 8.0f * g_ui_scale), "");
    if (t.names) {
        for (int g = 3; g >= 0; --g) {
            if (!by_grade[g][1]) continue;
            ImGui::TextColored(grade_colors[g], "\xE2\x97\x8F");
            ImGui::SameLine(0.0f, 4.0f * g_ui_scale);
            ImGui::Text("%s %d/%d", grade_name(g), by_grade[g][0], by_grade[g][1]);
            ImGui::SameLine(0.0f, 16.0f * g_ui_scale);
        }
        ImGui::NewLine();
    }
    ImGui::EndGroup();
    EndCard();

    // Names: the player's trophy key (the game's own texts, in its language).
    bool show_key = !t.names;
    if (t.builtin) {
        ImGui::Spacing();
        show_key = ImGui::CollapsingHeader(T("Usar los textos del propio juego (clave de trofeos)",
                                             "Use the game's own texts (trophy key)"));
    }
    if (show_key) {
        ImGui::Spacing();
        BeginCard("trophy_key");
        ImGui::TextColored(kAccent, "%s", ICON_INFO);
        ImGui::SameLine();
        ImGui::PushTextWrapPos(0.0f);
        if (t.builtin)
            ImGui::TextUnformatted(T("Los nombres son los oficiales y las descripciones, un resumen de qué hay que hacer. Los textos "
                                     "del propio juego (en su idioma) vienen cifrados con la clave de trofeos de la PS4, que no se "
                                     "incluye: si la tienes (la misma que usa shadPS4), pégala o impórtala para usarlos.",
                                     "Names are the official ones and descriptions a summary of what to do. The game's own texts "
                                     "(in its language) are encrypted with the PS4's trophy key, which is not included: if you "
                                     "have it (the same shadPS4 uses), paste or import it to use them."));
        else
            ImGui::TextUnformatted(T("Los logros se guardan igual al jugar y aquí se ven con su imagen. Sus nombres y descripciones "
                                     "vienen cifrados en el juego con la clave de trofeos de la PS4, que no se incluye: si la tienes "
                                     "(es la misma que usa shadPS4), pégala o impórtala para verlos.",
                                     "Trophies are recorded while you play and shown here with their pictures. Their names and "
                                     "descriptions are encrypted in the game with the PS4's trophy key, which is not included: if "
                                     "you have it (the same shadPS4 uses), paste or import it to see them."));
        ImGui::PopTextWrapPos();
        if (!t.note.empty()) ImGui::TextColored(kRed, "%s", t.note.c_str());
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
        ImGui::InputTextWithHint("##trophykey", T("Clave de trofeos (32 cifras hex)", "Trophy key (32 hex digits)"), g_trophy_key_input,
                                 sizeof g_trophy_key_input, ImGuiInputTextFlags_Password | ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::SameLine();
        if (ImGui::Button(T("Guardar", "Save"))) {
            if (TrophyKeyBytes(g_trophy_key_input).empty()) {
                a.notice = T("La clave tiene que tener 32 cifras hexadecimales.", "The key must be 32 hex digits.");
            } else {
                a.launcher.Set("trophy_key", g_trophy_key_input);
                SaveLauncher();
                g_trophies_reload = true;
            }
            std::memset(g_trophy_key_input, 0, sizeof g_trophy_key_input);
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Importar de shadPS4", "Import from shadPS4"))) {
            const std::string key = ShadPs4TrophyKey();
            if (TrophyKeyBytes(key).empty()) {
                a.notice = T("shadPS4 no tiene una clave de trofeos guardada (keys.json).",
                             "shadPS4 has no trophy key saved (keys.json).");
            } else {
                a.launcher.Set("trophy_key", key);
                SaveLauncher();
                g_trophies_reload = true;
            }
        }
        EndCard();
    }

    // Filter and actions.
    ImGui::Spacing();
    ImGui::RadioButton(T("Todos", "All"), &g_trophy_filter, 0);
    ImGui::SameLine();
    ImGui::RadioButton(T("Desbloqueados", "Unlocked"), &g_trophy_filter, 1);
    ImGui::SameLine();
    ImGui::RadioButton(T("Por conseguir", "Locked"), &g_trophy_filter, 2);
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 13.0f);
    if (ImGui::Button((std::string(ICON_FOLDER "  ") + T("Carpeta", "Folder")).c_str())) {
        std::error_code ec;
        fs::create_directories(TrophyDir(t.title_id), ec);
        OpenInShell(TrophyDir(t.title_id));
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!unlocked || a.game_running);
    if (ImGui::Button((std::string(ICON_UNDO "  ") + T("Reiniciar", "Reset")).c_str())) g_trophy_reset_confirm = true;
    ImGui::EndDisabled();

    // The list.
    ImGui::Spacing();
    const float icon = 60.0f * g_ui_scale;
    if (ImGui::BeginTable("trophies", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("icon", ImGuiTableColumnFlags_WidthFixed, icon);
        ImGui::TableSetupColumn("text", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 9.0f);
        // The latest unlocked first, then the rest in the game's order.
        std::vector<const Trophy*> order;
        for (const auto& tr : t.list) order.push_back(&tr);
        std::stable_sort(order.begin(), order.end(), [](const Trophy* x, const Trophy* y) {
            if (bool(x->unlocked) != bool(y->unlocked)) return bool(x->unlocked);
            return x->unlocked > y->unlocked;
        });
        for (const Trophy* item : order) {
            const Trophy& tr = *item;
            if ((g_trophy_filter == 1 && !tr.unlocked) || (g_trophy_filter == 2 && tr.unlocked)) continue;
            const bool secret = tr.hidden && !tr.unlocked;
            ImGui::TableNextRow(0, icon + 8.0f * g_ui_scale);
            ImGui::TableNextColumn();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (tr.icon && !secret) {
                dl->AddImageRounded(ImTextureRef(ImTextureID(reinterpret_cast<intptr_t>(tr.icon))), p, ImVec2(p.x + icon, p.y + icon),
                                    ImVec2(0, 0), ImVec2(1, 1),
                                    tr.unlocked ? IM_COL32_WHITE : IM_COL32(110, 110, 120, 255), 8.0f * g_ui_scale);
            } else {
                dl->AddRectFilled(p, ImVec2(p.x + icon, p.y + icon), Col(kFrame), 8.0f * g_ui_scale);
                const ImVec2 q = ImGui::CalcTextSize("?");
                dl->AddText(ImVec2(p.x + (icon - q.x) * 0.5f, p.y + (icon - q.y) * 0.5f), Col(kGrey), "?");
            }
            if (!tr.unlocked) dl->AddRect(p, ImVec2(p.x + icon, p.y + icon), Col(kBorder), 8.0f * g_ui_scale);
            ImGui::Dummy(ImVec2(icon, icon));
            ImGui::TableNextColumn();
            if (g_font_heading) ImGui::PushFont(g_font_heading, 0.0f);
            std::string name = secret ? T("Logro oculto", "Hidden trophy")
                               : tr.name.empty() ? std::string(T("Logro #", "Trophy #")) + std::to_string(tr.id)
                                                 : tr.name;
            ImGui::TextColored(tr.unlocked ? kText : kGrey, "%s", name.c_str());
            if (g_font_heading) ImGui::PopFont();
            ImGui::PushTextWrapPos(0.0f);
            if (secret) ImGui::TextColored(kGrey, "%s", T("Sigue jugando para descubrirlo.", "Keep playing to find it."));
            else if (!tr.detail.empty()) ImGui::TextColored(kGrey, "%s", tr.detail.c_str());
            ImGui::PopTextWrapPos();
            ImGui::TableNextColumn();
            const int g = grade_index(tr.grade);
            if (g >= 0) {
                ImGui::TextColored(grade_colors[g], "\xE2\x97\x8F %s", grade_name(g));
            }
            if (tr.unlocked > 1) {
                const std::time_t when = std::time_t(tr.unlocked);
                std::tm local{};
                localtime_s(&local, &when);
                char date[32];
                std::strftime(date, sizeof date, "%d/%m/%Y %H:%M", &local);
                ImGui::TextColored(kGreen, "%s %s", ICON_CHECK, date);
            } else if (tr.unlocked) {
                ImGui::TextColored(kGreen, "%s %s", ICON_CHECK, T("Conseguido", "Unlocked"));
            } else {
                ImGui::TextColored(kGrey, "%s", T("Por conseguir", "Locked"));
            }
        }
        ImGui::EndTable();
    }
    ImGui::TextColored(kGrey, "%s", T("Se desbloquean jugando, como en la PS4; el platino llega solo al tener todos los demás.",
                                      "They unlock as you play, as on a PS4; the platinum comes once all the others are."));

    if (g_trophy_reset_confirm) {
        ImGui::OpenPopup("trophy_reset");
        g_trophy_reset_confirm = false;
    }
    if (ImGui::BeginPopupModal("trophy_reset", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextUnformatted(T("¿Volver a bloquear todos los logros? Se guarda una copia por si acaso.",
                                 "Lock every trophy again? A copy is kept just in case."));
        if (ImGui::Button(T("Reiniciar", "Reset"))) {
            std::error_code ec;
            const fs::path file = TrophyDir(t.title_id) / L"unlocked.txt";
            fs::path backup = file;
            backup += L"." + std::to_wstring(std::time(nullptr)) + L".bak";
            fs::rename(file, backup, ec);
            if (ec) a.notice = std::string(T("No se pudo reiniciar: ", "Could not reset: ")) + ec.message();
            ReadUnlocked(t);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancelar", "Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}


// ---------------------------------------------------------------------------------------------
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
    if (std::strcmp(SS2R_VERSION, "dev") != 0) return SS2R_VERSION;
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

// ---------------------------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------------------------
// Co-op (Settings), v2: Off / Host / Join, the player name, this PC's addresses (Host) and the
// host's address (Join). At PLAY (StartGame) they become, for the game:
//   Host or Join  -> BB_ONLINE_SERVER=127.0.0.1  (runtime_net.c: network on, real sockets, PSN in)
//   Player name   -> BB_USER_NAME (the console user name the game shows), BB_ONLINE_ID (PSN name),
//                    seta net_player_name "..." in kexengine.cfg
//   Join address  -> BB_ONLINE_ALLOW (runtime_net.c may send there; LAN and VPN ranges need nothing)
// The host needs no list of joiners: the runtime answers whoever sent it a packet first.

/// Sets  seta <name> "<value>"  in kexengine.cfg; false when the game has not made the file yet.
bool SetGameCvar(const std::string& name, std::string value) {
    const fs::path cfg = SaveRoot() / L"SAVEDATA" / L"kexengine.cfg";
    if (!IsFile(cfg)) return false;
    value.erase(std::remove(value.begin(), value.end(), '"'), value.end());
    std::string text = ReadText(cfg);
    const std::string prefix = "seta " + name + " ";
    const std::string line = prefix + "\"" + value + "\"";
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        if (text.compare(at, prefix.size(), prefix) == 0) {
            const size_t stop = (end > at && text[end - 1] == '\r') ? end - 1 : end;
            if (text.compare(at, stop - at, line) == 0) return true;
            text.replace(at, stop - at, line);
            return WriteTextAtomic(cfg, text);
        }
        at = end + 1;
    }
    if (!text.empty() && text.back() != '\n') text += '\n';
    return WriteTextAtomic(cfg, text + line + "\n");
}

/// "off", "host" or "join" (a v1 launcher's "coop" checkbox counts as host).
std::string CoopMode() {
    std::string mode = g_app.launcher.Get("coop_mode");
    if (mode.empty()) mode = g_app.launcher.Flag("coop", false) ? "host" : "off";
    return mode == "host" || mode == "join" ? mode : "off";
}

/// The name the game shows: printable ASCII, at most 16 characters; empty keeps the default ("Player 1").
std::string CoopDisplayName() {
    std::string out;
    for (char c : g_app.launcher.Get("coop_name"))
        if (c >= 0x20 && c < 0x7f && out.size() < 16) out += c;
    while (!out.empty() && out.back() == ' ') out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    return out;
}

/// The PSN online ID: 3-16 of A-Z a-z 0-9 - _ from the display name (spaces become _).
std::string CoopPsnId() {
    std::string out;
    for (char c : CoopDisplayName()) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') out += c;
        else if (c == ' ') out += '_';
    }
    if (out.empty()) return "Player";
    while (out.size() < 3) out += '_';
    return out;
}

bool IsIPv4(const std::string& text) {
    in_addr addr{};
    return !text.empty() && inet_pton(AF_INET, text.c_str(), &addr) == 1;
}

struct LocalAddress {
    std::string ip, adapter;
};

/// This PC's IPv4 addresses on adapters that are up (LAN, Wi-Fi, VPNs), loopback left out.
std::vector<LocalAddress> ScanLocalAddresses() {
    std::vector<LocalAddress> list;
    ULONG size = 32 * 1024;
    std::vector<unsigned char> buffer(size);
    auto* first = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG result = GetAdaptersAddresses(AF_INET, flags, nullptr, first, &size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        first = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        result = GetAdaptersAddresses(AF_INET, flags, nullptr, first, &size);
    }
    if (result != NO_ERROR) return list;
    for (auto* ad = first; ad; ad = ad->Next) {
        if (ad->OperStatus != IfOperStatusUp || ad->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = ad->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            char text[64] = "";
            const auto* in = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
            if (!inet_ntop(AF_INET, &in->sin_addr, text, sizeof text)) continue;
            if (!std::strncmp(text, "169.254.", 8)) continue;  // no network yet (self-assigned)
            list.push_back({text, ad->FriendlyName ? Narrow(ad->FriendlyName) : std::string()});
        }
    }
    return list;
}

void CoopSettings() {
    App& a = g_app;
    Section("Co-op");
    static char name[32];
    static char join[64];
    static std::vector<LocalAddress> addresses;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        std::snprintf(name, sizeof name, "%s", a.launcher.Get("coop_name").c_str());
        std::snprintf(join, sizeof join, "%s", a.launcher.Get("coop_join").c_str());
        addresses = ScanLocalAddresses();
    }

    const std::string mode = CoopMode();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Mode");
    ImGui::SameLine(ImGui::GetFontSize() * 9.0f);
    for (const char* m : {"off", "host", "join"}) {
        const char* label = !std::strcmp(m, "off") ? "Off" : !std::strcmp(m, "host") ? "Host" : "Join";
        if (std::strcmp(m, "off")) ImGui::SameLine();
        if (ImGui::RadioButton(label, mode == m)) {
            a.launcher.Set("coop_mode", m);
            SaveLauncher();
            if (!std::strcmp(m, "host")) addresses = ScanLocalAddresses();
        }
    }
    HelpMarker("Host or Join switch the network on for the game's LAN co-op (Multiplayer > LAN). Off: "
               "the game sees no network.");

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Player name");
    ImGui::SameLine(ImGui::GetFontSize() * 9.0f);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    if (ImGui::InputTextWithHint("##coop_name", "Player 1", name, 17)) {
        a.launcher.Set("coop_name", name);
        SaveLauncher();
    }
    HelpMarker("The name the game shows (lobby, co-op), up to 16 characters. Empty: \"Player 1\".");

    if (mode == "host") {
        const std::string chosen = a.launcher.Get("coop_host_addr");
        const LocalAddress* selected = nullptr;
        for (const auto& addr : addresses)
            if (addr.ip == chosen) selected = &addr;
        if (!selected && !addresses.empty()) selected = &addresses.front();
        auto label = [](const LocalAddress& addr) {
            return addr.ip + (addr.adapter.empty() ? "" : "   (" + addr.adapter + ")");
        };
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Your address");
        ImGui::SameLine(ImGui::GetFontSize() * 9.0f);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 20.0f);
        if (ImGui::BeginCombo("##coop_host_addr", selected ? label(*selected).c_str() : "no network found")) {
            for (const auto& addr : addresses) {
                if (ImGui::Selectable(label(addr).c_str(), selected == &addr)) {
                    a.launcher.Set("coop_host_addr", addr.ip);
                    SaveLauncher();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Rescan")) addresses = ScanLocalAddresses();
        ImGui::SameLine();
        ImGui::BeginDisabled(!selected);
        if (ImGui::Button("Copy IP") && selected) {
            ImGui::SetClipboardText(selected->ip.c_str());
            a.notice = "Copied " + selected->ip + " to the clipboard.";
        }
        ImGui::EndDisabled();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(kGrey, "Friends join with: Multiplayer > LAN > Join game by IP > %s",
                           selected ? selected->ip.c_str() : "your address");
        ImGui::TextColored(kGrey, "%s", "Over the internet: use a VPN (Tailscale, ZeroTier, Radmin) and share its "
                                        "address, or forward UDP port 5069 on your router to this PC and share your "
                                        "public IP (search \"what is my IP\"). Allow SS2R through Windows Firewall.");
        ImGui::PopTextWrapPos();
    } else if (mode == "join") {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Host's address");
        ImGui::SameLine(ImGui::GetFontSize() * 9.0f);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        if (ImGui::InputTextWithHint("##coop_join", "e.g. 100.101.7.33", join, sizeof join,
                                     ImGuiInputTextFlags_CharsNoBlank)) {
            a.launcher.Set("coop_join", join);
            SaveLauncher();
        }
        ImGui::SameLine();
        if (!join[0]) ImGui::TextColored(kGrey, "%s", "(the IP the host sent you)");
        else if (IsIPv4(join)) ImGui::TextColored(kGreen, "%s", "valid");
        else ImGui::TextColored(kRed, "%s", "not an IPv4 address");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(kGrey, "In game: Multiplayer > LAN > Join game by IP > %s",
                           join[0] ? join : "the host's address");
        ImGui::PopTextWrapPos();
    }
}

void Frame() {
    App& a = g_app;
    if (a.probe_done && a.probe.joinable()) a.probe.join();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    DrawHeader();
    static bool was_controls = false;
    bool in_controls = false;
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem(ICON_PLAY "  Play")) {
            PlayTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ICON_SETTINGS "  Settings")) {
            ImGui::BeginChild("settings");
            SettingsTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ICON_GAMEPAD "  Controls")) {
            in_controls = true;
            ControlsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ICON_SAVE "  Saves")) {
            ImGui::BeginChild("saves");
            SavesTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ICON_TROPHY "  Trophies")) {
            ImGui::BeginChild("trophies_tab");
            TrophiesTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ICON_TOOLS "  Tools")) {
            ToolsTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    SaveConfirmations();
    if (was_controls && !in_controls) LeaveControls();
    was_controls = in_controls;
    if (!a.notice.empty()) ImGui::OpenPopup("notice");
    if (ImGui::BeginPopupModal("notice", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextUnformatted(a.notice.c_str());
        if (ImGui::Button("OK", ImVec2(120.0f * g_ui_scale, 0))) {
            a.notice.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::End();
}

void ApplyTheme(float scale) {
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowPadding = ImVec2(18 * scale, 14 * scale);
    st.FramePadding = ImVec2(10 * scale, 6 * scale);
    st.ItemSpacing = ImVec2(10 * scale, 8 * scale);
    st.ItemInnerSpacing = ImVec2(8 * scale, 6 * scale);
    st.CellPadding = ImVec2(8 * scale, 5 * scale);
    st.ScrollbarSize = 12 * scale;
    st.GrabMinSize = 12 * scale;
    st.WindowRounding = 0;
    st.ChildRounding = 8 * scale;
    st.FrameRounding = 6 * scale;
    st.PopupRounding = 8 * scale;
    st.ScrollbarRounding = 8 * scale;
    st.GrabRounding = 6 * scale;
    st.TabRounding = 6 * scale;
    st.WindowBorderSize = 0;
    st.ChildBorderSize = 1;
    st.PopupBorderSize = 1;
    st.SeparatorTextBorderSize = 2 * scale;
    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = kGrey;
    c[ImGuiCol_WindowBg] = kBg;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = kPanel;
    c[ImGuiCol_Border] = kBorder;
    c[ImGuiCol_FrameBg] = kFrame;
    c[ImGuiCol_FrameBgHovered] = kFrameHover;
    c[ImGuiCol_FrameBgActive] = kFrameActive;
    c[ImGuiCol_Button] = kFrame;
    c[ImGuiCol_ButtonHovered] = kFrameHover;
    c[ImGuiCol_ButtonActive] = kFrameActive;
    c[ImGuiCol_Header] = kFrame;
    c[ImGuiCol_HeaderHovered] = kFrameHover;
    c[ImGuiCol_HeaderActive] = kFrameActive;
    c[ImGuiCol_Tab] = kPanel;
    c[ImGuiCol_TabHovered] = kFrameHover;
    c[ImGuiCol_TabSelected] = kFrame;
    c[ImGuiCol_TabSelectedOverline] = kAccent;
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_SliderGrabActive] = kAccentHover;
    c[ImGuiCol_Separator] = kBorder;
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.03f);
}

// Segoe UI, with Segoe UI Symbol (PlayStation shapes) and the icon font merged in.
void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    wchar_t windows[MAX_PATH];
    const UINT n = GetWindowsDirectoryW(windows, MAX_PATH);
    const fs::path dir = fs::path(std::wstring(windows, n)) / L"Fonts";
    auto font = [&](std::initializer_list<const wchar_t*> names) -> std::string {
        for (const wchar_t* name : names)
            if (IsFile(dir / name)) return PathText(dir / name);
        return {};
    };
    const std::string body = font({L"segoeui.ttf"}), semibold = font({L"seguisb.ttf", L"segoeuib.ttf", L"segoeui.ttf"});
    const std::string title = font({L"bahnschrift.ttf", L"seguisb.ttf", L"segoeui.ttf"});
    const std::string symbols = font({L"seguisym.ttf"}), icons = font({L"SegoeIcons.ttf", L"segmdl2.ttf"});
    static const ImWchar kIconRanges[] = {0xE700, 0xF8FF, 0};
    auto merge = [&] {
        ImFontConfig cfg;
        cfg.MergeMode = true;
        if (!symbols.empty()) io.Fonts->AddFontFromFileTTF(symbols.c_str(), 0.0f, &cfg);
        if (!icons.empty()) {
            ImFontConfig ic;
            ic.MergeMode = true;
            ic.GlyphRanges = kIconRanges;
            ic.GlyphOffset = ImVec2(0.0f, 2.0f);
            io.Fonts->AddFontFromFileTTF(icons.c_str(), 0.0f, &ic);
        }
    };
    if (body.empty()) {
        io.Fonts->AddFontDefault();
        return;
    }
    io.Fonts->AddFontFromFileTTF(body.c_str());
    merge();
    g_font_heading = io.Fonts->AddFontFromFileTTF(semibold.c_str());
    merge();
    g_font_title = io.Fonts->AddFontFromFileTTF(title.c_str());
    merge();
}

// ---------------------------------------------------------------------------------------------
// Direct3D 11

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapchain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
UINT g_resize_w = 0, g_resize_h = 0;
bool g_occluded = false;

ID3D11ShaderResourceView* MakeTexture(const uint32_t* bgra, int width, int height) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = UINT(width);
    td.Height = UINT(height);
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{bgra, UINT(width * 4), 0};
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    if (!g_device || FAILED(g_device->CreateTexture2D(&td, &data, &texture))) return nullptr;
    g_device->CreateShaderResourceView(texture, nullptr, &view);
    texture->Release();
    return view;
}

void CreateRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    g_swapchain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}
void CleanupRenderTarget() {
    if (g_rtv) g_rtv->Release();
    g_rtv = nullptr;
}
bool CreateDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &g_swapchain, &g_device, &level, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
                                           &g_swapchain, &g_device, &level, &g_context);
    if (hr != S_OK) return false;
    CreateRenderTarget();
    return true;
}
void CleanupDevice() {
    CleanupRenderTarget();
    if (g_swapchain) g_swapchain->Release();
    if (g_context) g_context->Release();
    if (g_device) g_device->Release();
    g_swapchain = nullptr;
    g_context = nullptr;
    g_device = nullptr;
}

} // namespace

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    // Keyboard page: the next key's scan code, before ImGui sees it.
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && g_key_capture >= 0) {
        const unsigned code = unsigned((lparam >> 16) & 0xff) | ((lparam & (1 << 24)) ? 0x100u : 0u);
        g_key_captured = code + 1;
        return 0;
    }
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wparam == SIZE_MINIMIZED) return 0;
        g_resize_w = LOWORD(lparam);
        g_resize_h = HIWORD(lparam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wparam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    App& a = g_app;
    a.root = FindRoot();
    // A release keeps the game executable and everything Prepare makes in bin\; the source tree uses out\.
    if (IsFile(a.root / L"bin" / L"SS2R.exe")) a.out = a.root / L"bin";
    else a.out = a.root / L"out";
    if (!IsDir(a.out)) a.out = ExeDir();
    a.bin = a.out;
    a.python = FindPython(a.root);
    LoadSettings();
    StartProbe();

    ImGui_ImplWin32_EnableDpiAwareness();
    const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    g_ui_scale = scale;

    const HICON app_icon = LoadIconW(instance, MAKEINTRESOURCEW(1));  // res\SS2R-Launcher.ico
    WNDCLASSEXW wc{sizeof wc, CS_CLASSDC, WndProc, 0, 0, instance, app_icon, LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"Ss2rLauncher", app_icon};
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    const int width = int(1000 * scale), height = int(860 * scale);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int x = work.left + std::max(0, int(work.right - work.left - width) / 2);
    const int y = work.top + std::max(0, int(work.bottom - work.top - height) / 2);
    HWND hwnd = CreateWindowW(wc.lpszClassName, kWindowTitle, WS_OVERLAPPEDWINDOW, x, y, width, height, nullptr, nullptr,
                              instance, nullptr);
    a.hwnd = hwnd;
    if (!CreateDevice(hwnd)) {
        CleanupDevice();
        MessageBoxW(nullptr, L"Direct3D 11 is not available.", kWindowTitle, MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);
    {
        const BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
        const COLORREF caption = RGB(0x0F, 0x10, 0x13);
        DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof caption);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.FontSizeBase = 17.0f;
    style.FontScaleDpi = scale;
    ApplyTheme(scale);
    LoadFonts();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    bool done = false;
    while (!done) {
        MSG msg;
        if (IsIconic(hwnd)) MsgWaitForMultipleObjects(0, nullptr, FALSE, 250, QS_ALLINPUT);
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (IsIconic(hwnd)) continue;
        if (g_occluded && g_swapchain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(10);
            continue;
        }
        g_occluded = false;
        if (g_resize_w && g_resize_h) {
            CleanupRenderTarget();
            g_swapchain->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
            g_resize_w = g_resize_h = 0;
            CreateRenderTarget();
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        Frame();
        ImGui::Render();
        const float clear[4] = {kBg.x, kBg.y, kBg.z, 1.0f};
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_occluded = g_swapchain->Present(1, 0) == DXGI_STATUS_OCCLUDED;
    }

    // Closing the launcher ends whatever it started (the game included).
    if (a.busy) StopTask();
    if (a.worker.joinable()) a.worker.join();
    if (a.probe.joinable()) a.probe.join();
    Pads::Stop();
    SaveLauncher();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDevice();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    CoUninitialize();
    return 0;
}
