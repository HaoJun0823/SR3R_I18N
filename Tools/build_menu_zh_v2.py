# -*- coding: utf-8 -*-
"""
SRTT3 主菜单汉化 - v2 (空闲槽覆写模式)
======================================
崩溃根因(2026-09-04 06:00 实证): 字体字形 GPU 池(font gpu, 约17MB)按 vf3 count 全量分配。
  count 336→1724 使 font_body+font_header_pc 分配超池 → sub_140853430 返回 NULL → 0x859fef 崩。
修复策略: count 保持官方值(336/1607)不变 → 分配量与官方一致 → 零崩溃风险。
  US 英文文本(21 个 *_us.le_strings)只引用 183 个字形; 4 字体共同未引用槽 = 153 个
  (idx 64,94-128,132+,293-333 等)。把 24 汉字烘入图集空带后, 覆写空闲槽 idx 293..316
  的度量/坐标记录(槽码 325..348), 文本无需扩展。

产物:
  - 4 张 *_nobdr.gvbm_pc 底部空带烘入 24 汉字 (DXT5 字节级 patch)
  - 4 组 .vf3_pc 就地 patch: 空闲槽 idx 293..316 度量+坐标指向新字形 (count 不变)
  - menu_us.le_strings 文本替换 (保桶 repack)
"""
import sys, os, struct
import numpy as np
import freetype
sys.path.insert(0, r"C:\Users\haojun0823\WorkBuddy\2026-09-04-02-19-04")
import volition_tex as vt
from sr3le_extract import crc_volition

M = r"I:\SteamLibrary\steamapps\common\Saints Row The Third Remastered\unpack\misc"
FONT_SRC = r"C:\Windows\Fonts\msyh.ttc"
FREE_IDX0 = 293          # 空闲槽起始 idx (4 字体共同未引用, 槽码 = idx+32 = 325..348)

FONTS = [("font_body", "font_body_nobdr"),
         ("font_header", "font_header_nobdr"),
         ("font_header_pc", "font_header_pc_nobdr"),
         ("font_sk", "font_sk_nobdr")]

