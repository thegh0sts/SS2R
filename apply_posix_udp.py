#!/usr/bin/env python3
r"""SS2R co-op: the POSIX recvfrom/sendto the engine's LAN lobby driver uses. Run once from the project root:

    python apply_posix_udp.py

The LAN driver (udpService.cpp) opens a UDP socket through libScePosix (socket, bind: already in
src/runtime_net.c) and then polls recvfrom / sends with sendto, which were missing: every frame's
recvfrom fell into "missing symbol" and the lobby never heard anything. This maps both onto the
existing net_recvfrom / net_sendto (real Winsock, same rules as the rest of online mode).
Then ./build.sh.
"""
import sys
from pathlib import Path

p = Path('src/runtime_net.c')
if not p.exists():
    sys.exit('run this from the project root (src/runtime_net.c not found)')
s = p.read_text(encoding='utf-8')
if 'posix_recvfrom' in s:
    sys.exit('already applied')

anchor = 'static ABI int32_t posix_socket(int family,int type,int protocol) { return (int32_t)posix_result(net_socket("posix",family,type,protocol)); }'
if s.count(anchor) != 1:
    sys.exit('the posix_socket line was not found as expected (nothing changed)')
funcs = (anchor + '\n'
         '/* SS2R: the engine\'s LAN lobby (udpService.cpp) polls recvfrom and answers with sendto. */\n'
         'static ABI int64_t posix_recvfrom(int s,void *data,uint64_t size,int flags,void *addr,uint32_t *len) {\n'
         '    return (int64_t)posix_result(net_recvfrom(s,data,size,flags,addr,len));\n'
         '}\n'
         'static ABI int64_t posix_sendto(int s,const void *data,uint64_t size,int flags,const void *addr,uint32_t len) {\n'
         '    return (int64_t)posix_result(net_sendto(s,data,size,flags,addr,len));\n'
         '}')
s = s.replace(anchor, funcs, 1)

entry = '{"socket",posix_socket},'
if s.count(entry) != 1:
    sys.exit('the {"socket",posix_socket} table entry was not found (nothing changed)')
s = s.replace(entry, entry + ' {"recvfrom",posix_recvfrom}, {"sendto",posix_sendto},', 1)
p.write_text(s, encoding='utf-8', newline='\n')
print('changed: src/runtime_net.c (posix recvfrom, sendto)')
