// dllmain.cpp : SR3R_I18N - 黑道圣徒3重制版 外挂汉化 DLL（v6：文本替换 + 中文字形层）
//
// v6 = v5 文本替换层 + 运行时中文字形渲染层（零资源文件改动）
//
//  [文本层 v5]
//   1. 加载 scripts\text.btxt（BEXT 词典: 英文原文 -> 中文译文）
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

static constexpr size_t ARENA_BYTES   = 8u << 20;
static constexpr uint32_t DICT_BUCKETS = 1u << 15;
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
                    fwrite(utf8, 1, w, g_dumpFile);
                    fflush(g_dumpFile);
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

// ---------- Hook C: 字体对象查询 ----------
static void* __fastcall HookFontLookup(int fontId)
{
    void* off = g_origFontLookup(fontId);
    if (off)
    {
        uint32_t slot = (fontId >= 0) ? (uint32_t)fontId : ResolveSlot(off);
        if (slot < FAKE_FONT_MAX)
        {
            FakeFont* f = &g_fake[slot];
            if (f->state == 2)
            {
                // 校验官方对象仍在槽位（引擎重建字体则回退官方, 防悬空）
                void* cur = (*g_fontTabPtr)[slot];
                if (cur == f->official) return f->obj;
                f->state = 4;
                return cur;
            }
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
    if (offCount <= 0 || offCount >= 0x10000 || cellH < 8 || cellH > 128)
    {
        Log("font%u: bad header count=%d cellH=%u, abort", fontId, offCount, cellH);
        f->state = 4; return 0;
    }

    uint16_t cellW = cellH;  // 方块字 1:1
    f->cellW = cellW; f->cellH = cellH;
    f->official = off;

    // 收集字符集 -> 列表
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

    // 光栅化
    float scale = stbtt_ScaleForPixelHeight(&g_stb, (float)cellH);
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&g_stb, &ascent, &descent, &gap);
    int baseline = (int)((float)ascent * scale + 0.5f);
    if (baseline > cellH - 1) baseline = cellH - 1;
    if (baseline < 1) baseline = 1;

    uint32_t n = 0, missGlyph = 0;
    for (uint32_t cp = 0x80; cp <= 0xFFFD; ++cp)
    {
        if (!(g_charSet[cp >> 3] & (1u << (cp & 7)))) continue;
        if (n >= total) break;

        int g = stbtt_FindGlyphIndex(&g_stb, (int)cp);
        uint8_t* cell = f->cellBuf + (size_t)n * cellW * cellH;
        f->cps[n] = cp;

        if (g == 0)
        {
            ++missGlyph;
            f->advances[n] = cellW;   // 无字形: 空白格
            ++n;
            continue;
        }

        int x0, y0, x1, y1;
        stbtt_GetGlyphBitmapBox(&g_stb, g, scale, scale, &x0, &y0, &x1, &y1);
        int w = x1 - x0, h = y1 - y0;
        int ox = ((int)cellW - w) / 2;
        int oy = baseline + y0;

        int adv;
        stbtt_GetGlyphHMetrics(&g_stb, g, &adv, nullptr);
        f->advances[n] = adv > 0 ? (int)((float)adv * scale + 0.5f) : cellW;
        if (f->advances[n] <= 0) f->advances[n] = cellW / 2;

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
        ++n;
    }
    f->nCells = n;

    if (offBase != (int)FONT_BASECHAR_DEFAULT)
        Log("font%u: warn official baseChar=%d != 0x20", fontId, offBase);
    Log("font%u: rasterized %u cells (cellW=%u cellH=%u baseline=%d, missGlyph=%u, official count=%d)",
        fontId, n, cellW, cellH, baseline, missGlyph, offCount);

    InterlockedExchange(&f->state, 3);  // 待渲染线程 D3D 阶段
    return 0;
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
    uint32_t offH = dd.Height;
    uint32_t perRow = W / f->cellW;
    if (perRow == 0) { Log("font%u: atlas width %u < cellW %u, abort", fontId, W, f->cellW); srcTex->Release(); f->state = 4; return false; }
    uint32_t rows = (f->nCells + perRow - 1) / perRow;
    uint32_t H = offH + rows * f->cellH;
    if (H > 65535)
    {
        rows = (65535 - offH) / f->cellH;
        H = offH + rows * f->cellH;
        f->nCells = f->nCells < rows * perRow ? f->nCells : rows * perRow;
        Log("font%u: atlas height clamped to %u (%u cells)", fontId, H, f->nCells);
    }

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
    for (uint32_t row = 0; row < offH; ++row)
        memcpy(atlas + (SIZE_T)row * pitch,
               (uint8_t*)ms.pData + (SIZE_T)row * ms.RowPitch, pitch);
    ctx->Unmap(stag, 0);
    stag->Release();

    // 官方图集像素样本（调试: 判断字形存储格式）
    {
        uint32_t sx = *(uint32_t*)((uint8_t*)offXtab + 4 * ('W' - 0x20));
        uint32_t sy = *(uint32_t*)((uint8_t*)offYtab + 4 * ('W' - 0x20));
        if (sx + 8 < W && sy + 8 < offH)
        {
            uint32_t* px = (uint32_t*)(atlas + (SIZE_T)sy * pitch + (SIZE_T)sx * 4);
            Log("font%u: atlas %ux%u fmt=%u, sample W@(%u,%u): %08X %08X %08X",
                fontId, W, offH, (unsigned)dd.Format, sx, sy, px[0], px[1], px[pitch / 8]);
        }
    }

    // 2) 中文区 blit（灰度 -> BGRA 白字）
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
                dst[c] = 0x00FFFFFFu | ((uint32_t)src[c] << 24);   // B=G=R=255, A=coverage
        }
    }

    // 3) 创建纹理 + SRV
    D3D11_TEXTURE2D_DESC nd = dd;
    nd.Width     = W;
    nd.Height    = H;
    nd.MipLevels = 1;
    nd.ArraySize = 1;
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

    // 中文字符区
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

    Log("font%u: LIVE atlas=%ux%u cells=%u blob=%zuKB", fontId, W, H, f->nCells, blobSize >> 10);
    InterlockedExchange(&f->state, 2);
    return true;
}

