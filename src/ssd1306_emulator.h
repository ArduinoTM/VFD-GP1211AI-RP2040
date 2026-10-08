/*
 * ssd1306_emulator —— SSD1306 驱动芯片的**行为模拟**（平台无关，可在 PC 上完整测试）。
 *
 * 用途：把本工程的 RP2040 变成"一片 SSD1306 OLED"：
 * 主机 MCU（任何用 Adafruit_SSD1306 / u8g2 / 裸驱动的板子）通过 4 线 SPI 把命令与
 * GDDRAM 数据发过来，本模块解析命令、维护 128×64 GDDRAM（8 页 × 128 字节），
 * 再按 SSD1306 的显示规则（段重映射 / COM 扫描方向 / 显示偏移 / 反显 / 全亮 / 开关）
 * 渲染成本工程 VFD 驱动的帧缓冲（1bpp 页式，布局与 SSD1306 GDDRAM 一致）。
 *
 *     k = 8*m + b   (b = 字节内位号, 详见 vfd_scanpack.h)
 *
 * 支持的 SSD1306 命令（其余命令按"忽略但计数"处理，保证与各种主机库兼容）：
 *   0x00–0x0F  低列地址（页模式）        0x10–0x1F  高列地址（页模式）
 *   0x20       寻址模式（0 水平 / 1 垂直 / 2 页）
 *   0x21/0x22  列地址窗口 / 页地址窗口（各 2 参数）
 *   0x40–0x7F  显示起始行（记录，不参与渲染）
 *   0x81       对比度（1 参数 → 映射到 VFD 亮度）
 *   0xA0/0xA1  段重映射关/开             0xA4/0xA5  全亮关/开
 *   0xA6/0xA7  正常/反显                  0xAE/0xAF  显示关/开
 *   0xA8       多路复用比（记录）         0xB0–0xB7  页地址（页模式）
 *   0xC0/0xC8  COM 扫描方向              0xD3       显示偏移（1 参数）
 *   0x8D/0xD5/0xD9/0xDA/0xDB  电荷泵/时钟/预充/COM 配置/VCOMH（参数被吞掉）
 *   0x26/0x27  水平滚动（右/左，6 参数）  0x29/0x2A  垂直+水平滚动（右/左，6 参数）
 *   0x2E/0x2F  停止/启动滚动
 *
 * 滚动按**规格书的语义**实现：滚动是"改写 GDDRAM"（内容在页窗口内循环平移），
 * 因此渲染路径完全不用改；时间基准见 §滚动 注释（1 帧 = 本机刷新周期 8 ms）。
 *
 * 注意：主机的 SPI 模式必须是模式 0（CPOL=0, CPHA=0），与 SSD1306 4 线 SPI 一致；
 *       每字节的 DC 电平由传输层逐字节提供（见 ssd1306_slave_rp2040.h）。
 */
#ifndef VFD_SSD1306_EMULATOR_H
#define VFD_SSD1306_EMULATOR_H

#include <stdint.h>

namespace vfd {

constexpr int SSD1306_WIDTH = 128;
constexpr int SSD1306_HEIGHT = 64;
constexpr int SSD1306_PAGES = SSD1306_HEIGHT / 8;
constexpr int SSD1306_GDDRAM_SIZE = SSD1306_WIDTH * SSD1306_PAGES; /* 1024 B */

/* 滚动时间基准：SSD1306 的"帧"折算成本机 VFD 帧周期（123.05 Hz ⇒ 8.13 ms，取 8 ms）。
 * 规格书的间隔参数是以"帧"为单位的（见 SCROLL_STEP_FRAMES）。 */
constexpr uint32_t SSD1306_SCROLL_FRAME_MS = 8;

/* 间隔参数（0x26/0x27/0x29/0x2A 的第 3 个参数）→ 每步间隔的帧数（规格书 Table 9-1） */
constexpr uint16_t SSD1306_SCROLL_STEP_FRAMES[8] = { 5, 64, 128, 256, 3, 4, 25, 2 };

/* 单次 tick() 最多补执行的步数（长时间阻塞后不"快进"补播） */
constexpr uint32_t SSD1306_SCROLL_MAX_CATCHUP = 8;

class Ssd1306Emulator {
public:
    /* 寻址模式（SSD1306 0x20 命令） */
    enum AddressingMode : uint8_t {
        ADDR_HORIZONTAL = 0,
        ADDR_VERTICAL = 1,
        ADDR_PAGE = 2,
    };

    Ssd1306Emulator() { reset(); }

    /* 上电/复位状态：GDDRAM 清 0、页寻址模式、显示关、对比度 0x7F（SSD1306 复位默认） */
    void reset();

    /* 送入一个字节；dc=false 表示命令（DC 低），true 表示数据（DC 高） */
    void pushByte(uint8_t value, bool dc);

    /* 主循环周期调用：按时间推进滚动（滚动活跃时每个间隔步改写一次 GDDRAM）。
     * 时间基准：1 "SSD1306 帧" = 8 ms（本机 VFD 123 Hz 帧周期）。 */
    void tick(uint32_t nowMs);

