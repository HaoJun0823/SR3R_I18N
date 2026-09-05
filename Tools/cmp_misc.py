# -*- coding: utf-8 -*-
"""对比两个 misc.vpp_pc: 逐文件解压 md5, 输出 changed / only-in-A / only-in-B"""
import sys, struct, hashlib
sys.path.insert(0, r"C:\Users\haojun0823\WorkBuddy\2026-09-04-02-19-04")
from vpp_pack import read_vpp, lz4_decompress, CHUNK, MAGIC1, MAGIC2, NEG

def extract_all(path):
    hdr, entries, names = read_vpp(path)
    d = open(path, "rb").read()
    out = {}
    # 物理定位: 复刻 pack_vpp 的对齐逻辑(与官方一致)
    phys = hdr["data_off"]
    for i, ent in enumerate(entries):
        csz = ent["csz"]; usz = ent["usz"]
        raw = d[phys:phys + csz]
        if csz == NEG:
            data = raw  # 未压缩
        else:
            m1, m2, l4len, u4 = struct.unpack_from("<4I", raw, 0)
            data = lz4_decompress(raw[16:16 + l4len], l4len, usz)
        out[ent["name"]] = data
        is_last = (i == len(entries) - 1)
        size = csz if csz != NEG else usz
        phys += size if is_last else ((size + CHUNK - 1) // CHUNK) * CHUNK
    return out

def md5(b):
    return hashlib.md5(b).hexdigest()

def main():
    a = extract_all(r"I:\SteamLibrary\steamapps\common\Saints Row The Third Remastered\cache\misc.vpp_pc.orig")
    b = extract_all(r"I:\SteamLibrary\steamapps\common\Saints Row The Third Remastered\汉化\游侠\cache\misc.vpp_pc")
    print("官方(orig) %d 文件, 游侠 %d 文件" % (len(a), len(b)))
    changed, only_a, only_b = [], [], []
    for n in sorted(set(a) | set(b)):
        if n not in a: only_b.append(n)
        elif n not in b: only_a.append(n)
        elif a[n] != b[n]: changed.append(n)
    print("\n== changed (%d) ==" % len(changed))
    for n in changed:
        print("  %s  (官方 %dB md5 %s.. | 游侠 %dB md5 %s..)" % (
            n, len(a[n]), md5(a[n])[:8], len(b[n]), md5(b[n])[:8]))
    print("\n== only-in-官方 (被游侠删除, %d) ==" % len(only_a))
    for n in only_a: print("  -", n)
    print("\n== only-in-游侠 (新增, %d) ==" % len(only_b))
    for n in only_b: print("  +", n)

if __name__ == "__main__":
    main()
