# -*- coding: utf-8 -*-
"""端到端验证: 把 font_body 图集文字像素染红 -> 重编码 -> 输出可部署三件套
若游戏 UI/字幕文字变红 => loose 文件加载 + DXT5 重编码链路全部正确
"""
import os, sys, shutil
import volition_tex as V


def main():
    if len(sys.argv) < 4:
        print('usage: font_tint.py <misc_dir> <stem> <out_dir>')
        sys.exit(1)
    base, stem, out_dir = sys.argv[1], sys.argv[2], sys.argv[3]
    cv = os.path.join(base, stem + '.cvbm_pc')
    gv = os.path.join(base, stem + '.gvbm_pc')
    vf = os.path.join(base, stem + '.vf3_pc')
    os.makedirs(out_dir, exist_ok=True)

    hdr, recs = V.parse_cvbm(cv)
    g = open(gv, 'rb').read()
    r = recs[0]
    print(f'== {stem}: {r["name"]} {r["w"]}x{r["h"]} fmt={r["fmt"]} mips={r["mips"]}')

    # 重算每级 mip 在 gvbm 中的 offset（与容器一致布局）
    sizes = []
    for lv in range(r['mips']):
        lw, lh = r['w'] >> lv, r['h'] >> lv
        sizes.append(max(1, lw) * max(1, lh))  # DXT5 1B/px
    offs = []
    o = 0
    for s in sizes:
        offs.append(o)
        o += s

    new_g = bytearray()
    total_px = 0
    for lv in range(r['mips']):
        lw, lh = r['w'] >> lv, r['h'] >> lv
        if lw < 1: lw = 1
        if lh < 1: lh = 1
        data = g[offs[lv]:offs[lv] + sizes[lv]]
        rgba = V.decode_dxt5(data, lw, lh)
        # 染红: alpha>=96 的文字像素 RGB -> (255, 0, 0)  [rgba 顺序]
        changed = 0
        buf = bytearray(rgba)
        for i in range(0, len(buf), 4):
            if buf[i+3] >= 96:
                buf[i] = 255    # R
                buf[i+1] = 0    # G
                buf[i+2] = 0    # B
                changed += 1
        total_px += changed
        enc = V.encode_dxt5(bytes(buf), lw, lh)
        new_g += enc
    print(f'染红像素: {total_px}')

    # 写 cvbm + gvbm
    new_g_path = os.path.join(out_dir, stem + '.gvbm_pc')
    open(new_g_path, 'wb').write(bytes(new_g))
    new_recs = [dict(r)]
    new_recs[0]['off'] = 0
    new_recs[0]['size'] = len(new_g)
    new_cv_path = os.path.join(out_dir, stem + '.cvbm_pc')
    V.build_cvbm(hdr, new_recs, new_cv_path)
    new_vf_path = os.path.join(out_dir, stem + '.vf3_pc')
    shutil.copy(vf, new_vf_path)

    print(f'输出:')
    for p in (new_cv_path, new_g_path, new_vf_path):
        print(f'  {p}  {os.path.getsize(p)} B')


if __name__ == '__main__':
    main()