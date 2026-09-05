# -*- coding: utf-8 -*-
import os, sys, struct, json
sys.path.insert(0, ".")
import vpp_pack as vp
import volition_tex as vt
import le_strings_repack as lr

DEP = r"I:/SteamLibrary/steamapps/common/Saints Row The Third Remastered/cache/misc.vpp_pc"
OUT = r"C:/Users/haojun0823/WorkBuddy/2026-09-04-02-19-04/_verify_extract"
os.makedirs(OUT, exist_ok=True)
CHUNK = 0x1000
hdr, entries, names = vp.read_vpp(DEP)
d = open(DEP, "rb").read()
data_start = hdr["data_off"]
phys = 0
extract = {"font_body.vf3_pc", "font_body_nobdr.gvbm_pc", "menu_us.le_strings"}
for e in entries:
    name, usz, csz = e["name"], e["usz"], e["csz"]
    off = data_start + phys
    if name in extract:
        if csz == vp.NEG:
            raw = d[off:off+usz]
        else:
            raw = bytearray(); pos = off
            while len(raw) < usz:
                m1,m2,lz4len,blk = struct.unpack_from("<IIII", d, pos)
                comp = d[pos+16:pos+16+lz4len]
                raw += vp.lz4_decompress(comp, len(comp), blk); pos += 16+lz4len
            raw = bytes(raw)
        assert len(raw) == usz, name
        open(os.path.join(OUT, name), "wb").write(raw)
    size = usz if csz == vp.NEG else csz
    last = (e is entries[-1])
    phys += size if last else ((size+CHUNK-1)//CHUNK)*CHUNK

# --- 解析 vf3 ---
vf3 = open(os.path.join(OUT, "font_body.vf3_pc"), "rb").read()
count = struct.unpack_from("<I", vf3, 0x08)[0]
L = struct.unpack_from("<H", vf3, 0x16)[0]
base = struct.unpack_from("<I", vf3, 0x0C)[0]
met = 0xD0
z1 = (met + 16*count + 15) & ~15
z2 = (z1 + 4*count + 15) & ~15
print(f"vf3: count={count} L={L} base={base} met=0x{met:x} z1=0x{z1:x} z2=0x{z2:x}")

# --- 解码图集 alpha ---
gvbm = open(os.path.join(OUT, "font_body_nobdr.gvbm_pc"), "rb").read()
W = H = 4096
import numpy as np
rgba = np.frombuffer(vt.decode_dxt5(gvbm, W, H), dtype=np.uint8).reshape(H, W, 4)
alpha = rgba[:, :, 3]
print(f"图集解码: {alpha.shape}, 非零像素={int((alpha>8).sum()):,}")

def cell_nonzero(x, y, w, h):
    x0, y0 = max(0,x), max(0,y)
    x1, y1 = min(W, x+w), min(H, y+h)
    if x1<=x0 or y1<=y0: return 0
    return int((alpha[y0:y1, x0:x1] > 8).sum())

# --- 找 menu_us 里已中文化的串 (槽码 >= 329) ---
fid, ver, nb, nstr, es = lr.read_with_bucket(os.path.join(OUT, "menu_us.le_strings"))
zh_strings = []
for bb, hh, tt in es:
    if hh == 0 or len(tt) <= 2: continue
    n16 = (len(tt)-2)//2
    codes = struct.unpack(f"<{n16}H", tt[:n16*2])
    txt = "".join(chr(v) for v in codes if v)
    if any(c >= 329+base for c in codes):   # 含中文字形槽
        zh_strings.append((txt, codes))

print(f"\nmenu_us 中已中文化串: {len(zh_strings)} 条")
bad = 0
for txt, codes in zh_strings:
    for c in codes:
        if c < base: continue
        idx = c - base
        if idx >= count:
            bad += 1; continue
        adv, wid = struct.unpack_from("<II", vf3, met+16*idx)
        x = struct.unpack_from("<I", vf3, z1+4*idx)[0]
        y = struct.unpack_from("<I", vf3, z2+4*idx)[0]
        # 采样该字形单元格 (只查"有宽度却无像素"的真字形; 空格等宽0字形本就空白)
        nz = cell_nonzero(x, y, max(1,wid), L)
        if wid > 0 and nz == 0:
            bad += 1
            print(f"  ⚠ 空字形: {txt[:8]!r} 槽{c} idx{idx} @({x},{y}) w{wid}")
print(f"\n空字形/越界槽码计数: {bad}")
print("结论:", "全部中文字形有像素 ✓ 可上屏" if bad == 0 else f"有 {bad} 处风险")
