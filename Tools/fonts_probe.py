# -*- coding: utf-8 -*-
"""思源黑体选型侦查: 标准 SC vs HW-SC vs 现行 msyh
对比同一批汉字的位图字形宽度/advance/视觉, 输出拼图 PNG + 数值表
"""
import os
import numpy as np
import freetype

PATHS = {
    "SC_std":  r"G:\Archives\Fonts\02_SourceHanSans-VF\Variable\TTF\SourceHanSansSC-VF.ttf",
    "HW_SC":   r"G:\Archives\Fonts\02_SourceHanSans-VF\Variable\TTF\HW\SourceHanSansHWSC-VF.ttf",
    "msyh_now": r"C:\Windows\Fonts\msyh.ttc",
}
TEXT = "主线战役合作模式单人游戏选项社区下载内容查看信息"

def render(face, ch, px):
    face.set_pixel_sizes(0, px)
    face.load_char(ch, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_NORMAL)
    bmp = face.glyph.bitmap
    raw = bmp.buffer
    if not isinstance(raw, (bytes, bytearray)):
        raw = bytes(raw)
    buf = np.frombuffer(raw, dtype=np.uint8).reshape(bmp.rows, bmp.width) if raw else np.zeros((bmp.rows, bmp.width), np.uint8)
    adv = face.glyph.advance.x / 64.0
    return buf, adv, (bmp.width, bmp.rows)

px = 48
scale = 3
rows = []   # each element: (label, [glyph arrays], advances)
stats = {}
for key, p in PATHS.items():
    face = freetype.Face(p)
    # 尝试设字重 (VF wght 轴) - 失败则保持默认实例
    wt = "default-instance"
    for w in (500, 400):
        try:
            face.set_var_design_coordinates([w])
            wt = f"wght={w}"
            break
        except Exception as e:
            wt = f"set-fail:{type(e).__name__}:{e}"
    fam = face.family_name or "?"
    print(f"== {key}  ({os.path.basename(p)})  family={fam} style={face.style_name} {wt}")

    advs = []
    gls = []
    for ch in TEXT:
        buf, adv, sz = render(face, ch, px)
        advs.append(adv)
        gls.append(buf)
    stats[key] = {"adv_min": round(min(advs),1), "adv_max": round(max(advs),1),
                  "bmp_max_w": max(g.shape[1] for g in gls),
                  "bmp_max_h": max(g.shape[0] for g in gls)}
    print("   advance: min=%.1f max=%.1f px; bitmap max w=%d h=%d" % (
        min(advs), max(advs), stats[key]["bmp_max_w"], stats[key]["bmp_max_h"]))
    rows.append((key, gls, advs))

# 拼图: 每字体一行; 每字一格 (以放大后尺寸为准)
cell_w = (int(max(max(a) for _,_,a in rows)) + 10) * scale
row_h = (px + 6) * scale
canvas_w = cell_w * len(TEXT)
canvas_h = row_h * len(rows) + 24
img = np.full((canvas_h, canvas_w), 255, np.uint8)
y = 8
for label, gls, advs in rows:
    for i, g in enumerate(gls):
        x = 8 + i * cell_w
        gh, gw = g.shape
        if gh == 0 or gw == 0:
            continue
        # 放大 scale 倍
        big = np.kron(g, np.ones((scale, scale), np.uint8))
        bh, bw = big.shape
        # 顶部对齐 + 底部基线留白
        oy = y + 4
        ox = x + 4
        if bh and bw:
            img[oy:oy+bh, ox:ox+bw] = np.where(big > 120, 255, big)  # 黑字白底
    y += row_h

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fonts_probe_compare.png")
from PIL import Image
im = Image.fromarray(img, "L")
im.save(out)
print("\nsaved:", out, img.shape)
print("\n统计表:")
for k, s in stats.items():
    print(" ", k, s)
