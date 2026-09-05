# -*- coding: utf-8 -*-
"""
SRTT3 主菜单汉化测试 - 统一构建器
==================================
路线: 文本 u16 = 槽码 (base32); 引擎 idx = 槽码-32 查字体 vf3 表。
主菜单候选字体 = font_body/header/header_pc/sk (全部指向 *_nobdr 图集, 已确认)。
因 font_sk 官方表已 1607 项(idx 336-365 均被占用), 新字统一落在 idx 1700 起,
4 张表 count 全部扩到 1700+N, 空洞项置 0。
产物:
  - 4 组 .vf3_pc 重建 (追加 N 项 + 空洞)
  - 4 张 *_nobdr.gvbm_pc 烘入汉字 (DXT5 字节级 patch)
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
NEW_IDX0 = 1700          # 统一新字起始 idx (大于全部官方表 count)

# 字体组: (vf3 stem, gvbm stem)
FONTS = [("font_body", "font_body_nobdr"),
         ("font_header", "font_header_nobdr"),
         ("font_header_pc", "font_header_pc_nobdr"),
         ("font_sk", "font_sk_nobdr")]

# menu_us 主菜单可见条目翻译
MENU_ZH = {
    "MAINMENU_SINGLEPLAYER":  "单人游戏",
    "MAINMENU_COOP":          "合作模式",
    "MAINMENU_CAMPAIGN":      "主线战役",
    "MAINMENU_COOP_OPTION":   "合作战役",
    "MAINMENU_CHECK_MESSAGES":"查看信息",
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
    """FreeType 渲染 -> {ch: (alpha h,w)}"""
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


# ---------- DXT5 块编解码 (RGB 恒白, alpha 传递) ----------
def decode_dxt5_block(blk):
    a0, a1 = blk[0], blk[1]
    aidx = int.from_bytes(blk[2:8], 'little')
    al = [a0, a1, (6*a0+1*a1)//7, (5*a0+2*a1)//7, (4*a0+3*a1)//7,
          (3*a0+4*a1)//7, (2*a0+5*a1)//7, (1*a0+6*a1)//7]
    if a0 > a1:
        al[6] = 0; al[7] = 255
    out = []
    for i in range(16):
        out.append(al[(aidx >> (3*i)) & 7])
    return out   # 16 alpha


def encode_dxt5_block(alphas):
    """16 alpha -> 16B DXT5 (RGB 全白 c0=0xFFFF)"""
    out = bytearray(16)
    out[0] = 255; out[1] = 0
    order = [255, 0, 204, 153, 102, 51, 0, 255]
    bits = 0
    for i in range(16):
        av = alphas[i]
        best, bi = 999, 0
        for k, lv in enumerate(order):
            d = abs(av - lv)
            if d < best:
                best, bi = d, k
        bits |= bi << (3 * i)
    out[2] = bits & 0xFF; out[3] = (bits >> 8) & 0xFF
    out[4] = (bits >> 16) & 0xFF; out[5] = (bits >> 24) & 0xFF
    out[6] = 0xFF; out[7] = 0xFF; out[8] = 0xFF; out[9] = 0xFF
    out[10] = 0; out[11] = 0; out[12] = 0; out[13] = 0
    return bytes(out)


def patch_alpha_region(data, W, x0, y0, alpha_map, a_h, a_w):
    """把 alpha_map (a_h×a_w, 0..255) 烘到图集 (x0,y0) 起。DXT5 块级。"""
    for by in range(0, a_h, 4):
        for bx in range(0, a_w, 4):
            gx, gy = x0 + bx, y0 + by
            blk_off = (gy // 4) * (W // 4) * 16 + (gx // 4) * 16
            if blk_off + 16 > len(data):
                continue
            blk = bytes(data[blk_off:blk_off + 16])
            alphas = decode_dxt5_block(blk)
            for j in range(16):
                py = gy + j // 4; px = gx + j % 4
                ly, lx = py - y0, px - x0
                if 0 <= ly < a_h and 0 <= lx < a_w:
                    alphas[j] = int(alpha_map[ly, lx])
            nb = encode_dxt5_block(alphas)
            data[blk_off:blk_off + 16] = nb


def measure_bbox(alpha, x, y, limit=400):
    """在 (x,y) 起 limit×limit 窗口内找 alpha>40 的 bbox (字形区)。"""
    h, w = alpha.shape
    x1 = min(x + limit, w); y1 = min(y + limit, h)
    win = alpha[y:y1, x:x1]
    ys, xs = np.where(win > 40)
    if not len(xs):
        return None
    return int(xs.min()), int(xs.max()), int(ys.min()), int(ys.max())


def bake_font(vstem, gstem, han_list, han_entries_out):
    """对单个字体: 找空带 -> 烘字形(UV高=行高L) -> 重建 vf3(追加 idx NEW_IDX0+)"""
    print(f"\n===== {vstem} (+{gstem}) =====")
    vpath = os.path.join(M, vstem + ".vf3_pc")
    d = bytearray(open(vpath, 'rb').read())
    count = struct.unpack_from('<I', d, 8)[0]
    base = struct.unpack_from('<I', d, 12)[0]
    kern_cnt = struct.unpack_from('<I', d, 32)[0]
    assert base == 32 and count in (336, 1607), (vstem, base, count)
    L = struct.unpack_from('<H', d, 22)[0]      # 行高 = UV 矩形高
    met_old = (0xD0 + 6 * kern_cnt + 15) & ~15
    z1_old = (met_old + 16 * count + 15) & ~15
    z2_old = (z1_old + 4 * count + 15) & ~15
    print(f"  vf3: count={count} kern={kern_cnt} 行高(L)=u16@22={L} len={len(d):#x}")

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
    print(f"  图集 {W}x{H} 底部空带 y0={best_y0} 高={best_h}")
    if best_h < L or best_y0 is None:
        print("  ! 空带不足"); return

    # --- 烘字参数 ---
    P = round(L * 0.72)          # 字形渲染像素高 (~官方字形/行高比)
    w4 = P + 16                  # 字形宽字段 = advance (UV 内左右留 8px)
    pad_top = round((L - P) * 0.6)
    per_row = max(1, (W - 8) // w4)
    print(f"  字形像素 {P}px, 宽/advance={w4}, pad_top={pad_top}, per_row={per_row}")
    glyphs = render_glyphs("".join(han_list), P)
    han_entries = []
    n_baked = 0
    for t, ch in enumerate(han_list):
        col = t % per_row
        row = t // per_row
        y0 = best_y0 + row * L
        if y0 + L > H:
            print(f"  ! 图集垂直空间不足 t={t}/{len(han_list)} 行={row}"); break
        x0 = 8 + col * w4
        buf = glyphs[ch]
        gh, gw = buf.shape          # 渲染位图 (≈P×P)
        left_pad = (w4 - gw) // 2
        cell_map = np.zeros((L, w4), np.uint8)
        sx = max(0, -left_pad); sy = max(0, -pad_top)
        ex = min(gw, w4 - left_pad); ey = min(gh, L - pad_top)
        if ex > sx and ey > sy:
            cell_map[pad_top + sy:pad_top + ey, left_pad + sx:left_pad + ex] = buf[sy:ey, sx:ex]
        patch_alpha_region(gdata, W, x0, y0, cell_map, L, w4)
        idx = NEW_IDX0 + t
        han_entries.append((idx, ch, x0, y0, w4, L))
        n_baked += 1
    open(gv, 'wb').write(bytes(gdata))
    print(f"  烘入 {n_baked} 字 @ idx {NEW_IDX0}..{NEW_IDX0+n_baked-1}")

    # --- vf3 重建 (追加到 NEW_COUNT, 空洞清零) ---
    new_count = NEW_IDX0 + len(han_list)
    met_new = met_old
    z1_new = (met_new + 16 * new_count + 15) & ~15
    z2_new = (z1_new + 4 * new_count + 15) & ~15
    nd = bytearray(z2_new + 4 * new_count)
    nd[:met_old] = d[:met_old]
    nd[met_new:met_new + 16 * count] = d[met_old:met_old + 16 * count]
    for i in range(count):
        struct.pack_into('<I', nd, z1_new + 4 * i, struct.unpack_from('<I', d, z1_old + 4 * i)[0])
        struct.pack_into('<I', nd, z2_new + 4 * i, struct.unpack_from('<I', d, z2_old + 4 * i)[0])
    for idx, ch, x0, y0, adv, _L in han_entries:
        struct.pack_into('<II', nd, met_new + 16 * idx, adv, adv)   # +0 advance +4 宽
        struct.pack_into('<I', nd, met_new + 16 * idx + 8, 0)
        struct.pack_into('<h', nd, met_new + 16 * idx + 12, -1)
        struct.pack_into('<I', nd, z1_new + 4 * idx, x0)
        struct.pack_into('<I', nd, z2_new + 4 * idx, y0)
    struct.pack_into('<I', nd, 8, new_count)
    open(vpath, 'wb').write(bytes(nd))
    print(f"  vf3 重建: count {count}->{new_count} 文件 {len(d)}B->{len(nd)}B")
    for idx, ch, x0, y0, adv, _L in han_entries[:4]:
        print(f"    {ch} idx={idx} uv=({x0},{y0}) adv/宽={adv} 行高={_L}")
        han_entries_out.append((ch, idx, x0, y0))


def main():
    han_list = unique_han()
    print("唯一汉字 (%d): %s" % (len(han_list), "".join(han_list)))
    # 槽: ch -> 槽码
    slot = {ch: 32 + NEW_IDX0 + t for t, ch in enumerate(han_list)}
    print("槽位:", {ch: s for ch, s in list(slot.items())[:6]}, "...")

    han_entries_out = []
    for vstem, gstem in FONTS:
        bake_font(vstem, gstem, han_list, han_entries_out)

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
    print("menu_us.le_strings 已回写")

    # --- 槽表存档 ---
    with open(r"C:\Users\haojun0823\WorkBuddy\2026-09-04-02-19-04\menu_test_slots.json", "w", encoding="utf-8") as f:
        import json
        json.dump({"slot": {ch: slot[ch] for ch in han_list},
                   "NEW_IDX0": NEW_IDX0,
                   "menu_zh": MENU_ZH}, f, ensure_ascii=False, indent=1)
    print("槽表已存 menu_test_slots.json")


if __name__ == "__main__":
    main()
