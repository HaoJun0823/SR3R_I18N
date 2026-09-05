# -*- coding: utf-8 -*-
"""列出 vpp 文件清单, 筛 font/charlist/le_strings/str2 相关"""
import struct, sys, os

def list_vpp(path):
    """读 vpp 目录: header 前部解析 (magic 0x51890ACE ver6)"""
    d = open(path, 'rb').read()
    if len(d) < 0x200: return []
    magic = struct.unpack_from('<I', d, 0)[0]
    if magic != 0x51890ACE:
        return []
    count = struct.unpack_from('<Q', d, 0x158)[0]
    dir_size = struct.unpack_from('<Q', d, 0x168)[0]
    # chunk0 = header; directory 起点 = 0x1000
    names = []
    ent = 0x1000
    name_off_base = None
    # entry 48B: {u64 nameOffset, u64 unk, u64 dataOffset, u64 usz, u64 csz, u64 unk}
    for i in range(count):
        noff, unk1, doff, usz, csz, unk2 = struct.unpack_from('<6Q', d, ent + i*48)
        if noff < len(d):
            pass
        # nameOffset 相对 names 区起点; names 区在 directory 之后
    # 更稳: 直接扫 nameOffset 最大值 -> names 区实际起点未知, 用保守法:
    # nameOffset 通常从 0 开始, names 基址 = directory_end
    dir_end = 0x1000 + count*48
    names_base = dir_end
    out = []
    for i in range(count):
        noff, unk1, doff, usz, csz, unk2 = struct.unpack_from('<6Q', d, 0x1000 + i*48)
        if csz == 0xFFFFFFFFFFFFFFFF:
            pass
        absn = names_base + noff
        if absn < len(d):
            nm = d[absn:d.find(b'\x00', absn)].decode('ascii', 'replace')
            out.append((nm, usz))
    return out

if __name__ == '__main__':
    CACHE = r"I:/SteamLibrary/steamapps/common/Saints Row The Third Remastered/cache"
    targets = sys.argv[1:] or ['interface.vpp_pc','interface_startup.vpp_pc','misc_tables.vpp_pc','patch_uncompressed.vpp_pc','startup.vpp_pc','preload_effects.vpp_pc']
    for t in targets:
        p = os.path.join(CACHE, t)
        if not os.path.exists(p):
            print(f'-- {t}: 不存在'); continue
        files = list_vpp(p)
        print(f'-- {t}: {len(files)} 文件')
        hits = [n for n,_ in files if any(k in n.lower() for k in ('font','charlist','str2','le_strings'))]
        for h in hits[:40]:
            print(f'    {h}')
        if not hits: print('    (无 font/charlist/le_strings 命中)')
