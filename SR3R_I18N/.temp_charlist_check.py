# -*- coding: utf-8 -*-
import os, re

cl_path = r"I:\SteamLibrary\steamapps\common\Saints Row The Third Remastered\scripts\charlist.txt"
dict_dir = r"I:\SteamLibrary\steamapps\common\Saints Row The Third Remastered\scripts\dict"

# 1) charlist.txt 格式探测
raw = open(cl_path, "rb").read()
print("charlist.txt bytes:", len(raw), "BOM:", raw[:3].hex())
try:
    text = raw.decode("utf-8-sig")
    enc = "utf-8"
except UnicodeDecodeError:
    text = raw.decode("gbk")
    enc = "gbk"
print("encoding:", enc, "lines:", text.count("\n") + 1)
lines = [l for l in text.splitlines() if l.strip()]
print("non-empty lines:", len(lines))
for l in lines[:8]:
    print("  head:", repr(l[:80]))

# 2) 从 charlist 提取字符（去空格/分隔）
joined = "".join(lines)
chars_cl = set(ch for ch in joined if not ch.isspace())
print("charlist unique chars:", len(chars_cl))

# 3) 词典收集（模拟 DLL: KEY=英文原文 VALUE=中文译文, 收集译文非ASCII字符 + extras）
extra = "，。？！：；、·—…“”‘’（）《》〈〉【】〔〕「」『』％℃°±×÷©®™" \
        "０１２３４５６７８９ＡＢＣＤＥＦＧＨＩＪＫＬＭＮＯＰＱＲＳＴＵＶＷＸＹＺ" \
        "ａｂｃｄｅｆｇｈｉｊｋｌｍｎｏｐｑｒｓｔｕｖｗｘｙｚ"
charset = set()
freq = {}
for fn in os.listdir(dict_dir):
    if not fn.lower().endswith(".txt"): continue
    for line in open(os.path.join(dict_dir, fn), encoding="utf-8-sig", errors="replace"):
        m = re.match(r'\s*"((?:[^"\\]|\\.)*)"\s*:\s*"((?:[^"\\]|\\.)*)"', line)
        if not m: continue
        # 反转义（只关心值）
        val = m.group(2).replace('\\"','"').replace("\\n","\n").replace("\\r","\r").replace("\\\\","\\")
        for ch in val:
            if ord(ch) >= 0x80:
                charset.add(ch)
                freq[ch] = freq.get(ch, 0) + 1
for ch in extra:
    charset.add(ch)
    freq.setdefault(ch, 0)
print("dict charset size (DLL view):", len(charset))

# 4) 关键字检查: 齿/场 在不在字符集? 频率多少? 词典哪些条目含它们?
for target in "齿场":
    in_cl = target in chars_cl
    in_dict = target in charset
    fr = freq.get(target, 0)
    print(f"\nchar '{target}': in_charlist={in_cl} in_dict_charset={in_dict} freq_in_dict_trans={fr}")

# 5) 模拟 font1 截断: 频率降序, 2368 保留 → 找出被截断的 12 个字
by_freq = sorted((c for c in charset), key=lambda c: freq.get(c, 0))
truncated = by_freq[:12]  # 频率最低 12 个
print("\nfont1 clamped 12 lowest-freq chars (blank render):")
print("  " + " ".join(truncated))
print("freqs:", [freq.get(c,0) for c in truncated])

# 6) charlist 有而词典缺的字符
only_cl = chars_cl - charset
print("\nchars in charlist but NOT in dict charset:", len(only_cl))
if only_cl:
    print("  " + " ".join(sorted(only_cl)[:100]))