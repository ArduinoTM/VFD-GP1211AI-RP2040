#ifndef VFD_SCANPACK_H
#define VFD_SCANPACK_H

#include <stdint.h>

#include "vfd_hw_spec.h" /* SCAN_PERIOD_US / BLANK_GUARD_US / SCANS_PER_FRAME / SCAN_BYTES / FRAME_SIZE */

/* ==================================================================== vfd_scanpack
 * 把 Adafruit_SSD1306 风格的页式帧缓冲重排成 GP1211AI 的"每时序 384 bit"数据流。
 *
 * 布局（与原 STM32 驱动、与屏规格书 Sheet 6 的数据号一致）：
 *   k = 48*page + 6*m + off      （k 是 384 bit 链里的位号，最先移出的是 k=0）
 *   page = y>>3（第几页），m = y&7（页内行），off = 6 个阳极槽之一。
 *
 * 槽位 off ↔ 字母（Sheet 6 的 a,f,b,e,c,d + 移位方向 SIa→384…1→SOa）：
 *   off 5=a, 3=b, 1=c, 0=d, 2=e, 4=f
 *
 * 实机定标（2026-10-08，用"运行时常量轮换"诊断固件扫出来的，见 docs/00 §14.12/§14.23）：
 *   · 一般时序：t 偶 → off 0,2,4（d,e,f）；t 奇 → off 5,3,1（a,b,c）
 *     其中 t = 42 - scan（一帧 43 个时序，扫描顺序与手册第 5 页一致：T43,T1,…,T42）
 *   · T43（t == 42，手册 Note 12：只能用 a,b，c,d,e,f 必须 OFF）：
 *     a → off 2、b → off 4、k 整体 −2、列不动，且只写两个槽（第三列不产生数据）
 * ================================================================================= */

