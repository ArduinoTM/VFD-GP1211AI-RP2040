#include <cstdio>
#include "vfd_gp1211ai.h"

#include <string.h>

/* 颜色归一化：
 * 原版 drawPixel() 的 switch 没有 default，颜色 >= 3 会被静默丢弃，
 * 而 Adafruit_GFX 基类的默认文字色恰好是 0xFFFF —— 导致"用默认色画字什么也看不到"。
 * 这里把除 BLACK/INVERSE 之外的所有颜色都当作 WHITE。 */
uint16_t VFD_GP1211AI::normalizeColor(uint16_t color)
{
    if (color == BLACK || color == INVERSE)
        return color;
    return WHITE;
}

VFD_GP1211AI::VFD_GP1211AI(vfd::Platform &platform)
    : Adafruit_GFX(vfd::WIDTH, vfd::HEIGHT)
    , _platform(platform)
    , _writeIndex(1) /* begin() 发布 _frameBuffer[0] 作为首帧 */
    , _invert(false)
    , _brightness(200)
    , _published(false)
    , _lastSeq(0)
    , _frameErrors(0)
    , _lastScanCount(0)
    , _lastScanMillis(0)
    , _faults(0)
    , _recoverAttempts(0)
    , _fatal(false)
{
    memset(_framebuffer, 0, sizeof(_framebuffer));
    memset(_frameBuffer, 0, sizeof(_frameBuffer));

    setTextColor(WHITE);
    setTextSize(1);
    cp437(true);
}

void VFD_GP1211AI::begin(uint32_t preheat_ms)
{
    memset(_framebuffer, 0, sizeof(_framebuffer));

    _platform.init();
    _platform.setBrightness(_brightness);

    /* 先发布一帧合法（空白）数据，避免扫描引擎启动瞬间读到未初始化缓冲 */
    vfd::packFrame(_framebuffer, _invert, _frameBuffer[0]);
    vfd::wirePrepareFrame(_frameBuffer[0], _platform.wireReversesByteBits());
    _platform.publishFrame(_frameBuffer[0]);
    _lastSeq = _platform.frameStartCount();
    _published = true;
    _writeIndex = 1;

    _platform.powerUp(preheat_ms);
    _platform.setBrightness(_brightness);

    _lastScanCount = _platform.scanCount();
    _lastScanMillis = _platform.millis();
    _recoverAttempts = 0;
    _fatal = false;
}

/* ------------------------------------------------------------------ 绘图原语 */

void VFD_GP1211AI::drawPixel(int16_t x, int16_t y, uint16_t color)
{
    /* 用固定常量而不是 _width/_height：本驱动不允许旋转，避免任何越界写 */
    if (x < 0 || x >= vfd::WIDTH || y < 0 || y >= vfd::HEIGHT)
        return;

    uint8_t *p = &_framebuffer[(y >> 3) * vfd::WIDTH + x];
    const uint8_t mask = static_cast<uint8_t>(1u << (y & 7));

    switch (normalizeColor(color)) {
    case INVERSE:
        *p ^= mask;
        break;
    case BLACK:
        *p &= static_cast<uint8_t>(~mask);
        break;
    default: /* WHITE */
        *p |= mask;
        break;
    }
}

void VFD_GP1211AI::drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color)
{
    if (y < 0 || y >= vfd::HEIGHT)
        return;
    if (x < 0) {
        w += x;
        x = 0;
    }
    if ((x + w) > vfd::WIDTH)
        w = static_cast<int16_t>(vfd::WIDTH - x);
    if (w <= 0)
        return;

    uint8_t *p = &_framebuffer[(y >> 3) * vfd::WIDTH + x];
    const uint8_t mask = static_cast<uint8_t>(1u << (y & 7));

    switch (normalizeColor(color)) {
    case INVERSE:
        while (w--)
            *p++ ^= mask;
        break;
    case BLACK: {
        const uint8_t notMask = static_cast<uint8_t>(~mask);
        while (w--)
            *p++ &= notMask;
        break;
    }
    default: /* WHITE */
        while (w--)
            *p++ |= mask;
        break;
    }
}

