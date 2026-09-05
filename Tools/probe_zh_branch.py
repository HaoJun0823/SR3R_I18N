# -*- coding: utf-8 -*-
"""方向 B 侦查: 摸清官方 zh 分支的 4 个关键资源"""
import os, struct, sys, hashlib
sys.path.insert(0, r"C:/Users/haojun0823/WorkBuddy/2026-09-04-02-19-04")
import volition_tex as vt

CUR = r"C:/Users/haojun0823/WorkBuddy/2026-09-04-02-19-04/cur_misc"

# ===== 1. menu_zh.le_strings 全量解析 =====
print("=" * 70)
print("1. menu_zh.le_strings (560B) 全量解析")
print("=" * 70)
d = open(os.path.join(CUR, "menu_zh.le_strings"), "rb").read()
print(f"  size={len(d)}B  magic={d[:4].hex()}({'A84C7F73' if d[:4]==bytes.fromhex('73 7F 4C A8'.replace(' ','')) else '?'})")
ver, n_buckets, n_strings = struct.unpack_from('<HHI', d, 4)
print(f"  ver={ver}  n_buckets={n_buckets}  n_strings={n_strings}")
# header 12B, 然后 16B/bucket (count, pad, off, pad)
buckets = []
for i in range(n_buckets):
    cnt, _pad1, off, _pad2 = struct.unpack_from('<IIII', d, 12 + i*16)
    buckets.append((cnt, off))
# 字符串区起始 = 12 + 16*n_buckets + 4*n_strings
str_start = 12 + 16*n_buckets + 4*n_strings
print(f"  字符串区起始 = 0x{str_start:x} = {str_start}")
n_nonzero = 0
for i, (cnt, off) in enumerate(buckets):
    if cnt == 0 and off == 0:
        continue
    n_nonzero += 1
    # 字符串: 4B hash + utf16le + 2B 0
    s_off = str_start + off
    h = struct.unpack_from('<I', d, s_off)[0]
    # 读直到 0x0000 终止
    p = s_off + 4
    chars = []
    while p < len(d) - 1:
        c = struct.unpack_from('<H', d, p)[0]
        p += 2
        if c == 0:
            break
        chars.append(c)
    text = ''.join(chr(c) for c in chars)
    hex_chars = ' '.join(f'{c:04X}' for c in chars)
    print(f"  bucket[{i}] cnt={cnt} str_off=0x{off:x} hash=0x{h:08x} u16={hex_chars} -> {text!r}")
    if n_nonzero >= 10:
        print(f"  ... (truncated, total non-zero {n_nonzero})")
        break
print(f"  共有 {n_nonzero} 个非空 bucket")

# ===== 2. font_zh.vf3 头 + 关键 idx 区 =====
print()
print("=" * 70)
print("2. font_zh.vf3 (9,568B) 头 + 关键 idx 区")
print("=" * 70)
v = open(os.path.join(CUR, "font_zh.vf3_pc"), "rb").read()
print(f"  magic={v[:4]!r}  ver={struct.unpack_from('<I',v,4)[0]}  count={struct.unpack_from('<I',v,8)[0]}")
print(f"  baseChar@0xC = {struct.unpack_from('<I',v,0xC)[0]}")
print(f"  +0x10 = {struct.unpack_from('<I',v,0x10)[0]}")
print(f"  L@0x14 = {struct.unpack_from('<H',v,0x14)[0]}  L@0x16 = {struct.unpack_from('<H',v,0x16)[0]}")
kern = struct.unpack_from('<I', v, 0x20)[0]
print(f"  kern@0x20 = {kern}")
met = (0xD0 + 6*kern + 15) & ~15
print(f"  met = 0x{met:x}")
# 找 idx 132..184, 291..344 区(关键观察)
import collections
nonempty = []
for i in range(389):
    adv, wid = struct.unpack_from('<II', v, met + 16*i)
    x = struct.unpack_from('<I', v, met + 16*389 + ((met + 16*389 + 4*389 + 15) & ~15) - met + 4*i)[0] if False else 0
