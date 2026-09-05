"""
全图集染红测试 v2: 用修复后的 build_cvbm 把每套字体整个图集 fill 成纯红.
之前染红失败可能是 cvbm 字段错位导致 game fallback 到 cache 原版.
若文字依然不变 → 真的"重着色"(alpha-only 调制) → 改字体只需保证 alpha 形状对即可.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import volition_tex as V


def tint_solid(rgba, w, h, color=(255, 0, 0), alpha=255):
    """整个图集 fill 为单一颜色 (alpha=255 everywhere)"""
    buf = bytearray(rgba)
    for i in range(0, len(buf), 4):
        buf[i] = color[0]; buf[i+1] = color[1]; buf[i+2] = color[2]; buf[i+3] = alpha
    return buf


def process(misc_dir, stem, out_dir, color):
    cvbm_p = os.path.join(misc_dir, stem + '.cvbm_pc')
    gvbm_p = os.path.join(misc_dir, stem + '.gvbm_pc')
    vf3_p  = os.path.join(misc_dir, stem + '.vf3_pc')
    hdr, recs = V.parse_cvbm(cvbm_p)
    g = open(gvbm_p, 'rb').read()
    rgba, w, h = V.render_mip(g, recs[0], 0)
    buf = tint_solid(bytearray(rgba), w, h, color=color, alpha=255)
    enc = V.encode_dxt5(bytes(buf), w, h)
    os.makedirs(out_dir, exist_ok=True)
    open(os.path.join(out_dir, stem + '.gvbm_pc'), 'wb').write(enc)
    rec_out = [dict(recs[0])]
    rec_out[0]['mips'] = 1
    rec_out[0]['size'] = len(enc)
    V.build_cvbm(cvbm_p, rec_out, os.path.join(out_dir, stem + '.cvbm_pc'))
    open(os.path.join(out_dir, stem + '.vf3_pc'), 'wb').write(open(vf3_p, 'rb').read())
    print(f'  {stem}: fill RGB{color} mip0 {len(enc)}B -> {out_dir}')


if __name__ == '__main__':
    misc_dir, out_dir = sys.argv[1], sys.argv[2]
    color = tuple(int(x) for x in sys.argv[3:6]) if len(sys.argv) >= 6 else (255, 0, 0)
    stems = sys.argv[6:] if len(sys.argv) > 6 else [
        'font_body', 'font_header', 'font_header_pc', 'font_sk', 'font_zh']
    for s in stems:
        process(misc_dir, s, out_dir, color)
    print('DONE')