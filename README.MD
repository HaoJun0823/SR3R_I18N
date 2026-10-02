# 该项目已经与4代工程合并：https://github.com/HaoJun0823/SR_GAME_CHINESE

# SR3R 汉化工程总目录（SR3R_DLL）

《黑道圣徒3：重制版》（Saints Row The Third Remastered，SR3R）简体中文汉化工程的**自包含归档**：外挂汉化 DLL 源码工程、全部构建/逆向/翻译工具链、词典与字体资产、翻译交接包、以及完整的逆向结论与踩坑记录。

游戏目录：`I:\SteamLibrary\steamapps\common\Saints Row The Third Remastered`
工程目录（工作区，含大量未归档中间产物）：上述目录下的 `工程目录\`

> **姊妹项目**：同作者的《黑道圣徒4：重制版》汉化见 **[SR4R_I18N](https://github.com/HaoJun0823/SR4R_I18N)**（本仓库的 SR4R 移植版，dinput8/binkw64 代理注入、词典+字形架构同源）。两个仓库共享同一套外挂注入/字形注入/词典替换方法论，互可对照阅读。

---

## 一、最终架构（一句话版）

**内核文本汉化（misc_kernel.vpp_pc，11524 条全命中）+ 外挂字形/字幕 DLL（SR3R_I18N.asi）双层混合**：

| 层 | 承担内容 | 载体 |
|---|---|---|
| 内核层 | le_strings 文本（菜单/HUD/任务/静态文本，11524 条） | `misc_kernel.vpp_pc` 资源包替换，源文为 `le_data\schinese` 词典 |
| 外挂层 | 中文字形渲染 + 语音字幕词典替换（9630 条） | `SR3R_I18N.asi`（本仓库 `SR3R_I18N\`），内存 hook，不碰资源文件 |

两层互补：内核层管"文本内容"，外挂层管"中文字怎么画出来 + 语音字幕"。词典缺失时 DLL 自动进 font-only 模式（只补字形，不替换文本），因此两层可独立启停。

### 关键结论：为什么字库放外挂 DLL 而不是资源包

`SRTT3_CN_ACHIEVEMENTS.md` 里"大字库崩溃 = GPU 字形池 count≈3448 上限"的推断**已被推翻**。真实根因（反汇编实证）：

- **font CPU 池上限 64KB**：`.rdata` 常量 `qword_140E4FEB0 = 0x10000`，vf3 加载器 `sub_140859F20` 按 vf3 文件总字节数线性分配。全字符集 vf3 远超 64KB → 资源包内放大字库必然崩溃，此路不通。
- 游侠（ali213）并非纯资源方案：其 DLL（binkw64.dll PE 补丁 → ALI213.DLL）运行时特征码定位后 `WriteProcessMemory` 扩池，本质也是运行时补丁。
- 因此资源包只能装文本（le_strings），字形必须由外挂 DLL 在运行时自建图集解决——这就是混合架构的由来。

---

## 二、目录清单

```
SR3R_DLL\
├── SR3R_I18N\        外挂汉化 DLL 工程（VS2017 v141 x64 /MT，v7.5.2）
│   ├── dllmain.cpp   全部实现（约 2800 行：hook A–J + 字形层 + 词典层）
│   └── README.md     DLL 自身详细文档（架构/词典格式/配置/陷阱，中英双语）
├── le_data\          简体中文本地化 **独立 git 仓库**（54 个提交 + 未跟踪 voice_dumps）
│   ├── schinese\     内核层简体中文词典（misc_kernel 的文本源）
│   ├── voice_dumps_chinese\  语音字幕中文化 dump（voice_001~010 词典的上游）
│   ├── original\     英文原文基准
│   ├── .qa\ 审计/校对产物（terms_cand.md、审计问题清单.xlsx、QA 汇总、校对发现）
│   └── .temp\ .workbuddy\  中间产物（未跟踪）
│   └── **注意：独立 git 仓库，需单独克隆/复制，不随主仓库分发**
├── Tools\            52 个 Python 工具（vpp 打包解包/纹理烘焙/构建/校验/崩溃分析）
├── Docs\             4 份核心逆向文档（见下）
├── Dict_CHS\         词典资产：voice_001~010.txt（9630 条语音字幕）+ charlist.txt + ini 样例
├── Handoff\          翻译交接包：source.jsonl（5763 条）+ 术语表 + 回填脚本 + 已译分片
├── Fonts\            SourceHanSansHWSC-VF.ttf（思源黑体，运行时字体；le_data 内有同款副本）
├── packages\         MinHook 1.3.3 静态库依赖（许可证见 LICENSE-MinHook.txt）
└── SR3R_DLL.slnx     解决方案
```

### Docs\ 四份文档分工

| 文件 | 内容 | 状态 |
|---|---|---|
| `SRTT3_CN_HANDBOOK.md`（531 行） | 格式逆向手册：vpp_pc/le_strings/vf3/cvbm/gvbm/DXT5 全格式规格与解包打包方法 | 有效 |
| `SRTT3_CN_ACHIEVEMENTS.md`（222 行） | 历史攻坚记录：内核文本替换路线全过程 | **部分结论已过时**（GPU 池推断被 CPU 池 64KB 根因取代，见上文） |
| `3DM-hook重写技术规格.md`（399 行） | 3DM 汉化 DLL 完整逆向规格：hook 点、补丁表、trampoline 解码 | 参考用（本项目最终未采用 3DM 路线） |
| `字幕换行英文残留问题解决全记录.md`（约 300 行） | v7.3→v7.5 语音字幕折行残留/时长控制码排障实录，Hook J 落点的由来 | 有效（历史决策依据） |

### 有用/无用判定（游戏目录内未归档部分）

| 位置 | 内容 | 判定 |
|---|---|---|
| `工程目录\mod_src\` | 全语言 le_strings + 字体五件套 + charlist（400+MB） | **只记录路径不归档**（可随时从游戏重新解包，`Tools\vpp_extract_all.py`） |
| `工程目录\al213_misc\` `cur_misc\` | 游侠解包/官方 375 文件基准 | 同上，可重新生成 |
| `工程目录\.temp\`（201 个脚本） | 历史分析/一次性脚本 | 中间产物，不归档；核心能力已沉淀进 Tools\ |
| `dumps\`（93 个 mdmp，705MB） | 历史崩溃转储 | 排障已结束，可清理；分析方法见 `Tools\mdmp_parse.py` |
| `汉化\3DM\` `汉化\游侠\` | 第三方汉化 + 各自 IDA 库 .i64 | 参考资产，留在游戏目录；规格已沉淀进 Docs\ |
| `scripts\` | 当前部署位（asi/ini/dict/字体） | 部署位本身，勿动 |

---

## 三、ASI Loader 注入机制（binkw64 代理）

SR3R 与 SR4R 使用同一套**代理 DLL（proxy DLL）注入**方式，不修改游戏 EXE、不依赖第三方注入器：

1. **原版改名**：游戏根目录的原版 `binkw64.dll` 重命名为 `binkw64Hooked.dll`。
2. **代理加载器**：放一个同名的 `binkw64.dll`（本仓库的 ASI Loader，源自 dinput8/Ultimate ASI Loader 方案）顶替原版。游戏启动时加载的是这个代理 DLL。
3. **转发 + 注入**：代理 DLL 在其 `DllMain` 里（a）`LoadLibrary("binkw64Hooked.dll")` 把原版函数转发过去，保证游戏对 binkw64 的调用正常；（b）扫描 `scripts\` 目录，把 `*.asi`（我们的 `SR3R_I18N.asi`）全部 `LoadLibrary` 加载进进程。
4. **生效**：`SR3R_I18N.asi` 的 `DllMain` 里安装各 Hook（MinHook），此后文本替换与字形注入生效。卸载/删除代理 DLL 即完全恢复原版。

```
游戏根目录/
├── binkw64.dll        (代理 loader，即 ASI Loader，勿删)
├── binkw64Hooked.dll   (原版 binkw64，被代理转发)
└── scripts/
    ├── SR3R_I18N.asi   (本工程 DLL，被代理加载)
    ├── SR3R_I18N.ini
    ├── dict\voice_001~010.txt
    ├── charlist.txt
    └── SourceHanSansHWSC-VF.ttf
