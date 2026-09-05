# -*- coding: utf-8 -*-
"""字体三件套 round-trip PoC:
解 gvbm → RGBA → 重新编码为 DXT5 → 写出新 gvbm → 写新 cvbm → 字节级自验证
vf3 在 PoC 阶段原样保留(不修改字形度量表)。
"""
import hashlib, os, sys, struct
import volition_tex as V


def md5(p):
    return hashlib.md5(open(p, 'rb').read()).hexdigest()


def mip_sizes(w, h, mips, bpp=1.0):
    """返回每级 mip 的字节数 (DXT)"""
    sizes = []
    for lv in range(mips):
        lw, lh = w >> lv, h >> lv
        if lw < 1: lw = 1
        if lh < 1: lh = 1
        sizes.append(int(lw * lh * bpp))
    return sizes


def main():
    if len(sys.argv) < 4:
        print('usage: font_roundtrip.py <misc_dir> <stem> <out_dir>')
        sys.exit(1)
    base, stem, out_dir = sys.argv[1], sys.argv[2], sys.argv[3]
    cv = os.path.join(base, stem + '.cvbm_pc')
    gv = os.path.join(base, stem + '.gvbm_pc')
    vf = os.path.join(base, stem + '.vf3_pc')
    os.makedirs(out_dir, exist_ok=True)

    hdr, recs = V.parse_cvbm(cv)
    g = open(gv, 'rb').read()
    print(f'== {stem}  v={hdr["version"]}  {len(recs)}  rec(s)')

    new_gvbm = bytearray()
    fmt = recs[0]['fmt']
    if fmt == V.FMT_DXT5 or fmt == V.FMT_DXT3:
        bpp = 1.0
    else:
        bpp = 0.5
    sizes = mip_sizes(recs[0]['w'], recs[0]['h'], recs[0]['mips'], bpp)

    # 计算每 mip 在 gvbm 中的起始偏移(原数据与新数据都按相同布局)
    mip_off = []
    o = 0
    for s in sizes:
        mip_off.append(o)
        o += s

    # 对每级 mip: 解码 → 编码
    decoded_per_mip = []
    for lv in range(recs[0]['mips']):
        lw = recs[0]['w'] >> lv
        lh = recs[0]['h'] >> lv
        if lw < 1: lw = 1
        if lh < 1: lh = 1
        data = g[mip_off[lv]:mip_off[lv] + sizes[lv]]
        if fmt == V.FMT_DXT5:
            rgba = V.decode_dxt5(data, lw, lh)
        else:
            rgba = V.decode_dxt1(data, lw, lh)
        decoded_per_mip.append((lw, lh, rgba))

    # 编码
    encoded_per_mip = []
    for lw, lh, rgba in decoded_per_mip:
        if fmt == V.FMT_DXT5:
            encoded_per_mip.append(V.encode_dxt5(rgba, lw, lh))
        else:
            raise NotImplementedError('dxt1 encode todo')

    # 写新 gvbm
    for chunk in encoded_per_mip:
        new_gvbm += chunk
    new_gv_path = os.path.join(out_dir, stem + '.gvbm_pc')
    open(new_gv_path, 'wb').write(bytes(new_gvbm))

    # 写新 cvbm (用新的 size)
    new_size = sum(len(c) for c in encoded_per_mip)
    new_recs = [dict(recs[0])]
    new_recs[0]['off'] = 0
    new_recs[0]['size'] = new_size
    new_cv_path = os.path.join(out_dir, stem + '.cvbm_pc')
    V.build_cvbm(hdr, new_recs, new_cv_path)

    # 复制 vf3 (本 PoC 不动)
    new_vf_path = os.path.join(out_dir, stem + '.vf3_pc')
    open(new_vf_path, 'wb').write(open(vf, 'rb').read())

    # 像素差统计
    print('--- 像素差 (BC 量化误差) ---')
    max_err = 0
    for lv, (lw, lh, rgba0) in enumerate(decoded_per_mip):
        if fmt == V.FMT_DXT5:
            rgba1 = V.decode_dxt5(encoded_per_mip[lv], lw, lh)
        else:
            rgba1 = V.decode_dxt1(encoded_per_mip[lv], lw, lh)
        # 4 通道每像素差
        diff = 0; cnt = 0; maxd = 0
        for i in range(0, len(rgba0), 4):
            d = max(abs(rgba0[i+k] - rgba1[i+k]) for k in range(4))
            diff += d; cnt += 1
            if d > maxd: maxd = d
        avg = diff / cnt if cnt else 0
        print(f'   mip{lv} {lw}x{lh}: avg={avg:.3f} max={maxd}')
        if maxd > max_err: max_err = maxd

    print(f'--- 输出文件 MD5 ---')
    print(f'   {stem}.cvbm_pc: orig={md5(cv)} new={md5(new_cv_path)}')
    print(f'   {stem}.gvbm_pc: orig={md5(gv)} new={md5(new_gv_path)}')
    print(f'   {stem}.vf3_pc : orig={md5(vf)} new={md5(new_vf_path)} (unchanged)')

    # 再解析新 cvbm 验证回环
    nhdr, nrecs = V.parse_cvbm(new_cv_path)
    ng = open(new_gv_path, 'rb').read()
    print(f'--- 自验证: 解析新 cvbm ---')
    print(f'   {nrecs[0]["name"]!r} {nrecs[0]["w"]}x{nrecs[0]["h"]} '
          f'fmt={nrecs[0]["fmt"]} mips={nrecs[0]["mips"]} size={nrecs[0]["size"]} '
          f'gvbm_len={len(ng)} -> {"OK" if nrecs[0]["size"]==len(ng) else "MISMATCH"}')

    print(f'\n最大像素误差: {max_err}/255 (DXT5 BC 有损, 字体可接受)')
    print(f'三件套已写到: {out_dir}')


if __name__ == '__main__':
    main()