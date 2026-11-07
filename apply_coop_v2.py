#!/usr/bin/env python3
r"""SS2R co-op, v2: Settings > Co-op as Off / Host / Join, plus two runtime changes. Run once from the
project root (after apply_coop_lan.py and apply_saves_tab.py; replaces apply_coop_settings.py's section
if that was applied):

    python apply_coop_v2.py

Launcher (launcher/windows/ss2_launcher.cpp, gpu/CMakeLists.txt):
  Mode           Off / Host / Join
  Player name    up to 16 characters -> BB_USER_NAME (the name the game shows), BB_ONLINE_ID (PSN),
                 seta net_player_name in kexengine.cfg
  Host           this PC's addresses (all adapters that are up: LAN, Wi-Fi, VPNs), Rescan, Copy IP
  Join           the host's address -> BB_ONLINE_ALLOW
  Host or Join   -> BB_ONLINE_SERVER=127.0.0.1 (the runtime's network mode); Off removes it
Runtime (src/runtime_net.c):
  * an address that sends the game a packet may be answered: the host needs no list of joiners;
  * Tailscale (100.64.0.0/10) and Radmin VPN (26.0.0.0/8) addresses count as local, like 192.168.x.
Then ./build.sh.
"""
import re
import sys
from pathlib import Path

COOP = r'''// ---------------------------------------------------------------------------------------------
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

'''

lp = Path('launcher/windows/ss2_launcher.cpp')
cm = Path('gpu/CMakeLists.txt')
net = Path('src/runtime_net.c')
for f in (lp, cm, net):
    if not f.exists():
        sys.exit(f'run this from the project root ({f} not found)')
changes = []

# --- launcher ----------------------------------------------------------------------------------
s = lp.read_text(encoding='utf-8')
if 'std::string CoopMode()' in s:
    print('ss2_launcher.cpp: already applied')
else:
    if 'std::wstring SaveTitleId()' not in s:
        sys.exit('apply_saves_tab.py first (this uses its save folder lookup)')
    if '#include <winsock2.h>' not in s:
        s = s.replace('#include <windows.h>\n', '#include <winsock2.h>  // before windows.h\n#include <ws2tcpip.h>\n#include <windows.h>\n', 1)
    if '#include <iphlpapi.h>' not in s:
        s = s.replace('#include <windows.h>\n', '#include <windows.h>\n#include <iphlpapi.h>\n', 1)

    decls = ('std::string CoopMode();  // Co-op settings, below\n'
             'std::string CoopDisplayName();\n'
             'std::string CoopPsnId();\n'
             'bool SetGameCvar(const std::string& name, std::string value);\n\n')
    v1_decls = ('std::string CoopName();  // Co-op settings, below\n'
                'bool SetGameCvar(const std::string& name, std::string value);\n\n')
    if v1_decls in s:
        s = s.replace(v1_decls, decls, 1)
    elif s.count('void StartGame() {') == 1:
        s = s.replace('void StartGame() {', decls + 'void StartGame() {', 1)
    else:
        sys.exit('ss2_launcher.cpp: StartGame not found (nothing changed)')

    start_block = ('    {   // Co-op (Settings > Co-op): the runtime\'s network mode, the player name\n'
                   '        const std::string mode = CoopMode();\n'
                   '        const bool coop = mode != "off";\n'
                   '        const std::string shown = CoopDisplayName();\n'
                   '        SetEnv(L"BB_USER_NAME", shown);\n'
                   '        SetEnv(L"BB_ONLINE_SERVER", coop ? "127.0.0.1" : "");\n'
                   '        SetEnv(L"BB_ONLINE_ID", coop ? CoopPsnId() : "");\n'
                   '        SetEnv(L"BB_ONLINE_ALLOW", mode == "join" ? a.launcher.Get("coop_join") : "");\n'
                   '        if (!shown.empty()) SetGameCvar("net_player_name", shown);\n'
                   '    }\n')
    v1_block = ('    {   // Co-op: the runtime\'s network mode for the game\'s LAN lobby (Settings > Co-op)\n'
                '        const bool coop = a.launcher.Flag("coop", false);\n'
                '        const std::string name = CoopName();\n'
                '        SetEnv(L"BB_ONLINE_SERVER", coop ? "127.0.0.1" : "");\n'
                '        SetEnv(L"BB_ONLINE_ID", coop ? name : "");\n'
                '        SetEnv(L"BB_ONLINE_ALLOW", coop ? a.launcher.Get("coop_allow") : "");\n'
                '        if (coop) SetGameCvar("net_player_name", name);\n'
                '    }\n')
    if v1_block in s:
        s = s.replace(v1_block, start_block, 1)
    elif s.count('    SetEnv(L"BB_MENU_LANGUAGE", "en");\n') == 1:
        s = s.replace('    SetEnv(L"BB_MENU_LANGUAGE", "en");\n', start_block + '    SetEnv(L"BB_MENU_LANGUAGE", "en");\n', 1)
    else:
        sys.exit('ss2_launcher.cpp: the place for the co-op settings in StartGame was not found (nothing changed)')

    if 'void CoopSettings();  // below' not in s:
        s = s.replace('void SettingsTab() {', 'void CoopSettings();  // below\n\nvoid SettingsTab() {', 1)
        if s.count('    Section("Console");') != 1:
            sys.exit('ss2_launcher.cpp: Section("Console") not found (nothing changed)')
        s = s.replace('    Section("Console");', '    CoopSettings();\n\n    Section("Console");', 1)

    v1_mark = '// Co-op (Settings): the engine\'s LAN lobby needs'
    if v1_mark in s:  # replace apply_coop_settings.py's block
        begin = s.rfind('// ----', 0, s.index(v1_mark))
        fn = s.index('void CoopSettings() {', begin)
        end = s.index('\n}\n', fn) + 3
        while end < len(s) and s[end] == '\n':
            end += 1
        s = s[:begin] + COOP + s[end:]
    else:
        if s.count('void Frame() {') != 1:
            sys.exit('ss2_launcher.cpp: Frame not found (nothing changed)')
        s = s.replace('void Frame() {', COOP + 'void Frame() {', 1)
    lp.write_text(s, encoding='utf-8', newline='\n')
    changes.append(str(lp))

