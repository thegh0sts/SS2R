#!/usr/bin/env python3
r"""SS2R: add a Trophies tab to the launcher (ported from the CTR launcher's Achievements tab).
Run once from the project root, after apply_saves_tab.py:

    python apply_trophies_tab.py

The tab (between Saves and Tools) shows SS2's 50 trophies with their pictures, which ones you have
and when you got them. The game doesn't talk to the launcher: the runtime writes each unlock to
<game folder>\user\trophies\<title ID>\unlocked.txt and the tab re-reads it every second.

Names, descriptions and grades are encrypted in the game with the PS4's trophy key (not included).
Paste yours in the tab, or import it from shadPS4 (keys.json), and the tab decrypts them once and
saves trophies.tsv, which the runtime then uses for the in-game "Trophy unlocked" notice.

Changes:
  launcher/windows/ss2_launcher.cpp   the tab
  gpu/CMakeLists.txt                  the launcher links bcrypt (AES) and windowscodecs (PNG icons)
Then ./build.sh.
"""
import sys
from pathlib import Path

BLOCK = r'''// ---------------------------------------------------------------------------------------------
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


'''

MAKE_TEXTURE = r'''ID3D11ShaderResourceView* MakeTexture(const uint32_t* bgra, int width, int height) {
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

'''

p = Path('launcher/windows/ss2_launcher.cpp')
cm = Path('gpu/CMakeLists.txt')
if not p.exists() or not cm.exists():
    sys.exit('run this from the project root (launcher/windows/ss2_launcher.cpp or gpu/CMakeLists.txt not found)')
s = p.read_text(encoding='utf-8')
if 'void TrophiesTab()' in s:
    print('ss2_launcher.cpp: already applied')
else:
    if 'std::wstring SaveTitleId()' not in s:
        sys.exit('apply_saves_tab.py first (the Trophies tab uses its title ID lookup)')
    edits = [
        ('#include <shobjidl.h>\n', '#include <shobjidl.h>\n#include <wincodec.h>\n#include <bcrypt.h>\n'),
        ('#define ICON_SAVE "\\xEE\\x9D\\x8E"      // E74E\n',
         '#define ICON_SAVE "\\xEE\\x9D\\x8E"      // E74E\n'
         '#define ICON_TROPHY "\\xEE\\x9C\\xB4"    // E734 (star)\n'
         '#define ICON_FOLDER "\\xEE\\xA2\\xB7"    // E8B7\n'
         '#define ICON_CHECK "\\xEE\\x9C\\xBE"     // E73E\n'),
        ('void Frame() {', BLOCK + 'void Frame() {'),
        ('void CreateRenderTarget() {', MAKE_TEXTURE + 'void CreateRenderTarget() {'),
        ('        if (ImGui::BeginTabItem(ICON_TOOLS "  Tools")) {',
         '        if (ImGui::BeginTabItem(ICON_TROPHY "  Trophies")) {\n'
         '            ImGui::BeginChild("trophies_tab");\n'
         '            TrophiesTab();\n'
         '            ImGui::EndChild();\n'
         '            ImGui::EndTabItem();\n'
         '        }\n'
         '        if (ImGui::BeginTabItem(ICON_TOOLS "  Tools")) {'),
    ]
    for old, new in edits:
        if s.count(old) != 1:
            sys.exit(f'expected exactly one match in ss2_launcher.cpp (nothing changed):\n{old}')
        s = s.replace(old, new)
    p.write_text(s, encoding='utf-8', newline='\n')
    print('changed: launcher/windows/ss2_launcher.cpp (Trophies tab)')

c = cm.read_text(encoding='utf-8')
if 'windowscodecs' in c:
    print('gpu/CMakeLists.txt: already applied')
else:
    c = c.rstrip('\n') + """

# SS2R launcher, Trophies tab: AES (bcrypt) for the trophy texts, WIC (windowscodecs) for the icons.
if(TARGET ss2-launcher)
    set_property(TARGET ss2-launcher APPEND PROPERTY LINK_LIBRARIES bcrypt windowscodecs)
endif()
"""
    cm.write_text(c, encoding='utf-8', newline='\n')
    print('changed: gpu/CMakeLists.txt (bcrypt, windowscodecs)')
