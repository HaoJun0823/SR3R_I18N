"""
保留 alpha 形状、把字形 RGB 染红的字体重打包(验证 vpp 重打包链路).
若游戏 UI 变红字 => 新 vpp 被加载且 RGB 生效;
若白字不变     => vpp 加载成功但引擎 alpha-only 调制;
若出现方块/缺字 => 资源格式问题.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import volition_tex as V


def tint_keep_alpha(rgba, color=(255, 0, 0), thresh=40):
    """字形像素(alpha>=thresh)RGB 改为 color, alpha 不动."""
    buf = bytearray(rgba)
    changed = 0
    for i in range(0, len(buf), 4):
        if buf[i + 3] >= thresh:
            buf[i] = color[0]
            buf[i + 1] = color[1]
            buf[i + 2] = color[2]
            changed += 1
    return buf, changed


def process(misc_dir, stem, out_dir, color):
    cvbm_p = os.path.join(misc_dir, stem + '.cvbm_pc')
    gvbm_p = os.path.join(misc_dir, stem + '.gvbm_pc')
    vf3_p = os.path.join(misc_dir, stem + '.vf3_pc')
    hdr, recs = V.parse_cvbm(cvbm_p)
    g = open(gvbm_p, 'rb').read()
    rgba, w, h = V.render_mip(g, recs[0], 0)
    buf, changed = tint_keep_alpha(bytearray(rgba), color=color)
    enc = V.encode_dxt5(bytes(buf), w, h)
    os.makedirs(out_dir, exist_ok=True)
    open(os.path.join(out_dir, stem + '.gvbm_pc'), 'wb').write(enc)
    rec_out = [dict(recs[0])]
    rec_out[0]['mips'] = 1
    rec_out[0]['size'] = len(enc)
    V.build_cvbm(cvbm_p, rec_out, os.path.join(out_dir, stem + '.cvbm_pc'))
    open(os.path.join(out_dir, stem + '.vf3_pc'), 'wb').write(open(vf3_p, 'rb').read())
    print('  %s: %dx%d 染红 %d px -> %s' % (stem, w, h, changed, out_dir))


if __name__ == '__main__':
    misc_dir, out_dir = sys.argv[1], sys.argv[2]
    color = tuple(int(x) for x in sys.argv[3:6]) if len(sys.argv) >= 6 else (255, 0, 0)
    stems = sys.argv[6:] if len(sys.argv) > 6 else [
        'font_body', 'font_header', 'font_header_pc', 'font_sk', 'font_zh']
    for s in stems:
        process(misc_dir, s, out_dir, color)
    print('DONE')
