#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""反汇编 SRTTR.exe 指定 RVA 周围。"""
import struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

EXE = r"I:\SteamLibrary\steamapps\common\Saints Row The Third Remastered\SRTTR.exe"

def rva_to_off(data, rva):
    # PE header
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe_off + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe_off + 20)[0]
    sec_off = pe_off + 24 + opt_size
    for i in range(nsec):
        off = sec_off + i * 40
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, off + 8)
        if vaddr <= rva < vaddr + max(vsize, rawsize):
            return rawptr + (rva - vaddr), rawsize - (rva - vaddr) if rva - vaddr < rawsize else 0
    return None, 0

def disasm(rva, length=0x100):
    with open(EXE, "rb") as f:
        data = f.read()
    off, avail = rva_to_off(data, rva)
    if off is None:
        print(f"RVA 0x{rva:x} not in any section"); return
    start = max(0, off - 0x40)
    blob = data[start: start + length + 0x80]
    base_rva = rva - (off - start)
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = False
    print(f";; SRTTR.exe +0x{rva:x} (file off 0x{off:x})")
    for ins in md.disasm(blob, 0x140000000 + base_rva):
        delta = ins.address - (0x140000000 + rva)
        mark = "  <<<< CRASH" if delta == 0 else ""
        if -0x40 <= delta <= 0x80:
            print(f"  {ins.address - 0x140000000:08x} {ins.mnemonic:8s} {ins.op_str}{mark}")

if __name__ == "__main__":
    disasm(int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x859FEF)
