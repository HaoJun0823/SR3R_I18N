// dllmain.cpp : SR3R_I18N - 黑道圣徒3重制版 外挂汉化 DLL（v6：文本替换 + 中文字形层）
//
// v6 = v5 文本替换层 + 运行时中文字形渲染层（零资源文件改动）
//
//  [文本层 v5]
//   1. 加载 scripts\<dict>\*.txt（le_strings 格式: "英文原文": "中文译文"; SR3R_I18N.ini 可配置）
//   2. Hook A  sub_1408B5FF0  宽字符绘制核心   — R9  = 文本指针 -> 查词典替换
//   3. Hook B  sub_140812610  Volition formatter — RDX = 格式串 -> 查词典替换
//   4. DumpText.dtxt 未命中文本去重收集（仅英文，过滤 CJK）
//
//  [字形层 v6]（引擎结构 2026-09-05 IDA 逆向实证）
//   字体对象布局（cpeg cvbm_pc, 208B 头 + 变长区）:
//     +8  u32 字形数  +12 u32 baseChar  +16 i32 missAdvance  +20 u16 行高
//     +22 u16 cell高  +28 i32 全局字距 +32 u32 kern数  +36/+38 i16 顶部/左侧偏移
//     +104 图集名     +168 kern表(6B/项{u16 left,u16 right,i8 off})  +176 metrics(16B/项)
//     +184 u32 texId  +192 xtab(u32/项 图集X)  +200 ytab(u32/项 图集Y)
//   metrics 16B/项: +0 i32 advance  +4 i32 cell宽  +12 i16 kern起始idx(-1=无)
//   渲染(DrawWide): quad宽=metrics[+4], quad高=font[+22], UV=(xtab,ytab)+cell尺寸
//
//   方案:
//   5. Hook C sub_140859B10 字体对象查询 -> 命中含中文文本的字体时返回"伪字体对象"
//      （count 扩为 0xFFE0 覆盖 0x20..0xFFFF; 官方槽码区 metrics/xtab/ytab 照抄,
//        中文字符槽码=cp-0x20 填新图集坐标; kern 表直接指官方）
//   6. Hook D sub_14085DB30 纹理对象查询 -> MAGIC texId 返回伪纹理对象（宽/高）
//   7. Hook E sub_140915D60 texId->SRV 解析 -> MAGIC texId 返回自建图集 SRV
//      MAGIC = 0x60000000 + fontId: bit24=0 不入动态纹理分支, 远超纹理注册数
//      -> SRV 缓存表(sub_140916DD0)对界外 id 自动跳过读写, 无越界
//   8. 两阶段构建（避免渲染线程长卡顿）:
//      后台线程: stb_truetype 按 cellH 光栅化字符集 -> 位图缓存
//      渲染线程: 首次绘制该字体时读回官方图集(staging) + 拼接中文区
//                + CreateTexture2D/SRV + 组装伪对象（~15ms）
//   9. 官方图集原样 blit 保留 -> 未翻译内容(Credits 等)渲染不变
//
// 安全性:
//   - 伪对象不在 fontTab, 引擎卸载(sub_14085A250)遍历不到, 无双重释放
//   - HookFontLookup 校验 fontTab[slot]==构建时官方指针, 引擎若重建字体自动回退
//   - 词典加载失败 -> 只装文本 hook 未装字体 hook, 行为=v5
//   - hook 安装前比对目标入口 16 字节特征, 防游戏更新后错位

#include "pch.h"
#include <MinHook.h>
#include <d3d11.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

// ---------- 配置 ----------
static constexpr uint64_t GAME_BASE = 0x140000000ULL;

// hook 目标（VA）与入口特征（IDA 2026-09-05 实测）
static constexpr uint64_t VA_DRAW_WIDE   = 0x1408B5FF0ULL;  // (ctx,x,y,text,scale,flag,fontId,arg8)
static constexpr uint64_t VA_FORMAT      = 0x140812610ULL;  // (dst,fmt,cap,args,argc)
static constexpr uint64_t VA_FONT_LOOKUP = 0x140859B10ULL;  // (fontId) -> 字体对象
static constexpr uint64_t VA_TEXOBJ      = 0x14085DB30ULL;  // (texId) -> 纹理对象
static constexpr uint64_t VA_SRV_RESOLVE = 0x140915D60ULL;  // (texId, useStream) -> SRV
static const uint8_t SIG_DRAW_WIDE[16] = {
    0x40,0x53,0x56,0x57,0x48,0x81,0xEC,0x10,0x01,0x00,0x00,0x8B,0xBC,0x24,0x60,0x01 };
static const uint8_t SIG_FORMAT[16] = {
    0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0xAC };
static const uint8_t SIG_FONT_LOOKUP[16] = {
    0x83,0xF9,0xFF,0x7D,0x3E,0x8D,0x81,0xFF,0xFF,0xFF,0x7F,0x83,0xF8,0xFF,0x7E,0x2E };
static const uint8_t SIG_TEXOBJ[16] = {
    0x4C,0x63,0xC1,0x44,0x3B,0x05,0x26,0xAB,0x13,0x02,0x73,0x71,0x4C,0x8B,0x0D,0x0D };
static const uint8_t SIG_SRV_RESOLVE[16] = {
    0x48,0x83,0xEC,0x28,0x4C,0x63,0xC1,0x44,0x0F,0xB6,0xD2,0x41,0x83,0xF8,0xFF,0x0F };

// 引擎全局（VA）
static constexpr uint64_t VA_FONTTAB     = 0x142998168ULL;  // 字体对象指针表
static constexpr uint64_t VA_FONTCOUNT   = 0x142998160ULL;  // 字体数
static constexpr uint64_t VA_D3D_DEVICE  = 0x1430784F8ULL;  // ID3D11Device*
static constexpr uint64_t VA_D3D_CONTEXT = 0x143078500ULL;  // ID3D11DeviceContext*

static constexpr uint32_t MAGIC_TEXID_BASE = 0x60000000u;   // +fontId（bit24=0, 界外）
static constexpr uint32_t FAKE_FONT_MAX    = 256;
static constexpr uint32_t FAKE_GLYPHS      = 0xFFE0u;       // 0x20..0xFFFF 全覆盖
static constexpr uint32_t FONT_BASECHAR_DEFAULT = 0x20u;

static constexpr size_t ARENA_BYTES   = 128u << 20;   // 词典字符串区（几万条译文上限安全值）
static constexpr uint32_t DICT_BUCKETS = 1u << 15;
static constexpr uint32_t ORIG_BUCKETS = 1u << 14;   // origin 联表 16384 桶（1 万+ ID 键）
static constexpr uint32_t DUMP_BUCKETS = 1u << 12;
static constexpr size_t DUMP_MAX_CHARS = 256;
static constexpr DWORD  STATS_PERIOD_MS = 30000;

// ---------- VA -> 本进程地址 ----------
template <typename T = uint8_t*>
static inline T VA(uint64_t va)
{
    return reinterpret_cast<T>(va - GAME_BASE + reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr)));
}

// ---------- 日志 ----------
static FILE*             g_log = nullptr;
static CRITICAL_SECTION  g_logCS;

static void LogOpen(HMODULE hSelf)
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(hSelf, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) *(slash + 1) = L'\0'; else path[0] = L'\0';
    wcscat_s(path, L"SR3R_I18N.log");
    _wfopen_s(&g_log, path, L"wb");
}

static void Log(const char* fmt, ...)
{
    if (!g_log) return;
    EnterCriticalSection(&g_logCS);
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_log, "[%02u:%02u:%02u.%03u T%lu] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logCS);
}

// ---------- 配置（SR3R_I18N.ini, 与 asi 同目录; 缺失时用默认值） ----------
struct Config
{
    wchar_t dictDir[MAX_PATH];    // 词典文件夹（相对 asi 目录）
    wchar_t originDir[MAX_PATH];  // 联表文件夹（ID/HASH_ -> 英文原文; 相对 asi 目录）
    wchar_t fontFile[MAX_PATH];   // 字体 TTF 文件名（相对 asi 目录）
    bool     dumpEnabled;         // 未命中文本收集（DumpText.dtxt）
};

static Config g_cfg = {
    L"dict",                       // 默认: scripts/dict/
    L"origin",                     // 默认: scripts/origin/
    L"SourceHanSansHWSC-VF.ttf",   // 默认字体
    true,
};

