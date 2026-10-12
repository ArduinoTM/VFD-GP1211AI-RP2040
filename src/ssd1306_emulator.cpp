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

    memset(_cmdSeen, 0, sizeof(_cmdSeen));
    memset(_paramSeen, 0, sizeof(_paramSeen));
    _commandCount = 0;
    _dataCount = 0;
    _resyncCount = 0;
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
    /* 垂直滚动区域复位默认（规格书 0xA3：A 复位 0 行、B 复位 64 行 = 整个屏幕） */
    _vAreaTopRows = 0;
    _vAreaScrollRows = SSD1306_HEIGHT;
    _vOffsetRows = 0;
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
    case 0xA3: /* 垂直滚动区域（Set Vertical Scroll Area）：**必须吞掉 2 个参数**
                * ⚠️ 以前漏登记 ⇒ 主机若发 `A3 00 3F`，那两个参数会被当成独立命令解析
                *    （`0x00` 落在"设置低列地址"⇒ 写指针被挪走、画面错位），且永不自行纠正。
                *    现在这两个参数**有语义**：A = 顶部固定行数（复位 0）、B = 滚动区行数（复位 64），
                *    见 applyCommand() 的 0xA3 分支与 docs/05 §3.6。 */
        return 2;
    case 0x26: /* 水平滚动设置（右/左）：**6 个参数**
                * 主机库实际字节流：`26|27 00 <起始页> <间隔> <结束页> 00 FF` + `2F` */
    case 0x27:
        return 6;
    case 0x29: /* 垂直+水平滚动设置（右/左）：**5 个参数**（结尾没有第二个空字节！）
                * 三个独立实现一致：Adafruit_SSD1306 2.5.17（`29|2A 00 <起始页> <间隔> <结束页> 01`
                * 之后**紧接** `2F`）、dotnet/iot `ContinuousVerticalAndHorizontalScrollSetup`
                * （Id,0x00,start,rate,end,vertOffset）、Parallax badgewxtools `oled_scrollRightDiag.c`。
                * ⚠️ 以前按 6 个参数登记 ⇒ 主机紧跟的 `2F`（启动滚动）被当成第 6 个参数吃掉，
                *    滚动**永不启动**：水平滚动能动、斜向/垂直一动不动（现场症状）。
                *    规格书表格里 0x29/0x2A 只有 A..E 五个参数（E = 垂直偏移）。 */
    case 0x2A:
        return 5;
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

void Ssd1306Emulator::endTransaction()
{
    /* 只作废"没收齐参数"的那条命令。已收齐并 apply 过的命令、以及数据字节都不受影响 ——
     * 与真 SSD1306 在 CS 释放时"未完成的多字节命令作废"一致。
     * ⚠️ 不要在这里复位寻址模式/写指针：它们是显示器的持久状态，真机也不会因一次 CS 释放改变。 */
    if (_pendingParams != 0) {
        _pendingParams = 0;
        _pendingGot = 0;
        _resyncCount++;
    }
}

