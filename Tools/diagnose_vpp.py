# -*- coding: utf-8 -*-
"""反推 Volition 引擎对 vpp 目录项 dataOffset 的真实语义:
- 若 ORIG(原厂) 的 doff == csz 累积(物理) -> 引擎按物理偏移读 -> 我们 pack 写虚拟(usz)即为 bug
- 若 ORIG 的 doff == usz 累积(虚拟) -> 引擎按虚拟读 -> 我们 pack 正确, 崩溃另有原因
"""
import sys, struct
ROOT = r"C:/Users/haojun0823/WorkBuddy/2026-09-04-02-19-04"
sys.path.insert(0, ROOT)
import vpp_pack as vp

CHUNK = 0x1000
NEG = vp.NEG

def analyze(path, label):
    try:
        hdr, entries, names = vp.read_vpp(path)
    except Exception as e:
        print(f"[{label}] read_vpp 失败: {e}")
        return
    data_start = hdr["data_off"]
    phys = 0          # csz 累积 (相对数据区)
    virt = 0          # usz 累积 (相对数据区)
    n_lz4 = n_store = 0
    mism_cs = mism_vs = 0
    gvbm = vf3 = strings = []
    for i, e in enumerate(entries):
        is_last = (i == len(entries) - 1)
        if e["csz"] == NEG:
            sz = e["usz"]; mode = "STORE"; n_store += 1
        else:
            sz = e["csz"]; mode = "LZ4"; n_lz4 += 1
        palign = sz if is_last else ((sz + CHUNK - 1) // CHUNK) * CHUNK
        valign = e["usz"] if is_last else ((e["usz"] + CHUNK - 1) // CHUNK) * CHUNK
        doff = e["data_off"]
        if doff != phys: mism_cs += 1
        if doff != virt: mism_vs += 1
        if e["name"].endswith(".gvbm_pc") or e["name"].endswith(".vf3_pc") or e["name"].endswith(".le_strings"):
            gvbm.append((e["name"], mode, e["usz"], e["csz"], doff, phys, virt))
        phys += palign
        virt += valign
    print(f"\n=== {label}: {path}")
    print(f"  count={len(entries)} data_start=0x{data_start:x} "
          f"cdata=0x{hdr['cdata']:x} udata=0x{hdr['udata']:x}")
    print(f"  LZ4={n_lz4} STORE={n_store}  | doff==phys_csz 不匹配数={mism_cs}  | doff==virt_usz 不匹配数={mism_vs}")
    print(f"  phys_csz_total=0x{phys:x}(==cdata? {phys==hdr['cdata']})  virt_usz_total=0x{virt:x}(==udata? {virt==hdr['udata']})")
    print(f"  >>> 引擎语义判定: doff 匹配 {'物理(csz)' if mism_cs==0 else ('虚拟(usz)' if mism_vs==0 else '两者皆不匹配!')}")
    print(f"  --- 关键资源 ---")
    for nm, mode, usz, csz, doff, phys, virt in gvbm:
        csz_s = "STORE" if csz == NEG else csz
        flag = ""
        if doff != phys: flag += " [doff!=phys]"
        if doff != virt: flag += " [doff!=virt]"
        print(f"    {nm[:38]:38} {mode:4} usz={usz:>9} csz={str(csz_s):>9} doff=0x{doff:x}{flag}")

GAME = r"I:/SteamLibrary/steamapps/common/Saints Row The Third Remastered/cache"
analyze(GAME + r"\misc.vpp_pc.orig",       "ORIG (原厂)")
analyze(GAME + r"\misc.vpp_pc.v6ok",       "V6OK (成功版)")
analyze(GAME + r"\misc.vpp_pc",            "V7   (当前部署/崩溃)")
