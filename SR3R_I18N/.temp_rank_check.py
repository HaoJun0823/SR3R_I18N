# -*- coding: utf-8 -*-
import os, re

cl_path = r"I:\SteamLibrary\steampack_analysis\none.txt"  # placeholder, unused
cl_path = r"I:\SteamLibrary\steamapps\common\Saints Row The Third Remastered\scripts\charlist.txt"
dict_dir = r"I:\SteamLibrary\steamapps\common\Saints Row The Third Remastered\scripts\dict"

text = open(cl_path, "rb").read().decode("utf-8-sig")
lines = [l for l in text.splitlines() if l.strip()]
char_lines = [l for l in lines if not l.startswith(";")]
joined = "".join(char_lines)
chars_cl = set(ch for ch in joined if ord(ch) > 0x20 and ord(ch) < 0x2000)
print("charlist unique chars (>0x20, excl zwsp):", len(chars_cl))

charset = set(); freq = {}
for fn in os.listdir(dict_dir):
    if not fn.lower().endswith(".txt"): continue
    for line in open(os.path.join(dict_dir, fn), encoding="utf-8-used" if False else "utf-8-sig", errors="replace"):
        m = re.match(r'\s*"((?:[^"\\]|\\.)*)"\s*:\s*"((?:[^"\\]|\\.)*)"', line)
        if not m: continue
        val = m.group(2).replace('\\"','"').replace("\\n","\n").replace("\\r","\r").replace("\\\\","\\")
        for ch in val:
            if ord(ch) >= 0x80:
                charset.add(ch); freq[ch] = freq.get(ch, 0) + 1
print("dict charset:", len(charset))

# 齿/场 精确验证
for t in "齿场":
    print(f"\n'{t}': in_charlist={t in chars_cl}, in_dict={t in charset}, freq={freq.get(t,0)}")

# 模拟 DLL 排序（稳定插入排序=稳定排序, 初始数组按码点升序, 同频取码点小者）
array_order = sorted(charset)  # RasterizeThread 收集顺序 = 码点升序
k = 2368
kept = sorted(array_order, key=lambda c: -freq.get(c, 0), )[:k]  # sorted 稳定, 同频码点升序 = 插入排序语义
# 等一下, 插入排序 "while j>0 and freq[cps[j-1]] < kf: shift" 是稳定降序（同频保持原序=码点升序）✓
truncated = [c for c in array_order if c not in set(kept)]
print("\nfont1 kept:", len(kept), "truncated:", len(truncated))
print("truncated chars:", " ".join(truncated))
print("their freqs:", [freq.get(c,0) for c in truncated])
print("'场' truncated?", "场" in truncated)
print("'场' freq rank:", sum(1 for c in charset if freq.get(c,0) > freq.get('场',0)) + 1)