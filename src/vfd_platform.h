/*
 * vfd_platform.h —— 显示驱动与 MCU 之间的抽象接口。
 *
 * 关键设计：
 *   - 驱动（VFD_GP1211AI）只负责"画面 → 43×48 字节移位链数据"的重排；
 *   - 平台实现负责扫描引擎（定时器/DMA/SPI/PWM）与上电时序；
 *   - 帧缓冲以"整帧发布 + 帧边界切换"的方式交给引擎（双缓冲），
 *     驱动在写入工作缓冲前必须确认引擎已经离开该缓冲（见 display() 协议）。
 *
 * GP1211AI 的栅极扫描绝对不能长时间停止（Noritake MN12864K 手册 Note 14：
 * 停止扫描可能永久损坏屏），因此平台必须能报告"扫描心跳"并支持故障恢复。
 */
#ifndef VFD_PLATFORM_H
#define VFD_PLATFORM_H

#include <stdint.h>

#include "vfd_scanpack.h"

namespace vfd {

/* 上电时灯丝预热时间：手册未给出，社区实测原厂板需要数秒；这里默认 400 ms，
 * 比原驱动的 100 ms 更保守（灯丝未预热就上高压会影响寿命与亮度均匀性）。 */
constexpr uint32_t DEFAULT_PREHEAT_MS = 400;

class Platform {
public:
    virtual ~Platform() {}

    /* 引脚 / SPI / DMA / PWM / 扫描定时器初始化，并启动扫描引擎。
     * 返回后允许立刻 publishFrame()。 */
    virtual void init() = 0;

    /* 上电时序：先逻辑与灯丝、预热、后高压（手册 Note 3）。阻塞版：返回时高压已上电。 */
    virtual void powerUp(uint32_t preheat_ms) = 0;

    /* 同一段上电时序的**非阻塞**版本：powerUpBegin() 立即返回（关高压 → FLEN 脉冲
     * → 灯丝上电 → 开始预热），powerUpPoll() 由主循环反复调用推进，返回 true 表示
     * 整段时序完成（幂等，可重复调用）。
     *
     * 为什么需要：预热默认 400 ms。若这段时间 CPU 只是 sleep，任何需要及时服务的
     * 硬件（例如 SSD1306 从机的 DMA 环形缓冲）就只能靠有限缓冲兜底，主机的初始化
     * 序列与首批画面会丢 —— 上层用这两个接口就能"边预热边服务"。
     *
     * 默认实现退化为阻塞版 powerUp()，因此既有平台实现（含宿主测试的 MockPlatform）
     * 不需要任何改动。 */
    virtual void powerUpBegin(uint32_t preheat_ms) { powerUp(preheat_ms); }
    virtual bool powerUpPoll() { return true; }

    /* 紧急关断：先关高压再停扫描（保护屏），用于扫描故障无法恢复时。 */
    virtual void emergencyOff() = 0;

    /* 亮度 0..255（0 = 全暗）。实现方式为"点亮窗口宽度"控制，
     * 不违反手册 Note 7②"数据传输期间不得改变 BK"。 */
    virtual void setBrightness(uint8_t brightness) = 0;

    /* 发布一帧 FRAME_SIZE 字节数据；引擎在下一个帧边界切换过去。
     * frame 在引擎切走之前必须保持有效（由驱动双缓冲保证）。 */
    virtual void publishFrame(const uint8_t *frame) = 0;

    /* 传输层对帧字节的要求（默认与手册/原驱动的 LSB-first 语义一致）。
     * tick（SPI/PL022 只能 MSB-first）返回 true；PIO（out 右移）返回 false。 */
    virtual bool wireReversesByteBits() const { return false; }

    /* 自引擎启动以来"已开始的帧数"，每帧开始（scan==0）自增。
     * 驱动的帧同步协议依赖它判断"引擎是否已取走上一帧"。 */
    virtual uint32_t frameStartCount() const = 0;

    /* 扫描心跳：每完成一次扫描自增（正常约 43 × 123 = 5270 次/秒）。 */
    virtual uint32_t scanCount() const = 0;

    /* 主循环杂务（清 SPI RX FIFO 等），应在 loop() 中周期调用。 */
    virtual void service() = 0;

    /* 扫描停摆后的恢复：重启定时器/SPI/DMA，并复位扫描序号。 */
    virtual void recover() = 0;

    virtual uint32_t millis() const = 0;
    virtual void delayMs(uint32_t ms) = 0;
};

} /* namespace vfd */

#endif /* VFD_PLATFORM_H */