MENU_ZH = {
    "MAINMENU_SINGLEPLAYER":  "单人游戏",
    "MAINMENU_COOP":          "合作模式",
    "MAINMENU_CAMPAIGN":      "主线战役",
    "MAINMENU_COOP_OPTION":   "合作战役",
    "MAINMENU_CHECK_MESSAGES": "查看信息",
    "MAINMENU_COMMUNITY":     "社区",
    "MAINMENU_DLC":           "下载内容",
    "PAUSE_MENU_OPTIONS":     "选项",
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


def decode_dxt5_block(blk):
    """标准 DXT5 alpha 解码 (a0>a1: 8级ramp; a0<=a1: 4插值+code6=0/code7=255)"""
    a0, a1 = blk[0], blk[1]
    aidx = int.from_bytes(blk[2:8], 'little')
    if a0 > a1:
        ramp = [a0, a1,
                (6*a0 + 1*a1)//7, (5*a0 + 2*a1)//7, (4*a0 + 3*a1)//7,
                (3*a0 + 4*a1)//7, (2*a0 + 5*a1)//7, (1*a0 + 6*a1)//7]
    else:
        ramp = [a0, a1,
                (4*a0 + 1*a1)//5, (3*a0 + 2*a1)//5, (2*a0 + 3*a1)//5, (1*a0 + 4*a1)//5,
                0, 255]
    return [ramp[(aidx >> (3*i)) & 7] for i in range(16)]


def encode_dxt5_block(alphas):
    """标准 DXT5 alpha 编码: 端点取块内 max/min (保证 a0>a1 -> 全 8 级 ramp 覆盖 0..255)"""
    a0 = max(alphas); a1 = min(alphas)
    if a0 == a1:
        return bytes([a0, a0]) + b"\x00" * 6
    ramp = [a0, a1,
            (6*a0 + 1*a1)//7, (5*a0 + 2*a1)//7, (4*a0 + 3*a1)//7,
            (3*a0 + 4*a1)//7, (2*a0 + 5*a1)//7, (1*a0 + 6*a1)//7]
    bits = 0
    for i in range(16):
        best, bd = 0, 1 << 30
        for k in range(8):
            d = abs(alphas[i] - ramp[k])
            if d < bd:
                bd, best = d, k
        bits |= best << (3*i)
    return bytes([a0, a1]) + bits.to_bytes(6, 'little')


def patch_alpha_region(data, W, x0, y0, alpha_map, a_h, a_w):
    """把 a_h x a_w 的 alpha 块写入 DXT5 大图 (data 为 gvbm 裸 DXT5, 1B/px)"""
    for by in range(a_h):
        for bx in range(a_w):
            gx, gy = x0 + bx, y0 + by
            blk_off = (gy // 4) * (W // 4) + (gx // 4)
            in_blk = (gy % 4) * 4 + (gx % 4)
            b0 = blk_off * 8
            if bx % 4 == 0 and by % 4 == 0:
                pass
            # 读整块 alpha -> 改 -> 重写
            blk = bytearray(data[b0:b0+8])
            al = decode_dxt5_block(blk)
            al[in_blk] = int(alpha_map[by, bx])
            data[b0:b0+8] = encode_dxt5_block(al)


def measure_bbox(alpha, x, y, limit=400):
    """从 (x,y) 找字形 bbox (像素遍历上行/列)"""
    h, w = alpha.shape
    x0, x1, y0, y1 = x, x, y, y
    # 保守: 返回官方同策略的占位
    return x, y, x + limit, y + limit


def bake_font_v2(vstem, gstem, han_list, han_entries_out):
    """覆写空闲槽: count 不变; 烘字到图集底部空带; patch 槽记录 (FREE_IDX0+t)"""
    print(f"\n===== {vstem} (+{gstem}) [v2 overwrite] =====")
    vpath = os.path.join(M, vstem + ".vf3_pc")
    d = bytearray(open(vpath, 'rb').read())
    count = struct.unpack_from('<I', d, 8)[0]
    base = struct.unpack_from('<I', d, 12)[0]
    kern_cnt = struct.unpack_from('<I', d, 32)[0]
    L = struct.unpack_from('<H', d, 22)[0]
    assert base == 32 and count in (336, 1607), (vstem, base, count)
    met = (0xD0 + 6 * kern_cnt + 15) & ~15
    z1 = (met + 16 * count + 15) & ~15
    z2 = (z1 + 4 * count + 15) & ~15
    # 安全断言: FREE_IDX0+N-1 必须 < count (不越界)
    assert FREE_IDX0 + len(han_list) <= count, (count, FREE_IDX0 + len(han_list))
    print(f"  vf3: count={count} kern={kern_cnt} 行高L={L} met=0x{met:x} z1=0x{z1:x} z2=0x{z2:x}")

    # --- 图集 ---
    cv = os.path.join(M, gstem + ".cvbm_pc")
    gv = os.path.join(M, gstem + ".gvbm_pc")
    hdr, recs = vt.parse_cvbm(cv)
    r = recs[0]
    W, H = r['w'], r['h']
    gdata = bytearray(open(gv, 'rb').read())
    rgba, rw, rh = vt.render_mip(bytes(gdata), r, 0)
    alpha = np.frombuffer(rgba, dtype=np.uint8)[3::4].reshape(H, W)
    del rgba
    print(f"  图集 {W}x{H}")

    # --- 找底部最大空带 (整行 alpha<=8) ---
    col_empty = (alpha <= 8).all(axis=1)
    best_y0, best_h = None, 0
    y = H - 1
    while y >= 0:
        if col_empty[y]:
            y0 = y
            while y0 - 1 >= 0 and col_empty[y0 - 1]:
                y0 -= 1
            hh = y - y0 + 1
            if hh > best_h:
                best_h, best_y0 = hh, y0
            y = y0 - 1
        else:
            y -= 1
    print(f"  底部空带 y0={best_y0} 高={best_h}")
    if best_h < L or best_y0 is None:
        print("  ! 空带不足, 中止"); return

    # --- 烘字参数 (同 v1) ---
    P = round(L * 0.72)
    w4 = P + 16
    pad_top = round((L - P) * 0.6)
    per_row = max(1, (W - 8) // w4)
    print(f"  字形 {P}px adv/宽={w4} pad_top={pad_top} per_row={per_row}")
    glyphs = render_glyphs("".join(han_list), P)

    for t, ch in enumerate(han_list):
        col = t % per_row
        row = t // per_row
        y0 = best_y0 + row * L
        if y0 + L > H:
            print(f"  ! 图集垂直空间不足 t={t}"); break
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
        idx = FREE_IDX0 + t
        # --- 就地覆写槽记录: 度量 16B + 坐标 8B ---
        struct.pack_into('<II', d, met + 16 * idx, w4, w4)   # +0 advance +4 宽
        struct.pack_into('<I', d, met + 16 * idx + 8, 0)
        struct.pack_into('<h', d, met + 16 * idx + 12, -1)
        struct.pack_into('<I', d, z1 + 4 * idx, x0)
        struct.pack_into('<I', d, z2 + 4 * idx, y0)
        han_entries_out.append((ch, idx, x0, y0, w4))
    open(gv, 'wb').write(bytes(gdata))
    open(vpath, 'wb').write(bytes(d))      # count 不变, 就地改写
    print(f"  烘入并覆写 {len(han_entries_out)} 槽 @ idx {FREE_IDX0}..{FREE_IDX0+len(han_entries_out)-1}")
    for ch, idx, x0, y0, w4 in han_entries_out[:4]:
        print(f"    {ch} idx={idx} 槽码={idx+32} uv=({x0},{y0}) adv/宽={w4} 行高={L}")


def main():
    han_list = unique_han()
    print("唯一汉字 (%d): %s" % (len(han_list), "".join(han_list)))
    slot = {ch: 32 + FREE_IDX0 + t for t, ch in enumerate(han_list)}
    print("槽码:", {ch: s for ch, s in list(slot.items())[:6]}, "...")

    han_entries_out = []
    for vstem, gstem in FONTS:
        bake_font_v2(vstem, gstem, han_list, han_entries_out)

    # --- menu_us.le_strings 替换 ---
    import le_strings_repack as lr
    src = os.path.join(M, "menu_us.le_strings")
    pairs = {}
    for key, zh in MENU_ZH.items():
        h = crc_volition(key)
        tb = b"".join(struct.pack("<H", slot[ch]) for ch in zh) + b"\x00\x00"
        pairs[h] = tb
        print(f"  {key} hash={h:#x} -> {zh} 槽{list(slot[ch] for ch in zh)}")
    lr.repack(src, pairs, src)
    print("menu_us.le_strings 已回写 (槽码 325..348, idx 293..316)")

    # --- 槽表存档 ---
    import json
    with open(r"C:\Users\haojun0823\WorkBuddy\2026-09-04-02-19-04\menu_test_slots.json", "w", encoding="utf-8") as f:
        json.dump({"slot": {ch: slot[ch] for ch in han_list},
                   "FREE_IDX0": FREE_IDX0,
                   "menu_zh": MENU_ZH,
                   "note": "v2 overwrite mode: count unchanged, idx 293..316"}, f, ensure_ascii=False, indent=1)
    print("槽表已存 menu_test_slots.json")


if __name__ == "__main__":
    main()
