# -*- coding: utf-8 -*-
"""
方案 A 判别实验: 把 misc 中全部 10 个字体图集 gvbm (5 基名 x bdr/nobdr) 染红.
- 只改像素 RGB (保留 alpha), cvbm 壳与 mip 链布局不变, vf3 不动
- 游戏 UI 文字变红 => misc.vpp_pc 字体图集被加载且 RGB 参与调制 => 可行, 上 C
- 仍白字            => alpha-only 调制或非本文件, 需破坏 alpha 判别
用法: python tint_all.py <misc_dir>
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import volition_tex as V

STEMS = ['font_body', 'font_body_nobdr', 'font_header', 'font_header_nobdr',
         'font_header_pc', 'font_header_pc_nobdr', 'font_sk', 'font_sk_nobdr',
         'font_zh', 'font_zh_nobdr']


def tint_keep_alpha(rgba, color=(255, 0, 0), thresh=32):
    buf = bytearray(rgba)
    changed = 0
    for i in range(0, len(buf), 4):
        if buf[i + 3] >= thresh:
            buf[i] = color[0]
            buf[i + 1] = color[1]
            buf[i + 2] = color[2]
            changed += 1
    return buf, changed


def decode_level(gbytes, rec, level):
    """按 cvbm rec 从 gvbm 取第 level 级 mip 并解码(改自 render_mip 的 off 计算)"""
    fmt = rec['fmt']
    bpp = 1.0 if fmt in (V.FMT_DXT5, V.FMT_DXT3) else 0.5
    off = rec['off']
    for lv in range(level):
        lw, lh = rec['w'] >> lv, rec['h'] >> lv
        off += int(max(1, lw) * max(1, lh) * bpp)
    w, h = max(1, rec['w'] >> level), max(1, rec['h'] >> level)
    size = int(w * h * bpp)
    data = gbytes[off:off + size]
    if fmt == V.FMT_DXT5:
        return V.decode_dxt5(data, w, h), w, h
    if fmt == V.FMT_DXT1:
        return V.decode_dxt1(data, w, h), w, h
    raise ValueError('unsupported fmt %s' % fmt)


def level_size(rec, level):
    fmt = rec['fmt']
    bpp = 1.0 if fmt in (V.FMT_DXT5, V.FMT_DXT3) else 0.5
    w, h = max(1, rec['w'] >> level), max(1, rec['h'] >> level)
    return int(w * h * bpp)


def tint_font(misc_dir, stem, color=(255, 0, 0)):
    cvbm_p = os.path.join(misc_dir, stem + '.cvbm_pc')
    gvbm_p = os.path.join(misc_dir, stem + '.gvbm_pc')
    hdr, recs = V.parse_cvbm(cvbm_p)
    g = open(gvbm_p, 'rb').read()
    total_changed = 0
    out = bytearray()
    for r in recs:
        nmm = r['mips']
        for lv in range(nmm):
            rgba, w, h = decode_level(g, r, lv)
            buf, changed = tint_keep_alpha(rgba, color=color)
            enc = V.encode_dxt5(bytes(buf), w, h)
            assert len(enc) == level_size(r, lv), '%s mip%d size %d != %d' % (stem, lv, len(enc), level_size(r, lv))
            out += enc
            total_changed += changed
    assert len(out) == len(g), '%s total %d != %d' % (stem, len(out), len(g))
    open(gvbm_p, 'wb').write(bytes(out))
    print('  [%s] %d tex %dx%d mips=%d 染红 %d px, 大小不变 %dB' %
          (stem, len(recs), recs[0]['w'], recs[0]['h'], recs[0]['mips'], total_changed, len(out)))


if __name__ == '__main__':
    misc_dir = sys.argv[1]
    color = tuple(int(x) for x in sys.argv[2:5]) if len(sys.argv) >= 5 else (255, 0, 0)
    stems = sys.argv[5:] if len(sys.argv) > 5 else STEMS
    for s in stems:
        tint_font(misc_dir, s, color)
    print('DONE')