// UTF-8 无 BOM/带 BOM ini 行解析（手工实现, 避免路径中文问题）
static void LoadConfig(const wchar_t* iniPath)
{
    HANDLE f = CreateFileW(iniPath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { Log("cfg: %ls not found, defaults", iniPath); return; }
    LARGE_INTEGER sz;
    GetFileSizeEx(f, &sz);
    if (sz.QuadPart <= 0 || sz.QuadPart > (1 << 20)) { CloseHandle(f); return; }
    auto* buf = static_cast<uint8_t*>(VirtualAlloc(nullptr, (SIZE_T)sz.QuadPart,
                                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    DWORD rd = 0;
    BOOL ok = buf && ReadFile(f, buf, (DWORD)sz.QuadPart, &rd, nullptr);
    CloseHandle(f);
    if (!ok) { if (buf) VirtualFree(buf, 0, MEM_RELEASE); return; }

    // UTF-8 -> UTF-16（跳 BOM）
    int utf8Off = (rd >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) ? 3 : 0;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, (const char*)buf + utf8Off,
                                   (int)(rd - utf8Off), nullptr, 0);
    if (wlen <= 0 || wlen > 65536) { VirtualFree(buf, 0, MEM_RELEASE); return; }
    auto* wbuf = static_cast<wchar_t*>(VirtualAlloc(nullptr, (wlen + 1) * sizeof(wchar_t),
                                                    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!wbuf) { VirtualFree(buf, 0, MEM_RELEASE); return; }
    MultiByteToWideChar(CP_UTF8, 0, (const char*)buf + utf8Off, (int)(rd - utf8Off), wbuf, wlen);
    wbuf[wlen] = L'\0';
    VirtualFree(buf, 0, MEM_RELEASE);

    // 逐行取 key=value（忽略节名/注释/空行）
    wchar_t* ctx = nullptr;
    wchar_t* line = wcstok_s(wbuf, L"\r\n", &ctx);
    while (line)
    {
        wchar_t* eq = wcschr(line, L'=');
        if (!eq) { line = wcstok_s(nullptr, L"\r\n", &ctx); continue; }
        *eq = L'\0';
        wchar_t* key = line;
        wchar_t* val = eq + 1;
        // 去首尾空白
        while (*key == L' ' || *key == L'\t') ++key;
        wchar_t* ke = key + wcslen(key);
        while (ke > key && (ke[-1] == L' ' || ke[-1] == L'\t')) *--ke = L'\0';
        while (*val == L' ' || *val == L'\t') ++val;
        wchar_t* ve = val + wcslen(val);
        while (ve > val && (ve[-1] == L' ' || ve[-1] == L'\t')) *--ve = L'\0';

        if (_wcsicmp(key, L"dict_dir") == 0)         wcscpy_s(g_cfg.dictDir, val);
        else if (_wcsicmp(key, L"origin_dir") == 0)  wcscpy_s(g_cfg.originDir, val);
        else if (_wcsicmp(key, L"font_file") == 0)   wcscpy_s(g_cfg.fontFile, val);
        else if (_wcsicmp(key, L"dump_enabled") == 0) g_cfg.dumpEnabled = (*val != L'0');

        line = wcstok_s(nullptr, L"\r\n", &ctx);
    }
    VirtualFree(wbuf, 0, MEM_RELEASE);
    Log("cfg: %ls loaded (dict_dir=%ls origin_dir=%ls font_file=%ls dump=%d)",
        iniPath, g_cfg.dictDir, g_cfg.originDir, g_cfg.fontFile, (int)g_cfg.dumpEnabled);
}

// ---------- CRC-32 (IEEE 反射, 与 zlib.crc32 一致) ----------
static uint32_t g_crcTable[256];

static void CrcInit()
{
    for (uint32_t i = 0; i < 256; ++i)
    {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crcTable[i] = c;
    }
}

static uint32_t CrcText(const wchar_t* s, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(s);
    const size_t n = (len + 1) * 2;
    for (size_t i = 0; i < n; ++i)
        crc = g_crcTable[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// ---------- 词典（开链 CRC 哈希, 常驻 arena, 只读无锁） ----------
struct DictNode
{
    uint32_t      crc;      // 原文 CRC
    uint32_t      len;      // 原文长度（不含 NUL）
    const wchar_t* orig;    // 原文（碰撞校验）
    const wchar_t* trans;   // 译文（返回值）
    DictNode*     next;
    uint8_t       hasCjk;   // 译文含非 ASCII（触发字体升级）
};

static DictNode** g_dictBuckets = nullptr;
static uint32_t   g_dictMask    = 0;
static uint32_t   g_dictCount   = 0;

static uint8_t* g_arena     = nullptr;
static size_t   g_arenaUsed = 0;

static void* ArenaAlloc(size_t n)
{
    n = (n + 15) & ~size_t(15);
    if (g_arenaUsed + n > ARENA_BYTES) return nullptr;
    void* p = g_arena + g_arenaUsed;
    g_arenaUsed += n;
    return p;
}

// 词典加载后置 1（未加载时字形层不激活）
static volatile LONG g_dictReady = 0;

// 中文/非 ASCII 字符集（65536 位 bitmap, 词典译文收集）
static uint8_t  g_charSet[8192];
static uint32_t g_charCount = 0;

// 字符使用频率（词典全文出现次数, RasterizeThread 按频率降序光栅化: 高频字优先入图集）
static uint16_t g_charFreq[65536];

static void CharSetAdd(wchar_t c)
{
    if (c < 0x80) return;
    if (c > 0xFFFD) return;
    uint32_t i = (uint32_t)c;
    if (!(g_charSet[i >> 3] & (1u << (i & 7))))
    {
        g_charSet[i >> 3] |= (uint8_t)(1u << (i & 7));
        ++g_charCount;
    }
    if (g_charFreq[i] < 0xFFFFu) ++g_charFreq[i];   // 频率饱和计数
}

static bool DictInsert(const wchar_t* key, uint32_t keyLen, const wchar_t* trans)
{
    auto* node = static_cast<DictNode*>(ArenaAlloc(sizeof(DictNode)));
    if (!node) return false;
    auto* keyCopy = static_cast<wchar_t*>(ArenaAlloc((keyLen + 1) * sizeof(wchar_t)));
    if (!keyCopy) return false;
    memcpy(keyCopy, key, keyLen * sizeof(wchar_t));
    keyCopy[keyLen] = L'\0';

    node->crc   = CrcText(keyCopy, keyLen);
    node->len   = keyLen;
    node->orig  = keyCopy;
    node->trans = trans;
    node->hasCjk = 0;
    for (const wchar_t* p = trans; *p; ++p)
        if (*p >= 0x80) { node->hasCjk = 1; break; }
    uint32_t h = node->crc & g_dictMask;
    node->next  = g_dictBuckets[h];
    g_dictBuckets[h] = node;
    ++g_dictCount;
    return true;
}

static const DictNode* DictLookup(const wchar_t* s, size_t len)
{
    if (!g_dictBuckets) return nullptr;
    uint32_t crc = CrcText(s, len);
    for (DictNode* n = g_dictBuckets[crc & g_dictMask]; n; n = n->next)
        if (n->crc == crc && n->len == len && wmemcmp(n->orig, s, len) == 0)
            return n;
    return nullptr;
}

// ---------- origin 联表（消息 ID/HASH_ -> 英文原文; 加载后只读无锁, 与词典同型） ----------
// scripts\origin\*.txt: "消息ID或HASH_xxx": "英文明文"
// 引擎运行时绘制的是解析后的英文明文（DumpText 实证）, 消息 ID 不会到达 hook 层;
// 词典键为消息 ID 时经此表消解为英文原文再入主表。
struct OrigNode
{
    uint32_t      crc;      // 键（消息 ID/HASH_）CRC
    uint32_t      len;      // 英文原文长度（不含 NUL）
    const wchar_t* key;     // 键（碰撞校验）
    const wchar_t* val;     // 英文原文
    OrigNode*     next;
};

static OrigNode** g_origBuckets = nullptr;
static uint32_t   g_origMask    = 0;
static uint32_t   g_origCount   = 0;

static bool OrigInsert(const wchar_t* key, uint32_t keyLen, const wchar_t* val, uint32_t valLen)
{
    auto* node = static_cast<OrigNode*>(ArenaAlloc(sizeof(OrigNode)));
    if (!node) return false;
    auto* keyCopy = static_cast<wchar_t*>(ArenaAlloc((keyLen + 1) * sizeof(wchar_t)));
    if (!keyCopy) return false;
    auto* valCopy = static_cast<wchar_t*>(ArenaAlloc((valLen + 1) * sizeof(wchar_t)));
    if (!valCopy) return false;
    memcpy(keyCopy, key, keyLen * sizeof(wchar_t));
    keyCopy[keyLen] = L'\0';
    memcpy(valCopy, val, valLen * sizeof(wchar_t));
    valCopy[valLen] = L'\0';

    node->crc  = CrcText(keyCopy, keyLen);
    node->len  = valLen;
    node->key  = keyCopy;
    node->val  = valCopy;
    uint32_t h = node->crc & g_origMask;
    node->next = g_origBuckets[h];
    g_origBuckets[h] = node;
    ++g_origCount;
    return true;
}

static const OrigNode* OrigLookup(const wchar_t* s, size_t len)
{
    if (!g_origBuckets) return nullptr;
    uint32_t crc = CrcText(s, len);
    for (OrigNode* n = g_origBuckets[crc & g_origMask]; n; n = n->next)
        if (n->crc == crc && n->len == len && wmemcmp(n->key, s, len) == 0)
            return n;
    return nullptr;
}

static bool TrimRange(const wchar_t* s, size_t len, size_t* outStart, size_t* outLen)
{
    size_t b = 0, e = len;
    while (b < e && s[b] <= 0x20) ++b;
    while (e > b && s[e - 1] <= 0x20) --e;
    if (b >= e) return false;
    *outStart = b;
    *outLen   = e - b;
    return true;
}

// ---------- DumpText（未命中收集, SRWLOCK 保护） ----------
struct DumpNode { uint32_t crc; DumpNode* next; };

static DumpNode** g_dumpBuckets = nullptr;
static SRWLOCK    g_dumpLock    = SRWLOCK_INIT;
static FILE*      g_dumpFile    = nullptr;
static uint32_t   g_dumpCount   = 0;

static bool DumpWorthy(const wchar_t* s, size_t len)
{
    if (len < 2 || len > DUMP_MAX_CHARS) return false;
    for (size_t i = 0; i < len; ++i)
    {
        wchar_t c = s[i];
        if (c >= 0x2E80) return false;
        if (c < 0x20 && c != L'\n' && c != L'\t') return false;
    }
    return true;
}

static void DumpText(const wchar_t* s, size_t len)
{
    if (!g_cfg.dumpEnabled) return;
    uint32_t crc = CrcText(s, len);
    AcquireSRWLockExclusive(&g_dumpLock);
    if (g_dumpBuckets && g_dumpFile)
    {
        bool seen = false;
        for (DumpNode* n = g_dumpBuckets[crc & (DUMP_BUCKETS - 1)]; n; n = n->next)
            if (n->crc == crc) { seen = true; break; }
        if (!seen)
        {
            auto* node = static_cast<DumpNode*>(ArenaAlloc(sizeof(DumpNode)));
            if (node)
            {
                node->crc  = crc;
                uint32_t h = crc & (DUMP_BUCKETS - 1);
                node->next = g_dumpBuckets[h];
                g_dumpBuckets[h] = node;
                ++g_dumpCount;

                char utf8[DUMP_MAX_CHARS * 3 + 8];
                int u = WideCharToMultiByte(CP_UTF8, 0, s, (int)len,
                                            utf8 + 1, sizeof(utf8) - 3, nullptr, nullptr);
                if (u > 0)
                {
                    utf8[0] = '"';
                    int w = u + 1;
                    utf8[w++] = '"';
                    utf8[w++] = '\n';
                    // 只写不刷: 逐条 fflush 同步磁盘 I/O 会卡渲染线程（切界面时新 miss 集中涌入）,
                    // 改由 StatsThread 周期 fflush + 进程退出 fclose 落盘（崩溃最多丢 STATS_PERIOD_MS 内的收集）
                    fwrite(utf8, 1, w, g_dumpFile);
                }
            }
        }
    }
    ReleaseSRWLockExclusive(&g_dumpLock);
}

// ---------- Hook 共用: 查词典 + miss 统计/dump ----------
static volatile LONG g_hitA = 0, g_missA = 0, g_hitB = 0, g_missB = 0;

static const DictNode* LookupNode(const wchar_t* s, volatile LONG* hit, volatile LONG* miss)
{
    if (!s || !*s) return nullptr;
    size_t len = wcslen(s);
    if (len > DUMP_MAX_CHARS * 4) return nullptr;

    const DictNode* r = DictLookup(s, len);
    if (r) { InterlockedIncrement(hit); return r; }

    size_t b, tl;
    if (TrimRange(s, len, &b, &tl))
    {
        if (tl <= 512)
        {
            wchar_t tmp[513];
            wmemcpy(tmp, s + b, tl);
            tmp[tl] = L'\0';
            r = DictLookup(tmp, tl);
            if (r) { InterlockedIncrement(hit); return r; }
        }
    }

    InterlockedIncrement(miss);
    DumpText(s, len);
    return nullptr;
}

// =====================================================================
// v6 字形层
// =====================================================================

// 伪字体（每 fontId 一个, 两阶段构建）
//   state: 0=idle 1=后台光栅化中 2=live 3=光栅化完待D3D 4=失败
struct FakeFont
{
    volatile LONG   state;
    uint32_t        fontId;
    void*           official;   // 构建时的官方对象（fontTab[slot] 校验用）
    void*           obj;        // 伪字体对象 blob
    ID3D11ShaderResourceView* srv;
    ID3D11Texture2D*           tex;
    // 光栅化产物（后台线程填, D3D 阶段消费后释放）
    uint8_t*  cellBuf;          // nCells * cellW * cellH 灰度
    int32_t*  advances;         // nCells
    uint32_t* cps;              // nCells
    uint32_t  nCells;
    uint32_t  blankY;           // 预留空白 cell 的图集 Y（缺字槽位指到这里, 超容量字符空白渲染）
    uint16_t  cellW, cellH;
    // 伪纹理对象（sub_14085D930 读 +8/+10 宽高; +20 变体数; +34 速度）
    uint8_t   fakeTexObj[64];
};
static FakeFont g_fake[FAKE_FONT_MAX];

// stb 字体状态
static uint8_t       g_ttfData[1];  // 占位（实际 VirtualAlloc 到 g_ttfBuf）
static uint8_t*      g_ttfBuf = nullptr;
static stbtt_fontinfo g_stb;
static volatile LONG g_stbReady = 0;

// 引擎指针（安装时解析）
static void***        g_fontTabPtr   = nullptr;  // -> qword_142998168
static volatile int*  g_fontCountPtr = nullptr;  // -> dword_142998160
static ID3D11Device**        g_devSlot  = nullptr;
static ID3D11DeviceContext** g_ctxSlot  = nullptr;

// 原函数
using FontLookup_t = void* (__fastcall*)(int);
using TexObj_t     = void* (__fastcall*)(int);
using SrvResolve_t = void* (__fastcall*)(unsigned int, char);
static FontLookup_t g_origFontLookup = nullptr;
static TexObj_t     g_origTexObj     = nullptr;
static SrvResolve_t g_origSrvResolve = nullptr;

// 官方对象 -> fontTab 槽位号（找不到返回 0xFFFFFFFF）
static uint32_t ResolveSlot(void* off);
static bool     FinishFont(FakeFont* f);
static void     RequestFont(uint32_t slot);

// ---------- Hook C: 字体对象查询（两渲染器公共必经点） ----------
// 触发升级 + 伪对象替换
// ptr 缓存（官方对象 -> FakeFont）, 高频路径避免线性扫 fontTab
struct PtrCache { void* off; FakeFont* f; };
static PtrCache     g_ptrCache[16];
static volatile LONG g_ptrCacheN = 0;

static void EnsureFontReady(void* off)
{
    LONG n = g_ptrCacheN; if (n > 16) n = 16;
    for (LONG i = 0; i < n; ++i)
    {
        if (g_ptrCache[i].off != off) continue;
        return;   // D3D 阶段已移至后台线程, 渲染线程只读 state 不推进（防卡顿）
    }
    uint32_t slot = ResolveSlot(off);
    if (slot >= FAKE_FONT_MAX) return;
    FakeFont* f = &g_fake[slot];
    LONG idx = InterlockedIncrement(&g_ptrCacheN) - 1;
    if (idx < 16) { g_ptrCache[idx].off = off; g_ptrCache[idx].f = f; }
    if (f->state == 0) RequestFont(slot);
}

static void* __fastcall HookFontLookup(int fontId)
{
    void* off = g_origFontLookup(fontId);
    if (!off) return off;

    // 触发/推进升级（菜单文本走 sub_14016E7D0 渲染器, 不经 DrawWide,
    // 故在此公共必经点触发; 英文渲染不受影响, 官方 cell 照抄）
    if (g_stbReady && g_dictReady) EnsureFontReady(off);

    // 已 live 的伪对象替换（校验官方对象仍在槽位）
    uint32_t slot = (fontId >= 0 && (uint32_t)fontId < FAKE_FONT_MAX)
                    ? (uint32_t)fontId : ResolveSlot(off);
    if (slot < FAKE_FONT_MAX)
    {
        FakeFont* f = &g_fake[slot];
        if (f->state == 2)
        {
            void* cur = (*g_fontTabPtr)[slot];
            if (cur == f->official) return f->obj;
            f->state = 4;   // 引擎重建了字体, 回退官方
        }
    }
    return off;
}

// ---------- Hook D: 纹理对象查询 ----------
static void* __fastcall HookTexObj(int texId)
{
    uint32_t t = (uint32_t)texId;
    if (t >= MAGIC_TEXID_BASE && t < MAGIC_TEXID_BASE + FAKE_FONT_MAX)
    {
        FakeFont* f = &g_fake[t - MAGIC_TEXID_BASE];
        if (f->state == 2) return f->fakeTexObj;
    }
    return g_origTexObj(texId);
}

// ---------- Hook E: texId -> SRV ----------
static void* __fastcall HookSrvResolve(unsigned int texId, char useStream)
{
    if (texId >= MAGIC_TEXID_BASE && texId < MAGIC_TEXID_BASE + FAKE_FONT_MAX)
    {
        FakeFont* f = &g_fake[texId - MAGIC_TEXID_BASE];
        if (f->state == 2 && f->srv) return f->srv;
        return nullptr;
    }
    return g_origSrvResolve(texId, useStream);
}

// ---------- 后台阶段: 光栅化字符集 ----------
static DWORD WINAPI RasterizeThread(LPVOID arg)
{
    FakeFont* f = static_cast<FakeFont*>(arg);
    uint32_t fontId = f->fontId;

    void* off = g_origFontLookup((int)fontId);
    if (!off) { Log("font%u: official object null, abort", fontId); f->state = 4; return 0; }

    // 读官方头
    int      offCount  = *(int*)( (uint8_t*)off + 8);
    int      offBase   = *(int*)( (uint8_t*)off + 12);
    uint16_t cellH     = *(uint16_t*)((uint8_t*)off + 22);
    if (offCount <= 0 || offCount >= 0x10000 || cellH < 8 || cellH > 256)
    {
        Log("font%u: bad header count=%d cellH=%u, abort", fontId, offCount, cellH);
        f->state = 4; return 0;
    }

    uint16_t cellW = cellH;  // 方块字 1:1
    f->cellW = cellW; f->cellH = cellH;
    f->official = off;

    // 收集字符集 -> 列表（按词典使用频率降序: 高频字优先入图集, 低频字容量不足时被截断）
    uint32_t total = g_charCount;
    f->cps      = static_cast<uint32_t*>(malloc(sizeof(uint32_t) * (total ? total : 1)));
    f->advances = static_cast<int32_t*>(malloc(sizeof(int32_t) * (total ? total : 1)));
    f->cellBuf  = static_cast<uint8_t*>(malloc((size_t)cellW * cellH * (total ? total : 1)));
    if (!f->cps || !f->advances || !f->cellBuf)
    {
        Log("font%u: raster alloc failed", fontId);
        f->state = 4; return 0;
    }
    memset(f->cellBuf, 0, (size_t)cellW * cellH * total);

    uint32_t n = 0;
    for (uint32_t cp = 0x80; cp <= 0xFFFD && n < total; ++cp)
    {
        if (!(g_charSet[cp >> 3] & (1u << (cp & 7)))) continue;
        f->cps[n++] = cp;
    }
    // 插入排序按频率降序（数组初始升序, 近乎有序时接近 O(n)）
    for (uint32_t i = 1; i < n; ++i)
    {
        uint32_t kc = f->cps[i];
        uint32_t kf = g_charFreq[kc];
        uint32_t j = i;
        while (j > 0 && g_charFreq[f->cps[j - 1]] < kf)
        {
            f->cps[j] = f->cps[j - 1];
            --j;
        }
        f->cps[j] = kc;
    }

    // 光栅化
    float scale = stbtt_ScaleForPixelHeight(&g_stb, (float)cellH);
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&g_stb, &ascent, &descent, &gap);
    int baseline = (int)((float)ascent * scale + 0.5f);
    if (baseline > cellH - 1) baseline = cellH - 1;
    if (baseline < 1) baseline = 1;

    uint32_t missGlyph = 0;
    for (uint32_t idx = 0; idx < n; ++idx)
    {
        uint32_t cp = f->cps[idx];
        uint8_t* cell = f->cellBuf + (size_t)idx * cellW * cellH;

        int g = stbtt_FindGlyphIndex(&g_stb, (int)cp);
        if (g == 0)
        {
            ++missGlyph;
            f->advances[idx] = cellW;   // 无字形: 空白格
            continue;
        }

        int x0, y0, x1, y1;
        stbtt_GetGlyphBitmapBox(&g_stb, g, scale, scale, &x0, &y0, &x1, &y1);
        int w = x1 - x0, h = y1 - y0;
        int ox = ((int)cellW - w) / 2;
        int oy = baseline + y0;

        int adv;
        stbtt_GetGlyphHMetrics(&g_stb, g, &adv, nullptr);
        f->advances[idx] = adv > 0 ? (int)((float)adv * scale + 0.5f) : cellW;
        if (f->advances[idx] <= 0) f->advances[idx] = cellW / 2;

        if (w > 0 && h > 0)
        {
            // 光栅化到临时再拷入 cell（裁剪到 cell 内）
            uint8_t* tmp = static_cast<uint8_t*>(malloc((size_t)w * h));
            if (tmp)
            {
                stbtt_MakeGlyphBitmap(&g_stb, tmp, w, h, w, scale, scale, g);
                for (int row = 0; row < h; ++row)
                {
                    int dy = oy + row;
                    if (dy < 0 || dy >= cellH) continue;
                    for (int col = 0; col < w; ++col)
                    {
                        int dx = ox + col;
                        if (dx < 0 || dx >= cellW) continue;
                        cell[dy * cellW + dx] = tmp[row * w + col];
                    }
                }
                free(tmp);
            }
        }
    }
    f->nCells = n;

    if (offBase != (int)FONT_BASECHAR_DEFAULT)
        Log("font%u: warn official baseChar=%d != 0x20", fontId, offBase);
    Log("font%u: rasterized %u cells (cellW=%u cellH=%u baseline=%d, missGlyph=%u, official count=%d)",
        fontId, n, cellW, cellH, baseline, missGlyph, offCount);

    InterlockedExchange(&f->state, 3);  // 待 D3D 阶段

    // D3D 阶段也在本后台线程完成（FinishFont 内 CAS 3->5 防并发）:
    // 原设计在渲染线程做, 官方图集读回+大纹理上传会卡主界面首帧; 后台做完后伪对象才 live,
    // 未就绪期间引擎用官方字体渲染中文 -> 槽码越界被 sub_140858C10 边界检查挡住 -> 安全空白回退
    // 暂时性失败（D3D/官方SRV未就绪, state 回 3）: 后台重试至多 30s
    for (int retry = 0; retry < 300; ++retry)
    {
        if (FinishFont(f)) return 0;
        if (f->state != 3) return 0;   // 永久失败(4)/已被别的线程完成, 不再重试
        Sleep(100);
    }
    Log("font%u: D3D stage gave up after 30s (state=%ld)", fontId, f->state);
    return 0;
}

// ---------- DXGI 格式辅助（v6.4: 官方图集读回格式感知） ----------
// 返回每像素字节数; 压缩/未知格式返回 0
static uint32_t BppOf(DXGI_FORMAT fmt)
{
    switch (fmt)
    {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return 4;
    case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM:
    case DXGI_FORMAT_B4G4R4A4_UNORM:
        return 2;
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_A8_UNORM:
        return 1;
    default:
        return 0;
    }
}

static bool IsBcFormat(DXGI_FORMAT fmt)
{
    switch (fmt)
    {
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_SNORM:
        return true;
    default:
        return false;
    }
}

static const char* FmtName(DXGI_FORMAT fmt)
{
    switch (fmt)
    {
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
    case DXGI_FORMAT_B8G8R8X8_UNORM: return "BGRX8";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
    case DXGI_FORMAT_B5G6R5_UNORM:   return "B5G6R5";
    case DXGI_FORMAT_B5G5R5A1_UNORM: return "B5G5R5A1";
    case DXGI_FORMAT_B4G4R4A4_UNORM: return "B4G4R4A4";
    case DXGI_FORMAT_R8_UNORM:       return "R8";
    case DXGI_FORMAT_A8_UNORM:       return "A8";
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM_SRGB: return "BC1";
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM_SRGB: return "BC2";
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM_SRGB: return "BC3";
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_SNORM:      return "BC4";
    default:                          return "?";
    }
}

// BC 块字节数
static uint32_t BcBlockBytes(DXGI_FORMAT fmt)
{
    // BC1/BC4: 8B/块; BC2/BC3: 16B/块
    return (fmt == DXGI_FORMAT_BC1_UNORM || fmt == DXGI_FORMAT_BC1_TYPELESS ||
            fmt == DXGI_FORMAT_BC1_UNORM_SRGB ||
            fmt == DXGI_FORMAT_BC4_UNORM || fmt == DXGI_FORMAT_BC4_TYPELESS ||
            fmt == DXGI_FORMAT_BC4_SNORM)
               ? 8u : 16u;
}

static inline uint32_t Rgb565(uint16_t v, uint8_t* r, uint8_t* g, uint8_t* b)
{
    *r = (uint8_t)(((v >> 11) & 0x1F) * 255 / 31);
    *g = (uint8_t)(((v >> 5)  & 0x3F) * 255 / 63);
    *b = (uint8_t)(( v        & 0x1F) * 255 / 31);
    return 0;
}

// BC1(DXT1) 单块解码 -> 4x4 BGRA
static void DecodeBc1(const uint8_t* blk, uint32_t* out16)
{
    uint16_t c0 = blk[0] | (blk[1] << 8);
    uint16_t c1 = blk[2] | (blk[3] << 8);
    uint8_t r0, g0, b0, r1, g1, b1;
    Rgb565(c0, &r0, &g0, &b0);
    Rgb565(c1, &r1, &g1, &b1);
    uint32_t pal[4];
    pal[0] = 0xFF000000u | (b0 << 16) | (g0 << 8) | r0;
    pal[1] = 0xFF000000u | (b1 << 16) | (g1 << 8) | r1;
    if (c0 > c1)
    {
        pal[2] = 0xFF000000u | ((((b0 + b0 + b1) / 3) & 0xFF) << 16)
                           | ((((g0 + g0 + g1) / 3) & 0xFF) << 8)
                           | (((r0 + r0 + r1) / 3) & 0xFF);
        pal[3] = 0xFF000000u | (((b0 + b1 + b1) / 3) << 16)
                           | (((g0 + g1 + g1) / 3) << 8)
                           | (((r0 + r1 + r1) / 3) & 0xFF);
    }
    else
    {
        pal[2] = 0xFF000000u | (((b0 + b1) / 2) << 16) | (((g0 + g1) / 2) << 8) | ((r0 + r1) / 2);
        pal[3] = 0x00000000u;   // 透明黑
    }
    for (int i = 0; i < 4; ++i)
    {
        uint32_t bits = blk[4 + i];
        for (int j = 0; j < 4; ++j)
            out16[i * 4 + j] = pal[(bits >> (j * 2)) & 3];
    }
}

// BC2(DXT3) 单块解码: 显式 4bit alpha + BC1 色
static void DecodeBc2(const uint8_t* blk, uint32_t* out16)
{
    DecodeBc1(blk + 8, out16);
    for (int i = 0; i < 16; ++i)   // 像素 i 的 4bit alpha: 字节 i>>1, 半字节 (i&1)*4
    {
        uint8_t byte = blk[i >> 1];
        uint8_t nib  = (i & 1) ? (uint8_t)(byte >> 4) : (uint8_t)(byte & 0xF);
        uint8_t a    = (uint8_t)(nib * 17);   // 0..15 -> 0..255
        out16[i] = (out16[i] & 0x00FFFFFFu) | ((uint32_t)a << 24);
    }
}

// BC3(DXT5) 单块解码: 8-alpha 插值 + BC1 色
static void DecodeBc3(const uint8_t* blk, uint32_t* out16)
{
    DecodeBc1(blk + 8, out16);
    uint8_t a[8];
    a[0] = blk[0];
    a[1] = blk[1];
    if (a[0] > a[1])
    {
        for (int i = 0; i < 6; ++i) a[2 + i] = (uint8_t)(((6 - i) * a[0] + (1 + i) * a[1]) / 7);
    }
    else
    {
        for (int i = 0; i < 4; ++i) a[2 + i] = (uint8_t)(((4 - i) * a[0] + (1 + i) * a[1]) / 5);
        a[6] = 0; a[7] = 255;
    }
    // 16 个 3bit 索引, 48bit 从 blk[2..7], 每像素低位在前（跨字节时拼两字节, 尾块不越界）
    for (int i = 0; i < 16; ++i)
    {
        int bit = i * 3;
        int byteIdx = 2 + (bit >> 3);
        uint32_t v = blk[byteIdx];
        if (byteIdx < 7 && (bit & 7) > 5) v |= (uint32_t)blk[byteIdx + 1] << 8;
        uint8_t al = a[(v >> (bit & 7)) & 7];
        out16[i] = (out16[i] & 0x00FFFFFFu) | ((uint32_t)al << 24);
    }
}

// BC4 单块解码 -> 4x4, 取 R 通道复制到 BGRA（灰度语义）
static void DecodeBc4(const uint8_t* blk, uint32_t* out16)
{
    uint8_t r[8];
    r[0] = blk[0];
    r[1] = blk[1];
    if (r[0] > r[1])
    {
        for (int i = 0; i < 6; ++i) r[2 + i] = (uint8_t)(((6 - i) * r[0] + (1 + i) * r[1]) / 7);
    }
    else
    {
        for (int i = 0; i < 4; ++i) r[2 + i] = (uint8_t)(((4 - i) * r[0] + (1 + i) * r[1]) / 5);
        r[6] = 0; r[7] = 255;
    }
    for (int i = 0; i < 16; ++i)
    {
        int bit = i * 3;
        int byteIdx = 2 + (bit >> 3);
        uint32_t v = blk[byteIdx];
        if (byteIdx < 7 && (bit & 7) > 5) v |= (uint32_t)blk[byteIdx + 1] << 8;
        uint8_t g = r[(v >> (bit & 7)) & 7];
        out16[i] = 0xFF000000u | ((uint32_t)g << 16) | ((uint32_t)g << 8) | g;
    }
}

// BC 纹理按块行解码: 解一个 4 像素高条带（每块只解一次）, 写入 atlas 的 [y0,y0+4) 行
// （宽 W 裁剪, 高 hMax 裁剪; atlas 为 BGRA, pitch 字节）
static void DecodeBcStrip(DXGI_FORMAT fmt, const uint8_t* src, uint32_t srcRowBytes,
                          uint32_t blockY, uint32_t W, uint32_t hMax,
                          uint8_t* atlas, uint32_t pitch)
{
    uint32_t blocksX = (W + 3) >> 2;
    uint32_t bb      = BcBlockBytes(fmt);
    const uint8_t* rowBlk = src + (SIZE_T)blockY * srcRowBytes;
    uint32_t y0 = blockY * 4;
    uint32_t tmp[16];
    for (uint32_t bx = 0; bx < blocksX; ++bx)
    {
        const uint8_t* blk = rowBlk + (SIZE_T)bx * bb;
        switch (fmt)
        {
        case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM_SRGB:
            DecodeBc1(blk, tmp); break;
        case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM_SRGB:
            DecodeBc2(blk, tmp); break;
        case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM_SRGB:
            DecodeBc3(blk, tmp); break;
        case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_TYPELESS:
            DecodeBc4(blk, tmp); break;
        default:
            memset(tmp, 0, sizeof(tmp)); break;
        }
        uint32_t x0 = bx * 4;
        for (uint32_t r = 0; r < 4; ++r)
        {
            uint32_t y = y0 + r;
            if (y >= hMax) break;
            uint32_t* dst = (uint32_t*)(atlas + (SIZE_T)y * pitch);
            for (uint32_t c = 0; c < 4; ++c)
                if (x0 + c < W) dst[x0 + c] = tmp[r * 4 + c];
        }
    }
}

// ---------- 渲染线程阶段: 官方图集读回 + 拼接 + D3D 创建 + 伪对象组装 ----------
static bool FinishFont(FakeFont* f)
{
    // 并发守卫: 只允许一个线程从 state 3 进入构建（5=building）
    if (InterlockedCompareExchange(&f->state, 5, 3) != 3) return false;
    uint32_t fontId = f->fontId;
    uint8_t* off = static_cast<uint8_t*>(f->official);

    ID3D11Device* dev = *g_devSlot;
    ID3D11DeviceContext* ctx = *g_ctxSlot;
    if (!dev || !ctx) { Log("font%u: d3d not ready, retry later", fontId); f->state = 3; return false; }

    int      offCount = *(int*)(off + 8);
    uint32_t offTexId = *(uint32_t*)(off + 184);
    void*    offMet   = *(void**)(off + 176);
    void*    offXtab  = *(void**)(off + 192);
    void*    offYtab  = *(void**)(off + 200);
    if (!offMet || !offXtab || !offYtab || offTexId == 0xFFFFFFFFu)
    { Log("font%u: official blob bad, abort", fontId); f->state = 4; return false; }

    // 官方 SRV -> 纹理 -> 尺寸/格式
    ID3D11ShaderResourceView* offSrv =
        static_cast<ID3D11ShaderResourceView*>(g_origSrvResolve(offTexId, 0));
    if (!offSrv) { Log("font%u: official SRV null (texId=%u), retry", fontId, offTexId); f->state = 3; return false; }

    ID3D11Resource* res = nullptr;
    offSrv->GetResource(&res);
    ID3D11Texture2D* srcTex = static_cast<ID3D11Texture2D*>(res);
    if (!srcTex) { Log("font%u: official texture null", fontId); f->state = 4; return false; }

    D3D11_TEXTURE2D_DESC dd{};
    srcTex->GetDesc(&dd);

    uint32_t W = dd.Width;
    uint32_t offW = W;               // 官方图集宽（读回官方区按此宽; 加宽后官方区只占伪图集左侧）
    uint32_t offH = dd.Height;
    uint32_t perRow = W / f->cellW;
    if (perRow == 0) { Log("font%u: atlas width %u < cellW %u, abort", fontId, W, f->cellW); srcTex->Release(); f->state = 4; return false; }
    uint32_t nCellsWanted = f->nCells;   // 截断前记录（日志用）
    uint32_t rows = (f->nCells + perRow - 1) / perRow;
    uint32_t maxRows = (16384 - offH) / f->cellH - 1;   // 末尾恒留 1 行空白 cell（缺字槽位指向这里）
    if (rows > maxRows && W < 16384)
    {
        // 显存换全字覆盖: 加宽伪图集减少截断
        // （font1 官方 4096 宽仅 18 列; 8192 宽 37 列仍不够 2656 字, 16384 宽 74 列上限 4736 字;
        //   BGRA8 16384 宽满高上限约 1GB, 实际 font1 约 670MB, 现代 8G 显卡无压力）
        uint32_t oldW = W;
        W = 16384;
        perRow = W / f->cellW;
        rows = (f->nCells + perRow - 1) / perRow;
        maxRows = (16384 - offH) / f->cellH - 1;
        Log("font%u: atlas widened %u -> %u (perRow %u -> %u)", fontId, oldW, W, oldW / f->cellW, perRow);
    }
    if (rows > maxRows)
    {
        rows = maxRows;
        f->nCells = rows * perRow;   // 截断低频字（数组已按频率降序, 尾部被截）
        Log("font%u: atlas cells clamped %u -> %u (low-freq chars blank)",
            fontId, nCellsWanted, f->nCells);
    }
    uint32_t H = offH + (rows + 1) * f->cellH;
    f->blankY = offH + rows * f->cellH;   // 空白行: 图集该区已 memset 0
    if (H > 16384) { Log("font%u: H=%u overflow, abort", fontId, H); srcTex->Release(); f->state = 4; return false; }

    // 拼接 buffer
    uint32_t pitch = W * 4;
    uint8_t* atlas = static_cast<uint8_t*>(VirtualAlloc(nullptr, (SIZE_T)pitch * H,
                                                         MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!atlas) { Log("font%u: atlas alloc failed", fontId); srcTex->Release(); f->state = 4; return false; }
    memset(atlas, 0, (SIZE_T)pitch * H);

    // 1) 读回官方图集（staging）
    D3D11_TEXTURE2D_DESC sd = dd;
    sd.Usage          = D3D11_USAGE_STAGING;
    sd.BindFlags      = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags      = 0;
    ID3D11Texture2D* stag = nullptr;
    if (FAILED(dev->CreateTexture2D(&sd, nullptr, &stag)))
    { Log("font%u: staging create failed", fontId); VirtualFree(atlas, 0, MEM_RELEASE); srcTex->Release(); f->state = 4; return false; }
    ctx->CopyResource(stag, srcTex);
    srcTex->Release();

    D3D11_MAPPED_SUBRESOURCE ms{};
    if (FAILED(ctx->Map(stag, 0, D3D11_MAP_READ, 0, &ms)))
    {
        Log("font%u: staging map failed, abort upgrade", fontId);
        stag->Release();
        VirtualFree(atlas, 0, MEM_RELEASE);
        f->state = 4;
        return false;
    }

    // 格式信息前置记录（拷贝循环前, 崩溃也能拿到; 宽高指官方源图集）
    Log("font%u: src atlas %ux%u fmt=%s(%u) mips=%u arr=%u RowPitch=%u",
        fontId, offW, offH, FmtName(dd.Format), (unsigned)dd.Format,
        dd.MipLevels, dd.ArraySize, ms.RowPitch);

    // 格式感知读回 -> atlas 统一为 BGRA8
    bool     bc  = IsBcFormat(dd.Format);
    uint32_t bpp = BppOf(dd.Format);
    if (!bc && bpp == 0)
    {
        Log("font%u: unsupported format %s(%u), graceful abort (official font kept)",
            fontId, FmtName(dd.Format), (unsigned)dd.Format);
        ctx->Unmap(stag, 0);
        stag->Release();
        VirtualFree(atlas, 0, MEM_RELEASE);
        f->state = 4;
        return false;
    }

    if (bc)
    {
        uint32_t blocksY = (offH + 3) >> 2;
        for (uint32_t by = 0; by < blocksY; ++by)
            DecodeBcStrip(dd.Format, (const uint8_t*)ms.pData, ms.RowPitch,
                          by, offW, offH, atlas, pitch);   // 源宽 offW, 写入加宽后的 pitch
    }
    else if (bpp == 4)
    {
        for (uint32_t row = 0; row < offH; ++row)
            memcpy(atlas + (SIZE_T)row * pitch,
                   (const uint8_t*)ms.pData + (SIZE_T)row * ms.RowPitch, (SIZE_T)offW * 4);
    }
    else if (bpp == 2)
    {
        for (uint32_t row = 0; row < offH; ++row)
        {
            const uint16_t* s = (const uint16_t*)((const uint8_t*)ms.pData + (SIZE_T)row * ms.RowPitch);
            uint32_t* d = (uint32_t*)(atlas + (SIZE_T)row * pitch);
            for (uint32_t x = 0; x < offW; ++x)
            {
                uint16_t v = s[x];
                uint8_t r = (uint8_t)(((v >> 11) & 0x1F) * 255 / 31);
                uint8_t g = (uint8_t)(((v >> 5)  & 0x3F) * 255 / 63);
                uint8_t b = (uint8_t)(( v        & 0x1F) * 255 / 31);
                uint8_t a = (dd.Format == DXGI_FORMAT_B5G5R5A1_UNORM) ? (uint8_t)(((v >> 15) & 1) * 255)
                          : (dd.Format == DXGI_FORMAT_B4G4R4A4_UNORM) ? (uint8_t)(((v >> 12) & 0xF) * 17)
                          : 255;
                d[x] = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
            }
        }
    }
    else   // bpp == 1 (R8/A8): 灰度展开, 覆盖值进全部通道（兼容任意采样通道）
    {
        for (uint32_t row = 0; row < offH; ++row)
        {
            const uint8_t* s = (const uint8_t*)ms.pData + (SIZE_T)row * ms.RowPitch;
            uint32_t* d = (uint32_t*)(atlas + (SIZE_T)row * pitch);
            for (uint32_t x = 0; x < offW; ++x)
            {
                uint32_t c = s[x];
                d[x] = (c << 24) | (c << 16) | (c << 8) | c;
            }
        }
    }
    ctx->Unmap(stag, 0);
    stag->Release();

    // 官方图集像素样本（调试: 判断字形存储格式/覆盖通道语义）
    {
        uint32_t sx = *(uint32_t*)((uint8_t*)offXtab + 4 * ('W' - 0x20));
        uint32_t sy = *(uint32_t*)((uint8_t*)offYtab + 4 * ('W' - 0x20));
        if (sx + 8 < offW && sy + 8 < offH)
        {
            uint32_t* px = (uint32_t*)(atlas + (SIZE_T)sy * pitch + (SIZE_T)sx * 4);
            Log("font%u: sample W@(%u,%u): %08X %08X %08X %08X",
                fontId, sx, sy, px[0], px[1], px[pitch / 8], px[pitch / 8 + 1]);
        }
    }

    // 2) 中文区 blit（灰度 -> BGRA, 覆盖值进全部通道: 无论 shader 采 .r/.a 都正确）
    for (uint32_t i = 0; i < f->nCells; ++i)
    {
        uint32_t cx = (i % perRow) * f->cellW;
        uint32_t cy = offH + (i / perRow) * f->cellH;
        const uint8_t* cell = f->cellBuf + (SIZE_T)i * f->cellW * f->cellH;
        for (uint32_t r = 0; r < f->cellH; ++r)
        {
            uint32_t* dst = (uint32_t*)(atlas + (SIZE_T)(cy + r) * pitch + (SIZE_T)cx * 4);
            const uint8_t* src = cell + (SIZE_T)r * f->cellW;
            for (uint32_t c = 0; c < f->cellW; ++c)
            {
                uint32_t cov = src[c];
                dst[c] = (cov << 24) | (cov << 16) | (cov << 8) | cov;
            }
        }
    }

    // 3) 创建纹理 + SRV（显式 BGRA8: 不继承官方压缩格式, 上传数据即 atlas 布局）
    D3D11_TEXTURE2D_DESC nd = dd;
    nd.Width     = W;
    nd.Height    = H;
    nd.MipLevels = 1;
    nd.ArraySize = 1;
    nd.Format          = DXGI_FORMAT_B8G8R8A8_UNORM;
    nd.SampleDesc.Count = 1;
    nd.SampleDesc.Quality = 0;
    nd.Usage          = D3D11_USAGE_DEFAULT;
    nd.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
    nd.CPUAccessFlags = 0;
    nd.MiscFlags      = 0;
    D3D11_SUBRESOURCE_DATA initData{ atlas, pitch, 0 };
    HRESULT hr = dev->CreateTexture2D(&nd, &initData, &f->tex);
    VirtualFree(atlas, 0, MEM_RELEASE);
    if (FAILED(hr)) { Log("font%u: CreateTexture2D failed hr=%08X", fontId, (unsigned)hr); f->state = 4; return false; }
    hr = dev->CreateShaderResourceView(f->tex, nullptr, &f->srv);
    if (FAILED(hr)) { Log("font%u: CreateSRV failed hr=%08X", fontId, (unsigned)hr); f->tex->Release(); f->tex = nullptr; f->state = 4; return false; }

    // 4) 伪字体对象 blob: 208B 头 + metrics(16B) + xtab(4B) + ytab(4B)
    size_t blobSize = 208 + (size_t)FAKE_GLYPHS * 16 + (size_t)FAKE_GLYPHS * 4 * 2;
    uint8_t* obj = static_cast<uint8_t*>(VirtualAlloc(nullptr, blobSize,
                                                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!obj) { Log("font%u: blob alloc failed", fontId); f->srv->Release(); f->srv = nullptr; f->tex->Release(); f->tex = nullptr; f->state = 4; return false; }
    memset(obj, 0, blobSize);

    memcpy(obj, off, 208);                          // 头照抄（行高/cellH/偏移/图集名/kern 指针域+计数）
    *(int*)(obj + 8)   = (int)FAKE_GLYPHS;          // count: 覆盖 0x20..0xFFFF
    *(int*)(obj + 12)  = (int)FONT_BASECHAR_DEFAULT;// baseChar=0x20（照官方）
    *(uint32_t*)(obj + 184) = MAGIC_TEXID_BASE + fontId;

    uint8_t* met = obj + 208;
    uint8_t* xt  = met + (size_t)FAKE_GLYPHS * 16;
    uint8_t* yt  = xt  + (size_t)FAKE_GLYPHS * 4;
    *(void**)(obj + 176) = met;
    *(void**)(obj + 192) = xt;
    *(void**)(obj + 200) = yt;
    // kern 表(+168 指针/+32 计数) 已随头照抄, 指官方 blob（官方槽码不变, kern 语义保持）

    // 官方槽码区照抄
    memcpy(met, offMet, (size_t)offCount * 16);
    memcpy(xt,  offXtab, (size_t)offCount * 4);
    memcpy(yt,  offYtab, (size_t)offCount * 4);

    // 1) 漏填槽位先填默认兜底（含超容量字符）: 空白 cell, 无 kern
    //    （必须先于中文字符区填充, 否则会把中文槽位的真实 UV/kernStart 覆盖掉）
    for (uint32_t slot = (uint32_t)offCount; slot < FAKE_GLYPHS; ++slot)
    {
        *(int32_t*)(met + (size_t)slot * 16 + 0)  = f->cellW;
        *(int32_t*)(met + (size_t)slot * 16 + 4)  = f->cellW;
        *(int16_t*)(met + (size_t)slot * 16 + 12) = -1;                  // 无 kern（防 kern 表空指针崩溃）
        *(uint32_t*)(yt + (size_t)slot * 4) = f->blankY;                 // 指向预留空白行（透明, 不遮挡）
    }
    // 2) 中文字符区后填, 覆盖默认兜底值（容量截断后的字符）
    for (uint32_t i = 0; i < f->nCells; ++i)
    {
        uint32_t cp = f->cps[i];
        uint32_t slot = cp - FONT_BASECHAR_DEFAULT;
        if (slot < (uint32_t)offCount || slot >= FAKE_GLYPHS) continue;  // 不覆盖官方区
        *(int32_t*)(met + (size_t)slot * 16 + 0)  = f->advances[i];
        *(int32_t*)(met + (size_t)slot * 16 + 4)  = f->cellW;
        *(int16_t*)(met + (size_t)slot * 16 + 12) = -1;                  // 无 kern
        *(uint32_t*)(xt + (size_t)slot * 4) = (i % perRow) * f->cellW;
        *(uint32_t*)(yt + (size_t)slot * 4) = offH + (i / perRow) * f->cellH;
    }

    f->obj = obj;

    // 伪纹理对象（+8 u16 W, +10 u16 H, +20 u16 变体=1, +34 u8 速度=0）
    memset(f->fakeTexObj, 0, sizeof(f->fakeTexObj));
    *(uint16_t*)(f->fakeTexObj + 8)  = (uint16_t)W;
    *(uint16_t*)(f->fakeTexObj + 10) = (uint16_t)H;
    *(uint16_t*)(f->fakeTexObj + 20) = 1;

    // 释放光栅化缓存
    free(f->cellBuf); f->cellBuf = nullptr;
    free(f->advances); f->advances = nullptr;
    free(f->cps); f->cps = nullptr;

    Log("font%u: LIVE atlas=%ux%u cells=%u kept=%u blob=%zuKB", fontId, W, H, nCellsWanted, f->nCells, blobSize >> 10);
    InterlockedExchange(&f->state, 2);
    return true;
}

// ---------- 请求升级字体（渲染线程, DrawWide 内调用） ----------
// 文本是否需要中文字形（词典字符集位图: 含任一 >=0x80 字符即需要）
static bool TextNeedsGlyphs(const wchar_t* s)
{
    if (!g_stbReady || !g_dictReady) return false;
    for (const wchar_t* p = s; *p; ++p)
        if (*p >= 0x80 && (g_charSet[(uint32_t)*p >> 3] & (1u << (*p & 7))))
            return true;
    return false;
}

// slot: fontTab 槽位号（已规范化, 非 DrawWide 原始 fontId）
static void RequestFont(uint32_t slot)
{
    if (!g_stbReady || !g_dictReady) return;
    if (slot >= FAKE_FONT_MAX) return;
    FakeFont* f = &g_fake[slot];
    LONG st = f->state;
    if (st != 0) return;
    if (InterlockedCompareExchange(&f->state, 1, 0) != 0) return;
    f->fontId = slot;
    Log("font%u: upgrade requested (first CJK text)", slot);
    CloseHandle(CreateThread(nullptr, 0, RasterizeThread, f, 0, nullptr));
}

// 官方对象 -> fontTab 槽位号（找不到返回 0xFFFFFFFF）
static uint32_t ResolveSlot(void* off)
{
    if (!g_fontTabPtr || !off) return 0xFFFFFFFFu;
    int n = *g_fontCountPtr;
    if (n < 0) return 0xFFFFFFFFu;
    if (n > (int)FAKE_FONT_MAX) n = (int)FAKE_FONT_MAX;
    for (int i = 0; i < n; ++i)
        if ((*g_fontTabPtr)[i] == off) return (uint32_t)i;
    return 0xFFFFFFFFu;
}

// DrawWide 命中含中文译文时调用: 规范化 fontId（-1/负组编码/槽位）-> 槽位 -> 触发/推进
// rawFontId -> FakeFont* 缓存（避免每帧线性扫 fontTab; DrawWide 端高频调用）
struct FontIdCache { unsigned int raw; FakeFont* f; };
static FontIdCache g_fidCache[16];
static volatile LONG g_fidCacheN = 0;

static void EnsureFontFor(unsigned int rawFontId)
{
    if (!g_stbReady || !g_dictReady) return;

    // 已缓存: D3D 阶段由后台线程推进, 此处不再调 FinishFont（渲染线程防卡顿）
    LONG n = g_fidCacheN;
    if (n > 16) n = 16;
    for (LONG i = 0; i < n; ++i)
    {
        if (g_fidCache[i].raw == rawFontId) return;
    }

    // 新 fontId: 解析官方对象 -> 反查槽位
    void* off = g_origFontLookup((int)rawFontId);
    if (!off) return;
    uint32_t slot = ResolveSlot(off);
    if (slot >= FAKE_FONT_MAX) return;
    FakeFont* f = &g_fake[slot];

    LONG idx = InterlockedIncrement(&g_fidCacheN) - 1;   // 1 起
    if (idx < 16)
    {
        g_fidCache[idx].raw = rawFontId;
        g_fidCache[idx].f   = f;
    }

    if (f->state == 0) RequestFont(slot);   // 首次: 起后台光栅化
}

// ---------- 字体初始化线程: 读 TTF + stbtt_InitFont ----------
static DWORD WINAPI FontFileThread(LPVOID hSelf)
{
    wchar_t dir[MAX_PATH];
    GetModuleFileNameW((HMODULE)hSelf, dir, MAX_PATH);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash) *slash = L'\0'; else *dir = L'\0';

    // 等词典就绪（字符集收集完毕）
    for (int i = 0; i < 300; ++i) { if (g_dictReady) break; Sleep(100); }
    if (!g_dictReady) { Log("font: dict not ready, glyph layer disabled"); return 0; }

    // 词典字符集 + 全角标点/符号保险集
    static const wchar_t extra[] =
        L"，。？！：；、·—…“”‘’（）《》〈〉【】〔〕「」『』％℃°±×÷©®™"
        L"０１２３４５６７８９ＡＢＣＤＥＦＧＨＩＪＫＬＭＮＯＰＱＲＳＴＵＶＷＸＹＺ"
        L"ａｂｃｄｅｆｇｈｉｊｋｌｍｎｏｐｑｒｓｔｕｖｗｘｙｚ";
    for (const wchar_t* p = extra; *p; ++p) CharSetAdd(*p);
    Log("font: charset %u chars (dict) + extras", g_charCount);

    // 读 TTF（ini 字体文件名; 兼容旧名 font.ttf）
    wchar_t ttf[MAX_PATH];
    _snwprintf_s(ttf, MAX_PATH, _TRUNCATE, L"%ls\\%ls", dir, g_cfg.fontFile);

    HANDLE f = CreateFileW(ttf, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
    {
        // 兼容: ini 未配置时的默认名
        wchar_t ttf1[MAX_PATH];
        wcscpy_s(ttf1, dir); wcscat_s(ttf1, L"\\font.ttf");
        f = CreateFileW(ttf1, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        wcscpy_s(ttf, ttf1);
    }
    if (f == INVALID_HANDLE_VALUE)
    {
        Log("font: %ls not found, glyph layer disabled (GLE=%lu)", ttf, GetLastError());
        return 0;
    }
    LARGE_INTEGER sz;
    GetFileSizeEx(f, &sz);
    if (sz.QuadPart <= 0 || sz.QuadPart > (128 << 20))
    {
        Log("font: bad ttf size %lld", sz.QuadPart);
        CloseHandle(f); return 0;
    }
    g_ttfBuf = static_cast<uint8_t*>(VirtualAlloc(nullptr, (SIZE_T)sz.QuadPart,
                                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    DWORD rd = 0;
    BOOL ok = g_ttfBuf && ReadFile(f, g_ttfBuf, (DWORD)sz.QuadPart, &rd, nullptr);
    CloseHandle(f);
    if (!ok || rd != (DWORD)sz.QuadPart)
    {
        Log("font: ttf read failed");
        if (g_ttfBuf) { VirtualFree(g_ttfBuf, 0, MEM_RELEASE); g_ttfBuf = nullptr; }
        return 0;
    }
    Log("font: %ls (%u bytes)", ttf, rd);

    if (!stbtt_InitFont(&g_stb, g_ttfBuf, 0))
    {
        Log("font: stbtt_InitFont failed, glyph layer disabled");
        VirtualFree(g_ttfBuf, 0, MEM_RELEASE); g_ttfBuf = nullptr;
        return 0;
    }
    int upem = 0;
    stbtt_GetFontVMetrics(&g_stb, &upem, nullptr, nullptr);  // 复用变量取 ascent
    Log("font: stbtt ok (numGlyphs=%d)", g_stb.numGlyphs);
    InterlockedExchange(&g_stbReady, 1);
    return 0;
}

// =====================================================================
// 文本层 Hook A/B（v5）
// =====================================================================

using DrawWide_t = __int64(__fastcall*)(void*, float, float, const wchar_t*, float, char, unsigned int, void*);
static DrawWide_t g_origDrawWide = nullptr;

static __int64 __fastcall HookDrawWide(void* a1, float x, float y, const wchar_t* text,
                                        float scale, char flag, unsigned int fontId, void* a8)
{
    if (text && *text)
    {
        const DictNode* r = LookupNode(text, &g_hitA, &g_missA);
        if (r) text = r->trans;
        // 文本含中文字符（无论替换来自 Hook A 还是 Hook B）-> 确保该字体已升级
        if (TextNeedsGlyphs(text)) EnsureFontFor(fontId);
    }
    return g_origDrawWide(a1, x, y, text, scale, flag, fontId, a8);
}

using Format_t = __int64(__fastcall*)(wchar_t*, const wchar_t*, unsigned long long, void*, unsigned int);
static Format_t g_origFormat = nullptr;

static __int64 __fastcall HookFormat(wchar_t* dst, const wchar_t* fmt,
                                      unsigned long long cap, void* args, unsigned int argc)
{
    if (fmt && *fmt)
    {
        const DictNode* r = LookupNode(fmt, &g_hitB, &g_missB);
        if (r) fmt = r->trans;
    }
    return g_origFormat(dst, fmt, cap, args, argc);
}

// ---------- txt 词典加载（le_strings 格式: "KEY": "VALUE", KEY=英文原文/槽位名） ----------
// 格式规则（与 schinese/*.txt 游戏原生格式一致）:
//   - 每行 "KEY": "VALUE";  引号内的 \\ \" \n 为转义
//   - HASH_xxxxxxxx 键 = 引擎字符串槽位名（值不是查表键）, 跳过不插入哈希表
//   - 值为空 / 纯 ASCII 值跳过（无翻译意义）; KEY 与值相同跳过
struct LeLine
{
    wchar_t* key;
    wchar_t* val;
};

// 解析 "KEY": "VALUE" 行（内存内原地反转义）; 返回 false = 非条目行
static bool ParseLeLine(wchar_t* line, LeLine* out)
{
    wchar_t* p = line;
    while (*p == L' ' || *p == L'\t') ++p;
    if (*p != L'"') return false;
    ++p;
    wchar_t* key = p;
    // KEY 内转义在 le_strings 键中不存在（键为消息名/原文, 无引号）; 找闭合引号
    while (*p && *p != L'"') ++p;
    if (!*p) return false;
    *p = L'\0';
    ++p;
    while (*p == L' ' || *p == L'\t') ++p;
    if (*p != L':') return false;
    ++p;
    while (*p == L' ' || *p == L'\t') ++p;
    if (*p != L'"') return false;
    ++p;
    wchar_t* val = p;

    // 值反转义（\\ \" \n \r）, 原地写（sr3le_extract.py 输出含 \r 转义, 不处理会混入字母 r）
    wchar_t* w = p;
    while (*p)
    {
        if (*p == L'\\')
        {
            ++p;
            if      (*p == L'n')  *w++ = L'\n';
            else if (*p == L'r')  *w++ = L'\r';
            else if (*p == L'\\') *w++ = L'\\';
            else if (*p == L'"')  *w++ = L'"';
            else if (*p)          *w++ = *p;   // 未知转义按原样
            else break;
            ++p;
        }
        else if (*p == L'"')   // 闭合引号, 值结束
        {
            *w = L'\0';
            out->key = key;
            out->val = val;
            return true;
        }
        else *w++ = *p++;
    }
    return false;   // 未闭合
}

// 加载一个 origin txt（UTF-8, 带/不带 BOM; CRLF/LF; 格式: "消息ID": "英文原文"）
// 结果入 origin 表; 不收集字符集; HASH_ 键同样入表（联表后无法消解时才在 AddDictEntry 跳过）
static bool LoadOriginFile(const wchar_t* path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    GetFileSizeEx(f, &sz);
    if (sz.QuadPart <= 0 || sz.QuadPart > (32 << 20))
    { Log("origin: %ls bad size %lld", path, sz.QuadPart); CloseHandle(f); return false; }

    auto* buf = static_cast<uint8_t*>(VirtualAlloc(nullptr, (SIZE_T)sz.QuadPart,
                                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    DWORD rd = 0;
    BOOL ok = buf && ReadFile(f, buf, (DWORD)sz.QuadPart, &rd, nullptr);
    CloseHandle(f);
    if (!ok || rd != (DWORD)sz.QuadPart)
    { if (buf) VirtualFree(buf, 0, MEM_RELEASE); return false; }

    int utf8Off = (rd >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) ? 3 : 0;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, (const char*)buf + utf8Off,
                                   (int)(rd - utf8Off), nullptr, 0);
    if (wlen <= 0)
    { Log("origin: %ls not valid UTF-8", path); VirtualFree(buf, 0, MEM_RELEASE); return false; }
    auto* wbuf = static_cast<wchar_t*>(VirtualAlloc(nullptr, (wlen + 2) * sizeof(wchar_t),
                                                    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!wbuf) { VirtualFree(buf, 0, MEM_RELEASE); return false; }
    MultiByteToWideChar(CP_UTF8, 0, (const char*)buf + utf8Off, (int)(rd - utf8Off), wbuf, wlen);
    wbuf[wlen] = L'\0';
    VirtualFree(buf, 0, MEM_RELEASE);

    uint32_t entries = 0;
    wchar_t* ctx = nullptr;
    wchar_t* line = wcstok_s(wbuf, L"\r\n", &ctx);
    while (line)
    {
        LeLine le;
        if (ParseLeLine(line, &le))
        {
            size_t kn = wcslen(le.key), vn = wcslen(le.val);
            if (kn > 0 && kn <= 512 && vn > 0 && vn <= 8192)
            {
                if (OrigInsert(le.key, (uint32_t)kn, le.val, (uint32_t)vn)) ++entries;
            }
        }
        line = wcstok_s(nullptr, L"\r\n", &ctx);
    }
    VirtualFree(wbuf, 0, MEM_RELEASE);
    return entries > 0;
}

// 扫描 origin 文件夹（*.txt; 后加载的同键覆盖前面 = 头插哈希表）
static bool LoadOriginDir(const wchar_t* dir)
{
    wchar_t pat[MAX_PATH];
    _snwprintf_s(pat, MAX_PATH, _TRUNCATE, L"%ls\\*.txt", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        Log("origin: folder %ls not found (GLE=%lu), ID/HASH_ dict keys will be skipped",
            dir, GetLastError());
        return false;
    }
    uint32_t files = 0;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        wchar_t path[MAX_PATH];
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%ls\\%ls", dir, fd.cFileName);
        if (LoadOriginFile(path)) ++files;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    Log("origin: %u files, %u ids", files, g_origCount);
    return files > 0;
}

// 处理一条 key/val: ID/HASH_ 键经 origin 联表转英文原文; 其余按原文键入表
static void AddDictEntry(const wchar_t* key, const wchar_t* val,
                         uint32_t* loaded, uint32_t* hashKeys, uint32_t* cjkEntries)
{
    size_t on = wcslen(key), tn = wcslen(val);
    if (on == 0 || tn == 0 || on > 4096 || tn > 4096) return;

    // 键消解: origin 联表（ID/HASH_xxx -> 英文原文）
    //   - 命中   -> 用英文原文当键（引擎运行时只画解析后的文本, ID 不会到达 hook 层）
    //   - 未命中 -> 键即原文（"Blonde"类条目两条路线通吃）
    //   - 未命中且是 HASH_ -> 无法消解的槽位名, 跳过
    const wchar_t* effKey = key;
    size_t effLen = on;
    if (g_origBuckets)
    {
        const OrigNode* o = OrigLookup(key, on);
        if (o && o->len > 0) { effKey = o->val; effLen = o->len; }
        else if (!o && on > 5 && _wcsnicmp(key, L"HASH_", 5) == 0)
        { ++*hashKeys; return; }   // origin 里也查不到的 HASH_ 槽位名
    }
    else if (on > 5 && _wcsnicmp(key, L"HASH_", 5) == 0)
    { ++*hashKeys; return; }       // 无 origin 表时维持旧行为

    auto* transW = static_cast<wchar_t*>(ArenaAlloc((tn + 1) * sizeof(wchar_t)));
    if (!transW) return;
    memcpy(transW, val, tn * sizeof(wchar_t));
    transW[tn] = L'\0';

    // 收集译文非 ASCII 字符集
    bool hasCjk = false;
    for (const wchar_t* p = transW; *p; ++p)
        if (*p >= 0x80) { CharSetAdd(*p); hasCjk = true; }
    if (hasCjk) ++*cjkEntries;

    auto* origW = static_cast<wchar_t*>(ArenaAlloc((effLen + 1) * sizeof(wchar_t)));
    if (!origW) return;
    memcpy(origW, effKey, effLen * sizeof(wchar_t));
    origW[effLen] = L'\0';

    if (DictInsert(origW, (uint32_t)effLen, transW)) ++*loaded;
    size_t b, tl;
    if (TrimRange(origW, effLen, &b, &tl) && !(b == 0 && tl == effLen))
    {
        if (DictInsert(origW + b, (uint32_t)tl, transW)) ++*loaded;   // 计入 trim 键
    }
}

// 加载一个 txt 文件（UTF-8, 带/不带 BOM; CRLF/LF）
static bool LoadDictFile(const wchar_t* path, uint32_t* loaded, uint32_t* hashKeys,
                         uint32_t* cjkEntries, uint32_t* lineCount, uint32_t* badLines)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    GetFileSizeEx(f, &sz);
    if (sz.QuadPart <= 0 || sz.QuadPart > (32 << 20))
    { Log("dict: %ls bad size %lld", path, sz.QuadPart); CloseHandle(f); return false; }

    auto* buf = static_cast<uint8_t*>(VirtualAlloc(nullptr, (SIZE_T)sz.QuadPart,
                                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    DWORD rd = 0;
    BOOL ok = buf && ReadFile(f, buf, (DWORD)sz.QuadPart, &rd, nullptr);
    CloseHandle(f);
    if (!ok || rd != (DWORD)sz.QuadPart)
    { if (buf) VirtualFree(buf, 0, MEM_RELEASE); return false; }

    int utf8Off = (rd >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) ? 3 : 0;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, (const char*)buf + utf8Off,
                                   (int)(rd - utf8Off), nullptr, 0);
    if (wlen <= 0)
    { Log("dict: %ls not valid UTF-8", path); VirtualFree(buf, 0, MEM_RELEASE); return false; }
    auto* wbuf = static_cast<wchar_t*>(VirtualAlloc(nullptr, (wlen + 2) * sizeof(wchar_t),
                                                    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!wbuf) { VirtualFree(buf, 0, MEM_RELEASE); return false; }
    MultiByteToWideChar(CP_UTF8, 0, (const char*)buf + utf8Off, (int)(rd - utf8Off), wbuf, wlen);
    wbuf[wlen] = L'\0';
    VirtualFree(buf, 0, MEM_RELEASE);

    wchar_t* ctx = nullptr;
    wchar_t* line = wcstok_s(wbuf, L"\r\n", &ctx);
    while (line)
    {
        ++*lineCount;
        LeLine le;
        if (ParseLeLine(line, &le))
            AddDictEntry(le.key, le.val, loaded, hashKeys, cjkEntries);
        else
        {
            // 空行/尾逗号行等非条目不算错误; 只有含引号但解析失败才计
            wchar_t* q = wcschr(line, L'"');
            if (q) { ++*badLines; if (*badLines <= 5) Log("dict: %ls bad line: %.60ls", path, line); }
        }
        line = wcstok_s(nullptr, L"\r\n", &ctx);
    }
    VirtualFree(wbuf, 0, MEM_RELEASE);
    return true;
}

// 扫描词典文件夹（*.txt; 按 FindFirstFile 字典序, 后加载的同键覆盖前面 = 头插哈希表）
static bool LoadDictDir(const wchar_t* dir, uint32_t* outFiles, uint32_t* outLoaded,
                        uint32_t* outHash, uint32_t* outCjk)
{
    wchar_t pat[MAX_PATH];
    _snwprintf_s(pat, MAX_PATH, _TRUNCATE, L"%ls\\*.txt", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        Log("dict: folder %ls not found (GLE=%lu)", dir, GetLastError());
        return false;
    }
    uint32_t files = 0, loaded = 0, hashKeys = 0, cjkEntries = 0, badLines = 0, lines = 0;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        wchar_t path[MAX_PATH];
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%ls\\%ls", dir, fd.cFileName);
        if (LoadDictFile(path, &loaded, &hashKeys, &cjkEntries, &lines, &badLines))
            ++files;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    Log("dict: %u files, %u lines, %u keys, %u HASH_ skipped, %u cjk, %u bad, arena %zu/%zu KB",
        files, lines, loaded, hashKeys, cjkEntries, badLines,
        g_arenaUsed >> 10, ARENA_BYTES >> 10);
    *outFiles = files; *outLoaded = loaded; *outHash = hashKeys; *outCjk = cjkEntries;
    return files > 0 && loaded > 0;
}

// ---------- 统计线程 ----------
static DWORD WINAPI StatsThread(LPVOID)
{
    for (;;)
    {
        Sleep(STATS_PERIOD_MS);
        Log("stats: draw hit=%ld miss=%ld | format hit=%ld miss=%ld | dumped=%u | fonts=%ld",
            g_hitA, g_missA, g_hitB, g_missB, g_dumpCount, g_fidCacheN);
        // 周期落盘 dump 收集（替代逐条 fflush, 防切界面卡顿; 崩溃最多丢本轮周期数据）
        if (g_dumpFile)
        {
            AcquireSRWLockExclusive(&g_dumpLock);
            fflush(g_dumpFile);
            ReleaseSRWLockExclusive(&g_dumpLock);
        }
    }
}

// ---------- 安装 ----------
static bool InstallHook(uint64_t va, const uint8_t* expect, const char* name,
                        void* detour, void** orig)
{
    uint8_t* target = VA<uint8_t*>(va);
    if (memcmp(target, expect, 16) != 0)
    {
        Log("hook %s @%p: signature mismatch, ABORT (game updated?)", name, (void*)target);
        return false;
    }
    if (MH_CreateHook(target, detour, orig) != MH_OK)
    {
        Log("hook %s: MH_CreateHook failed", name);
        return false;
    }
    if (MH_EnableHook(target) != MH_OK)
    {
        Log("hook %s: MH_EnableHook failed", name);
        return false;
    }
    Log("hook %s @%p installed", name, (void*)target);
    return true;
}

// ---------- 主线程 ----------
static DWORD WINAPI MainThread(LPVOID hSelf)
{
    Log("==== SR3R_I18N v7: text replacement + CJK glyph layer (txt dict) ====");

    wchar_t dir[MAX_PATH], iniPath[MAX_PATH], dictDir[MAX_PATH], dtxt[MAX_PATH];
    GetModuleFileNameW((HMODULE)hSelf, dir, MAX_PATH);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash) *slash = L'\0'; else *dir = L'\0';

    wcscpy_s(iniPath, dir); wcscat_s(iniPath, L"\\SR3R_I18N.ini");
    LoadConfig(iniPath);

    _snwprintf_s(dictDir, MAX_PATH, _TRUNCATE, L"%ls\\%ls", dir, g_cfg.dictDir);
    wcscpy_s(dtxt, dir); wcscat_s(dtxt, L"\\DumpText.dtxt");

    // 1. 初始化（先建 arena/桶, LoadDictDir 直接入表）
    CrcInit();
    g_arena = static_cast<uint8_t*>(VirtualAlloc(nullptr, ARENA_BYTES,
                                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_dictBuckets = static_cast<DictNode**>(VirtualAlloc(nullptr, DICT_BUCKETS * sizeof(DictNode*),
                                                          MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_dumpBuckets = static_cast<DumpNode**>(VirtualAlloc(nullptr, DUMP_BUCKETS * sizeof(DumpNode*),
                                                          MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_origBuckets = static_cast<OrigNode**>(VirtualAlloc(nullptr, ORIG_BUCKETS * sizeof(OrigNode*),
                                                          MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!g_arena || !g_dictBuckets || !g_dumpBuckets || !g_origBuckets)
    {
        Log("alloc failed, abort");
        return 0;
    }
    g_dictMask = DICT_BUCKETS - 1;

    // 2. origin 联表（消息 ID -> 英文原文; 缺失/失败时退化为旧行为, ID/HASH_ 键跳过）
    wchar_t originDir[MAX_PATH];
    _snwprintf_s(originDir, MAX_PATH, _TRUNCATE, L"%ls\\%ls", dir, g_cfg.originDir);
    g_origMask = ORIG_BUCKETS - 1;      // 必须先于 LoadOriginDir（OrigInsert 在加载中就要用）
    if (!LoadOriginDir(originDir))
        g_origBuckets = nullptr;   // 无 origin: OrigLookup 直接返 null, AddDictEntry 走旧行为

    // 3. 词典（scripts\<dict_dir>\*.txt, le_strings 格式）
    uint32_t files = 0, loaded = 0, hashKeys = 0, cjkEntries = 0;
    if (!LoadDictDir(dictDir, &files, &loaded, &hashKeys, &cjkEntries))
    {
        Log("dict: load failed, idle mode (no hooks)");
        return 0;
    }

    // 3. DumpText（ini 可关）
    if (g_cfg.dumpEnabled)
    {
    _wfopen_s(&g_dumpFile, dtxt, L"ab");
    if (g_dumpFile) setvbuf(g_dumpFile, nullptr, _IOFBF, 1 << 20);   // 1MB 全缓冲: 避免 CRT 小块直写
    Log("dump: %ls %s", dtxt, g_dumpFile ? "opened (append)" : "open failed");
    }
    else
    {
        Log("dump: disabled by ini");
    }

    // 4. 引擎指针
    g_fontTabPtr   = VA<void***>(VA_FONTTAB);
    g_fontCountPtr = VA<volatile int*>(VA_FONTCOUNT);
    g_devSlot      = VA<ID3D11Device**>(VA_D3D_DEVICE);
    g_ctxSlot      = VA<ID3D11DeviceContext**>(VA_D3D_CONTEXT);

    // 5. MinHook
    if (MH_Initialize() != MH_OK)
    {
        Log("MH_Initialize failed");
        return 0;
    }
    bool a = InstallHook(VA_DRAW_WIDE,   SIG_DRAW_WIDE,   "DrawWide",   (void*)HookDrawWide,   (void**)&g_origDrawWide);
    bool b = InstallHook(VA_FORMAT,      SIG_FORMAT,      "Format",     (void*)HookFormat,     (void**)&g_origFormat);
    bool c = InstallHook(VA_FONT_LOOKUP, SIG_FONT_LOOKUP, "FontLookup", (void*)HookFontLookup, (void**)&g_origFontLookup);
    bool d = InstallHook(VA_TEXOBJ,      SIG_TEXOBJ,      "TexObj",     (void*)HookTexObj,     (void**)&g_origTexObj);
    bool e = InstallHook(VA_SRV_RESOLVE, SIG_SRV_RESOLVE, "SrvResolve", (void*)HookSrvResolve, (void**)&g_origSrvResolve);
    if (!a && !b && !c && !d && !e)
    {
        Log("no hooks installed, idle");
        return 0;
    }

    InterlockedExchange(&g_dictReady, 1);

    // 6. 字体文件后台线程（读 TTF + InitFont）
    CloseHandle(CreateThread(nullptr, 0, FontFileThread, hSelf, 0, nullptr));

    CloseHandle(CreateThread(nullptr, 0, StatsThread, nullptr, 0, nullptr));
    Log("v7 active: dict=%u keys (%u files), hooks A=%d B=%d C=%d D=%d E=%d, idling",
        g_dictCount, files, (int)a, (int)b, (int)c, (int)d, (int)e);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        InitializeCriticalSection(&g_logCS);
        LogOpen(hModule);
        if (g_log)
        {
			Log("[Info] SR3R Font Extend By HaoJun0823 https://www.haojun0823.xyz | https://github.com/HaoJun0823/SR3R_I18N");
            Log("[DllMain] ATTACH v6");
            CloseHandle(CreateThread(nullptr, 0, MainThread, hModule, 0, nullptr));
        }
        break;
    case DLL_PROCESS_DETACH:
        if (g_dumpFile) { fclose(g_dumpFile); g_dumpFile = nullptr; }
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        if (g_log) { fclose(g_log); g_log = nullptr; }
        DeleteCriticalSection(&g_logCS);
        break;
    }
    return TRUE;
}
