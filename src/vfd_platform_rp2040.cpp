/*
 * vfd_platform_rp2040.cpp —— 两种扫描引擎共用的部分：
 *   引脚/上电时序/亮度 PWM/帧发布/看护/恢复 + 公共诊断。
 * 引擎相关的实现分别在：
 *   vfd_platform_rp2040_tick.cpp  （VFD_SCAN_ENGINE=tick，默认）
 *   vfd_platform_rp2040_pio.cpp   （VFD_SCAN_ENGINE=pio）
 */
#include "vfd_platform_rp2040.h"

#include <string.h>

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/timer.h"
#include "pico/time.h"

namespace vfd {

/* BK 是"高 = 消隐、低 = 点亮"：PWM 输出高的时间 = blank_guard_us，
 * 点亮窗口 = 周期剩余时间，宽度由亮度决定。 */
static inline void applyPwmLevel(int slice, uint channel, uint32_t wrap, uint32_t litUs)
{
    pwm_set_chan_level(static_cast<uint>(slice), channel,
        static_cast<uint16_t>(wrap + 1 - litUs));
}

Rp2040Config defaultRp2040Config()
{
    Rp2040Config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.pin_clka = VFD_PIN_CLKA;
    cfg.pin_sia = VFD_PIN_SIA;
    cfg.pin_lat = VFD_PIN_LAT;
    cfg.pin_clkg = VFD_PIN_CLKG;
    cfg.pin_sig = VFD_PIN_SIG;
    cfg.pin_bk = VFD_PIN_BK;
    cfg.pin_hv_en = VFD_PIN_HV_EN;
    cfg.pin_fl_en = VFD_PIN_FL_EN;
    cfg.scan_period_us = 189; /* 43 × 189 us = 8.12 ms -> 123.1 Hz（手册要求 >= 120 Hz） */
    cfg.blank_guard_us = 100;
    cfg.preheat_ms = DEFAULT_PREHEAT_MS;

#if VFD_SCAN_ENGINE_PIO
    cfg.pio = VFD_PIO_INST ? pio1 : pio0;
    cfg.sm = VFD_PIO_SM;
    cfg.dma_ch = VFD_DMA_CH;
    cfg.pio_clk_hz = VFD_PIO_CLK_HZ;
#else
    cfg.spi = spi0;
    cfg.dma_tx = VFD_DMA_TX_CH;
    cfg.dma_rx = VFD_DMA_RX_CH;
    cfg.spi_hz = 4500000; /* 手册要求 fCLK <= 5 MHz，4.5 MHz 留有裕量 */
#endif
    return cfg;
}

/* 微秒级忙等：只读 1 MHz 硬件计时器，不访问 Flash/XIP（中断里也可以安全使用） */
void __not_in_flash_func(rp2040ShortDelayUs)(uint32_t us)
{
    const uint32_t start = timer_hw->timerawl;
    while (static_cast<uint32_t>(timer_hw->timerawl - start) < us)
        tight_loop_contents();
}

/* 上电采样测试模式脚：内部上拉，低电平 = 进入测试模式（渲染内置测试图像）。
 * 只在启动时判断一次，避免运行中抖动引起模式切换。 */
bool rp2040TestModeSelected(uint8_t pin)
{
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_set_input_enabled(pin, true);
    gpio_pull_up(pin);
    sleep_ms(2); /* 等上拉把电平拉稳 */
    return gpio_get(pin) == 0;
}

/* ------------------------------------------------------------------ 生命周期 */

Rp2040Platform::Rp2040Platform(const Rp2040Config &cfg)
    : _cfg(cfg)
    , _pwmSlice(0)
    , _pwmChannel(0)
    , _pwmWrap(0)
    , _maxLitUs(0)
    , _litWindowUs(0)
    /* 初始化列表顺序必须与头文件里的成员声明顺序一致（否则 -Wreorder 警告）：
     * _published → _active → _frameCopy → _copyLatest → … */
    , _published(nullptr)
    , _active(nullptr)
    , _copyLatest(0)
    , _frameStarts(0)
    , _scans(0)
    , _irqUs(0)
    , _dmaBusyErrors(0)
    , _scan(0)
    , _engineError(false)
#if VFD_SCAN_ENGINE_PIO
    , _pioOffset(-1)
    , _dmaIrqReady(false)
    , _dmaConfig(dma_channel_get_default_config(static_cast<uint>(cfg.dma_ch < 0 ? 0 : cfg.dma_ch)))
    , _pioClkHz(0)
    , _guardMarginUs(0)
#else
    , _timerRunning(false)
    , _spiBaudrate(0)
    , _rxSink(0)
    , _guardMarginUs(0)
#endif
{
#if VFD_SCAN_ENGINE_PIO
    memset(_blankFrame, 0, sizeof(_blankFrame));
#endif
}

Rp2040Platform::~Rp2040Platform()
{
    engineStop();
    engineDeinit();
    pwm_set_enabled(static_cast<uint>(_pwmSlice), false);
}

void Rp2040Platform::init()
{
    /* ---- 1. 高压/灯丝控制脚：先都关掉 ---- */
    const uint8_t ctrlPins[] = { _cfg.pin_hv_en, _cfg.pin_fl_en };
    for (uint8_t pin : ctrlPins) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_OUT);
    }
    gpio_put(_cfg.pin_hv_en, 0); /* HVEN 低 = 关高压 */
    gpio_put(_cfg.pin_fl_en, 1); /* LM9022 ST 高 = 灯丝驱动关断 */

    /* ---- 2. BK 的 PWM：一个 PWM 周期 = 一个扫描周期，先全消隐 ---- */
    _pwmSlice = static_cast<int>(pwm_gpio_to_slice_num(_cfg.pin_bk));
    _pwmChannel = pwm_gpio_to_channel(_cfg.pin_bk);
    _pwmWrap = _cfg.scan_period_us - 1;
    _maxLitUs = _cfg.scan_period_us - _cfg.blank_guard_us;
    if (_maxLitUs == 0)
        _maxLitUs = 1;

    gpio_set_function(_cfg.pin_bk, GPIO_FUNC_PWM);
    pwm_config pc = pwm_get_default_config();
    /* 1 MHz 计数时钟：计数 1 个 = 1 us，与扫描周期的 1 us 基准一致 */
    pwm_config_set_clkdiv(&pc, static_cast<float>(clock_get_hz(clk_sys)) / 1000000.0f);
    pwm_config_set_wrap(&pc, static_cast<uint16_t>(_pwmWrap));
    pwm_init(static_cast<uint>(_pwmSlice), &pc, false);
    applyPwmLevel(_pwmSlice, _pwmChannel, _pwmWrap, 0);
    pwm_set_enabled(static_cast<uint>(_pwmSlice), true);

    /* ---- 3. 状态复位 ---- */
    _published = nullptr;
    _active = nullptr;
    _frameStarts = 0;
    _scans = 0;
    _irqUs = 0;
    _scan = 0;
    _dmaBusyErrors = 0;
    _engineError = false;

    /* ---- 4. 引擎（可能抬高 blank_guard_us，因此再刷新一次消隐电平）---- */
    engineInit();
    _maxLitUs = _cfg.scan_period_us - _cfg.blank_guard_us;
    if (_maxLitUs == 0)
        _maxLitUs = 1;
    applyPwmLevel(_pwmSlice, _pwmChannel, _pwmWrap, 0);

    /* ---- 5. 启动扫描 ---- */
    engineStart();
}

