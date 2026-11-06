// SPDX-License-Identifier: GPL-2.0-or-later
// SS2R launcher (Windows): sets up and starts System Shock 2: 25th Anniversary Remaster with the
// sacredfire loader. "Prepare" runs prepare.py, link_modules.py (SS2's module list),
// gen_import_names.py and content_profile.py; "Play" starts out\sacredfire.exe with the prepared
// files. Output appears in the Log panel and is also written to out\run.txt.
// Dear ImGui on Win32 + Direct3D 11. Settings: ss2_launcher.ini next to this exe.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

namespace fs = std::filesystem;

namespace {

// The sce_module PRXs linked natively, in this order. Party.prx stays module 4: probe.c skips
// its initializer by index.
constexpr const wchar_t* kModules =
    L"libc.prx libSceFios2.prx libfmod.prx libPlayFabMultiplayer.prx Party.prx libSceNpToolkit2.prx";
constexpr const wchar_t* kWindowTitle = L"System Shock 2 Remastered - SS2R";

// ---------------------------------------------------------------------------------------------
// Small helpers

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
std::string TrimEnd(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}
bool IsFile(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}
bool IsDir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

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
// Settings: ss2_launcher.ini, "key=value" lines.

std::map<std::string, std::string> LoadIni(const fs::path& file) {
    std::map<std::string, std::string> values;
    std::ifstream in(file, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        line = TrimEnd(line);
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        values[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return values;
}

void SaveIni(const fs::path& file, const std::map<std::string, std::string>& values) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    for (const auto& [k, v] : values) out << k << "=" << v << "\n";
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
    fs::path root, out;
    std::optional<Python> python;
    std::map<std::string, std::string> ini;
    fs::path game_dir;
    char extra_args[512] = "";
    char display[64] = "";

    std::mutex mutex;
    std::deque<LogLine> log;
    std::string last_stop, last_api;
    std::string status = "Ready.";

    std::thread worker;
    std::atomic<bool> busy{false}, stop_requested{false};
    std::atomic<HANDLE> job{nullptr};
    bool auto_scroll = true;
    bool scroll_to_end = false;
    std::string notice;
};

App g_app;
ImFont* g_font_title = nullptr;
ImFont* g_font_heading = nullptr;
float g_scale = 1.0f;

const ImVec4 kBg{0.059f, 0.063f, 0.075f, 1.0f};
const ImVec4 kPanel{0.098f, 0.106f, 0.125f, 1.0f};
const ImVec4 kText{0.90f, 0.92f, 0.93f, 1.0f};
const ImVec4 kGrey{0.55f, 0.59f, 0.63f, 1.0f};
const ImVec4 kGreen{0.40f, 0.85f, 0.55f, 1.0f};
const ImVec4 kRed{1.0f, 0.40f, 0.38f, 1.0f};
const ImVec4 kYellow{1.0f, 0.80f, 0.32f, 1.0f};
const ImVec4 kCyan{0.36f, 0.82f, 0.86f, 1.0f};  // SHODAN green-cyan
const ImVec4 kCyanHover{0.48f, 0.90f, 0.93f, 1.0f};
const ImVec4 kCyanActive{0.26f, 0.68f, 0.72f, 1.0f};

fs::path IniPath() { return g_app.out / L"ss2_launcher.ini"; }

void SaveSettings() {
    App& a = g_app;
    a.ini["game_dir"] = PathText(a.game_dir);
    a.ini["extra_args"] = a.extra_args;
    a.ini["display"] = a.display;
    SaveIni(IniPath(), a.ini);
}

void LoadSettings() {
    App& a = g_app;
    a.ini = LoadIni(IniPath());
    if (!a.ini["game_dir"].empty()) a.game_dir = Widen(a.ini["game_dir"]);
    else if (IsFile(a.root / L"game" / L"eboot.bin")) a.game_dir = a.root / L"game";
    std::snprintf(a.extra_args, sizeof a.extra_args, "%s", a.ini["extra_args"].c_str());
    std::snprintf(a.display, sizeof a.display, "%s", a.ini["display"].c_str());
}

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
    const std::string line = TrimEnd(raw);
    std::scoped_lock lock{g_app.mutex};
    App& a = g_app;
    const LineKind kind = Classify(line);
    if (line.rfind("STOP:", 0) == 0 || line.rfind("Fault:", 0) == 0) {
        a.last_stop = line;
        a.last_api.clear();
    }
    if (line.rfind("API:", 0) == 0 && !a.last_stop.empty() && a.last_api.empty()) a.last_api = line;
    a.log.push_back({line, kind});
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
                result = "Game exited with code " + std::to_string(code) + ".";
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
        app.busy = false;
    });
}

