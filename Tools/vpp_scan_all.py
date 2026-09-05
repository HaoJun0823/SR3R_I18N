# -*- coding: utf-8 -*-
"""扫描 cache 全部 vpp, 列出 font/charlist/le_strings 家族文件 (只读目录不读数据)"""
import os, struct, glob, math

def list_vpp_names(path):
    """按 vpp_pack.py 布局公式读目录 names"""
    with open(path, 'rb') as f:
        hdr = f.read(0x188)
        if len(hdr) < 0x188: return []
        magic = struct.unpack_from('<I', hdr, 0)[0]
        if magic != 0x51890ACE: return []
        count, psize, dsize, nsize, udata, cdata = struct.unpack_from('<QQQQQQ', hdr, 0x158)
        dir_off = 0x1000
        names_off = dir_off + ((dsize + 0xFFF) // 0x1000) * 0x1000
        f.seek(dir_off)
        dirents = f.read(count * 48)
        f.seek(names_off)
        names_raw = f.read(nsize)
    out = []
    for i in range(count):
        no = struct.unpack_from('<Q', dirents, i*48)[0]
        if no >= len(names_raw): continue
        end = names_raw.find(b'\x00', no)
        if end == -1: end = len(names_raw)
        nm = names_raw[no:end].decode('ascii', 'replace')
        out.append(nm)
    return out

CACHE = r"I:/SteamLibrary/steamapps/common/Saints Row The Third Remastered/cache"
hits_total = 0
for p in sorted(glob.glob(os.path.join(CACHE, '*.vpp_pc'))):
    sz = os.path.getsize(p)
    try:
        names = list_vpp_names(p)
    except Exception as e:
        print(f'-- {os.path.basename(p)}: 解析失败 {e}')
        continue
    hits = [n for n in names if any(k in n.lower() for k in ('font', 'charlist', 'le_strings', 'menu_'))]
    tag = f'  <<< {len(hits)} 命中' if hits else ''
    print(f'-- {os.path.basename(p)}: {len(names)} 文件 {sz/1e6:.1f}MB{tag}')
    for h in hits[:60]:
        print(f'    {h}')
    hits_total += len(hits)
print(f'\\n总命中: {hits_total}')