/* tick 引擎走 SPI：PL022 只支持 MSB-first，而位映射按 LSB-first 设计
 * ⇒ 必须在打包后逐字节反序（见 vfd_scanpack.h 的说明）。
 * pio 引擎用 out pins（OSR 右移）= LSB-first，位序本来就对；
 * 但它的帧首播种顺序比手册/原驱动"晚一格"，用扫描相位 +1 在数据侧补偿。 */
bool Rp2040Platform::wireReversesByteBits() const
{
#if VFD_SCAN_ENGINE_PIO
    /* PIO 用 `out pins,1`（OSR 右移）= LSB-first，总线位序本来就对 ⇒ 不需要反序。
     * ⚠️ 这里必须是按引擎分支的！曾经在 tick 分支上把整段 if/else 换成无条件 return true，
     *    导致 PIO 版也被逐字节反序 ⇒ 画面"一团乱麻"（元素位置仍可辨认，因为栅极/扫描不受影响）。 */
    return false;
#else
    /* tick 走 SPI/PL022，而 PL022 只支持 MSB-first（Pico SDK: order Must be SPI_MSB_FIRST）
     * ⇒ 打包后逐字节反序，使总线上的位序等价于 LSB-first。抓包反解（match_tick.js）证实：
     *   不开反序时与金标准(pio)位流差 ~18%，开了之后收敛。 */
    return true;
#endif
}