    /* 渲染到 1bpp 页式帧缓冲（1024 B，布局见 vfd_scanpack.h）。
     * 内部会应用：显示开关、全亮、段重映射、COM 扫描方向、显示偏移、反显。 */
    void renderToFramebuffer(uint8_t *framebuffer) const;

    /* ---- 状态与诊断（渲染策略 / 测试 / 串口打印用）---- */
    const uint8_t *gdram() const { return _gdram; }
    /* GDDRAM 1024 B 的 CRC32（IEEE）—— "两端对照"的端到端判据：
     * 主控算出自己发出去的图像 CRC，与从机串口打印的 gdram_crc 一比即可，
     * 不需要相机。算法见 vfd_crc32.h。 */
    uint32_t gdramCrc32() const;
    bool dirty() const { return _dirty; }
    void clearDirty() { _dirty = false; }

    bool displayOn() const { return _displayOn; }
    bool entireDisplayOn() const { return _entireOn; }
    bool inverse() const { return _inverse; }
    bool segmentRemap() const { return _segRemap; }
    bool comScanReversed() const { return _comScanReversed; }
    uint8_t contrast() const { return _contrast; }
    uint8_t addressingMode() const { return _mode; }
    uint8_t columnAddress() const { return _col; }
    uint8_t pageAddress() const { return _page; }
    uint8_t displayOffset() const { return _offset; }
    uint8_t displayStartLine() const { return _startLine; }
    uint8_t multiplexRatio() const { return _multiplex; }

    uint32_t commandCount() const { return _commandCount; }
    uint32_t dataCount() const { return _dataCount; }
    uint32_t unknownCommandCount() const { return _unknownCount; }
    uint8_t lastCommand() const { return _lastCommand; }

    /* ---- 滚动状态（0x26/0x27/0x29/0x2A 设置，0x2E 停止，0x2F 启动）---- */
    enum ScrollMode : uint8_t {
        SCROLL_OFF = 0,
        SCROLL_HORIZONTAL = 1,
        SCROLL_VERTICAL_HORIZONTAL = 2,
    };

    bool scrollConfigured() const { return _scrollConfigured; }
    bool scrollActive() const { return _scrollActive; }
    ScrollMode scrollMode() const { return _scrollMode; }
    bool scrollRight() const { return _scrollRight; }
    uint8_t scrollStartPage() const { return _scrollStartPage; }
    uint8_t scrollEndPage() const { return _scrollEndPage; }
    uint8_t scrollInterval() const { return _scrollInterval; }
    uint8_t scrollVerticalOffset() const { return _scrollVertOffset; }
    uint32_t scrollStepCount() const { return _scrollSteps; }
    /* 当前配置下每步的间隔（ms）= 帧数 × 8 ms */
    uint32_t scrollPeriodMs() const;

    /* 主循环渲染策略：有数据写入且距上次渲染 >= minIntervalMs 时返回 true */
    bool shouldRender(uint32_t nowMs, uint32_t minIntervalMs);

private:
    void handleCommand(uint8_t cmd);
    void applyCommand(uint8_t cmd); /* 参数收齐后执行 */
    void handleData(uint8_t value);
    static uint8_t paramCountOf(uint8_t cmd);

    /* 滚动 */
    void applyScrollCommand(uint8_t cmd); /* 6 个参数收齐后配置滚动 */
    void scrollStep();                    /* 执行一步（水平，或垂直+水平） */
    void scrollHorizontal(bool right);    /* 页窗口内每页整体平移 1 列（循环） */
    void scrollVertical(uint8_t rows);    /* 页窗口内整体下移 rows 行（循环） */

    uint8_t _gdram[SSD1306_GDDRAM_SIZE];

    /* 寻址与窗口 */
    uint8_t _mode;                /* AddressingMode */
    uint8_t _col, _page;          /* 当前写入指针 */
    uint8_t _colStart, _colEnd;   /* 0x21 设置的列窗口 */
    uint8_t _pageStart, _pageEnd; /* 0x22 设置的页窗口 */

    /* 多字节命令解析（参数最多 6 个：滚动设置命令） */
    uint8_t _pendingCmd;
    uint8_t _pendingParams;
    uint8_t _pendingGot;
    uint8_t _params[6];

    /* 显示配置 */
    bool _displayOn;
    bool _entireOn;
    bool _inverse;
    bool _segRemap;
    bool _comScanReversed;
    uint8_t _contrast;
    uint8_t _offset;
    uint8_t _startLine;
    uint8_t _multiplex;

    /* 统计 */
    uint32_t _commandCount;
    uint32_t _dataCount;
    uint32_t _unknownCount;
    uint8_t _lastCommand;

    bool _dirty;
    uint32_t _lastRenderMs;

    /* 滚动状态 */
    bool _scrollConfigured;
    bool _scrollActive;
    bool _scrollRight;
    bool _scrollRestart; /* 0x2F 之后从当前时刻重新计时 */
    ScrollMode _scrollMode;
    uint8_t _scrollStartPage;
    uint8_t _scrollEndPage;
    uint8_t _scrollInterval;
    uint8_t _scrollVertOffset;
    uint32_t _scrollSteps;
    uint32_t _scrollLastMs;
};

} /* namespace vfd */

#endif /* VFD_SSD1306_EMULATOR_H */
