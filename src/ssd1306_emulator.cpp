#include "ssd1306_emulator.h"

#include <string.h>

#include "vfd_crc32.h"

namespace vfd {

/* GDDRAM 的 CRC32（"两端对照"用；算法与主控测试程序里的同名函数一致，见 vfd_crc32.h） */
uint32_t Ssd1306Emulator::gdramCrc32() const
{
    return crc32(_gdram, SSD1306_GDDRAM_SIZE);
}

void Ssd1306Emulator::reset()
{
    memset(_gdram, 0, sizeof(_gdram));

    /* SSD1306 复位默认值（见规格书 Table 8-4）：
     * 显示关、页寻址、列窗口 0..127、页窗口 0..7、对比度 0x7F、正常显示 */
    _mode = ADDR_PAGE;
    _col = 0;
    _page = 0;
    _colStart = 0;
    _colEnd = SSD1306_WIDTH - 1;
    _pageStart = 0;
    _pageEnd = SSD1306_PAGES - 1;

    _pendingCmd = 0;
    _pendingParams = 0;
    _pendingGot = 0;
    memset(_params, 0, sizeof(_params));

    _displayOn = false;
    _entireOn = false;
    _inverse = false;
    _segRemap = false;
    _comScanReversed = false;
    _contrast = 0x7F;
    _offset = 0;
    _startLine = 0;
    _multiplex = SSD1306_HEIGHT - 1;

    _commandCount = 0;
    _dataCount = 0;
    _unknownCount = 0;
    _lastCommand = 0;

    _dirty = true;
    _lastRenderMs = 0;

    /* 滚动：默认未配置、未启动（SSD1306 复位后需主机显式设置并 0x2F 启动） */
    _scrollConfigured = false;
    _scrollActive = false;
    _scrollRight = true;
    _scrollRestart = false;
    _scrollMode = SCROLL_OFF;
    _scrollStartPage = 0;
    _scrollEndPage = 0;
    _scrollInterval = 0;
    _scrollVertOffset = 0;
    _scrollSteps = 0;
    _scrollLastMs = 0;
}

uint32_t Ssd1306Emulator::scrollPeriodMs() const
{
    return static_cast<uint32_t>(SSD1306_SCROLL_STEP_FRAMES[_scrollInterval & 0x07])
        * SSD1306_SCROLL_FRAME_MS;
}

uint8_t Ssd1306Emulator::paramCountOf(uint8_t cmd)
{
    switch (cmd) {
    case 0x20: /* 寻址模式 */
    case 0x81: /* 对比度 */
    case 0x8D: /* 电荷泵 */
    case 0xA8: /* 多路复用比 */
    case 0xD3: /* 显示偏移 */
    case 0xD5: /* 时钟分频 */
    case 0xD9: /* 预充电周期 */
    case 0xDA: /* COM 引脚配置 */
    case 0xDB: /* VCOMH */
        return 1;
    case 0x21: /* 列地址窗口 */
    case 0x22: /* 页地址窗口 */
        return 2;
    case 0x26: /* 水平滚动设置 */
    case 0x27:
    case 0x29: /* 垂直+水平滚动设置 */
    case 0x2A:
        return 6;
    default:
        return 0;
    }
}

void Ssd1306Emulator::pushByte(uint8_t value, bool dc)
{
    if (dc)
        handleData(value);
    else
        handleCommand(value);
}

void Ssd1306Emulator::handleCommand(uint8_t cmd)
{
    _commandCount++;
    _lastCommand = cmd;

    /* 正在收集参数（多字节命令，参数同样以 DC=0 传输） */
    if (_pendingParams != 0) {
        if (_pendingGot < sizeof(_params))
            _params[_pendingGot] = cmd;
        _pendingGot++;
        if (_pendingGot >= _pendingParams) {
            applyCommand(_pendingCmd);
            _pendingParams = 0;
            _pendingGot = 0;
        }
        return;
    }

    /* ---- 无参数命令 ---- */
    if (cmd <= 0x0F) { /* 低列地址（页模式）：列地址是 7 位，只取低 4 位 */
        _col = static_cast<uint8_t>((_col & 0x70) | (cmd & 0x0F));
        return;
    }
    if (cmd <= 0x1F) { /* 高列地址（页模式）：只取低 3 位（0..127） */
        _col = static_cast<uint8_t>((_col & 0x0F) | ((cmd & 0x07) << 4));
        return;
    }
    if (cmd >= 0x40 && cmd <= 0x7F) { /* 显示起始行（仅记录，不参与渲染） */
        _startLine = static_cast<uint8_t>(cmd & 0x3F);
        return;
    }
    if (cmd >= 0xB0 && cmd <= 0xB7) { /* 页地址（页模式） */
        _page = static_cast<uint8_t>(cmd & 0x07);
        return;
    }

    /* ⚠️ 凡是会**改变渲染结果**的命令都必须置 `_dirty`：应用侧（见 src/main.cpp）只在
     * `shouldRender()` 为真时才调 `renderToFramebuffer()`，而 `shouldRender()` 以 `_dirty` 为前提。
     * 曾经只有 0xAE/0xAF 置 dirty ⇒ 宿主"只发命令、不发数据"时（反显/全亮/显示开关）**永远不会重绘**，
     * 屏幕停在上一帧：现场表现为"0xA7/0xA5 没反应；0xAE→0xAF 时屏幕变成全亮（那一刻才重绘，
     * 而 `_entireOn` 还是 0xA5 留下的 true），0xA4/0xA6 又改不回来，直到滚动相位
     * （滚动步进会置 dirty）才恢复"（2026-10-06 用户实测）。 */
    switch (cmd) {
    case 0xA0:
        _segRemap = false;
        _dirty = true;
        return;
    case 0xA1:
        _segRemap = true;
        _dirty = true;
        return;
    case 0xA4:
        _entireOn = false;
        _dirty = true;
        return;
    case 0xA5:
        _entireOn = true;
        _dirty = true;
        return;
    case 0xA6:
        _inverse = false;
        _dirty = true;
        return;
    case 0xA7:
        _inverse = true;
        _dirty = true;
        return;
    case 0xAE:
        _displayOn = false;
        _dirty = true;
        return;
    case 0xAF:
        _displayOn = true;
        _dirty = true;
        return;
    case 0xC0:
        _comScanReversed = false;
        _dirty = true;
        return;
    case 0xC8:
        _comScanReversed = true;
        _dirty = true;
        return;
    case 0x2E: /* 停止滚动 */
        _scrollActive = false;
        return;
    case 0x2F: /* 启动滚动（未配置过则忽略，与真实芯片一致） */
        if (_scrollConfigured && _scrollMode != SCROLL_OFF) {
            _scrollActive = true;
            _scrollRestart = true;
        }
        return;
    case 0xE3: /* NOP */
        return;
    default:
        break;
    }

    /* ---- 带参数命令 ---- */
    const uint8_t n = paramCountOf(cmd);
    if (n != 0) {
        _pendingCmd = cmd;
        _pendingParams = n;
        _pendingGot = 0;
        return;
    }

    /* 未知命令：忽略但计数（保证兼容不同主机库的额外初始化命令） */
    _unknownCount++;
}

void Ssd1306Emulator::applyCommand(uint8_t cmd)
{
    /* 滚动设置命令（6 参数）单独处理 */
    if (cmd == 0x26 || cmd == 0x27 || cmd == 0x29 || cmd == 0x2A) {
        applyScrollCommand(cmd);
        return;
    }

    switch (cmd) {
    case 0x20: /* 寻址模式 */
        if (_params[0] <= ADDR_PAGE)
            _mode = _params[0];
        break;
    case 0x21: { /* 列地址窗口：写入指针回到窗口起点 */
        _colStart = static_cast<uint8_t>(_params[0] & 0x7F);
        _colEnd = static_cast<uint8_t>(_params[1] & 0x7F);
        if (_colEnd < _colStart)
            _colEnd = _colStart;
        _col = _colStart;
        break;
    }
    case 0x22: { /* 页地址窗口 */
        _pageStart = static_cast<uint8_t>(_params[0] & 0x07);
        _pageEnd = static_cast<uint8_t>(_params[1] & 0x07);
        if (_pageEnd < _pageStart)
            _pageEnd = _pageStart;
        _page = _pageStart;
        break;
    }
    case 0x81: /* 对比度 → 由应用映射到 VFD 亮度 */
        _contrast = _params[0];
        break;
    case 0xA8:
        _multiplex = _params[0];
        break;
    case 0xD3: /* 显示偏移：影响渲染 ⇒ 也要置 dirty */
        _offset = static_cast<uint8_t>(_params[0] & 0x3F);
        _dirty = true;
        break;
    case 0x8D: /* 电荷泵：模拟器无需真实升压，吞掉参数 */
    case 0xD5:
    case 0xD9:
    case 0xDA:
    case 0xDB:
        break;
    default:
        _unknownCount++;
        break;
    }
}

void Ssd1306Emulator::handleData(uint8_t value){
    _dataCount++;

    uint8_t *cell = &_gdram[_page * SSD1306_WIDTH + _col];
    *cell = value;
    _dirty = true;

    /* 写入指针推进规则（SSD1306 规格书 8.1.2~8.1.4） */
    switch (_mode) {
    case ADDR_PAGE: /* 页寻址：列自动加一，越界回到该页列 0 */
        _col = static_cast<uint8_t>((_col + 1) & (SSD1306_WIDTH - 1));
        break;
    case ADDR_HORIZONTAL: /* 水平寻址：列满→回到列起点、页加一，页满→回到页起点 */
        if (_col >= _colEnd) {
            _col = _colStart;
            _page = (_page >= _pageEnd) ? _pageStart : static_cast<uint8_t>(_page + 1);
        } else {
            _col++;
        }
        break;
    case ADDR_VERTICAL: /* 垂直寻址：页满→回到页起点、列加一，列满→回到列起点 */
        if (_page >= _pageEnd) {
            _page = _pageStart;
            _col = (_col >= _colEnd) ? _colStart : static_cast<uint8_t>(_col + 1);
        } else {
            _page++;
        }
        break;
    default:
        break;
    }
}

void Ssd1306Emulator::renderToFramebuffer(uint8_t *framebuffer) const
{
    memset(framebuffer, 0, SSD1306_GDDRAM_SIZE);

    if (!_displayOn)
        return; /* 显示关闭 → 全黑 */
    if (_entireOn) {
        memset(framebuffer, 0xFF, SSD1306_GDDRAM_SIZE); /* 0xA5：全亮 */
        return;
    }

    for (int page = 0; page < SSD1306_PAGES; ++page) {
        for (int col = 0; col < SSD1306_WIDTH; ++col) {
            const uint8_t bits = _gdram[page * SSD1306_WIDTH + col];
            if (bits == 0)
                continue;

            /* 段重映射（0xA0/0xA1）与 COM 扫描方向（0xC0/0xC8）按**标准模块**约定解释：
             * Adafruit_SSD1306 / u8g2 等库默认发送 0xA1 + 0xC8，此时主机缓冲与面板显示同向，
             * 因此这里不做镜像；相反设置则镜像对应方向（等价于模块被翻转安装）。 */
            const int x = _segRemap ? col : (SSD1306_WIDTH - 1 - col);

            for (int b = 0; b < 8; ++b) {
                if (((bits >> b) & 1u) == 0)
                    continue;
                /* GDDRAM 行（0..63，D0 在页顶） */
                const int row = page * 8 + b;
                /* 显示偏移（0xD3）：整幅上移 offset 行 */
                const int shifted = (row - _offset) & (SSD1306_HEIGHT - 1);
                /* COM 扫描方向：0xC8 为正常方向 */
                const int y = _comScanReversed ? shifted : (SSD1306_HEIGHT - 1 - shifted);
                framebuffer[(y >> 3) * SSD1306_WIDTH + x] |= static_cast<uint8_t>(1u << (y & 7));
            }
        }
    }

    if (_inverse) { /* 0xA7：反显 */
        for (int i = 0; i < SSD1306_GDDRAM_SIZE; ++i)
            framebuffer[i] = static_cast<uint8_t>(~framebuffer[i]);
    }
}

bool Ssd1306Emulator::shouldRender(uint32_t nowMs, uint32_t minIntervalMs)
{
    if (!_dirty)
        return false;
    if (static_cast<uint32_t>(nowMs - _lastRenderMs) < minIntervalMs)
        return false;
    _lastRenderMs = nowMs;
    _dirty = false;
    return true;
}

/* ------------------------------------------------------------------ 滚动 */

/*
 * SSD1306 的滚动是"硬件改写 GDDRAM"：显示内容在【页窗口】内循环平移，
 * 移出窗口的数据从另一侧回到窗口（因此是循环滚动，不会丢内容）。
 * 参数布局（6 个参数，均以 DC=0 传输）：
 *   [0] 空字节         [1] 起始页        [2] 间隔（0..7 → 帧数见 SSD1306_SCROLL_STEP_FRAMES）
 *   [3] 结束页         [4] 垂直偏移      [5] 空字节
 * 0x26/0x27 = 纯水平（右/左），0x29/0x2A = 垂直+水平（右/左）。
 * 时间基准：1 帧 = 8 ms（本机 VFD 123 Hz 帧周期；真实芯片由内部振荡器与分频决定，量级相同）。
 */
void Ssd1306Emulator::applyScrollCommand(uint8_t cmd)
{
    _scrollMode = (cmd == 0x26 || cmd == 0x27) ? SCROLL_HORIZONTAL : SCROLL_VERTICAL_HORIZONTAL;
    _scrollRight = (cmd == 0x26 || cmd == 0x29);

    _scrollStartPage = static_cast<uint8_t>(_params[1] & 0x07);
    _scrollEndPage = static_cast<uint8_t>(_params[3] & 0x07);
    if (_scrollEndPage < _scrollStartPage)
        _scrollEndPage = _scrollStartPage; /* 规格书要求 结束页 >= 起始页，异常时收敛 */

    _scrollInterval = static_cast<uint8_t>(_params[2] & 0x07);

    /* 垂直偏移：规格书规定 0x01..0x3F（0 非法），异常值收敛为 1 */
    uint8_t v = static_cast<uint8_t>(_params[4] & 0x3F);
    _scrollVertOffset = (v == 0) ? 1 : v;

    _scrollConfigured = true;
    _scrollRestart = true; /* 从现在开始计时（避免立刻补播一堆步） */
}

void Ssd1306Emulator::scrollHorizontal(bool right)
{
    for (uint8_t p = _scrollStartPage; p <= _scrollEndPage; ++p) {
        uint8_t *row = &_gdram[p * SSD1306_WIDTH];
        if (right) { /* 内容右移：new[c] = old[c-1]，第 127 列回到第 0 列 */
            const uint8_t last = row[SSD1306_WIDTH - 1];
            memmove(row + 1, row, SSD1306_WIDTH - 1);
            row[0] = last;
        } else { /* 内容左移：new[c] = old[c+1]，第 0 列回到第 127 列 */
            const uint8_t first = row[0];
            memmove(row, row + 1, SSD1306_WIDTH - 1);
            row[SSD1306_WIDTH - 1] = first;
        }
    }
}

void Ssd1306Emulator::scrollVertical(uint8_t rows)
{
    const uint8_t pageCount = static_cast<uint8_t>(_scrollEndPage - _scrollStartPage + 1);
    const uint8_t rowCount = static_cast<uint8_t>(pageCount * 8); /* 8..64 */
    const uint8_t shift = static_cast<uint8_t>(rows % rowCount);
    if (shift == 0)
        return;

    for (int x = 0; x < SSD1306_WIDTH; ++x) {
        /* 把该列在窗口内的 rowCount 位取成位向量（bit r = 窗口内第 r 行） */
        uint64_t col = 0;
        for (uint8_t r = 0; r < rowCount; ++r) {
            const uint8_t page = static_cast<uint8_t>(_scrollStartPage + (r >> 3));
            const uint8_t bit = static_cast<uint8_t>(r & 7);
            if ((_gdram[page * SSD1306_WIDTH + x] >> bit) & 1u)
                col |= (static_cast<uint64_t>(1) << r);
        }

        /* 内容下移 shift 行：new[r] = old[r-shift] ⇒ 位向量左旋 shift */
        col = (col << shift) | (col >> (rowCount - shift));

        /* 写回窗口内该列的全部行 */
        for (uint8_t r = 0; r < rowCount; ++r) {
            const uint8_t page = static_cast<uint8_t>(_scrollStartPage + (r >> 3));
            const uint8_t bit = static_cast<uint8_t>(r & 7);
            uint8_t &cell = _gdram[page * SSD1306_WIDTH + x];
            if ((col >> r) & 1u)
                cell |= static_cast<uint8_t>(1u << bit);
            else
                cell &= static_cast<uint8_t>(~(1u << bit));
        }
    }
}

void Ssd1306Emulator::scrollStep()
{
    _scrollSteps++;
    if (_scrollMode == SCROLL_VERTICAL_HORIZONTAL)
        scrollVertical(_scrollVertOffset);
    if (_scrollMode != SCROLL_OFF)
        scrollHorizontal(_scrollRight);
    _dirty = true;
}

void Ssd1306Emulator::tick(uint32_t nowMs)
{
    if (_scrollRestart) { /* 0x2F（或重新配置）之后从当前时刻起算 */
        _scrollLastMs = nowMs;
        _scrollRestart = false;
    }
    if (!_scrollActive || _scrollMode == SCROLL_OFF)
        return;

    const uint32_t period = scrollPeriodMs();
    if (period == 0)
        return;

    uint32_t steps = 0;
    while (static_cast<uint32_t>(nowMs - _scrollLastMs) >= period
        && steps < SSD1306_SCROLL_MAX_CATCHUP) {
        _scrollLastMs += period;
        scrollStep();
        steps++;
    }
    if (steps >= SSD1306_SCROLL_MAX_CATCHUP)
        _scrollLastMs = nowMs; /* 长时间阻塞后不与现实脱节：丢弃未补的步数 */
}

} /* namespace vfd */
