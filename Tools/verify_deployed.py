# -*- coding: utf-8 -*-
import os, sys, struct, hashlib
sys.path.insert(0, ".")
import vpp_pack as vp
DEP = r"I:/SteamLibrary/steamapps/common/Saints Row The Third Remastered/cache/misc.vpp_pc"
CUR = r"C:/Users/haojun0823/WorkBuddy/2026-09-04-02-19-04/cur_misc"
# 已部署的 v7 改动文件应从最新产物取 (misc_v7.vpp_pc), gpeg 比对用 cur_misc(原厂)
PROD = r"C:/Users/haojun0823/WorkBuddy/2026-09-04-02-19-04/misc_v7.vpp_pc"
CHUNK = 0x1000
for label, path in (("部署文件 cache/misc.vpp_pc", DEP), ("产物 misc_v7.vpp_pc", PROD)):
    hdr, entries, names = vp.read_vpp(path)
    d = open(path, "rb").read()
    data_start = hdr["data_off"]
    phys = 0
    gpeg_ok = 0
    for i, e in enumerate(entries):
        name, usz, csz = e["name"], e["usz"], e["csz"]
        off = data_start + phys
        if csz == vp.NEG:
            raw = d[off:off+usz]
        else:
            raw = bytearray(); pos = off
            while len(raw) < usz:
                m1,m2,lz4len,blk = struct.unpack_from("<IIII", d, pos)
                comp = d[pos+16:pos+16+lz4len]
                raw += vp.lz4_decompress(comp, len(comp), blk); pos += 16 + lz4len
            raw = bytes(raw)
        assert len(raw) == usz, f"{name}"
        if name in ("interface-backend.gpeg_pc", "always_loaded_veh.gpeg_pc"):
            orig = open(os.path.join(CUR, name), "rb").read()
            if raw == orig: gpeg_ok += 1
        size = usz if csz == vp.NEG else csz
        last = (i == len(entries)-1)
        phys += size if last else ((size+CHUNK-1)//CHUNK)*CHUNK
    print(f"{label}: 物理末偏移匹配={phys==hdr['cdata']} 巨型gpeg原厂一致={gpeg_ok}/2 体积={os.path.getsize(path):,}")
