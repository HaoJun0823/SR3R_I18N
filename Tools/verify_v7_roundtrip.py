# -*- coding: utf-8 -*-
import os, sys, struct, hashlib
sys.path.insert(0, ".")
import vpp_pack as vp

OUT = r"C:/Users/haojun0823/WorkBuddy/2026-09-04-02-19-04/misc_v7.vpp_pc"
CUR = r"C:/Users/haojun0823/WorkBuddy/2026-09-04-02-19-04/cur_misc"
CHUNK = 0x1000
hdr, entries, names = vp.read_vpp(OUT)
d = open(OUT, "rb").read()
data_start = hdr["data_off"]
phys = 0
ok = bad = 0
checked = {}
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
            b = vp.lz4_decompress(comp, len(comp), blk)
            raw += b; pos += 16 + lz4len
        raw = bytes(raw)
    assert len(raw) == usz, f"{name} {len(raw)}!={usz}"
    # 抽样校验关键文件
    if name in ("interface-backend.gpeg_pc", "always_loaded_veh.gpeg_pc",
                "font_body.vf3_pc", "font_header_pc.vf3_pc",
                "font_body_nobdr.gvbm_pc", "menu_us.le_strings"):
        orig = open(os.path.join(CUR, name), "rb").read()
        same = (raw == orig)
        checked[name] = (len(raw), same, hashlib.md5(raw).hexdigest()[:8])
        if same: ok += 1
        else: bad += 1
    size = usz if csz == vp.NEG else csz
    last = (i == len(entries)-1)
    phys += size if last else ((size+CHUNK-1)//CHUNK)*CHUNK

print(f"物理累积末偏移=0x{phys:x} cdata=0x{hdr['cdata']:x} 匹配={phys==hdr['cdata']}")
print("关键文件回环比对 (解包==原厂字节):")
for k,(sz,same,md5) in checked.items():
    print(f"  {k:<28} {sz:>12,}B  一致={same}  md5={md5}")
print(f"一致数={ok} 不一致数={bad}")