void StopTask() {
    App& a = g_app;
    a.stop_requested = true;
    if (HANDLE job = a.job.load()) TerminateJobObject(job, 1);
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
    return {IsFile(o / L"sacredfire.exe"), IsFile(o / L"boot-linked.bin"), IsFile(o / L"import_names.txt"),
            IsFile(o / L"content.bin")};
}

Step PyStep(const std::string& description, const fs::path& script, const std::wstring& args) {
    App& a = g_app;
    return {description, a.python->exe, PythonCommand(script.wstring(), args), a.root, false};
}

std::vector<Step> PrepareSteps() {
    App& a = g_app;
    const std::wstring game = Quote(a.game_dir.wstring());
    return {
        PyStep("Preparing the executable (prepare.py)", a.root / L"scripts" / L"prepare.py", game),
        PyStep("Linking the game's modules (link_modules.py)", a.root / L"scripts" / L"link_modules.py",
               game + L" --modules " + kModules),
        PyStep("Naming the remaining imports (gen_import_names.py)", a.root / L"tools" / L"gen_import_names.py", L""),
        PyStep("Writing the content profile (content_profile.py)", a.root / L"scripts" / L"content_profile.py", game),
    };
}

std::wstring LoaderArgs() {
    App& a = g_app;
    std::wstring args = L" boot-linked.bin --app0 " + Quote(a.game_dir.wstring()) +
                        L" --import-names import_names.txt --content-profile content.bin";
    if (a.extra_args[0]) args += L" " + Widen(a.extra_args);
    return args;
}

void StartGame() {
    App& a = g_app;
    SaveSettings();
    SetEnvironmentVariableW(L"BB_DISPLAY", a.display[0] ? Widen(a.display).c_str() : nullptr);
    const fs::path exe = a.out / L"sacredfire.exe";
    StartTask("Game", {{"Starting sacredfire.exe", exe.wstring(), Quote(exe.wstring()) + LoaderArgs(), a.out, true}}, true);
}

void StartImportSummary() {
    App& a = g_app;
    const fs::path exe = a.out / L"sacredfire.exe";
    StartTask("Import list",
              {{"Listing how every import is resolved (--list-imports)", exe.wstring(),
                Quote(exe.wstring()) + LoaderArgs() + L" --list-imports", a.out, false}},
              false);
}

// ---------------------------------------------------------------------------------------------
// UI

void ApplyTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0.0f;
    s.ChildRounding = 6.0f;
    s.FrameRounding = 5.0f;
    s.GrabRounding = 5.0f;
    s.TabRounding = 5.0f;
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(10, 8);
    s.WindowPadding = ImVec2(18, 14);
    s.ScaleAllSizes(g_scale);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = kBg;
    c[ImGuiCol_ChildBg] = kPanel;
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = kGrey;
    c[ImGuiCol_Border] = ImVec4(0.20f, 0.22f, 0.26f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.15f, 0.18f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.20f, 0.24f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.24f, 0.28f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.16f, 0.18f, 0.21f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.25f, 0.29f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.12f, 0.14f, 0.17f, 1.0f);
    c[ImGuiCol_Tab] = ImVec4(0.12f, 0.13f, 0.16f, 1.0f);
    c[ImGuiCol_TabHovered] = ImVec4(0.20f, 0.23f, 0.27f, 1.0f);
    c[ImGuiCol_TabSelected] = ImVec4(0.17f, 0.20f, 0.23f, 1.0f);
    c[ImGuiCol_CheckMark] = kCyan;
    c[ImGuiCol_Header] = ImVec4(0.16f, 0.18f, 0.21f, 1.0f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.22f, 0.25f, 0.29f, 1.0f);
    c[ImGuiCol_Separator] = ImVec4(0.20f, 0.22f, 0.26f, 1.0f);
}

