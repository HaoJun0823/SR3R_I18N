# -*- coding: utf-8 -*-
"""
二分实验 F1: 官方文件 零改动 原样重打包
================================================
目的: 判别三次崩溃 (同点 SRTTR.exe+0x3FD144) 是否由 vpp 打包器本身引入。
      此前"官方对照"用的是官方 vpp 原文件 (不经打包器);
      而 v2/二分A/v3 全部是重打包产物 -> 若打包器对布局理解有 bug,
      内容怎么改都会崩。

F1 内容:
  - unpack/misc <- cur_misc/ 官方基准 375 文件 (menu_us 也用官方原文件, 零 repack)
  - 用官方 orig vpp 作 template 重打包 -> misc_F1.vpp_pc

判定:
  崩   -> 打包器/格式 bug 坐实 -> 先修复打包器 (对比官方 vpp 物理布局)
  不崩 -> 打包器 OK -> 问题在 menu_us repack 或文本内容 -> F2 (repack 零改动)
"""
import sys, os, shutil, subprocess, hashlib, struct

ROOT = r"C:\Users\haojun0823\WorkBuddy\2026-09-04-02-19-04"
GAME = r"I:\SteamLibrary\steamapps\common\Saints Row The Third Remastered"
M = os.path.join(GAME, "unpack", "misc")
CUR = os.path.join(ROOT, "cur_misc")
CACHE = os.path.join(GAME, "cache")
ORIG = os.path.join(CACHE, "misc.vpp_pc.orig")

sys.path.insert(0, ROOT)
from vpp_pack import pack_vpp

PY = r"C:\Users\haojun0823\.workbuddy\binaries\python\envs\default\Scripts\python.exe"


def md5(p):
    return hashlib.md5(open(p, "rb").read()).hexdigest()


def main():
    # 1) 恢复官方基准 (robocopy 镜像, 避开 rmtree shim)
    print("恢复 unpack/misc <- cur_misc ...")
    subprocess.run(["robocopy", CUR, M, "/MIR", "/NJH", "/NJS", "/NP"],
                   capture_output=True)
    n = len(os.listdir(M))
    assert n == 375, f"unpack/misc 应为 375 文件, 实际 {n}"
    print(f"  已恢复 {n} 文件")

    # 2) 校验 menu_us 与官方一致 (零 repack)
    a = md5(os.path.join(M, "menu_us.le_strings"))
    b = md5(os.path.join(CUR, "menu_us.le_strings"))
    assert a == b, "menu_us 不是官方原样!"
    print("  menu_us = 官方原样 OK")

    # 3) 重打包 (template=官方 orig)
    out = os.path.join(ROOT, "misc_F1.vpp_pc")
    pack_vpp(ORIG, M, out, verbose=True)
    print(f"F1 产物: {out} ({os.path.getsize(out):,}B)")

    # 4) 自检: 提取全部文件与 cur_misc 逐 MD5 对比
    print("自检: 提取并对比 375 文件 ...")
    sys.path.insert(0, ROOT)
    import vpp_pack as vp
    hdr, entries, names = vp.read_vpp(out)
    import lz4.block as lb
    raw = open(out, "rb").read()
    pos = hdr["data_off"]
    bad = []
    for e in entries:
        nm = e["name"]
        usz, csz = e["usz"], e["csz"]
        sz = usz if csz == 0xFFFFFFFFFFFFFFFF else csz
        payload = raw[pos: pos + sz]
        if csz == 0xFFFFFFFFFFFFFFFF:
            data = payload
            pos += usz
        else:
            m1, m2, ln, u2 = struct.unpack_from("<4I", payload, 0)
            data = lb.decompress(payload[16:16 + ln], uncompressed_size=usz)
            pos += ((csz + 0xFFF) // 0x1000) * 0x1000
        src_p = os.path.join(CUR, nm)
        if not os.path.exists(src_p):
            bad.append((nm, "missing-in-cur"))
            continue
        if hashlib.md5(data).hexdigest() != md5(src_p):
            bad.append((nm, "md5-diff"))
    print(f"  对比完成: {len(entries)} 文件, 差异 {len(bad)}")
    for x in bad[:20]:
        print("   DIFF:", x)

    # 5) 部署
    if bad:
        print("!! 自检失败, 不部署")
        return 1
    bak = os.path.join(CACHE, "misc.vpp_pc.v3crash")
    cur_deploy = os.path.join(CACHE, "misc.vpp_pc")
    if os.path.exists(cur_deploy):
        if not os.path.exists(bak):
            os.rename(cur_deploy, bak)
            print(f"当前部署备份 -> {bak}")
    shutil.copy2(out, cur_deploy)
    print(f"F1 已部署: {cur_deploy} ({os.path.getsize(cur_deploy):,}B)")
    print("请启动游戏到主菜单, 报告 崩/不崩")
    return 0


if __name__ == "__main__":
    sys.exit(main())
