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

/* BK 是"高 = 消隐、低 = 点亮"：PWM 输出高的时间 = 消隐窗口 = 扫描周期 − 点亮窗口，
 * 点亮窗口的宽度**只由亮度决定**（vfd_pio_seed_timing.h 的映射，两个引擎共用）。
 * 电平换算的唯一实现在头文件的 rp2040ApplyPwmLevel()（inline：tick 的扫描中断里也要用）。 */

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
    /* 硬件常量集中在 src/vfd_hw_spec.h（唯一来源；第三方工程评审 §4.3） */
    cfg.scan_period_us = static_cast<uint32_t>(SCAN_PERIOD_US); /* 43 × 189 us = 8.127 ms -> 123.0 Hz（手册 >= 120 Hz） */
    cfg.blank_guard_us = static_cast<uint32_t>(BLANK_GUARD_US);
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
    , _preheatMs(0)
    , _powerUpDeadlineUs(0)
    , _powerUpPhase(0)
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

#if VFD_PIN_LED != 255
    /* 板载状态 LED（编译期常量，见 vfd_platform_rp2040.h）：上电先灭，正常运行时由驱动心跳翻转 */
    gpio_init(VFD_PIN_LED);
    gpio_set_dir(VFD_PIN_LED, GPIO_OUT);
    gpio_put(VFD_PIN_LED, 0);
#endif

    /* ---- 2. BK 的 PWM：一个 PWM 周期 = 一个扫描周期，先全消隐 ---- */
    _pwmSlice = static_cast<int>(pwm_gpio_to_slice_num(_cfg.pin_bk));
    _pwmChannel = pwm_gpio_to_channel(_cfg.pin_bk);
    _pwmWrap = _cfg.scan_period_us - 1;
    /* 峰值点亮窗口：两个引擎都取产品档 80%（= 151 µs，亮度映射的另一端；见 vfd_pio_seed_timing.h）。
     * ⚠️ 选项 C 起 `_cfg.blank_guard_us` **两个引擎都不再用**：消隐窗口 = 扫描周期 − 点亮窗口。 */
    _maxLitUs = PIO_LIT_MAX_US;
    if (_maxLitUs == 0)
        _maxLitUs = 1;

    gpio_set_function(_cfg.pin_bk, GPIO_FUNC_PWM);
    pwm_config pc = pwm_get_default_config();
    /* 1 MHz 计数时钟：计数 1 个 = 1 us，与扫描周期的 1 us 基准一致 */
    pwm_config_set_clkdiv(&pc, static_cast<float>(clock_get_hz(clk_sys)) / 1000000.0f);
    pwm_config_set_wrap(&pc, static_cast<uint16_t>(_pwmWrap));
    pwm_init(static_cast<uint>(_pwmSlice), &pc, false);
    rp2040ApplyPwmLevel(_pwmSlice, _pwmChannel, _pwmWrap, 0);
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

    /* ---- 4. 引擎初始化（把消隐电平打到"全消隐"，等 setBrightness 来开窗口）---- */
    engineInit();
    _maxLitUs = PIO_LIT_MAX_US;
    if (_maxLitUs == 0)
        _maxLitUs = 1;
    rp2040ApplyPwmLevel(_pwmSlice, _pwmChannel, _pwmWrap, 0);

    /* ---- 5. 启动扫描 ---- */
    engineStart();
}

/* tick 引擎走 SPI：PL022 只支持 MSB-first，而位映射按 LSB-first 设计
 * ⇒ 必须在打包后逐字节反序（见 vfd_scanpack.h 的说明）。
 * pio 引擎用 out pins（OSR 右移）= LSB-first，位序本来就对；
 * 但它的帧首播种顺序比手册/原驱动"晚一格"，用**扫描相位 −1**在数据侧补偿
 * （固定值，旋钮已在 2026-10-08 清理中移除，见 vfd_scanpack.h §说明）。 */
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

/* 板载状态 LED（见 VFD_PIN_LED；编译期常量，不进 Rp2040Config）。
 * 调用频率：心跳 1 Hz 翻转 + fatal 每次 task() 置位 ⇒ 都是一条 IO 写；
 * 该板没有 GPIO LED（255）时整体编译成空操作。 */
void Rp2040Platform::setStatusLed(bool on)
{
#if VFD_PIN_LED == 255
    (void)on;
#else
    gpio_put(VFD_PIN_LED, on ? 1 : 0);
#endif
}

void Rp2040Platform::setBrightness(uint8_t brightness)
{
    /* 选项 C（2026-10-17，**两个引擎同一套**）：点亮窗口 = **20%–80%**（38–151 µs），
     * 两段线性 + 跳过 4 µs 死区，整数映射、无浮点、无 gamma（映射本体在
     * vfd_pio_seed_timing.h，宿主机测试逐点验证）。
     * 低亮度段走"搬运落在消隐窗口内"的落点，高亮度段（≥ 102 µs）走手册 Figure 5 的落点。 */
    _litWindowUs = pioLitWindowUs(brightness);
    /* 落点必须跟着亮度走：否则切换后的那一帧里"新亮度 + 旧落点"会让搬运跨过 BK 边沿
     * （Note 7②）。两个引擎各自实现 applyBrightnessMode()：
     *   · pio：等状态机停到 wait 上，把搬运闸门在 `wait 0 pin BK` / `nop` 之间打补丁；
     *   · tick：把新亮度挂起，等下一个扫描边界与 PWM 电平**一起**生效（在途搬运不会被打断）。
     * ⚠️ tick 的 PWM 电平不在这里写：中断里的搬运可能正在飞，此刻改电平会让 BK 边沿
     *    落进搬运内部（Note 7②）—— 见 vfd_platform_rp2040_tick.cpp 的 applyBrightnessMode()。 */
#if VFD_SCAN_ENGINE_PIO
    rp2040ApplyPwmLevel(_pwmSlice, _pwmChannel, _pwmWrap, _litWindowUs);
#endif
    applyBrightnessMode();
}