void Heading(const char* text) {
    if (g_font_heading) ImGui::PushFont(g_font_heading, ImGui::GetStyle().FontSizeBase * 1.15f);
    ImGui::TextUnformatted(text);
    if (g_font_heading) ImGui::PopFont();
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
    ImGui::PushStyleColor(ImGuiCol_Button, kCyan);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kCyanHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kCyanActive);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.03f, 0.06f, 0.07f, 1.0f));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}

void DrawHeader() {
    if (g_font_title) ImGui::PushFont(g_font_title, ImGui::GetStyle().FontSizeBase * 1.8f);
    ImGui::TextColored(kCyan, "SYSTEM SHOCK 2");
    if (g_font_title) ImGui::PopFont();
    ImGui::SameLine();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10.0f * g_scale);
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
                case kGameLog: color = kCyan; break;
                case kRuntime: color = kGrey; break;
                case kCommand: color = kYellow; break;
                default: break;
                }
                ImGui::TextColored(color, "%s", l.text.c_str());
            }
        }
    }
    if ((a.auto_scroll && a.busy) || a.scroll_to_end) {
        if (ImGui::GetScrollY() < ImGui::GetScrollMaxY() || a.scroll_to_end) ImGui::SetScrollHereY(1.0f);
        a.scroll_to_end = false;
    }
    ImGui::EndChild();
}

void DrawPlayTab() {
    App& a = g_app;
    const GameCheck game = CheckGame(a.game_dir);
    const PrepCheck prep = CheckPrepared();
    const bool busy = a.busy;

    ImGui::BeginChild("setup", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    Heading("Game folder");
    ImGui::TextColored(kGrey, "The merged game: base game with update 1.04 copied over it (eboot.bin, base.kpf, sce_module...).");
    ImGui::TextWrapped("%s", a.game_dir.empty() ? "(not chosen)" : PathText(a.game_dir).c_str());
    ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 120.0f * g_scale);
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Choose...", ImVec2(110.0f * g_scale, 0))) {
        if (auto picked = PickFolder(a.hwnd, L"Choose the System Shock 2 game folder", a.game_dir)) {
            a.game_dir = *picked;
            SaveSettings();
        }
    }
    ImGui::EndDisabled();
    if (!a.game_dir.empty()) {
        CheckLine(game.eboot, "eboot.bin", "");
        CheckLine(game.sys, "sce_sys\\param.sfo", "");
        CheckLine(game.libc, "sce_module\\libc.prx", "");
        CheckLine(game.data, "base.kpf (game data)", "copy the base game's files into this folder");
    }

    ImGui::Spacing();
    Heading("Prepared files");
    CheckLine(prep.loader, "out\\sacredfire.exe", "run build.sh");
    CheckLine(prep.linked, "out\\boot-linked.bin", "press Prepare");
    CheckLine(prep.names, "out\\import_names.txt", "press Prepare");
    CheckLine(prep.content, "out\\content.bin", "press Prepare");
    if (!a.python) ImGui::TextColored(kRed, "Python was not found: install it or MSYS2's python, then restart the launcher.");
    else ImGui::TextColored(kGrey, "Python: %s", a.python->description.c_str());
    ImGui::EndChild();

    ImGui::Spacing();
    const float w = ImGui::GetContentRegionAvail().x;
    const float btn_h = 44.0f * g_scale;
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
        if (AccentButton("PLAY", ImVec2(-FLT_MIN, btn_h))) StartGame();
        ImGui::EndDisabled();
    }
    ImGui::Spacing();
    DrawStatusBox();
    ImGui::Spacing();
    DrawLog();
}

