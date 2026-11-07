#!/usr/bin/env python3
r"""SS2R: add the Saves tab back to the launcher (ported from the CTR launcher). Run once from the project root:

    python apply_saves_tab.py

The tab sits between Controls and Tools. It lists the saves in <game folder>\user\savedata\<user>\<title ID>
(out\ in the source tree, bin\ in a release), with each save's title, size and date, and has:
  Back up now    copies every save to backups\saves-<date>  (backups\ sits next to the launcher)
  Export...      copies the saves to a folder you choose
  Import...      a backup, another PC's saves folder, or a decrypted PS4 save (Apollo / Save Wizard:
                 a folder with sce_sys\param.sfo); your current saves are backed up first
  Open folder
  Backups list   Restore (backs up the current saves first) and Delete, each asking for confirmation
Backing up, importing and restoring are switched off while the game runs.
"""
import sys
from pathlib import Path

p = Path('launcher/windows/ss2_launcher.cpp')
if not p.exists():
    sys.exit('run this from the project root (launcher/windows/ss2_launcher.cpp not found)')
s = p.read_text(encoding='utf-8')
if 'void SavesTab()' in s:
    sys.exit('already applied')

BLOCK = r'''// ---------------------------------------------------------------------------------------------
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

'''

TAB_OLD = '''        if (ImGui::BeginTabItem(ICON_TOOLS "  Tools")) {'''
TAB_NEW = '''        if (ImGui::BeginTabItem(ICON_SAVE "  Saves")) {
            ImGui::BeginChild("saves");
            SavesTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ICON_TOOLS "  Tools")) {'''

edits = [
    ('#include <cstring>\n', '#include <cstring>\n#include <ctime>\n'),
    ('#define ICON_TOOLS "\\xEE\\xA4\\x8F"     // E90F\n',
     '#define ICON_TOOLS "\\xEE\\xA4\\x8F"     // E90F\n#define ICON_SAVE "\\xEE\\x9D\\x8E"      // E74E\n'),
    ('void Frame() {', BLOCK + 'void Frame() {'),
    (TAB_OLD, TAB_NEW),
    ('    if (was_controls && !in_controls) LeaveControls();',
     '    SaveConfirmations();\n    if (was_controls && !in_controls) LeaveControls();'),
]
for old, new in edits:
    if s.count(old) != 1:
        sys.exit(f'expected exactly one match (nothing changed):\n{old}')
    s = s.replace(old, new)
p.write_text(s, encoding='utf-8', newline='\n')
print('changed: launcher/windows/ss2_launcher.cpp (Saves tab)')