void Ssd1306Emulator::handleCommand(uint8_t cmd)
{
    /* 诊断位图：把"这个 DC=0 字节到底被当成命令还是参数"记下来（排故用，不影响行为） */
    if (_pendingParams == 0) {
        _cmdSeen[cmd >> 3] |= static_cast<uint8_t>(1u << (cmd & 7));
    } else {
        _paramSeen[cmd >> 3] |= static_cast<uint8_t>(1u << (cmd & 7));
    }

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
    case 0x2E: /* 停止滚动：水平位移留在 GDDRAM 里（真机 0x2E 后仍需重写 RAM），
                * 而**垂直是显示级偏移 ⇒ 归零**（现场可见"上下位置自动复位、左右保留"）。 */
        _scrollActive = false;
        if (_vOffsetRows != 0) {
            _vOffsetRows = 0;
            _dirty = true; /* 复位本身也要重绘，否则画面停在旧位置 */
        }
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
    case 0xA3: { /* 垂直滚动区域：A = 顶部固定行数（复位 0）、B = 滚动区行数（复位 64）
                  * 滚动区从"顶部固定区的下一行"开始，区外的行在垂直方向固定不动。
                  * 本模拟器按规格书复位默认实现"全区滚动"，并支持局部区域（见 docs/05 §3.6）。 */
        uint8_t a = static_cast<uint8_t>(_params[0] & 0x3F); /* A[5:0] */
        uint8_t b = static_cast<uint8_t>(_params[1] & 0x7F); /* B[6:0] */
        if (a >= SSD1306_HEIGHT)
            a = 0;
        if (b == 0 || b > SSD1306_HEIGHT)
            b = static_cast<uint8_t>(SSD1306_HEIGHT - a); /* 异常/越界收敛，保证区非空 */
        if (static_cast<int>(a) + static_cast<int>(b) > SSD1306_HEIGHT)
            b = static_cast<uint8_t>(SSD1306_HEIGHT - a);
        if (b == 0) {
            a = 0;
            b = SSD1306_HEIGHT;
        }
        _vAreaTopRows = a;
        _vAreaScrollRows = b;
        _vOffsetRows = 0; /* 重新划分区域 ⇒ 垂直位置归零（与 0x2E 同理：偏移是显示级的） */
        _dirty = true;
        break;
    }
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
                int row = page * 8 + b;
                /* 垂直滚动（0x29/0x2A）：**显示级**行偏移，只在 0xA3 定义的滚动区内循环上移；
                 * 区外的行（顶部/底部固定区）不动。GDDRAM 不被改写 —— 这正是真机上
                 * "0x2E 停止后上下位置自动复位、左右位移保留"的原因（见 docs/05 §3.6）。 */
                if (_vOffsetRows != 0 && _vAreaScrollRows > 0) {
                    const int top = _vAreaTopRows;
                    const int rows = _vAreaScrollRows;
                    if (row >= top && row < top + rows)
                        row = top + (((row - top) - _vOffsetRows) % rows + rows) % rows;
                }
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
 * SSD1306 的滚动由**两条不同的通路**组成（与真机对齐的关键）：
 *   · 水平 = **改写 GDDRAM**：页窗口内每页循环平移 1 列，移出的列从另一侧回到窗口。
 *     因此 `0x2E` 停止之后水平位移仍留在显存里 —— 规格书正是这么写的：
 *     "使用 2Eh 命令关闭滚屏动作后，RAM 的数据需要重写"。
 *   · 垂直 = **显示级行偏移**：0x29/0x2A 每步把内容在 0xA3 定义的滚动区内上移 E 行（循环），
 *     GDDRAM 一个字节都不动。因此 `0x2E` 停止后**上下位置自动复位**，而左右保留
 *     （现场实测现象：禁用垂直滚动时画面上下回到原位、左右位移还在）。
 * 参数布局（均以 DC=0 传输；**个数按命令不同**，见 paramCountOf()）：
 *   [0] 空字节         [1] 起始页        [2] 间隔（0..7 → 帧数见 SSD1306_SCROLL_STEP_FRAMES）
 *   [3] 结束页         [4] 垂直偏移（仅 0x29/0x2A 有）         [5] 空字节（仅 0x26/0x27 有）
 *   0x26/0x27 = 纯水平（右/左）= 6 个参数；0x29/0x2A = 垂直+水平（右/左）= **5 个参数**。
 *   主机库随后紧跟一条 `0x2F`（启动滚动）——这正是 0x29/0x2A 只能吞 5 个参数的原因。
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

void Ssd1306Emulator::scrollStep()
{
    _scrollSteps++;
    /* 垂直分量：只累加**显示级**偏移，不碰 GDDRAM（见成员注释）。
     * 因此 0x2E 一停，垂直位置自动复位，而水平位移留在显存里继续可见。 */
    if (_scrollMode == SCROLL_VERTICAL_HORIZONTAL && _vAreaScrollRows > 0)
        _vOffsetRows = static_cast<uint8_t>((_vOffsetRows + _scrollVertOffset) % _vAreaScrollRows);
    if (_scrollMode != SCROLL_OFF)
        scrollHorizontal(_scrollRight); /* 水平：真机确实改写 GDDRAM（所以 0x2E 后仍在） */
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