void DrawToolsTab() {
    App& a = g_app;
    const bool busy = a.busy;
    Heading("Launch options");
    ImGui::SetNextItemWidth(420.0f * g_scale);
    if (ImGui::InputText("Extra loader arguments", a.extra_args, sizeof a.extra_args)) SaveSettings();
    ImGui::TextColored(kGrey, "Added to sacredfire.exe's command line, e.g. --timeout 120");
    ImGui::SetNextItemWidth(420.0f * g_scale);
    if (ImGui::InputText("Display (BB_DISPLAY)", a.display, sizeof a.display)) SaveSettings();
    ImGui::TextColored(kGrey, "Optional: the monitor number or name to open the game window on.");

    ImGui::Spacing();
    Heading("Individual steps");
    ImGui::BeginDisabled(busy || !a.python);
    if (ImGui::Button("Regenerate import names")) {
        StartTask("Import names",
                  {PyStep("Naming the remaining imports (gen_import_names.py)", a.root / L"tools" / L"gen_import_names.py", L"")},
                  false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Rewrite content profile") && !a.game_dir.empty()) {
        StartTask("Content profile",
                  {PyStep("Writing the content profile (content_profile.py)", a.root / L"scripts" / L"content_profile.py",
                          Quote(a.game_dir.wstring()))},
                  false);
    }
    ImGui::EndDisabled();
    ImGui::BeginDisabled(busy || !CheckPrepared().ok());
    ImGui::SameLine();
    if (ImGui::Button("List imports")) StartImportSummary();
    ImGui::EndDisabled();

    ImGui::Spacing();
    Heading("Folders");
    if (ImGui::Button("Open out folder")) OpenInShell(a.out);
    ImGui::SameLine();
    ImGui::BeginDisabled(a.game_dir.empty());
    if (ImGui::Button("Open game folder")) OpenInShell(a.game_dir);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Open project folder")) OpenInShell(a.root);

    ImGui::Spacing();
    ImGui::TextColored(kGrey, "Project: %s", PathText(a.root).c_str());
    ImGui::TextColored(kGrey, "Modules linked: %s", Narrow(kModules).c_str());
}

void Frame() {
    App& a = g_app;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    DrawHeader();
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("  Play  ")) {
            DrawPlayTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("  Tools  ")) {
            DrawToolsTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (!a.notice.empty()) {
        ImGui::OpenPopup("notice");
    }
    if (ImGui::BeginPopupModal("notice", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextUnformatted(a.notice.c_str());
        if (ImGui::Button("OK", ImVec2(120.0f * g_scale, 0))) {
            a.notice.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::End();
}

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
    const std::string body = font({L"segoeui.ttf"});
    const std::string semibold = font({L"seguisb.ttf", L"segoeuib.ttf", L"segoeui.ttf"});
    const std::string title = font({L"bahnschrift.ttf", L"seguisb.ttf", L"segoeui.ttf"});
    if (body.empty()) {
        io.Fonts->AddFontDefault();
        return;
    }
    io.Fonts->AddFontFromFileTTF(body.c_str());
    g_font_heading = io.Fonts->AddFontFromFileTTF(semibold.c_str());
    g_font_title = io.Fonts->AddFontFromFileTTF(title.c_str());
}

// ---------------------------------------------------------------------------------------------
// Direct3D 11

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapchain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
UINT g_resize_w = 0, g_resize_h = 0;
bool g_occluded = false;

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
    a.out = a.root / L"out";
    if (!IsDir(a.out)) a.out = ExeDir();
    a.python = FindPython(a.root);
    LoadSettings();

    ImGui_ImplWin32_EnableDpiAwareness();
    g_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc{sizeof wc, CS_CLASSDC, WndProc, 0, 0, instance, nullptr, nullptr, nullptr, nullptr, L"Ss2rLauncher", nullptr};
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    const int width = int(980 * g_scale), height = int(820 * g_scale);
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
    style.FontScaleDpi = g_scale;
    ApplyTheme();
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
    SaveSettings();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDevice();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    CoUninitialize();
    return 0;
}
