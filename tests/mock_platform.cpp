#include "mock_platform.h"

#include <string.h>

void MockPlatform::init()
{
    _published = nullptr;
    _active = nullptr;
    _frameStarts = 0;
    _scans = 0;
    _scan = 0;
    _tear = false;
    _engineStopped = false;
    _recoverFails = false;
    memset(_latched, 0, sizeof(_latched));
}

void MockPlatform::powerUp(uint32_t preheat_ms)
{
    _powerUps++;
    _hvOn = false;
    _filamentOn = false;
    delayMs(20);
    _filamentOn = true;
    const uint32_t t0 = millis();
    delayMs(preheat_ms);
    _preheatSeenMs = millis() - t0;
    _hvOn = true;
    delayMs(20);
}

void MockPlatform::emergencyOff()
{
    _emergencyOffs++;
    _hvOn = false;
    _litWindowUs = 0;
    _engineStopped = true;
}

void MockPlatform::setBrightness(uint8_t brightness)
{
    _brightness = brightness;
    _litWindowUs = brightness;
}

void MockPlatform::publishFrame(const uint8_t *frame)
{
    _published = frame;
}

void MockPlatform::service()
{
    _serviceCalls++;
    advanceUs(100); /* 模拟主循环里其它工作占用的时间 */
}

void MockPlatform::recover()
{
    _recovers++;
    if (_recoverFails)
        return;
    _engineStopped = false;
    _scan = 0;
    _scanAccUs = 0;
}

void MockPlatform::delayMs(uint32_t ms)
{
    advanceUs(static_cast<uint64_t>(ms) * 1000u);
}

void MockPlatform::advanceUs(uint64_t us)
{
    _nowUs += us;
    if (_engineStopped)
        return;

    _scanAccUs += us;
    while (_scanAccUs >= SCAN_PERIOD_US) {
        _scanAccUs -= SCAN_PERIOD_US;
        stepScan();
    }
}

uint32_t MockPlatform::hashFrame(const uint8_t *frame)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < vfd::FRAME_SIZE; ++i) {
        h ^= frame[i];
        h *= 16777619u;
    }
    return h;
}

void MockPlatform::stepScan()
{
    if (_scan == 0) {
        _active = _published; /* 帧边界：切到最新发布的帧缓冲 */
        _frameStarts++;
        if (_active != nullptr)
            _frameHash = hashFrame(_active);
    }

    if (_active != nullptr) {
        memcpy(_latched + _scan * vfd::SCAN_BYTES, _active + _scan * vfd::SCAN_BYTES,
            vfd::SCAN_BYTES);
        /* 帧末校验：如果这一帧里缓冲被改写过，说明 display() 与扫描引擎对同一块
         * 缓冲并发访问（撕裂），驱动的双缓冲协议就失效了 */
        if (_scan == vfd::SCANS_PER_FRAME - 1 && hashFrame(_active) != _frameHash)
            _tear = true;
    }

    _scan = (_scan + 1) % vfd::SCANS_PER_FRAME;
    _scans++;
}
