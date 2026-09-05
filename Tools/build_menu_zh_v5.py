# -*- coding: utf-8 -*-
"""
SRTT3 主菜单汉化 - v5 (合法码点空洞槽 + 原位覆盖版)
====================================================
v5 关键修正 (相对 v3/v4):
  - v3/v4 覆写 idx 95..118 (文本码点 0x7F-0x96) 崩于 0x3FD144
  - 取证官方 menu_us key=0x2c6dc5dc "字符全表" (0x21..0x16F 逐码点):
      [0x7F..0xA0] = 0 个码点 (禁区A), [0x145..0x168] = 0 个码点 (禁区B)
    -> 引擎字符集把 0x7F-0xA0 / 0x145-0x168 视为不可渲染/控制区
    -> v3/v4 文本码点 127-150 (0x7F-0x96) 落在禁区A; v2/二分A 码点 325-348 (0x145-0x15C) 落禁区B
    -> 三次崩溃全部踩中禁区 = 崩溃真因 (非 repack, 非空洞覆写本身)
  - v5 槽位 = 合法码点区 (0xA1..0x144) ∩ 4 字体共同空洞 ∩ 无 us/menu 实际引用
    24 槽: idx [132,133,135,136,140,141,143,145,146,147,149,150,151,152,153,
               156,157,158,163,165,176,181,183,184]
    文本 u16 值 = 码点 = idx+32 (全部 0xA4..0xD8 在字符全表合法区)
menu_us 沿用 v4 原位覆盖 (repack_inplace), 官方布局 100% 不变
"""
import sys, os, struct, shutil, subprocess
import numpy as np
import freetype

ROOT = r"C:\Users\haojun0823\WorkBuddy\2026-09-04-02-19-04"
GAME = r"I:\SteamLibrary\steamapps\common\Saints Row The Third Remastered"
M = os.path.join(GAME, "unpack", "misc")
CUR = os.path.join(ROOT, "cur_misc")
CACHE = os.path.join(GAME, "cache")
ORIG = os.path.join(CACHE, "misc.vpp_pc.orig")
FONT_SRC = r"C:\Windows\Fonts\msyh.ttc"

sys.path.insert(0, ROOT)
import volition_tex as vt
from sr3le_extract import crc_volition
import le_strings_repack as lr

# 合法码点区 (0xA1..0x144) 的 4 字体共同空洞, 排除 us/menu 实际引用后取 24 个
SLOT_IDX = [132, 133, 135, 136, 140, 141, 143, 145, 146, 147, 149, 150,
            151, 152, 153, 156, 157, 158, 163, 165, 176, 181, 183, 184]
N_SLOTS = len(SLOT_IDX)

FONTS = [("font_body", "font_body_nobdr"),
         ("font_header", "font_header_nobdr"),
         ("font_header_pc", "font_header_pc_nobdr"),
         ("font_sk", "font_sk_nobdr")]

MENU_ZH = {
    "MAINMENU_SINGLEPLAYER":   "单人游戏",
    "MAINMENU_COOP":           "合作模式",
    "MAINMENU_CAMPAIGN":       "主线战役",
    "MAINMENU_COOP_OPTION":    "合作战役",
    "MAINMENU_CHECK_MESSAGES": "查看信息",
    "MAINMENU_COMMUNITY":      "社区",
    "MAINMENU_DLC":            "下载内容",
    "PAUSE_MENU_OPTIONS":      "选项",
}


def unique_han():
    seen = []
    for v in MENU_ZH.values():
        for ch in v:
            if ch not in seen:
                seen.append(ch)
    return seen


def render_glyphs(text, pixel_h):
    face = freetype.Face(FONT_SRC)
    face.set_pixel_sizes(0, pixel_h)
    out = {}
    for ch in text:
        face.load_char(ch, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_NORMAL)
        bmp = face.glyph.bitmap
        w, h = bmp.width, bmp.rows
        raw = bmp.buffer
        if not isinstance(raw, (bytes, bytearray)):
            raw = bytes(raw)
        buf = np.frombuffer(raw, dtype=np.uint8).reshape(h, w) if raw else np.zeros((h, w), np.uint8)
        out[ch] = buf
    return out