/* 上电时序的固定等待（ms）；预热时长由调用方给定（默认 400，见 DEFAULT_PREHEAT_MS） */
static constexpr uint32_t POWERUP_FL_PULSE_MS = 20;
static constexpr uint32_t POWERUP_HV_SETTLE_MS = 20;

/* 截止时刻比较：对 time_us_32() 的 71 分钟回绕安全 */
static inline bool powerUpDeadlineReached(uint32_t deadlineUs)
{
    return static_cast<int32_t>(time_us_32() - deadlineUs) >= 0;
}

void Rp2040Platform::powerUp(uint32_t preheat_ms)
{
    /* 阻塞版 = 开始 + 轮询到完成，等待序列与旧实现逐项一致（20 + preheat + 20 ms，
     * 只是轮询粒度从整段 sleep 变成 1 ms）。 */
    powerUpBegin(preheat_ms);
    while (!powerUpPoll())
        sleep_ms(1);
}

void Rp2040Platform::powerUpBegin(uint32_t preheat_ms)
{
    /* 手册 Note 3 的上电时序，拆成可被主循环推进的三步：
     *   ① HVEN=0、FLEN 脉冲（灯丝驱动自检）      —— POWERUP_FL_PULSE_MS
     *   ② FLEN=0（灯丝上电）→ 预热                —— preheat_ms
     *   ③ HVEN=1（VDD2 ≈ 45 V 上电）→ 稳定        —— POWERUP_HV_SETTLE_MS
     * 预热期间上层可以继续服务其它硬件（见 Platform::powerUpBegin 的注释）。 */
    _preheatMs = preheat_ms;
    gpio_put(_cfg.pin_hv_en, 0);
    gpio_put(_cfg.pin_fl_en, 1);
    _powerUpDeadlineUs = time_us_32() + POWERUP_FL_PULSE_MS * 1000u;
    _powerUpPhase = 1;
}

bool Rp2040Platform::powerUpPoll()
{
    switch (_powerUpPhase) {
    case 1:
        if (!powerUpDeadlineReached(_powerUpDeadlineUs))
            return false;
        gpio_put(_cfg.pin_fl_en, 0); /* 灯丝上电 */
        _powerUpDeadlineUs = time_us_32() + _preheatMs * 1000u;
        _powerUpPhase = 2;
        return false;
    case 2:
        if (!powerUpDeadlineReached(_powerUpDeadlineUs))
            return false;
        gpio_put(_cfg.pin_hv_en, 1); /* 高压（VDD2 ≈ 45 V）上电 */
        _powerUpDeadlineUs = time_us_32() + POWERUP_HV_SETTLE_MS * 1000u;
        _powerUpPhase = 3;
        return false;
    case 3:
        if (!powerUpDeadlineReached(_powerUpDeadlineUs))
            return false;
        _powerUpPhase = 0; /* 时序完成（0 = 未开始/已完成，幂等） */
        return true;
    default:
        return true;
    }
}

void Rp2040Platform::emergencyOff()
{
    /* fatal 停泊（2026-10-16 加固，理由见 docs/07 本阶段记录）：
     *   · **HVEN = 0**：先关高压 —— 手册 Note 14 的毁屏工况就是"**高压 + 扫描停**"，
     *     停泊必须先断高压（这条口径不变）；
     *   · **FLEN = 1**：再关断 LM9022 灯丝驱动（本轮新增）⇒ 灯丝停止发射并降温、降低功耗；
     *   · BK 固定全消隐（applyPwmLevel(…, 0)，level = wrap+1 ⇒ 恒高、无点亮窗口）；
     *   · 停扫描；LED 由驱动层（VFD_GP1211AI::task()）置常亮。
     * 顺序与手册一致：**先关高压，再停扫描**。 */
    gpio_put(_cfg.pin_hv_en, 0);
    gpio_put(_cfg.pin_fl_en, 1);
    rp2040ApplyPwmLevel(_pwmSlice, _pwmChannel, _pwmWrap, 0);
    _litWindowUs = 0;
    /* "先关高压、再停扫描"之间留 PARK_SETTLE_US 让 VDD2 泄放。
     * ⚠️ 这里**不能用 sleep_ms()**（v1.5.2 健壮性修复，HIL 抓出来的）：sleep 依赖定时器中断
     *    （默认 alarm pool → TIMER_IRQ_3），而本函数要处理的故障之一恰恰可能是"定时器中断失效"；
     *    那样流程会挂在这句 sleep 上，engineStop() / _fatal / LED 常亮都走不到（安全三动作已生效，
     *    但状态与指示不完整）。⇒ 改为**直接读硬件计时器的忙等**，不依赖任何中断：
     *    timer_hw->timerawl 是 RP2040 TIMER 的自由运行微秒计数，与 alarm/中断无关；
     *    32 位差值比较天然处理回绕（PARK_SETTLE_US = 10 ms « 71 min）。 */
    {
        const uint32_t parkT0 = timer_hw->timerawl;
        while (static_cast<uint32_t>(timer_hw->timerawl - parkT0) < PARK_SETTLE_US) {
            tight_loop_contents();
        }
    }
    engineStop();
}

void Rp2040Platform::recover()
{
    engineStop();
    engineInit();
    engineStart();
}

} /* namespace vfd */