```

**为什么用 binkw64 而不是 dinput8**：SRTT Remastered（与 SR4R 一样）都是 x64 原生程序，使用常规的 dinput8 代理注入亦可，但游戏引擎加载路径中 `binkw64.dll` 是必加载的视频解码组件，作为代理宿主更隐蔽、更不易被反外挂误伤，且与 3DM/游侠时代的 .asi 生态兼容（.asi 文件本身就是 DLL，仅扩展名不同）。

> 注意：上面根目录的 `binkw64.dll` 是**注入器**而非本工程产物；真正干活的是 `scripts\SR3R_I18N.asi`。打包发布时不要把代理 loader 与汉化 DLL 混为一谈。

---

## 四、SR3R_I18N.asi 功能概览

v7.5.2，详见 `SR3R_I18N\README.md`（本节只列骨架）：

**文本替换层**（词典 = le_strings 行格式，CRC 哈希查表）：
- Hook A `DrawWide`：绘制时整串替换
- Hook B `Format`：格式化前替换
- Hook F `LangCur` / G `LangTxt`：语言服务文本
- **Hook J `sub_1402D2BC0`（v7.5 核心）**：语音字幕绘制入口、折行前整串替换。剥离尾部 `\n<ms>` 时长控制码后查词典、命中回填尾码，时长语义不变。此函数返回值即显示秒数。游侠 SIG3 同点位，佐证这是唯一可靠落点。

**中文字形层**（引擎字体无 CJK）：
- Hook C `FontLookup`：触发伪字体对象升级（3 字体，零售版前 2 个需要）
- Hook D `TexObj` + E `SrvResolve`：自建 BGRA8 图集 + MAGIC texId（`0x60000000+fontId`，引擎所有缓存对界外 id 安全）
- stb_truetype 光栅化思源黑体；官方英文图集 BC3 块解码读回拼接，未翻译内容照常渲染
- 容量不足时 cellW 自适应收窄（v7.5.1，等比缩放不变形）

**诊断层**：Hook H/I 探针（v7.5.2 已退役，仅留主统计行）。

**安全设计**：每个 hook 安装前比对 16 字节特征码，游戏更新即安全 idle 不盲 patch。

---

## 五、工具链（Tools\，52 个脚本）

按用途分组（全部 2026-09 期间为 SR3R 工程编写，Python 3.x，无第三方依赖为主）：

**vpp_pc 资源包**：`vpp_pack.py` / `vpp_extract_all.py` / `vpp_probe.py` / `vpp_scan_all.py` / `vpp_verify.py` / `vpp_selfcheck.py` / `vpp_list_fonts.py` / `diagnose_vpp.py` / `cmp_misc.py` / `extract_orig_misc.py` / `reextract_orig.py`

**le_strings 文本**：`Tools\sr3le_extract.py` / `Tools\le_strings_repack.py` / `Tools\merge_translations.py` / `Tools\check_match.py` / `Tools\verify_deployed.py` / `Tools\build_handoff.py`

**字体/纹理**：`volition_tex.py` / `bake_lib.py` / `font_roundtrip.py` / `font_shape_test.py` / `fonts_probe.py` / `scan_vpp_fonts.py` / `slot_table.py` / `check_deployed_vf3.py` / `restore_fonts.py` / 字体着色系列（`font_tint*.py` / `tint_*.py`）

**构建产物线**：`build_v7.py`（v7 内核词典包）/ `build_full.py` / `build_menu_zh*.py`（v1–v6 菜单迭代史）/ `build_btxt.py` / `build_bisect_*.py`（二分排障）

**崩溃/逆向辅助**：`mdmp_parse.py` / `mdmp_stackwalk.py` / `disasm_srttr.py` / `probe_gpeg.py` / `probe_zh_branch.py`

**第三方汉化解包**：`extract_ali.py`（游侠词典 XOR→zlib→XOR 解密，17411 条）

---

## 六、翻译交接包（Handoff\）与 le_data 仓库现状

### le_data 仓库（独立 git 仓库：`G:\Projects\SR3R_DLL\le_data`）

内核层简体中文翻译的**独立版本化仓库**（源位于游戏 `unpack\text\le_data`，54 个提交保留）。**注意：该目录自带 `.git`，主仓库通过 `.gitignore` 排除、以独立仓库方式分发——克隆主仓库后需单独克隆/复制 le_data 才能获取翻译词典与声音字幕资产**。提交史即翻译工作流实录：接管翻译批次（static/diversion/activity…）→ 审计入库 → 批次 A–D 逐条修复（占位符/引号/术语漂移/空格对齐）→ 三校/四校 → 敏感词软化（HEAD `edfc5d0`）。定名表 v1.1、审计问题清单、QA 汇总都在仓库内 `.qa\` 与根目录。`voice_dumps\`（英文原始 dump，20 个文件）未被 git 跟踪，是手动补齐的。

### Handoff 交接包

`source.jsonl`：5763 条 le_strings 文本任务（21 个源文件：menu/hud/mission/subtitle/customize/activity/diversion/static/dlc1-3/平台分支/voice_script 等）。

**回填进度（2026-09-06 实测）**：真实译文 **1464/5763**（25.4%），全部为 `customize_us`（t1 批次）。其余 4299 条 zh 字段为字面 `None` 占位——jsonl 里 zh 已"填满"但绝大多数是占位符，**不要被表面的 5763/5763 迷惑**。

- 已完成可合并：`t1_customize.jsonl`（1464）+ `_t1_p1~p5` + `_t2_p1~p4` 分片（约 875 条，合并去重后约 2000+ 条可用译文）
- 待翻译大头：`b2_mission`（334）/ `b3_hud`（797）/ `b4_menu`（484）/ `b7_subtitle`（358）/ `b3_static`（555）等批次
- 回填工具：`apply_batch.py`（读分片 → 写回 source.jsonl → repack 进 misc_kernel）
- 规范文档：`README_handoff.md`（翻译规范：占位符/换行严格对齐、口语自然、拒绝 AI 味）/ `terms_cand.md`（术语候选表 v1.1）

**术语分歧待统一**（内核汉化已部署版本 vs 术语表 v1.1）：元气教授↔元气博士、星球圣徒↔圣徒星球、边角活儿↔边缘工坊。语音字幕词典与内核文本同屏，标准不统一会穿帮，最终定版前不要批量回填。

---

## 七、历史路线演进（为什么是现在这个方案）

1. **内核资源替换探索期**：解包 vpp_pc → 逆向 le_strings/vf3/cvbm/gvbm 全格式 → 改文本可行，改字库撞 64KB CPU 池崩溃（93 个 dump 排障，最终根因见上文）。`SRTT3_CN_ACHIEVEMENTS.md` 是这一时期的完整记录。
2. **第三方方案逆向期**：3DM（.asi hook + text.btxt 词典）与游侠（资源 + 运行时扩池 DLL）两条路线全拆解。3DM hook 规格完整沉淀在 `3DM-hook重写技术规格.md`。结论：两者都依赖运行时补丁，纯资源方案不存在。
3. **自研外挂期（SR3R_I18N v6.x–v7.5.2）**：字形层（v6.6 修 kern 崩溃 → v6.7 修填充顺序 → v7 字形架构稳定）+ 文本层（词典 hook）+ 字幕专项（v7.3 折行重组状态机失败 → v7.5 Hook J 折行前整串替换成功）+ 容量（v7.5.1 cellW 自适应收窄）。
4. **混合架构定稿**：文本走内核包（misc_kernel，schinese 源），字形+字幕走外挂 DLL。当前游戏内实测效果即此架构。

---

## 八、踩坑经验精选

完整版见 `SR3R_I18N\README.md`「可靠性结论与陷阱」（13 条），此处列工程级教训：

1. **任何"容量上限"结论先找 .rdata 常量**：GPU 池 3448 的推断曾误导排障方向，真凶是 CPU 池 64KB（`qword_140E4FEB0`）。反汇编常量比现象推测可靠。
2. **v7.5 字幕冻结教训**：v7.3 折行重组状态机改模板内容风险大；Hook J 直接在绘制入口折行前整串替换，架构上天然免疫折行残段（词典 KEY=英文整句）。
3. **引擎按宽度折行会拆长句**：折行发生在字幕绘制函数内部（`sub_14085A1F0`），任何"折行后"的 hook 点都拿不到完整句——落点必须选折行前。
4. **语音字幕链不走 Format/LangSrv**：文本直出绘制入口，尾部带 `\n<毫秒>` 时长码（引擎 atoi/1000），剥离回填必须成对，否则字幕时长变化。
5. **cell 高度锁死只能收窄宽度**：quad/UV 高度取 font 对象 +22（中英共享），宽度每槽位独立（metrics+4）。等比缩放防变形（思源黑体 advance=100% em）。
6. **charlist.txt 第 0 行零宽水印**（0x200B-0x200D）必须跳过，否则混入幽灵字符。
7. **词典 round-trip 验证是交付铁律**：每次 repack 后用 `verify_v7_roundtrip.py` / `check_match.py` 验证读回一致。
8. **Windows 中文文本处理**：严禁 PowerShell 管道处理中文文本（GBK 乱码），统一 Python + `io.open(encoding='utf-8')`。

---

## 九、如何重建 / 继续开发

**重新部署运行环境**（游戏 `scripts\` 目录）：
```
SR3R_I18N.asi + SR3R_I18N.ini + dict\voice_001~010.txt + charlist.txt + SourceHanSansHWSC-VF.ttf
```
均在 `Dict_CHS\`（ini 样例改名 `SR3R_I18N.ini.example`）与 `SR3R_I18N\` 构建输出。游戏根目录 `binkw64.dll` 是 ASI Loader，勿动。

**构建 DLL**：VS2017 v141 工具集 /MT /C++17 x64，开 `SR3R_DLL.slnx`。MinHook 在 `packages\`（/GL 编译，链接时 LTCG 重启属正常）。

**继续翻译**：`Handoff\source.jsonl` 按批次（b2/b3/b4/b7…）翻译 → `apply_batch.py` 回填 → `le_strings_repack.py` + `build_v7.py` 重打 misc_kernel → `verify_deployed.py` 校验。

**游戏更新后**：DLL 特征码失配会安全 idle（日志 `signature mismatch, ABORT`），需 IDA 重新定位 hook 目标；内核包按 `SRTT3_CN_HANDBOOK.md` 重解包重打包。

---

## 十、版权与致谢

- 本工程：GPL-3.0。作者 HaoJun0823。
- MinHook（运行时 hook 库）：BSD-2-Clause，作者 Tsuda Kageyu，许可证全文见 `LICENSE-MinHook.txt`。
- stb_truetype（字形光栅化）：Public Domain / MIT，随 `SR3R_I18N\stb_truetype.h` 分发。
- 思源黑体（Source Han Sans HW SC）：SIL Open Font License 1.1，Adobe/Google。
- 游侠（ali213）、3DM 汉化仅作逆向学习参考，未分发其文件。
- 游戏本体资源版权归 Deep Silver/Volition 所有，本工程不分发游戏资源。
