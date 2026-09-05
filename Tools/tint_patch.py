# -*- coding: utf-8 -*-
"""
方案 A 判别实验 (DXT5 字节级 patch 版, 快):
把 misc 全部 10 个字体图集 gvbm 的非透明块 color0/color1 置为红系 (保留 alpha 形状).
cvbm 壳不动 / vf3 不动 / 文件字节数不变. 用法: python tint_patch.py <misc_dir>
"""
import os, sys, struct

STEMS = ['font_body', 'font_body_nobdr', 'font_header', 'font_header_nobdr',
         'font_header_pc', 'font_header_pc_nobdr', 'font_sk', 'font_sk_nobdr',
         'font_zh', 'font_zh_nobdr']

# 红系 565: 亮红 R31 0xF800 / 暗红 R16 0x8000
RED0 = 0xF800   # 11111 000000 00000
RED1 = 0x8000   # 10000 000000 00000


def patch_block(blk):
    """blk: 16B DXT5 块. 返回 (changed bool)"""
    a0, a1 = blk[0], blk[1]
    if a0 == 0 and a1 == 0 and blk[2:8] == b'\x00\x00\x00\x00\x00\x00':
        return False          # 全透明块, 不动
    c0 = blk[8] | (blk[9] << 8)
    c1 = blk[10] | (blk[11] << 8)
    # 保持原有亮度次序 (DXT colsel 语义: c0>=c1 通常; 颠倒亦可)
    if c0 >= c1:
        struct.pack_into('<HH', blk, 8, RED0, RED1)
    else:
        struct.pack_into('<HH', blk, 8, RED1, RED0)
    return True


def tint_file(gvbm_path):
    data = open(gvbm_path, 'rb').read()
    # 注意: 必须用 bytearray 的原地视图操作, 切片是拷贝会丢修改
    view = memoryview(bytearray(data)).cast('B')
    assert len(view) % 16 == 0, 'gvbm 长度非 16 对齐?'
    changed = 0
    for off in range(0, len(view), 16):
        blk = view[off:off + 16]   # memoryview 切片 = 视图, patch 落盘
        if patch_block(blk):
            changed += 1
    open(gvbm_path, 'wb').write(view.tobytes())
    return changed, len(view)


if __name__ == '__main__':
    misc_dir = sys.argv[1]
    stems = sys.argv[2:] if len(sys.argv) > 2 else STEMS
    for s in stems:
        gv = os.path.join(misc_dir, s + '.gvbm_pc')
        cv = os.path.join(misc_dir, s + '.cvbm_pc')
        if not os.path.exists(gv) or not os.path.exists(cv):
            print('  [SKIP] %s 缺文件' % s)
            continue
        n, size = tint_file(gv)
        print('  [%s] 染红 %d 块 / %d 字节' % (s, n, size))
    print('DONE')
