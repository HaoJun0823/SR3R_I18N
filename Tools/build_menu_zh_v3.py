# -*- coding: utf-8 -*-
"""
SRTT3 主菜单汉化 - v3 (真·空洞槽覆写模式)
==========================================
背景 (2026-09-04 06:45 实证):
  - 官方对照: 不崩
  - v2 (覆写 idx293-316 + 烘字 + 文本)     : 崩 SRTTR.exe+0x3FD144 (sub_1403FD0F0 14槽type注册表)
  - 二分A (官方字库 + 仅文本引用槽码325-348): 崩 同点同寄存器 (r10=0xa8947c8c, rsi=0x2AB)
    -> 崩溃 = 文本层! 引擎 us 分支对"引用 idx293-316 (官方真实字形)"的文本走特殊路径,
       查询 type=0xa8947c8c 组件 -> 桶元素坏指针 -> 崩。与 vf3/gvbm 覆写无关。
  - 决定性: 官方 4 字体全空槽=0 (任何槽都有数据); 但"空洞槽"(记录指向图集空白区)大量存在
    -> v3 覆写 空洞 ∩ 全语言未引用 ∩ 槽码<256 的槽 (idx 95..118, 槽码 127..150):
       * 避开官方真实字形区 (293-316) -> 不触发特殊 resolve
       * 槽码 < 256 -> 避开高位特殊区假设
       * 官方从无文本引用 -> 不破坏任何语言显示

产物:
  - 4 组 .vf3_pc 就地覆写 idx 95..118 记录 (count 不变)
  - 4 张 *_nobdr.gvbm_pc 底部空带烘入 24 汉字 (标准 DXT5)
  - menu_us.le_strings 8 条 -> 槽码 127..150
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

FREE_IDX0 = 95          # 覆写起始 idx (空洞∩未引用∩槽码<256 实证候选区)
N_SLOTS = 24            # 95..118, 槽码 127..150

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
            b0 = blk_off * 8
            blk = bytearray(data[b0:b0+8])
            al = decode_dxt5_block(blk)
            al[in_blk] = int(alpha_map[by, bx])
            data[b0:b0+8] = encode_dxt5_block(al)


def validate_hole_slots(vstem, gstem, idxs):
    """断言 idxs 在官方字体中确为空洞槽(记录区域图集全空) 才允许覆写"""
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
    print(f"\n===== {vstem} (+{gstem}) [v3 hole-slot overwrite] =====")
    vpath = os.path.join(M, vstem + ".vf3_pc")
    count, L, d = validate_hole_slots(vstem, gstem, slot_idx)
    assert max(slot_idx) < count, (count, max(slot_idx))
    kern = struct.unpack_from('<I', d, 32)[0]
    met = (0xD0 + 6*kern + 15) & ~15
    z1 = (met + 16*count + 15) & ~15
    z2 = (z1 + 4*count + 15) & ~15
    d = bytearray(d)
    print(f"  vf3: count={count} L={L} 校验通过 (idx {slot_idx[0]}..{slot_idx[-1]} 全空洞)")

    # 图集
    cv = os.path.join(M, gstem + ".cvbm_pc")
    gv = os.path.join(M, gstem + ".gvbm_pc")
    hdr, recs = vt.parse_cvbm(cv)
    r = recs[0]
    W, H = r['w'], r['h']
    gdata = bytearray(open(gv, 'rb').read())
    rgba, rw, rh = vt.render_mip(bytes(gdata), r, 0)
    alpha = np.frombuffer(rgba, dtype=np.uint8)[3::4].reshape(H, W)
    del rgba

    # 底部空带
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
        struct.pack_into('<II', d, met + 16*idx, w4, w4)   # adv / 宽
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
    slot_idx = list(range(FREE_IDX0, FREE_IDX0 + N_SLOTS))
    slot = {ch: idx + 32 for ch, idx in zip(han_list, slot_idx)}
    print("唯一汉字 (%d): %s" % (len(han_list), "".join(han_list)))
    print("覆写 idx %d..%d, 槽码 %d..%d" % (slot_idx[0], slot_idx[-1],
                                            slot_idx[0]+32, slot_idx[-1]+32))
    print("槽码映射:", {ch: s for ch, s in list(slot.items())[:8]}, "...")

    # 1) 恢复官方基准 (robocopy 镜像, 避免 rmtree 回收站限制)
    print("\n恢复 unpack/misc <- cur_misc (robocopy /MIR) ...")
    subprocess.run(["robocopy", CUR, M, "/MIR", "/NFL", "/NDL", "/NJH", "/NJS"],
                   check=False)
    n = len(os.listdir(M))
    assert n == 375, ("恢复失败, 文件数=%d" % n)
    print("  已恢复 %d 文件" % n)

    # 2) 烘字 + 覆写
    for vstem, gstem in FONTS:
        bake_font_v3(vstem, gstem, han_list, slot_idx)

    # 3) menu_us 文本
    src = os.path.join(M, "menu_us.le_strings")
    pairs = {}
    for key, zh in MENU_ZH.items():
        h = crc_volition(key)
        tb = b"".join(struct.pack("<H", slot[ch]) for ch in zh) + b"\x00\x00"
        pairs[h] = tb
        print(f"  {key} -> {zh} 槽{[slot[c] for c in zh]}")
    lr.repack(src, pairs, src)
    print("menu_us.le_strings 已回写")

    # 4) 槽表存档
    import json
    with open(os.path.join(ROOT, "menu_test_slots_v3.json"), "w", encoding="utf-8") as f:
        json.dump({"slot": {ch: slot[ch] for ch in han_list},
                   "slot_idx": slot_idx, "menu_zh": MENU_ZH,
                   "note": "v3 hole-slot overwrite: idx 95..118 (空洞∩未引用∩槽码<256)"},
                  f, ensure_ascii=False, indent=1)

    # 5) 打包 + 部署
    out_tmp = os.path.join(ROOT, "misc_v3.vpp_pc")
    print("\n打包 ->", out_tmp)
    py = r"C:\Users\haojun0823\.workbuddy\binaries\python\envs\default\Scripts\python.exe"
    subprocess.run([py, os.path.join(ROOT, "vpp_pack.py"), ORIG, M, out_tmp], check=True)
    bak = os.path.join(CACHE, "misc.vpp_pc.bisectA")
    if os.path.exists(os.path.join(CACHE, "misc.vpp_pc")) and not os.path.exists(bak):
        shutil.copy2(os.path.join(CACHE, "misc.vpp_pc"), bak)
        print("二分A版已留档 -> misc.vpp_pc.bisectA")
    shutil.copy2(out_tmp, os.path.join(CACHE, "misc.vpp_pc"))
    print("已部署 cache/misc.vpp_pc (v3 空洞槽版)")
    print("槽表: menu_test_slots_v3.json")


if __name__ == "__main__":
    main()