# ---------- 标准 DXT5 alpha ----------
def decode_dxt5_block(blk):
    a0, a1 = blk[0], blk[1]
    aidx = int.from_bytes(blk[2:8], 'little')
    if a0 > a1:
        ramp = [a0, a1,
                (6*a0+1*a1)//7, (5*a0+2*a1)//7, (4*a0+3*a1)//7,
                (3*a0+4*a1)//7, (2*a0+5*a1)//7, (1*a0+6*a1)//7]
    else:
        ramp = [a0, a1,
                (4*a0+1*a1)//5, (3*a0+2*a1)//5, (2*a0+3*a1)//5, (1*a0+4*a1)//5,
                0, 255]
    return [ramp[(aidx >> (3*i)) & 7] for i in range(16)]


def encode_dxt5_block(alphas):
    a0 = max(alphas); a1 = min(alphas)
    if a0 == a1:
        return bytes([a0, a0]) + b"\x00" * 6
    ramp = [a0, a1,
            (6*a0+1*a1)//7, (5*a0+2*a1)//7, (4*a0+3*a1)//7,
            (3*a0+4*a1)//7, (2*a0+5*a1)//7, (1*a0+6*a1)//7]
    bits = 0
    for i in range(16):
        best, bd = 0, 1 << 30
        for k in range(8):
            dd = abs(alphas[i] - ramp[k])
            if dd < bd:
                bd, best = dd, k
        bits |= best << (3*i)
    return bytes([a0, a1]) + bits.to_bytes(6, 'little')


def patch_alpha_region(data, W, x0, y0, alpha_map, a_h, a_w):
    for by in range(a_h):
        for bx in range(a_w):
            gx, gy = x0 + bx, y0 + by
            blk_off = (gy // 4) * (W // 4) + (gx // 4)
            in_blk = (gy % 4) * 4 + (gx % 4)
            b0 = blk_off * 16   # DXT5: 每块 16B (8B alpha + 8B color); 旧版误用 *8 导致烘字错位一半
            blk = bytearray(data[b0:b0+8])
            al = decode_dxt5_block(blk)
            al[in_blk] = int(alpha_map[by, bx])
            data[b0:b0+8] = encode_dxt5_block(al)


def validate_hole_slots(vstem, gstem, idxs):
    vpath = os.path.join(M, vstem + ".vf3_pc")
    d = open(vpath, 'rb').read()
    count = struct.unpack_from('<I', d, 8)[0]
    kern = struct.unpack_from('<I', d, 32)[0]
    met = (0xD0 + 6*kern + 15) & ~15
    z1 = (met + 16*count + 15) & ~15
    z2 = (z1 + 4*count + 15) & ~15
    cv = os.path.join(M, gstem + ".cvbm_pc")
    gv = os.path.join(M, gstem + ".gvbm_pc")
    hdr, recs = vt.parse_cvbm(cv)
    r = recs[0]
    W, H = r['w'], r['h']
    rgba, rw, rh = vt.render_mip(bytes(open(gv, 'rb').read()), r, 0)
    alpha = np.frombuffer(rgba, dtype=np.uint8)[3::4].reshape(H, W)
    for i in idxs:
        adv, wid = struct.unpack_from('<II', d, met + 16*i)
        x0 = struct.unpack_from('<I', d, z1 + 4*i)[0]
        y0 = struct.unpack_from('<I', d, z2 + 4*i)[0]
        if wid == 0:
            continue
        if x0 >= W or y0 >= H:
            continue
        x1, y1 = min(W, x0+int(wid)), min(H, y0+int(wid))
        if x1 <= x0 or y1 <= y0:
            continue
        nz = int((alpha[y0:y1, x0:x1] > 8).sum())
        assert nz == 0, f"{vstem} idx{i} 非空洞(非零像素 {nz}) -> 覆写危险, 中止"
    return count, L if (L := struct.unpack_from('<H', d, 22)[0]) else 0, d


def bake_font_v3(vstem, gstem, han_list, slot_idx):
    print(f"\n===== {vstem} (+{gstem}) =====")
    vpath = os.path.join(M, vstem + ".vf3_pc")
    count, L, d = validate_hole_slots(vstem, gstem, slot_idx)
    assert max(slot_idx) < count, (count, max(slot_idx))
    kern = struct.unpack_from('<I', d, 32)[0]
    met = (0xD0 + 6*kern + 15) & ~15
    z1 = (met + 16*count + 15) & ~15
    z2 = (z1 + 4*count + 15) & ~15
    d = bytearray(d)
    print(f"  vf3: count={count} L={L} 校验通过 (idx {slot_idx[0]}..{slot_idx[-1]} 全空洞)")

    cv = os.path.join(M, gstem + ".cvbm_pc")
    gv = os.path.join(M, gstem + ".gvbm_pc")
    hdr, recs = vt.parse_cvbm(cv)
    r = recs[0]
    W, H = r['w'], r['h']
    gdata = bytearray(open(gv, 'rb').read())
    rgba, rw, rh = vt.render_mip(bytes(gdata), r, 0)
    alpha = np.frombuffer(rgba, dtype=np.uint8)[3::4].reshape(H, W)
    del rgba

    row_empty = (alpha <= 8).all(axis=1)
    best_y0, best_h = None, 0
    y = H - 1
    while y >= 0:
        if row_empty[y]:
            y0 = y
            while y0 - 1 >= 0 and row_empty[y0 - 1]:
                y0 -= 1
            hh = y - y0 + 1
            if hh > best_h:
                best_h, best_y0 = hh, y0
            y = y0 - 1
        else:
            y -= 1
    print(f"  图集 {W}x{H} 底部空带 y0={best_y0} 高={best_h}")

    P = round(L * 0.72)
    w4 = P + 16
    pad_top = round((L - P) * 0.6)
    per_row = max(1, (W - 8) // w4)
    need_rows = (len(han_list) + per_row - 1) // per_row
    need_h = need_rows * L
    assert best_y0 is not None and best_h >= need_h, \
        f"空带不足: 需 {need_h}px (rows={need_rows}) 实有 {best_h}"
    print(f"  字形 {P}px adv/宽={w4} pad_top={pad_top} per_row={per_row} rows={need_rows}")

    glyphs = render_glyphs("".join(han_list), P)
    for t, ch in enumerate(han_list):
        col = t % per_row
        row = t // per_row
        y0 = best_y0 + row * L
        x0 = 8 + col * w4
        buf = glyphs[ch]
        gh, gw = buf.shape
        left_pad = (w4 - gw) // 2
        cell_map = np.zeros((L, w4), np.uint8)
        sx = max(0, -left_pad); sy = max(0, -pad_top)
        ex = min(gw, w4 - left_pad); ey = min(gh, L - pad_top)
        if ex > sx and ey > sy:
            cell_map[pad_top + sy:pad_top + ey, left_pad + sx:left_pad + ex] = buf[sy:ey, sx:ex]
        patch_alpha_region(gdata, W, x0, y0, cell_map, L, w4)
        idx = slot_idx[t]
        struct.pack_into('<II', d, met + 16*idx, w4, w4)
        struct.pack_into('<I',  d, met + 16*idx + 8, 0)
        struct.pack_into('<h',  d, met + 16*idx + 12, -1)
        struct.pack_into('<I',  d, z1 + 4*idx, x0)
        struct.pack_into('<I',  d, z2 + 4*idx, y0)
        print(f"    {ch} idx={idx} 槽码={idx+32} uv=({x0},{y0}) adv/宽={w4} L={L}")
    open(gv, 'wb').write(bytes(gdata))
    open(vpath, 'wb').write(bytes(d))
    print(f"  已烘 {len(han_list)} 字 + 覆写 idx {slot_idx[0]}..{slot_idx[-1]}")


def main():
    han_list = unique_han()
    assert len(han_list) == N_SLOTS, (len(han_list), N_SLOTS)
    slot_idx = list(SLOT_IDX)
    slot = {ch: idx + 32 for ch, idx in zip(han_list, slot_idx)}
    print("唯一汉字 (%d): %s" % (len(han_list), "".join(han_list)))
    print("覆写 idx %s" % slot_idx)
    print("文本码点 (idx+32): %s" % [i+32 for i in slot_idx])

    # 1) 恢复官方基准
    print("\n恢复 unpack/misc <- cur_misc (robocopy /MIR) ...")
    subprocess.run(["robocopy", CUR, M, "/MIR", "/NFL", "/NDL", "/NJH", "/NJS"],
                   check=False)
    n = len(os.listdir(M))
    assert n == 375, ("恢复失败, 文件数=%d" % n)
    print("  已恢复 %d 文件" % n)

    # 2) 烘字 + 覆写 (同 v3)
    for vstem, gstem in FONTS:
        bake_font_v3(vstem, gstem, han_list, slot_idx)

    # 3) menu_us 文本 -> 原位覆盖 (v4 关键差异)
    src = os.path.join(M, "menu_us.le_strings")
    pairs = {}
    for key, zh in MENU_ZH.items():
        h = crc_volition(key)
        tb = b"".join(struct.pack("<H", slot[ch]) for ch in zh) + b"\x00\x00"
        pairs[h] = tb
        print(f"  {key} -> {zh} 槽{[slot[c] for c in zh]}")
    rep = lr.repack_inplace(src, pairs, src)
    print("menu_us.le_strings 原位覆盖完成:")
    for h, (s_off, slot_len, new_len) in rep.items():
        print(f"  hash={h:#x} @0x{s_off:x} 槽长{slot_len}B 写入{new_len}B")

    # 4) 槽表存档
    import json
    with open(os.path.join(ROOT, "menu_test_slots_v5.json"), "w", encoding="utf-8") as f:
        json.dump({"slot": {ch: slot[ch] for ch in han_list},
                   "slot_idx": slot_idx, "menu_zh": MENU_ZH,
                   "note": "v5 合法码点(0xA1-0x144)空洞槽 + 原位覆盖"},
                  f, ensure_ascii=False, indent=1)

    # 5) 打包 + 部署
    out_tmp = os.path.join(ROOT, "misc_v5.vpp_pc")
    print("\n打包 ->", out_tmp)
    py = r"C:\Users\haojun0823\.workbuddy\binaries\python\envs\default\Scripts\python.exe"
    subprocess.run([py, os.path.join(ROOT, "vpp_pack.py"), ORIG, M, out_tmp], check=True)
    # 留档当前部署 (v4) -> v4crash (若尚未留档)
    cur_vpp = os.path.join(CACHE, "misc.vpp_pc")
    if os.path.exists(cur_vpp):
        bak4 = os.path.join(CACHE, "misc.vpp_pc.v4crash")
        if not os.path.exists(bak4):
            shutil.copy2(cur_vpp, bak4)
            print("v4 已留档 -> misc.vpp_pc.v4crash")
    shutil.copy2(out_tmp, cur_vpp)
    print("已部署 cache/misc.vpp_pc (v5 合法码点空洞槽 + 原位覆盖)")
    print("槽表: menu_test_slots_v5.json")


if __name__ == "__main__":
    main()
