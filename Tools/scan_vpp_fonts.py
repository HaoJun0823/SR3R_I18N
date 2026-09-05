# -*- coding: utf-8 -*-
"""只读各 vpp 头部目录, 列出含 font / 字体相关条目的文件名 (轻量, 不解压数据)"""
import os, struct, sys

CHUNK = 0x1000

def scan_names(path, want="font"):
    sz = os.path.getsize(path)
    with open(path, "rb") as f:
        head = f.read(0x2000)
        magic, ver = struct.unpack_from("<II", head, 0)
        if magic != 0x51890ACE:
            return None, "magic=%08X (非 v6)" % magic
        if ver != 6:
            return None, "ver=%d" % ver
        flags = struct.unpack_from("<I", head, 0x14C)[0]
        count, psize, dsize, nsize, udata, cdata = struct.unpack_from("<QQQQQQ", head, 0x158)
        dir_off = CHUNK
        names_off = dir_off + ((dsize + CHUNK - 1) // CHUNK) * CHUNK
        need = names_off + nsize
        f.seek(0)
        blob = f.read(need)
    hits = []
    for i in range(count):
        e = i * 48
        no = struct.unpack_from("<Q", blob, dir_off + e)[0]
        end = blob.index(b"\x00", names_off + no)
        name = blob[names_off + no:end].decode("ascii", "replace")
        if want.lower() in name.lower():
            hits.append(name)
    return hits, "count=%d dsize=%d nsize=%d" % (count, dsize, nsize)

def main():
    roots = sys.argv[1:] or [
        r"I:\SteamLibrary\steamapps\common\Saints Row The Third Remastered\cache",
        r"I:\SteamLibrary\steamapps\common\Saints Row The Third Remastered\汉化\游侠\cache",
    ]
    seen = {}
    for root in roots:
        if not os.path.isdir(root):
            continue
        for fn in sorted(os.listdir(root)):
            if not fn.lower().endswith(".vpp_pc"):
                continue
            p = os.path.join(root, fn)
            if os.path.getsize(p) < 0x3000:
                continue
            hits, info = scan_names(p)
            key = (root, fn)
            if hits is None:
                print("[SKIP] %s\\%s : %s" % (root, fn, info))
                continue
            fhits = [h for h in hits if "font" in h.lower()]
            if fhits:
                print("[FONT] %s\\%s (%s) -> %d font files" % (root, fn, info, len(fhits)))
                for h in fhits:
                    print("        %s" % h)

if __name__ == "__main__":
    main()
