"""
字体形状测试 v2: 用正确 build_cvbm 生成 shape-only 测试字体.
对每套字体把字形 cell 区域填为实心白色方块, DXT5 编码, 写 cvbm/gvbm/vf3.
mips 强制为 1 (只放 mip0), size 字段写 mip0 字节数.

用法: python font_shape_test.py <misc_dir> <out_dir> [font_stem ...]
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import volition_tex as V


def scan_grid(rgba, w, h, min_gap=3, alpha_th=8, sample_step=2):
    """从 RGBA 推断 cell 网格 (列中心, 行中心, 估算 cell 宽/高)"""
    def blank_axis(axis_is_y, th):
        ext = h if axis_is_y else w
        per = w if axis_is_y else h
        blanks = []
        for i in range(ext):
            has = False
            for j in range(0, per, sample_step):
                x = j if axis_is_y else i
                y = i if axis_is_y else j
                a = rgba[(y*w + x)*4 + 3]
                if a >= th:
                    has = True; break
            blanks.append(not has)
        return blanks
    def cluster(blank, min_len):
        gaps = []; in_g = False; s = 0
        for i, b in enumerate(blank):
            if b and not in_g:
                in_g = True; s = i
            elif (not b) and in_g:
                in_g = False
                if i - s >= min_len:
                    gaps.append((s, i))
        if in_g and len(blank) - s >= min_len:
            gaps.append((s, len(blank)))
        return gaps
    cgap = cluster(blank_axis(False, alpha_th), min_gap)
    rgap = cluster(blank_axis(True, alpha_th), min_gap)
    col_centers = []
    for k in range(len(cgap)+1):
        a = cgap[k-1][1] if k > 0 else 0
        b = cgap[k][0] if k < len(cgap) else w
        col_centers.append((a+b)//2)
    row_centers = []
    for k in range(len(rgap)+1):
        a = rgap[k-1][1] if k > 0 else 0
        b = rgap[k][0] if k < len(rgap) else h
        row_centers.append((a+b)//2)
    def med(centers, total):
        if len(centers) < 2: return total // max(1, len(centers))
        s = sorted(centers[i+1]-centers[i] for i in range(len(centers)-1))
        return s[len(s)//2]
    return col_centers, row_centers, med(col_centers, w), med(row_centers, h)


def fill_solid(rgba, w, h, cols, rows, cw, ch, frac_w=0.65, frac_h=0.55):
    """在每个 cell 中央画实心白色方块 (alpha=255 RGB=255)"""
    bw = max(4, int(cw * frac_w))
    bh = max(4, int(ch * frac_h))
    for cy in rows:
        for cx in cols:
            x0 = max(0, cx - bw//2); x1 = min(w, cx + bw//2)
            y0 = max(0, cy - bh//2); y1 = min(h, cy + bh//2)
            for y in range(y0, y1):
                base = (y*w + x0) * 4
                for x in range(x1 - x0):
                    o = base + x*4
                    rgba[o] = 255; rgba[o+1] = 255; rgba[o+2] = 255; rgba[o+3] = 255
    return rgba


def process(misc_dir, stem, out_dir):
    cvbm_p = os.path.join(misc_dir, stem + '.cvbm_pc')
    gvbm_p = os.path.join(misc_dir, stem + '.gvbm_pc')
    vf3_p  = os.path.join(misc_dir, stem + '.vf3_pc')

    hdr, recs = V.parse_cvbm(cvbm_p)
    g = open(gvbm_p, 'rb').read()
    rgba, w, h = V.render_mip(g, recs[0], 0)
    print(f'  {stem}: {w}x{h} mip0', end=' ')
    cols, rows, cw, ch = scan_grid(rgba, w, h)
    print(f'grid {len(cols)}x{len(rows)} cell~{cw}x{ch}')

    buf = bytearray(rgba)
    fill_solid(buf, w, h, cols, rows, cw, ch)
    enc = V.encode_dxt5(bytes(buf), w, h)
    print(f'    encoded mip0 {len(enc)}B')

    os.makedirs(out_dir, exist_ok=True)
    open(os.path.join(out_dir, stem + '.gvbm_pc'), 'wb').write(enc)

    # cvbm: 强制 mips=1, size=mip0 字节数
    rec_out = [dict(recs[0])]
    rec_out[0]['mips'] = 1
    rec_out[0]['size'] = len(enc)
    V.build_cvbm(cvbm_p, rec_out, os.path.join(out_dir, stem + '.cvbm_pc'))

    # vf3 原样
    open(os.path.join(out_dir, stem + '.vf3_pc'), 'wb').write(open(vf3_p, 'rb').read())
    print(f'    wrote 3 files in {out_dir}')


if __name__ == '__main__':
    misc_dir, out_dir = sys.argv[1], sys.argv[2]
    stems = sys.argv[3:] if len(sys.argv) > 3 else [
        'font_body', 'font_header', 'font_header_pc', 'font_sk', 'font_zh']
    for s in stems:
        process(misc_dir, s, out_dir)
    print('DONE')