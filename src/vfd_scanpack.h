#ifndef VFD_SCANPACK_H
#define VFD_SCANPACK_H

#include <stdint.h>

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
constexpr int SCANS_PER_FRAME = 43;     /* 一帧 43 个时序 */
constexpr int SCAN_BYTES = 48;          /* 每个时序 384 bit = 48 字节 */
constexpr int FRAMEBUFFER_SIZE = WIDTH * PAGES;                   /* 1024 B */
constexpr int FRAME_SIZE = SCANS_PER_FRAME * SCAN_BYTES;          /* 2064 B */

/* 帧缓冲访问：fb[y>>3][x] 的 bit(y&7)，与 Adafruit_SSD1306 布局一致 */
inline bool pixelGet(const uint8_t *fb, int x, int y)
{
    return (fb[(y >> 3) * WIDTH + x] >> (y & 7)) & 0x01u;
}

/* 重排单个扫描：out 必须是 SCAN_BYTES 字节，函数内部会先清零。
 * invert=true 时按像素取反（用于 invertDisplay），只翻转真实像素位，
 * 不会污染"另一组阳极必须为 0"的填充位。 */
void packScan(const uint8_t *fb, int scan, bool invert, uint8_t *out);

/* 重排整帧：out 必须是 FRAME_SIZE 字节，扫描 0 在前。 */
void packFrame(const uint8_t *fb, bool invert, uint8_t *out);

/* ------------------------------------------------------------------ 传输层适配
 *
 * 为什么需要它：把"逻辑打包"与"总线/移位链实际需要的顺序"分开。
 *
 *  · RP2040 的 SPI（PL022）**只支持 MSB-first**（Pico SDK: "order Must be
 *    SPI_MSB_FIRST, no other values supported on the PL022"），而本驱动的位映射
 *    （k = 48*page + 6*m + off，与原驱动、与手册数据号一致）是按 **LSB-first** 设计的
 *    ⇒ tick 引擎必须在打包后把**每个字节的 8 位反序**，才能让总线上出现正确的位顺序。
 *  · PIO 引擎用 `out pins,1`（OSR 右移）本身就是 LSB-first，不需要反序。
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

/* 就地把打包结果转成"该引擎传输层需要的顺序"：
 *   reverseBits : true → 逐字节位反序（tick/SPI 用）；false → 保持 LSB-first（PIO 用）
 *   扫描相位旋转固定为 −1（该旋钮已在 2026-10-08 清理中移除，理由见上面的注释）。 */
void wirePrepareFrame(uint8_t *frame, bool reverseBits);

} /* namespace vfd */

#endif /* VFD_SCANPACK_H */