z1 = (met + 16*389 + 15) & ~15
z2 = (z1 + 4*389 + 15) & ~15
print(f"  z1=0x{z1:x}  z2=0x{z2:x}")
# 度量非零 + xy 非零的 idx (可能含字形)
content = []
for i in range(389):
    adv, wid, u8, kern_idx = struct.unpack_from('<IIiH', v, met + 16*i)
    x = struct.unpack_from('<I', v, z1 + 4*i)[0]
    y = struct.unpack_from('<I', v, z2 + 4*i)[0]
    if adv or wid or x or y:
        content.append((i, adv, wid, kern_idx, x, y))
print(f"  度量/x/y 任一非零的 idx 数: {len(content)}")
print(f"  前 20 个: idx adv wid kern x y")
for (i, adv, wid, ki, x, y) in content[:20]:
    print(f"    {i:3d} {adv:5d} {wid:5d} {ki:5d} {x:5d} {y:5d}")
print(f"  ... 尾 20: idx adv wid kern x y")
for (i, adv, wid, ki, x, y) in content[-20:]:
    print(f"    {i:3d} {adv:5d} {wid:5d} {ki:5d} {x:5d} {y:5d}")
# 全空 idx 段
empty = [i for i in range(389) if i not in [c[0] for c in content]]
if empty:
    print(f"  全空 idx 段: 连续范围 + 数量")
    segs = []
    s = empty[0]; p = s
    for i in empty[1:]:
        if i == p + 1: p = i
        else: segs.append((s, p)); s = i; p = i
    segs.append((s, p))
    for s_, p_ in segs:
        print(f"    idx {s_}..{p_} (len {p_-s_+1})")

# ===== 3. charlist_zh.dat 字符表 =====
print()
print("=" * 70)
print("3. charlist_zh.dat (2,249B) 字符表")
print("=" * 70)
cl_path = os.path.join(CUR, "charlist_zh.dat")
print(f"  存在? {os.path.exists(cl_path)}, size={os.path.getsize(cl_path)}B")
# 读前 30 行
with open(cl_path, "r", encoding="utf-8", errors="replace") as f:
    lines = f.readlines()
print(f"  总行数: {len(lines)}")
print(f"  前 15 行:")
for ln in lines[:15]:
    print(f"    {ln.rstrip()!r}")
print(f"  后 15 行:")
for ln in lines[-15:]:
    print(f"    {ln.rstrip()!r}")
# 统计 Unicode 范围
ranges = collections.Counter()
for ln in lines:
    s = ln.strip()
    if not s or s.startswith('//') or s.startswith('count='):
        continue
    try:
        v = int(s)
    except ValueError:
        continue
    if v < 0x80: ranges['ascii'] += 1
    elif v < 0x100: ranges['latin-ext'] += 1
    elif v < 0x4E00: ranges['symbol'] += 1
    elif v < 0xA000: ranges['cjk-1'] += 1
    elif v < 0x10000: ranges['cjk-2'] += 1
    else: ranges['other'] += 1
print(f"  范围分布: {dict(ranges)}")

# ===== 4. font_zh_nobdr.gvbm 渲染 alpha 找空带 =====
print()
print("=" * 70)
print("4. font_zh_nobdr.gvbm (2.79MB) 渲染 alpha 找空带")
print("=" * 70)
import numpy as np
cv = vt.parse_cvbm(os.path.join(CUR, "font_zh_nobdr.cvbm_pc"))
gv = open(os.path.join(CUR, "font_zh_nobdr.gvbm_pc"), "rb").read()
r = cv[1][0]
print(f"  cvbm: {cv[0]}  rec[0]: w={r['w']} h={r['h']} fmt={r['fmt']} mips={r['mips']} size={r['size']}")
rgba, W, H = vt.render_mip(gv, r, 0)
alpha = np.frombuffer(rgba, dtype=np.uint8)[3::4].reshape(H, W)
print(f"  渲染 mip0: {W}x{H}, alpha 总非零像素: {(alpha>8).sum()}")
# 行空带扫描
row_empty = (alpha <= 8).all(axis=1)
y = H - 1
segs = []
while y >= 0:
    if row_empty[y]:
        y0 = y
        while y0 - 1 >= 0 and row_empty[y0 - 1]: y0 -= 1
        segs.append((y0, y - y0 + 1))
        y = y0 - 1
    else:
        y -= 1
segs.sort(key=lambda x: -x[1])
print(f"  顶部 5 个空带:")
for y0, hh in segs[:5]:
    print(f"    y0={y0} 高={hh}px")
