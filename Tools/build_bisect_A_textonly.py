# -*- coding: utf-8 -*-
"""
二分实验 A: 文本-only (字库全官方, 仅 menu_us.le_strings 替换)
================================================================
目的: 隔离 v2 崩溃 (SRTTR.exe+0x3FD144 sub_1403FD0F0 14槽type注册表查找)
      是文本层(槽码引用) 还是 字库层(vf3覆写idx293-316 + gvbm烘字) 引入。

版本 A 内容:
  - unpack/misc 从 cur_misc/ (官方基准 375 文件) 恢复
  - menu_us.le_strings 8 条 -> 槽码 325..348 (与 v2 完全相同的文本改动)
  - vf3/gvbm 官方原样 (idx 293..316 保持官方真实数据, 非汉字)

判定:
  崩   -> 文本层槽码引用机制有问题 (325-348 区间不可用) -> 换槽码区间/明文路线
  不崩 -> 字库层嫌疑坐实 -> 下一版做字库改动 (但官方零全空槽, 覆写无落点,
          需改"追加 count"路线: idx 336..359 / 槽码 368..391)
"""
import sys, os, struct, shutil, subprocess

ROOT = r"C:\Users\haojun0823\WorkBuddy\2026-09-04-02-19-04"
GAME = r"I:\SteamLibrary\steamapps\common\Saints Row The Third Remastered"
M = os.path.join(GAME, "unpack", "misc")
CUR = os.path.join(ROOT, "cur_misc")
CACHE = os.path.join(GAME, "cache")
ORIG = os.path.join(CACHE, "misc.vpp_pc.orig")

sys.path.insert(0, ROOT)
from sr3le_extract import crc_volition
import le_strings_repack as lr

# 槽码 325..348 = idx 293..316 (与 v2 相同, 官方有数据但 US 文本不引用)
SLOT0 = 325
MENU_ZH = {
    "MAINMENU_SINGLEPLAYER":    "单人游戏",
    "MAINMENU_COOP":            "合作模式",
    "MAINMENU_CAMPAIGN":        "主线战役",
    "MAINMENU_COOP_OPTION":     "合作战役",
    "MAINMENU_CHECK_MESSAGES":  "查看信息",
    "MAINMENU_COMMUNITY":       "社区",
    "MAINMENU_DLC":             "下载内容",
    "PAUSE_MENU_OPTIONS":       "选项",
}


def unique_han():
    seen = []
    for v in MENU_ZH.values():
        for ch in v:
            if ch not in seen:
                seen.append(ch)
    return seen


def main():
    han = unique_han()
    slot = {ch: SLOT0 + t for t, ch in enumerate(han)}
    print("唯一汉字 %d: %s" % (len(han), "".join(han)))
    print("槽码映射:", slot)

    # 1) 恢复官方基准
    print("恢复 unpack/misc <- cur_misc ...")
    if os.path.isdir(M):
        shutil.rmtree(M)
    shutil.copytree(CUR, M)
    n = len(os.listdir(M))
    assert n == 375, ("恢复失败, 文件数=%d" % n)
    print("  已恢复 %d 文件 (字库官方原样)" % n)

    # 2) 仅改 menu_us.le_strings
    src = os.path.join(M, "menu_us.le_strings")
    pairs = {}
    for key, zh in MENU_ZH.items():
        h = crc_volition(key)
        tb = b"".join(struct.pack("<H", slot[ch]) for ch in zh) + b"\x00\x00"
        pairs[h] = tb
        print("  %-28s -> %s 槽%s" % (key, zh, [slot[c] for c in zh]))
    lr.repack(src, pairs, src)
    print("menu_us.le_strings 已回写")

    # 3) 打包 (template = 官方 orig 顺序)
    out_tmp = os.path.join(ROOT, "misc_bisectA.vpp_pc")
    print("打包 ->", out_tmp)
    subprocess.run([sys.executable, os.path.join(ROOT, "vpp_pack.py"),
                    ORIG, M, out_tmp], check=True)

    # 4) 部署到 cache/
    bak = os.path.join(CACHE, "misc.vpp_pc.official_ok")
    if not os.path.exists(bak):
        shutil.copy2(os.path.join(CACHE, "misc.vpp_pc"), bak)
        print("官方对照版已留档 -> misc.vpp_pc.official_ok")
    shutil.copy2(out_tmp, os.path.join(CACHE, "misc.vpp_pc"))
    print("已部署 cache/misc.vpp_pc (二分A 文本-only)")
    print("备份: misc.vpp_pc.official_ok (官方可运行对照)")


if __name__ == "__main__":
    main()