void Rp2040Platform::publishFrame(const uint8_t *frame)
{
    if (frame == nullptr) {
        /* 允许"停止发布"：退回全黑帧（PIO 引擎）或空指针（tick 引擎） */
        _published = nullptr;
        return;
    }

    /* 拷进平台自己的双缓冲副本，再把 _published 指过去：
     * 上层（app）的两块 ping-pong 缓冲随后可以随便重写，不会再和引擎抢同一块内存。
     * 实机症状"最后一个 grid 内容抖动"就是这种竞争（DMA 正读到帧首/帧尾那几列时，
     * 上层开始重排下一帧）。拷贝 2064 B @123 Hz ≈ 254 KB/s，代价可忽略。 */
    const uint8_t next = static_cast<uint8_t>(_copyLatest ^ 1u);
    memcpy(_frameCopy[next], frame, vfd::FRAME_SIZE);
    _copyLatest = next;
    _published = _frameCopy[next];
}

void Rp2040Platform::setBrightness(uint8_t brightness)
{
    _litWindowUs = (brightness == 0)
        ? 0
        : 1u + (static_cast<uint32_t>(brightness) * (_maxLitUs - 1)) / 255u;
#if VFD_SCAN_ENGINE_PIO
    /* PIO 引擎靠 BK 的上升/下降沿来对齐扫描周期，因此即使亮度为 0
     * 也保留 1 us 的点亮窗口（占空 0.5%，视觉上等同全黑）。 */
    if (_litWindowUs == 0)
        _litWindowUs = 1;
#endif
    applyPwmLevel(_pwmSlice, _pwmChannel, _pwmWrap, _litWindowUs);
}

void Rp2040Platform::powerUp(uint32_t preheat_ms)
{
    gpio_put(_cfg.pin_hv_en, 0);
    gpio_put(_cfg.pin_fl_en, 1);
    sleep_ms(20);

    gpio_put(_cfg.pin_fl_en, 0); /* 灯丝上电 */
    sleep_ms(preheat_ms);        /* 预热（默认 400 ms） */

    gpio_put(_cfg.pin_hv_en, 1); /* 高压（VDD2 ≈ 45 V）上电 */
    sleep_ms(20);
}

void Rp2040Platform::emergencyOff()
{
    /* 手册 Note 14：栅极扫描停止可能永久损坏屏。
     * 所以先关高压，再停扫描；同时把 BK 固定为全消隐。 */
    gpio_put(_cfg.pin_hv_en, 0);
    applyPwmLevel(_pwmSlice, _pwmChannel, _pwmWrap, 0);
    _litWindowUs = 0;
    sleep_ms(10);
    engineStop();
}

void Rp2040Platform::recover()
{
    engineStop();
    engineInit();
    engineStart();
}

} /* namespace vfd */
