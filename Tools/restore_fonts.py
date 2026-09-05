# -*- coding: utf-8 -*-
"""从 misc.orig 抽 10 个 font gvbm 覆盖 unpack/misc (恢复官方基准), 再 tint_patch 染红"""
import sys, os, struct
sys.path.insert(0, r"C:\Users\haojun0823\WorkBuddy\2026-09-04-02-19-04")
from vpp_pack import read_vpp, lz4_decompress, CHUNK, NEG

ORIG = r"I:\SteamLibrary\steamapps\common\Saints Row The Third Remastered\cache\misc.vpp_pc.orig"
MISC = r"I:\SteamLibrary\steamapps\common\Saints Row The Third Remastered\unpack\misc"
STEMS = ['font_body', 'font_body_nobdr', 'font_header', 'font_header_nobdr',
         'font_header_pc', 'font_header_pc_nobdr', 'font_sk', 'font_sk_nobdr',
         'font_zh', 'font_zh_nobdr']
WANT = {s + '.gvbm_pc' for s in STEMS}

hdr, entries, names = read_vpp(ORIG)
d = open(ORIG, "rb").read()
phys = hdr["data_off"]
got = {}
for i, ent in enumerate(entries):
    name = ent["name"]; csz, usz = ent["csz"], ent["usz"]
    raw = d[phys:phys + min(csz, 0x10000000)]
    is_last = (i == len(entries) - 1)
    if csz == NEG:
        data = raw[:usz]
        phys += usz if is_last else ((usz + CHUNK - 1) // CHUNK) * CHUNK
    else:
        m1, m2, l4len, u4 = struct.unpack_from("<4I", raw, 0)
        data = lz4_decompress(raw[16:16 + l4len], l4len, u4)
        phys += csz if is_last else ((csz + CHUNK - 1) // CHUNK) * CHUNK
    if name in WANT:
        got[name] = data
        open(os.path.join(MISC, name), "wb").write(data)
missing = WANT - set(got)
print("恢复 %d/10, 缺: %s" % (len(got), missing or "无"))
