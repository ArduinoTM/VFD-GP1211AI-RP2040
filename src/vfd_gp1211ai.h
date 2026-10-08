/*
 * VFD_GP1211AI（RP2040 移植版）—— GP1211AI / Noritake MN12864K 128x64 VFD 驱动。
 *
 * 相对原 STM32 版（Libraries/VFD_GP1211AI）的改动见 docs/02-RP2040移植说明.md，
 * 要点：
 *   1. 位重排抽到平台无关的 vfd_scanpack，硬件访问交给 vfd::Platform；
 *   2. display() 使用双缓冲 + 帧边界握手，消除"重排期间被扫描中断读到半成品"的撕裂；
 *   3. 颜色归一化：>=3 的颜色（含 GFX 默认的 0xFFFF 文字色）按 WHITE 处理，
 *      修复原版"默认文字色画不出字"的问题；实现 invertDisplay()；
 *   4. fillScreen(INVERSE) 语义与 fillRect(..., INVERSE) 一致（整体取反）；
 *   5. 禁止 setRotation()（旋转会让 _width/_height 与帧缓冲步长不一致，
 *      原版会越界写坏 _sendBuffer）；
 *   6. 扫描看护：task() 检测扫描心跳停摆，自动 recover()，连续失败则 emergencyOff()
 *      （手册明确"栅极扫描停止可能永久损坏 VFD"）。
 */
#ifndef VFD_GP1211AI_H
#define VFD_GP1211AI_H

#include "vfd_platform.h"
#include "vfd_scanpack.h"

#include "Adafruit_GFX.h"

#ifndef BLACK
#define BLACK 0
#endif
#ifndef WHITE
#define WHITE 1
#endif
#ifndef INVERSE
#define INVERSE 2
#endif

class VFD_GP1211AI : public Adafruit_GFX {
public:
    /* 扫描心跳超时（ms）：超过该时间没有新扫描就认为引擎停摆 */
    static constexpr uint32_t SCAN_STALL_TIMEOUT_MS = 50;
    /* 连续故障多少次后判定为不可恢复并紧急关断 */
    static constexpr uint32_t MAX_RECOVER_ATTEMPTS = 3;
    /* display() 等待帧边界的超时（ms） */
    static constexpr uint32_t FRAME_SYNC_TIMEOUT_MS = 40;

    explicit VFD_GP1211AI(vfd::Platform &platform);

    /* 初始化平台 + 发布空白帧 + 上电时序（含灯丝预热） */
    void begin(uint32_t preheat_ms = vfd::DEFAULT_PREHEAT_MS);

    /* ---- Adafruit_GFX 绘图原语（只有这 4 个会被基类其它 API 复用） ---- */
    void drawPixel(int16_t x, int16_t y, uint16_t color) override;
    void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override;
    void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override;
    void fillScreen(uint16_t color) override;

    /* ---- 显示控制 ---- */
    void clearDisplay() { fillScreen(BLACK); }

    /* 整幅覆盖帧缓冲（1024 B，页式布局见 vfd_scanpack.h）。
     * 给"外部数据源直接生成整屏"的用法（如 SSD1306 行为模拟）用，避免逐像素调用。 */
    void blitFramebuffer(const uint8_t *framebuffer);

    /* 位重排 + 发布（内部会等待扫描引擎离开工作缓冲，最坏阻塞一个帧周期 ≈8.1 ms） */
    void display();

    /* 反显：在重排时按像素取反 */
    void invertDisplay(boolean invert) override;

    /* 只支持 rotation = 0；其它值被忽略（保护帧缓冲不被越界写） */
    void setRotation(uint8_t r) override;

    void setBrightness(uint8_t brightness);
    uint8_t getBrightness() const { return _brightness; }

    /* 主循环周期调用：清 FIFO 杂务 + 扫描心跳看护 */
    bool task();

    bool isFatal() const { return _fatal; }
    uint32_t faultCount() const { return _faults; }
    uint32_t frameErrorCount() const { return _frameErrors; }
    const uint8_t *framebuffer() const { return _framebuffer; }

private:
    static uint16_t normalizeColor(uint16_t color);
    void waitForFrameBoundary();

    vfd::Platform &_platform;
    uint8_t _framebuffer[vfd::FRAMEBUFFER_SIZE];
    /* 双缓冲：一块交给扫描引擎读，一块给 display() 重排 */
    uint8_t _frameBuffer[2][vfd::FRAME_SIZE];
    uint8_t _writeIndex;      /* 下一次重排写入的缓冲下标 */
    bool _invert;
    uint8_t _brightness;

    bool _published;          /* 是否已经发布过至少一帧 */
    uint32_t _lastSeq;        /* 上一次发布时的 frameStartCount */
    uint32_t _frameErrors;

    uint32_t _lastScanCount;
    uint32_t _lastScanMillis;
    uint32_t _faults;
    uint32_t _recoverAttempts;
    bool _fatal;
};

#endif /* VFD_GP1211AI_H */
