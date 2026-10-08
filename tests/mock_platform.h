/*
 * MockPlatform —— 宿主机上模拟扫描引擎的 vfd::Platform 实现。
 *
 * 语义与 RP2040 实现一致：
 *   - 每个"扫描中断"锁存当前扫描的 48 字节（记录到 latchedFrame()）；
 *   - scan == 0 时切到最新发布的帧缓冲，并让 frameStartCount() 自增；
 *   - 帧内对当前缓冲做哈希校验，检测 display() 是否在引擎读取期间改写了它（撕裂）。
 *
 * 时间模型：虚拟 1 MHz 时基，advanceUs() 每 189 us 触发一次扫描中断；
 * service()/delayMs() 都会推进时间（真实平台上时间自然流逝），
 * 因此驱动里的"等待帧边界""扫描看护"等逻辑都能被真实地跑出来。
 */
#ifndef VFD_TEST_MOCK_PLATFORM_H
#define VFD_TEST_MOCK_PLATFORM_H

#include <stdint.h>

#include "vfd_platform.h"

class MockPlatform : public vfd::Platform {
public:
    static constexpr uint32_t SCAN_PERIOD_US = 189;

    /* ---- vfd::Platform ---- */
    void init() override;
    void powerUp(uint32_t preheat_ms) override;
    void emergencyOff() override;
    void setBrightness(uint8_t brightness) override;
    void publishFrame(const uint8_t *frame) override;
    uint32_t frameStartCount() const override { return _frameStarts; }
    uint32_t scanCount() const override { return _scans; }
    void service() override;
    void recover() override;
    uint32_t millis() const override { return static_cast<uint32_t>(_nowUs / 1000u); }
    void delayMs(uint32_t ms) override;

    /* ---- 测试驱动 ---- */
    void advanceUs(uint64_t us); /* 推进虚拟时间，期间照常产生扫描中断 */
    void stepScan();             /* 跑一次扫描中断（不推进时间） */
    void stepFrame() { advanceUs(static_cast<uint64_t>(SCAN_PERIOD_US) * vfd::SCANS_PER_FRAME); }

    void setEngineStopped(bool stopped) { _engineStopped = stopped; }
    void setRecoverFails(bool fails) { _recoverFails = fails; }

    /* ---- 观测 ---- */
    bool tearDetected() const { return _tear; }
    const uint8_t *latchedFrame() const { return _latched; } /* 43 × 48 字节 */
    uint8_t brightness() const { return _brightness; }
    uint32_t litWindowUs() const { return _litWindowUs; }
    uint32_t powerUpCount() const { return _powerUps; }
    uint32_t emergencyOffCount() const { return _emergencyOffs; }
    uint32_t recoverCount() const { return _recovers; }
    uint32_t serviceCalls() const { return _serviceCalls; }
    bool hvOn() const { return _hvOn; }
    bool filamentOn() const { return _filamentOn; }
    uint32_t preheatSeenMs() const { return _preheatSeenMs; }
    int currentScan() const { return _scan; }

private:
    static uint32_t hashFrame(const uint8_t *frame);

    uint64_t _nowUs = 0;
    uint64_t _scanAccUs = 0;

    const uint8_t *_published = nullptr;
    const uint8_t *_active = nullptr;
    uint32_t _frameStarts = 0;
    uint32_t _scans = 0;
    int _scan = 0;

    uint32_t _frameHash = 0;
    bool _tear = false;
    bool _engineStopped = false;
    bool _recoverFails = false;

    uint8_t _latched[vfd::FRAME_SIZE] = { 0 };

    uint8_t _brightness = 0;
    uint32_t _litWindowUs = 0;
    uint32_t _powerUps = 0;
    uint32_t _emergencyOffs = 0;
    uint32_t _recovers = 0;
    uint32_t _serviceCalls = 0;
    bool _hvOn = false;
    bool _filamentOn = false;
    uint32_t _preheatSeenMs = 0;
};

#endif /* VFD_TEST_MOCK_PLATFORM_H */