namespace vfd {

constexpr int WIDTH = 128;              /* 屏宽（像素列） */
constexpr int HEIGHT = 64;              /* 屏高（像素行） */
constexpr int PAGES = HEIGHT / 8;       /* 页式帧缓冲：8 页 */
constexpr int FRAMEBUFFER_SIZE = WIDTH * PAGES;                   /* 1024 B */

/* ⚠️ SCANS_PER_FRAME / SCAN_BYTES / FRAME_SIZE 的**定义**已集中到 src/vfd_hw_spec.h
 *    （第三方工程评审 §4.3：原先这三处各有一份手抄副本，只靠注释维系一致）。
 *    本文件只做"帧结构自检"与布局推导，不再重复定义。 */

/* 帧缓冲访问：fb[y>>3][x] 的 bit(y&7)，与 Adafruit_SSD1306 布局一致 */
inline bool pixelGet(const uint8_t *fb, int x, int y)
{
    return (fb[(y >> 3) * WIDTH + x] >> (y & 7)) & 0x01u;
}

/* 重排单个扫描：out 必须是 SCAN_BYTES 字节，函数内部会先清零。
 * invert=true 时按像素取反（用于 invertDisplay），只翻转真实像素位，
 * 不会污染"另一组阳极必须为 0"的填充位。
 *
 * wireOrder=true 时**直接产出线序字节**（每字节 8 位反序，供只能 MSB-first 的 SPI/PL022 使用），
 * 等价于"逻辑打包 + wirePrepareFrame(…, true) 的那一遍逐字节反序"，但省掉整帧 2064 B 的遍历
 * （实测 tick 路径每帧省 ~43k 指令 ≈ 0.35 ms，见 docs/00 §4.2 的实测表）。 */
void packScan(const uint8_t *fb, int scan, bool invert, uint8_t *out, bool wireOrder = false);

/* 帧结构自检：下面几处按"扫描数 × 每扫描字节数"推地址，前提必须成立（常量本体见 vfd_hw_spec.h） */
static_assert(FRAME_SIZE == SCANS_PER_FRAME * SCAN_BYTES,
    "FRAME_SIZE 必须等于 SCANS_PER_FRAME × SCAN_BYTES");
static_assert(SCAN_BYTES * 8 == 384, "一条扫描对应 384 位栅极链，改了要同步改移位链宽度");

/* 重排整帧：out 必须是 FRAME_SIZE 字节，扫描 0 在前。wireOrder 语义同 packScan()。 */
void packFrame(const uint8_t *fb, bool invert, uint8_t *out, bool wireOrder = false);

/* ------------------------------------------------------------------ 打包用的查表（2026-10-13 提速）
 *
 * 原理：一条扫描里，第 p 页的 6 个字节 = 8 行 × 6 个阳极槽（48 bit），而它只由**3 个源字节**决定：
 *   源字节 dx 的第 m 位 → 组内第 (6*m + off(dx)) 位（off 见 §顶部定标：t 偶 {0,2,4} / t 奇 {5,3,1}）。
 * 于是把"逐位 scatter"换成"稠密 gather"：查表拿到整个源字节的摊开结果，再按列移位或起来：
 *   g_spread[b] 的 bit(6*m) = b 的 bit m   ⇒  lo 段放 m=0..3、hi 段放 m=4..7
 * 每 (scan, page) 只需 3 次查表 + 2 次移位 + 2 次或 + 6 字节写，且**天然把填充位留成 0**
 * （旧实现要先把 48 B 清零再逐位置 1）。表在编译期生成，落在 .rodata（各 1 KB）。
 *
 * 原"逐位 scatter"版本保留在 `tests/test_main.cpp` §8 里作为独立参考实现（逐位写、写法完全不同），
 * 新实现的等价性由它 + `tests/reference_pack.cpp`（原驱动 1:1 转录）双重锁死。 */
struct SpreadTables {
    uint32_t lo[256]; /* bit m (m=0..3) → bit(6*m)      */
    uint32_t hi[256]; /* bit m (m=4..7) → bit(6*(m-4))  */
    constexpr SpreadTables()
        : lo()
        , hi()
    {
        for (int b = 0; b < 256; ++b) {
            for (int m = 0; m < 8; ++m) {
                if (((b >> m) & 1) == 0)
                    continue;
                const int pos = 6 * m;
                if (pos < 24)
                    lo[b] |= 1u << pos;
                else
                    hi[b] |= 1u << (pos - 24);
            }
        }
    }
};
inline constexpr SpreadTables SPREAD {};

/* ------------------------------------------------------------------ 传输层适配
 *
 * 为什么需要它：把"逻辑打包"与"总线/移位链实际需要的顺序"分开。
 *
 *  · RP2040 的 SPI（PL022）**只支持 MSB-first**（Pico SDK: "order Must be
 *    SPI_MSB_FIRST, no other values supported on the PL022"），而本驱动的位映射
 *    （k = 48*page + 6*m + off，与原驱动、与手册数据号一致）是按 **LSB-first** 设计的
 *    ⇒ tick 引擎必须让总线上出现"每字节 8 位反序"的顺序。**2026-10-13 起这件事在打包时就做掉**
 *    （`packFrame(..., wireOrder=true)`，SWAR 反转每个输出字节），不再单独遍历整帧。
 *  · PIO 引擎用 `out pins,1`（OSR 右移）本身就是 LSB-first，`wireOrder=false` 即可。
 *  · 两者都要做一次**扫描相位旋转**：面板在扫描边界的 LAT 锁存的是**上一扫描**移入的
 *    48 字节（该边界位于本扫描移位之前），所以要把整帧按扫描"提前一格"发，
 *    屏幕上才对齐；不补偿就整体偏一格 = 水平错 3 列。相位固定为 −1。
 */
inline uint8_t reverseBits8(uint8_t v)
{
    v = static_cast<uint8_t>(((v & 0xF0u) >> 4) | ((v & 0x0Fu) << 4));
    v = static_cast<uint8_t>(((v & 0xCCu) >> 2) | ((v & 0x33u) << 2));
    v = static_cast<uint8_t>(((v & 0xAAu) >> 1) | ((v & 0x55u) << 1));
    return v;
}

/* 同样的"每字节 8 位反序"，但一次处理 4 个字节（SWAR）：打包路径用它把反序折进写出，避免整帧再遍历。 */
inline uint32_t reverseBitsPerByte32(uint32_t x)
{
    x = ((x & 0xF0F0F0F0u) >> 4) | ((x & 0x0F0F0F0Fu) << 4);
    x = ((x & 0xCCCCCCCCu) >> 2) | ((x & 0x33333333u) << 2);
    x = ((x & 0xAAAAAAAAu) >> 1) | ((x & 0x55555555u) << 1);
    return x;
}

/* 就地把打包结果转成"该引擎传输层需要的顺序"：
 *   reverseBits : true → 逐字节位反序（**历史接口**：tick 现在在 packFrame 里就产出线序，
 *                        生产路径已不再传 true；保留它是为了兼容与回归测试）
 *                 false → 保持 LSB-first（PIO 用；tick 也用它，只做相位旋转）
 *   扫描相位旋转固定为 −1（该旋钮已在 2026-10-08 清理中移除，理由见上面的注释）。 */
void wirePrepareFrame(uint8_t *frame, bool reverseBits);

} /* namespace vfd */

#endif /* VFD_SCANPACK_H */
