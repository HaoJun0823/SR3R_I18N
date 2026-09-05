# -*- coding: utf-8 -*-
import os, struct, sys
sys.path.insert(0, ".")
import vpp_pack as vp
ORIG = r"I:/SteamLibrary/steamapps/common/Saints Row The Third Remastered/cache/misc.vpp_pc.orig"
hdr, entries, names = vp.read_vpp(ORIG)
d = open(ORIG, "rb").read()
data_start = hdr["data_off"]
CHUNK = 0x1000
for name in ["interface-backend.gpeg_pc", "always_loaded_veh.gpeg_pc"]:
    e = next(x for x in entries if x["name"] == name)
    # 物理偏移: 顺序按 csz 累积
    phys = 0
    for i2, e2 in enumerate(entries):
        if e2["name"] == name: break
        sz = e2["usz"] if e2["csz"] == vp.NEG else e2["csz"]
        last = (i2 == len(entries)-1)
        phys += sz if last else ((sz+CHUNK-1)//CHUNK)*CHUNK
    off = data_start + phys
    lz4len, usz = struct.unpack_from("<II", d, off+8)
    comp = d[off+16:off+16+lz4len]
    print(f"\n=== {name} ===")
    print(f"  off=0x{off:x} lz4len={lz4len} usz={usz}")
    print(f"  magic1=0x{struct.unpack_from('<I',d,off)[0]:x} magic2=0x{struct.unpack_from('<I',d,off+4)[0]:x}")
    print(f"  comp[:8] hex = {comp[:8].hex()}")
    # 尝试 1: lz4.frame
    try:
        import lz4.frame as lf
        r = lf.decompress(comp)
        print(f"  [frame] 成功 len={len(r)}")
    except Exception as ex:
        print(f"  [frame] 失败: {ex}")
    # 尝试 2: 多块 lz4.block (store_size=True, 4字节前缀循环)
    try:
        import lz4.block as lb
        out = bytearray(); si = 0
        while si < len(comp):
            blen = struct.unpack_from("<I", comp, si)[0]; si += 4
            blk = comp[si:si+blen]; si += blen
            out += lb.decompress(blk)
        print(f"  [multiblock] 成功 len={len(out)}")
    except Exception as ex:
        print(f"  [multiblock] 失败: {ex}")
    # 尝试 3: lz4.block 单块 + uncompressed_size
    try:
        import lz4.block as lb
        r = lb.decompress(comp, uncompressed_size=usz)
        print(f"  [block/u] len={len(r)}")
    except Exception as ex:
        print(f"  [block/u] 失败: {ex}")