void VFD_GP1211AI::drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color)
{
    if (x < 0 || x >= vfd::WIDTH)
        return;
    if (y < 0) {
        h += y;
        y = 0;
    }
    if ((y + h) > vfd::HEIGHT)
        h = static_cast<int16_t>(vfd::HEIGHT - y);
    if (h <= 0)
        return;

    uint8_t *p = &_framebuffer[(y >> 3) * vfd::WIDTH + x];
    uint8_t mask = static_cast<uint8_t>(1u << (y & 7));
    const uint16_t c = normalizeColor(color);

    while (h--) {
        switch (c) {
        case INVERSE:
            *p ^= mask;
            break;
        case BLACK:
            *p &= static_cast<uint8_t>(~mask);
            break;
        default: /* WHITE */
            *p |= mask;
            break;
        }
        if (mask == 0x80) { /* 跨到下一页 */
            mask = 0x01;
            p += vfd::WIDTH;
        } else {
            mask = static_cast<uint8_t>(mask << 1);
        }
    }
}

void VFD_GP1211AI::fillScreen(uint16_t color)
{
    switch (normalizeColor(color)) {
    case INVERSE: /* 与 fillRect(..., INVERSE) 的取反语义保持一致 */
        for (int i = 0; i < vfd::FRAMEBUFFER_SIZE; ++i)
            _framebuffer[i] = static_cast<uint8_t>(~_framebuffer[i]);
        break;
    case BLACK:
        memset(_framebuffer, 0x00, sizeof(_framebuffer));
        break;
    default:
        memset(_framebuffer, 0xFF, sizeof(_framebuffer));
        break;
    }
}

void VFD_GP1211AI::blitFramebuffer(const uint8_t *framebuffer)
{
    /* 外部数据源（如 SSD1306 行为模拟）整幅覆盖时的快速通路 */
    if (framebuffer == nullptr)
        return;
    memcpy(_framebuffer, framebuffer, vfd::FRAMEBUFFER_SIZE);
}

void VFD_GP1211AI::setRotation(uint8_t r)
{
    /* 只允许 rotation 0：旋转会改变基类 _width/_height，而帧缓冲永远是
     * 128x64 页式布局，用旋转后的宽度当步长会越界写坏内存（原版缺陷 #6）。 */
    (void)r;
    Adafruit_GFX::setRotation(0);
}

void VFD_GP1211AI::invertDisplay(boolean invert)
{
    /* 下一帧重排时按像素取反（不会破坏"另一组阳极恒为 0"的填充位） */
    _invert = invert;
}

/* -------------------------------------------------------------------- 显示 */

void VFD_GP1211AI::waitForFrameBoundary()
{
    if (!_published)
        return;

    const uint32_t seq = _lastSeq;
    const uint32_t t0 = _platform.millis();

    /* 等待扫描引擎进入新的一帧：一旦它开始读"上一次发布的缓冲"，
     * 另一块缓冲就一定空闲，可以安全地重排进去。 */
    while (_platform.frameStartCount() == seq) {
        if (static_cast<uint32_t>(_platform.millis() - t0) > FRAME_SYNC_TIMEOUT_MS) {
            _frameErrors++; /* 引擎停摆或帧率异常：不再等待，直接发布 */
            break;
        }
        _platform.service();
    }
}

void VFD_GP1211AI::display()
{
    waitForFrameBoundary();

    uint8_t *dst = _frameBuffer[_writeIndex];
    vfd::packFrame(_framebuffer, _invert, dst);
    vfd::wirePrepareFrame(dst, _platform.wireReversesByteBits());

    _lastSeq = _platform.frameStartCount();
    _platform.publishFrame(dst);
    _writeIndex ^= 1u;
}

void VFD_GP1211AI::setBrightness(uint8_t brightness)
{
    _brightness = brightness;
    _platform.setBrightness(brightness);
}

/* ------------------------------------------------------------------ 看护 */

bool VFD_GP1211AI::task()
{
    _platform.service();

    if (_fatal)
        return false;

    const uint32_t scans = _platform.scanCount();
    if (scans != _lastScanCount) {
        _lastScanCount = scans;
        _lastScanMillis = _platform.millis();
        _recoverAttempts = 0;
        return true;
    }

    if (static_cast<uint32_t>(_platform.millis() - _lastScanMillis) < SCAN_STALL_TIMEOUT_MS)
        return true;

    /* 扫描停摆：手册 Note 14 明确"栅极扫描停止可能永久损坏 VFD"，必须尽快恢复 */
    _faults++;
    _lastScanMillis = _platform.millis();

    if (++_recoverAttempts > MAX_RECOVER_ATTEMPTS) {
        _platform.emergencyOff();
        _fatal = true;
        return false;
    }

    _platform.recover();
    return false;
}
