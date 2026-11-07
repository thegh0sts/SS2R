#!/usr/bin/env python3
r"""SS2R co-op (LAN driver), round 2. Run once from the project root, after apply_posix_udp.py:

    python apply_coop_lan.py

src/runtime_net.c
  * Online mode also lets the game reach the local network: private addresses (10.x, 172.16-31.x,
    192.168.x, 169.254.x), multicast (224.x-239.x) and broadcast. The LAN lobby finds hosts by
    sending to the multicast group 224.69.69.69:5069, which was refused ("not the private server").
    Public internet addresses still need BB_ONLINE_ALLOW.
  * setsockopt at the IP level passes the multicast options through (IP_MULTICAST_IF/TTL/LOOP,
    IP_ADD/DROP_MEMBERSHIP: the same numbers and layout on the PS4 and on Windows) and logs them.
src/runtime_services.c
  * sceImeDialogGetPanelSize / sceImeDialogGetPanelSizeExtended: the on-screen keyboard's size,
    asked before it opens (Session Name, Join game by IP). The text itself is typed into the game
    window as before.
Then ./build.sh.
"""
import sys
from pathlib import Path

net = Path('src/runtime_net.c')
svc = Path('src/runtime_services.c')
if not net.exists() or not svc.exists():
    sys.exit('run this from the project root (src/runtime_net.c or src/runtime_services.c not found)')
changes = []

# --- runtime_net.c ---------------------------------------------------------------------------
s = net.read_text(encoding='utf-8')
if 'SS2R LAN' not in s:
    old_allow = '    if (addr==cfg.server || (ntohl(addr)>>24)==127) return 1;\n'
    new_allow = ('    if (addr==cfg.server || (ntohl(addr)>>24)==127) return 1;\n'
                 '    {   /* SS2R LAN co-op: the local network, multicast and broadcast (no internet hosts) */\n'
                 '        const uint32_t h=ntohl(addr);\n'
                 '        if ((h>>24)==10 || (h>>20)==0xAC1 || (h>>16)==0xC0A8 || (h>>16)==0xA9FE) return 1;\n'
                 '        if ((h>>28)==0xE || h==0xFFFFFFFFu || (h&0xFF)==0xFF) return 1;\n'
                 '    }\n')
    old_ip = ('        else if (option==4) { native_level=IPPROTO_IP; native_option=IP_TTL; }\n'
              '        else return 0;\n')
    new_ip = ('        else if (option==4) { native_level=IPPROTO_IP; native_option=IP_TTL; }\n'
              '        /* SS2R LAN: multicast, same numbers and structures on the PS4 (BSD) and Windows:\n'
              '         * 9 IP_MULTICAST_IF (in_addr), 10 TTL, 11 LOOP, 12/13 ADD/DROP_MEMBERSHIP (ip_mreq). */\n'
              '        else if (option==9 || option==12 || option==13) {\n'
              '            printf("Net: setsockopt %d \'%s\' IP option %d (multicast, %u bytes)\\n",id,o->name,option,len);\n'
              '            return setsockopt(o->s,IPPROTO_IP,option,(const char *)value,(int)len) ? fail(native_error()) : 0;\n'
              '        }\n'
              '        else if (option==10 || option==11) {\n'
              '            printf("Net: setsockopt %d \'%s\' IP option %d = %d (multicast)\\n",id,o->name,option,v);\n'
              '            native_level=IPPROTO_IP; native_option=option;\n'
              '        }\n'
              '        else {\n'
              '            printf("Net: setsockopt %d \'%s\' IP option %d accepted (not passed on)\\n",id,o->name,option);\n'
              '            return 0;\n'
              '        }\n')
    for old, new in ((old_allow, new_allow), (old_ip, new_ip)):
        if s.count(old) != 1:
            sys.exit(f'src/runtime_net.c: expected one match (nothing changed):\n{old}')
        s = s.replace(old, new, 1)
    net.write_text(s, encoding='utf-8', newline='\n')
    changes.append(str(net))

# --- runtime_services.c ----------------------------------------------------------------------
t = svc.read_text(encoding='utf-8')
if 'ime_panel_size' not in t:
    anchor = '/* ImeDialog: text typed on the keyboard into the game window (title bar shows it).'
    table = '{"sceImeDialogTerm",ime_term}, {"sceImeDialogAbort",ime_term},'
    if t.count(anchor) != 1 or t.count(table) != 1:
        sys.exit('src/runtime_services.c: the ImeDialog block was not found as expected (nothing changed)')
    funcs = ('/* SS2R: the on-screen keyboard\'s panel size, asked before it opens (any reasonable size). */\n'
             'static ABI int32_t ime_panel_size(const void *param,uint32_t *width,uint32_t *height) {\n'
             '    (void)param;\n'
             '    if (width) *width=800;\n'
             '    if (height) *height=400;\n'
             '    return 0;\n'
             '}\n'
             'static ABI int32_t ime_panel_size_ext(const void *param,const void *extended,uint32_t *width,uint32_t *height) {\n'
             '    (void)extended;\n'
             '    return ime_panel_size(param,width,height);\n'
             '}\n')
    t = t.replace(anchor, funcs + anchor, 1)
    t = t.replace(table, table + ' {"sceImeDialogGetPanelSize",ime_panel_size}, '
                                 '{"sceImeDialogGetPanelSizeExtended",ime_panel_size_ext},', 1)
    svc.write_text(t, encoding='utf-8', newline='\n')
    changes.append(str(svc))

print('\n'.join('changed: ' + c for c in changes) if changes else 'already applied')
