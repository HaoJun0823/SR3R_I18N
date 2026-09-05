# -*- coding: utf-8 -*-
"""解剖 Remastered misc.vpp_pc 结构: header / directory / names / data / 压缩块头"""
import struct

VPP = r"I:\SteamLibrary\steamapps\common\Saints Row The Third Remastered\cache\misc.vpp_pc"
d = open(VPP, "rb").read()

magic, ver = struct.unpack_from("<II", d, 0)
flags = struct.unpack_from("<I", d, 0x14C)[0]
count, psize, dsize, nsize, udata, cdata = struct.unpack_from("<QQQQQQ", d, 0x158)
print("magic=0x%08X ver=%d flags=0x%04X" % (magic, ver, flags))
print("entries=%d packfileSize=%d dirSize=%d nameSize=%d" % (count, psize, dsize, nsize))
print("uncompressedData=%d compressedData=%d" % (udata, cdata))

dir_off = 0x1000
names_off = 0x1000 + ((dsize + 0xFFF) // 0x1000) * 0x1000
data_off = names_off + ((nsize + 0xFFF) // 0x1000) * 0x1000
print("dir@%#x names@%#x data@%#x data_end=%d file_len=%d" % (
    dir_off, names_off, data_off, data_off + cdata, len(d)))

want = ("font_zh.cvbm_pc", "font_zh.gvbm_pc", "charlist_zh.dat", "font_zh.vf3_pc", "font_body.cvbm_pc")
targets = {}
for i in range(count):
    e = dir_off + i * 48
    no, doff, usz, csz = struct.unpack_from("<QQQQ", d, e)
    end = d.index(b"\x00", names_off + no)
    name = d[names_off + no:end].decode("ascii", "replace")
    if name in want or i < 3:
        targets[i] = (name, doff, usz, csz)

for i, (name, doff, usz, csz) in targets.items():
    print("  [%d] %s: dataOff=%d usz=%d csz=%d (0x%X)" % (i, name, doff, usz, csz, csz))
    if csz != 0xFFFFFFFFFFFFFFFF:
        blk = data_off + doff
        hdr16 = d[blk:blk + 16]
        print("      16B头: %s" % hdr16.hex(" "))
        u32s = struct.unpack("<4I", hdr16)
        print("      u32: %s" % ([hex(x) for x in u32s],))
        print("      LZ4前8B: %s" % d[blk + 16:blk + 24].hex(" "))
    else:
        blk = data_off + doff
        print("      未压缩! 内容前16B: %s" % d[blk:blk + 16].hex(" "))