c = cm.read_text(encoding='utf-8')
if 'SS2R launcher, Co-op v2' not in c:
    c = c.rstrip('\n') + """

# SS2R launcher, Co-op v2: this PC's addresses (GetAdaptersAddresses, Winsock).
if(TARGET ss2-launcher)
    set_property(TARGET ss2-launcher APPEND PROPERTY LINK_LIBRARIES ws2_32 iphlpapi)
endif()
"""
    cm.write_text(c, encoding='utf-8', newline='\n')
    changes.append(str(cm))

# --- runtime -----------------------------------------------------------------------------------
n = net.read_text(encoding='utf-8')
if 'peers_lock' not in n:
    lan = '        if ((h>>24)==10 || (h>>20)==0xAC1 || (h>>16)==0xC0A8 || (h>>16)==0xA9FE) return 1;\n'
    if lan not in n:
        sys.exit('src/runtime_net.c: apply_coop_lan.py first (its LAN rule was not found)')
    n = n.replace(lan,
        '        if ((h>>24)==10 || (h>>20)==0xAC1 || (h>>16)==0xC0A8 || (h>>16)==0xA9FE) return 1;\n'
        '        if ((h>>22)==0x191 || (h>>24)==26) return 1;  /* Tailscale 100.64/10, Radmin VPN 26/8 */\n'
        '        if (known_peer(addr)) return 1;  /* it contacted us first */\n', 1)
    where = '/* Where the game may connect or send: the server, loopback, BB_ONLINE_ALLOW. */'
    if n.count(where) != 1:
        sys.exit('src/runtime_net.c: allowed() was not found as expected (nothing changed)')
    n = n.replace(where,
        '/* SS2R co-op: an address that sent the game a packet may be answered, so a host needs no list of\n'
        ' * joiners (they still need the host\'s address when it is on the internet). */\n'
        'static pthread_mutex_t peers_lock=PTHREAD_MUTEX_INITIALIZER;\n'
        'static uint32_t peers[64];\n'
        'static int peer_count;\n'
        'static int known_peer(uint32_t addr) {\n'
        '    int found=0;\n'
        '    pthread_mutex_lock(&peers_lock);\n'
        '    for (int i=0;i<peer_count && !found;++i) found=peers[i]==addr;\n'
        '    pthread_mutex_unlock(&peers_lock);\n'
        '    return found;\n'
        '}\n'
        'static void learn_peer(uint32_t addr) {\n'
        '    if (!addr || known_peer(addr)) return;\n'
        '    pthread_mutex_lock(&peers_lock);\n'
        '    const int added=peer_count<64;\n'
        '    if (added) peers[peer_count++]=addr;\n'
        '    pthread_mutex_unlock(&peers_lock);\n'
        '    char text[32];\n'
        '    ip_text(addr,text,sizeof text);\n'
        '    if (added) printf("Net: %s contacted us; it may be answered\\n",text);\n'
        '}\n' + where, 1)
    rcv = '    const int n=recvfrom(o->s,data,(int)size,native_flags(flags & ~PS_MSG_WAITALL),(struct sockaddr *)&from,&from_len);\n'
    if n.count(rcv) != 1:
        sys.exit('src/runtime_net.c: the recvfrom call in net_recvfrom was not found (nothing changed)')
    n = n.replace(rcv, rcv +
        '    if (n>0 && ((struct sockaddr *)&from)->sa_family==AF_INET && online())\n'
        '        learn_peer(((struct sockaddr_in *)&from)->sin_addr.s_addr);\n', 1)
    net.write_text(n, encoding='utf-8', newline='\n')
    changes.append(str(net))

print('\n'.join('changed: ' + c for c in changes) if changes else 'already applied')
users = [str(f) for f in Path('src').glob('*.c') if f.name != 'runtime_net.c' and 'BB_USER_NAME' in f.read_text(encoding='utf-8', errors='replace')]
if not users:
    print('CHECK: no runtime file besides runtime_net.c reads BB_USER_NAME: the game may keep showing "Player 1"')
