#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""检查部署版 misc.vpp_pc 内全部 vf3 的 count/base/图集名"""
import sys, struct
WS = r"C:\Users\haojun0823\WorkBuddy\2026-09-04-02-19-04"
sys.path.insert(0, WS)
import lz4.block
import vpp_pack

VPP = r"I:\SteamLibrary\steamapps\common\Saints Row The Third Remastered\cache\misc.vpp_pc"
raw = open(VPP, "rb").read()
meta, entries, names = vpp_pack.read_vpp(VPP)
data_off = meta["data_off"]

def get_data(e):
    off = data_off + e["data_off"]
    magic1, magic2, lz4len, usz = struct.unpack_from("<IIII", raw, off)
    body = raw[off + 16: off + 16 + lz4len]
    if usz != lz4len:
        return lz4.block.decompress(body, uncompressed_size=usz)
    return body

fonts = {}
for e in entries:
    n = e["name"].lower()
    if n.endswith(".vf3_pc"):
        fonts[n] = get_data(e)

print(f"vf3 in deployed misc.vpp_pc: {len(fonts)}")
for n in sorted(fonts):
    b = fonts[n]
    if b[:4] == b"TNFV":
        ver, cnt = struct.unpack_from("<II", b, 4)
        base = struct.unpack_from("<I", b, 12)[0]
        atlas = b[0x68:0x70].split(b"\x00")[0].decode("ascii", "replace")
        print(f"  {n:26s} count={cnt:5d} base={base} atlas={atlas}")
    else:
        print(f"  {n:26s} NOT VF3 {b[:8].hex()}")