// ---------- 请求升级字体（渲染线程, DrawWide 内调用） ----------
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
static void EnsureFontFor(unsigned int rawFontId)
{
    if (!g_stbReady || !g_dictReady) return;
    void* off = g_origFontLookup((int)rawFontId);
    if (!off) return;
    // 已跟踪的字体直接推进（避免每帧线性扫 fontTab）
    for (uint32_t i = 0; i < FAKE_FONT_MAX; ++i)
    {
        FakeFont* f = &g_fake[i];
        if (f->state != 0 && f->official == off)
        {
            if (f->state == 3) FinishFont(f);   // 光栅化完, 渲染线程做 D3D 阶段
            return;
        }
    }
    // 新字体: 反查槽位号后触发
    uint32_t slot = ResolveSlot(off);
    if (slot == 0xFFFFFFFFu) return;
    FakeFont* f = &g_fake[slot];
    if (f->state == 0) RequestFont(slot);
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

    // 读 TTF
    wchar_t ttf1[MAX_PATH], ttf2[MAX_PATH];
    wcscpy_s(ttf1, dir); wcscat_s(ttf1, L"\\font.ttf");
    wcscpy_s(ttf2, dir); wcscat_s(ttf2, L"\\SourceHanSansHWSC-VF.ttf");

    HANDLE f = CreateFileW(ttf2, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    wchar_t* used = ttf2;
    if (f == INVALID_HANDLE_VALUE)
    {
        f = CreateFileW(ttf1, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        used = ttf1;
    }
    if (f == INVALID_HANDLE_VALUE)
    {
        Log("font: %ls / font.ttf not found, glyph layer disabled (GLE=%lu)", ttf2, GetLastError());
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
    Log("font: %ls (%u bytes)", used, rd);

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
        if (r)
        {
            text = r->trans;
            if (r->hasCjk)
                EnsureFontFor(fontId);   // 规范化 -1/负组编码/槽位后触发
        }
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

// ---------- BEXT 词典加载 ----------
static bool LoadBext(const uint8_t* buf, size_t size)
{
    if (size < 12) { Log("BEXT: too small"); return false; }
    uint32_t magic, reserved, count;
    memcpy(&magic,    buf + 0, 4);
    memcpy(&reserved, buf + 4, 4);
    memcpy(&count,    buf + 8, 4);
    if (magic != 0x54584542u) { Log("BEXT: bad magic %08X", magic); return false; }

    size_t off = 12;
    uint32_t loaded = 0, trimKeys = 0, cjkEntries = 0;

    for (uint32_t i = 0; i < count; ++i)
    {
        const char* fields[3];
        uint32_t    fLens[3];
        bool ok = true;
        for (int k = 0; k < 3; ++k)
        {
            uint32_t l;
            if (off + 4 > size) { ok = false; break; }
            memcpy(&l, buf + off, 4); off += 4;
            if (l == 0 || off + l > size) { ok = false; break; }
            fields[k] = reinterpret_cast<const char*>(buf + off);
            fLens[k]  = l;
            off += l;
        }
        if (!ok) { Log("BEXT: truncated at entry %u", i); break; }

        const char* origUtf8  = fields[1]; uint32_t origLen  = fLens[1] - 1;
        const char* transUtf8 = fields[2]; uint32_t transLen = fLens[2] - 1;
        if (origLen == 0 || transLen == 0) continue;

        int on = MultiByteToWideChar(CP_UTF8, 0, origUtf8, (int)origLen, nullptr, 0);
        int tn = MultiByteToWideChar(CP_UTF8, 0, transUtf8, (int)transLen, nullptr, 0);
        if (on <= 0 || tn <= 0 || on > 4096 || tn > 4096) continue;

        auto* transW = static_cast<wchar_t*>(ArenaAlloc((tn + 1) * sizeof(wchar_t)));
        auto* origW  = static_cast<wchar_t*>(ArenaAlloc((on + 1) * sizeof(wchar_t)));
        if (!transW || !origW) { Log("BEXT: arena exhausted at %u", i); break; }
        MultiByteToWideChar(CP_UTF8, 0, transUtf8, (int)transLen, transW, tn);
        transW[tn] = L'\0';
        MultiByteToWideChar(CP_UTF8, 0, origUtf8, (int)origLen, origW, on);
        origW[on] = L'\0';

        // v6: 收集译文非 ASCII 字符集
        bool hasCjk = false;
        for (const wchar_t* p = transW; *p; ++p)
            if (*p >= 0x80) { CharSetAdd(*p); hasCjk = true; }
        if (hasCjk) ++cjkEntries;

        if (DictInsert(origW, (uint32_t)on, transW)) ++loaded;
        size_t b, tl;
        if (TrimRange(origW, (size_t)on, &b, &tl) && !(b == 0 && tl == (size_t)on))
        {
            if (DictInsert(origW + b, (uint32_t)tl, transW)) ++trimKeys;
        }
    }
    Log("BEXT: %u entries, loaded %u keys (+%u trim), %u cjk, charset %u, arena %zu/%zu KB",
        count, loaded, trimKeys, cjkEntries, g_charCount, g_arenaUsed >> 10, ARENA_BYTES >> 10);
    return loaded > 0;
}

// ---------- 统计线程 ----------
static DWORD WINAPI StatsThread(LPVOID)
{
    for (;;)
    {
        Sleep(STATS_PERIOD_MS);
        Log("stats: draw hit=%ld miss=%ld | format hit=%ld miss=%ld | dumped=%u",
            g_hitA, g_missA, g_hitB, g_missB, g_dumpCount);
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
    Log("==== SR3R_I18N v6: text replacement + CJK glyph layer ====");

    wchar_t dir[MAX_PATH], btxt[MAX_PATH], dtxt[MAX_PATH];
    GetModuleFileNameW((HMODULE)hSelf, dir, MAX_PATH);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash) *slash = L'\0'; else *dir = L'\0';
    wcscpy_s(btxt, dir); wcscat_s(btxt, L"\\text.btxt");
    wcscpy_s(dtxt, dir); wcscat_s(dtxt, L"\\DumpText.dtxt");

    // 1. 词典
    HANDLE f = CreateFileW(btxt, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
    {
        Log("dict: %ls not found (GLE=%lu), idle mode", btxt, GetLastError());
        return 0;
    }
    LARGE_INTEGER sz;
    GetFileSizeEx(f, &sz);
    if (sz.QuadPart <= 0 || sz.QuadPart > (16 << 20))
    {
        Log("dict: bad size %lld", sz.QuadPart);
        CloseHandle(f);
        return 0;
    }
    auto* buf = static_cast<uint8_t*>(VirtualAlloc(nullptr, (SIZE_T)sz.QuadPart,
                                                    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    DWORD rd = 0;
    BOOL ok = ReadFile(f, buf, (DWORD)sz.QuadPart, &rd, nullptr);
    CloseHandle(f);
    if (!ok || rd != (DWORD)sz.QuadPart) { Log("dict: read failed"); return 0; }
    Log("dict: %ls (%u bytes)", btxt, rd);

    // 2. 初始化
    CrcInit();
    g_arena = static_cast<uint8_t*>(VirtualAlloc(nullptr, ARENA_BYTES,
                                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_dictBuckets = static_cast<DictNode**>(VirtualAlloc(nullptr, DICT_BUCKETS * sizeof(DictNode*),
                                                          MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_dumpBuckets = static_cast<DumpNode**>(VirtualAlloc(nullptr, DUMP_BUCKETS * sizeof(DumpNode*),
                                                          MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!g_arena || !g_dictBuckets || !g_dumpBuckets)
    {
        Log("alloc failed, abort");
        return 0;
    }
    g_dictMask = DICT_BUCKETS - 1;

    if (!LoadBext(buf, rd))
    {
        Log("dict: load failed, idle mode (no hooks)");
        return 0;
    }
    VirtualFree(buf, 0, MEM_RELEASE);

    // 3. DumpText
    _wfopen_s(&g_dumpFile, dtxt, L"ab");
    Log("dump: %ls %s", dtxt, g_dumpFile ? "opened (append)" : "open failed");

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
    Log("v6 active: dict=%u keys, hooks A=%d B=%d C=%d D=%d E=%d, idling",
        g_dictCount, (int)a, (int)b, (int)c, (int)d, (int)e);
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
