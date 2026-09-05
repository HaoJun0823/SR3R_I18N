# -*- coding: utf-8 -*-
"""
SRTT3 count 扩展最小池实验 (v-exp)
==================================
只改 font_body.vf3: count 336 -> 700
新槽 metric/x/y 全 0 (空洞), 不烘字形, 不扩图集, 不改文本
启动游戏: 进主菜单 = cpu/gpu 池余量 >= 11.3KB vf3 增量 -> 机制可行
         崩(加载 font_body) = 池满 -> 佐证 us 多字体共享 64KB 锁死, zh 单字体路线更坚定
"""
import sys, os, struct, shutil, subprocess
ROOT = r"C:/Users/haojun0823/WorkBuddy/2026-09-04-02-19-04"
GAME = r"I:/SteamLibrary/steamapps/common/Saints Row The Third Remastered"
M = os.path.join(GAME, "unpack", "misc")
CUR = os.path.join(ROOT, "cur_misc")
CACHE = os.path.join(GAME, "cache")
ORIG = os.path.join(CACHE, "misc.vpp_pc.orig")
NEW_COUNT = 700

# 1. 备份当前 v6 部署
cur_vpp = os.path.join(CACHE, "misc.vpp_pc")
bak6 = os.path.join(CACHE, "misc.vpp_pc.v6ok")
if os.path.exists(cur_vpp) and not os.path.exists(bak6):
    shutil.copy2(cur_vpp, bak6)
    print("v6 已留档 -> misc.vpp_pc.v6ok")

# 2. 恢复干净基准
print("恢复 unpack/misc <- cur_misc ...")
subprocess.run(["robocopy", CUR, M, "/MIR", "/NFL", "/NDL", "/NJH", "/NJS"], check=False)
n = len(os.listdir(M))
assert n == 375, ("恢复失败", n)
print(f"  已恢复 {n} 文件")

# 3. 重建 font_body.vf3: count 336->700
vpath = os.path.join(M, "font_body.vf3_pc")
d = bytearray(open(vpath, "rb").read())
count = struct.unpack_from('<I', d, 8)[0]
assert count == 336, count
kern = struct.unpack_from('<I', d, 0x20)[0]
met_old = (0xD0 + 6*kern + 15) & ~15
z1_old = (met_old + 16*count + 15) & ~15
z2_old = (z1_old + 4*count + 15) & ~15
met_new = met_old
z1_new = (met_new + 16*NEW_COUNT + 15) & ~15
z2_new = (z1_new + 4*NEW_COUNT + 15) & ~15
nd = bytearray(z2_new + 4*NEW_COUNT)   # 全 0
nd[:met_old] = d[:met_old]             # 头部 0xD0 前保留 (magic/ver/base/L/kern...)
nd[met_new:met_new+16*count] = d[met_old:met_old+16*count]
for i in range(count):
    struct.pack_into('<I', nd, z1_new+4*i, struct.unpack_from('<I', d, z1_old+4*i)[0])
    struct.pack_into('<I', nd, z2_new+4*i, struct.unpack_from('<I', d, z2_old+4*i)[0])
struct.pack_into('<I', nd, 8, NEW_COUNT)
open(vpath, "wb").write(bytes(nd))
print(f"font_body.vf3: count {count}->{NEW_COUNT}  文件 {len(d)}B->{len(nd)}B (增量 {len(nd)-len(d)}B)")

# 4. 打包部署
out_tmp = os.path.join(ROOT, "misc_countexp.vpp_pc")
py = r"C:/Users/haojun0823/.workbuddy/binaries/python/envs/default/Scripts/python.exe"
print("打包 ->", out_tmp)
subprocess.run([py, os.path.join(ROOT, "vpp_pack.py"), ORIG, M, out_tmp], check=True)
shutil.copy2(out_tmp, cur_vpp)
print("已部署 cache/misc.vpp_pc (count 扩展实验 v-exp)")
print("预期: 进主菜单不崩 = count 扩展机制可行 / 崩 = 池余量不足")
