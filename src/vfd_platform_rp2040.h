/*
 * vfd_platform_rp2040.h —— RP2040（Pico SDK）平台实现。
 *
 * 提供两种可选扫描引擎（编译期二选一，见 CMake 选项 VFD_SCAN_ENGINE）：
 *
 * ┌ VFD_SCAN_ENGINE=tick（默认）—— "定时器 + SPI + DMA"
 * │   CLKa/SIa : SPI0（MODE3 + LSB-first，4.500 MHz ≤ 手册 fCLK 上限 5 MHz）
 * │   阳极数据 : DMA 从帧缓冲搬到 SPI TX FIFO（TX DREQ 节流）
 * │   RX FIFO  : 第二个 DMA 通道持续丢弃（PL022 在 RX FIFO 满时不再发起传输）
 * │   LAT/CLKg/SIg : GPIO 软件翻转（在 repeating_timer 中断里，函数放 RAM）
 * │   扫描节拍 : repeating_timer 189 us/扫描 × 43 ≈ 123 Hz
 * │   每帧一次 CPU 中断（5.29 kHz），中断内只做几次寄存器写 + 触发 DMA
 * └
 * ┌ VFD_SCAN_ENGINE=pio —— "PIO 状态机 + DMA"
 * │   PIO 一个状态机同时产生 CLKa / SIa / CLKg / LAT / SIg：
 * │     · 阳极数据由 DMA 连续灌入 TX FIFO（DREQ 节流，PIO 自己数 48 字节/扫描）
 * │     · 扫描周期由 BK(PWM) 的消隐边沿同步 —— 数据传输天然落在消隐窗口内
 * │     · 帧末的栅极链播种(1,1,0,0,0) 由 CPU 在帧完成中断里直接翻引脚（seedGrid()，2026-10-08 `da9b6a1` 起；
 * │       原先在 PIO 程序里（旧版 32 条满额，搬走后程序 25/32）—— 见 docs/08 §7 的 P6）
 * │   CPU 只在一帧结束时进一次中断（123 Hz）：换帧缓冲 + 重启 DMA
 * │   ⇒ 扫描时序不再依赖中断延迟；即使主循环/中断被拖慢，扫描仍按时序进行
 * └
 * 两种引擎的引脚接线完全相同，BK 仍由 PWM 产生消隐/调光。
 *
 * 时序（一个扫描周期 = 189 us，PWM 一个周期也是 189 us）—— **选项 C（2026-10-17 两个引擎统一）**：
 *
 *    0 us        PWM wrap → BK 变高（消隐开始）
 *   +0.2–2 us    边界四步：CLKg↓ → CLKg↑（栅极前进一位）→ LAT↑（锁存）→ LAT↓（释放）
 *   ┌ 低亮度（点亮 ≤ 97 us）：搬运紧跟边界 ⇒ 整段落在**消隐**窗口内（Note 7②：搬运期间 BK 不变）
 *   └ 高亮度（点亮 ≥ 102 us）：等 BK 变低（点亮起点）再触发搬运 ⇒ 整段落在**点亮**窗口内
 *                              （手册 Figure 5 形态；pio 用 wait 指令、tick 用一次性闹钟，都不忙等）
 *   帧末扫描     栅极链播种 1,1,0,0,0（紧跟边界之后、落在消隐窗口内）
 *   189 us      进入下一扫描
 *
 * “点亮窗口”不再由 blank_guard_us 决定，而是**亮度**决定的固定映射（20%–80% = 38–151 us，
 * 两段线性 + 跳过 4 us 死区）——唯一来源是 vfd_pio_seed_timing.h 的 pioLitWindowUs()，
 * 两个引擎共用。`Rp2040Config::blank_guard_us` 自选项 C 起**两个引擎都不再使用**（保留字段仅为
 * 配置/横幅兼容，见 docs/07）；消隐窗口 = 189 − 点亮窗口。落点会不会违反 Note 7②，
 * 由 vfd_pio_seed_timing.h 的 static_assert 在**编译期**保证（tick 的 TICK_* 与 pio 的 PIO_* 各一组）。
 */
#ifndef VFD_PLATFORM_RP2040_H
#define VFD_PLATFORM_RP2040_H

#include "pico/stdlib.h"

#include "hardware/dma.h"
#include "hardware/pwm.h" /* BK 电平换算的两个引擎共用 inline（见 rp2040ApplyPwmLevel）*/

/* ---- 扫描引擎选择：0 = tick(定时器+SPI+DMA)，1 = pio ---- */
#ifndef VFD_SCAN_ENGINE_PIO
#define VFD_SCAN_ENGINE_PIO 0
#endif

#if VFD_SCAN_ENGINE_PIO
#include "hardware/pio.h"
#else
#include "hardware/spi.h"
#endif

#include "vfd_platform.h"
#include "vfd_pio_seed_timing.h"

/* ---- 默认接线（可在 CMake 里用 -DVFD_PIN_xxx=n 覆盖） ---- */
#ifndef VFD_PIN_CLKA /* tick: SPI0 SCK / pio: sideset  | -> CLKa 阳极移位时钟 */
#define VFD_PIN_CLKA 2
#endif
#ifndef VFD_PIN_SIA /* tick: SPI0 TX  / pio: out      | -> SIa  阳极串行数据 */
#define VFD_PIN_SIA 3
#endif
#ifndef VFD_PIN_LAT /* -> LATa/LATg（板上并联） */
#define VFD_PIN_LAT 4
#endif

/* ---- LAT 极性（2026-10-06 依据电路图更正）----------------------------------
 * 手册 Figure 3/5 画的是**面板侧** LAT：**正脉冲有效**（tWL ≥ 300 ns 对应高电平）。
 * 原实现（2026-10-05）假设"MCU 与面板之间有反相级"，因此发"空闲高、低脉冲锁存"。
 * 核对驱动电路后确认**没有反相器**（MCU 直连 LATa/LATg）⇒ MCU 必须发
 * "**空闲低、高脉冲锁存**"（= 1，默认，两个引擎都按此实现）。
 *
 * 定义为 0 可把极性翻回旧假设（A/B 对照用；pio 引擎会在装载程序时自动取反 set 组的 LAT 位），
 * 不需要改 .pio 源码。 */
/* LAT 极性、扫描相位、带内槽序三组"调参旋钮"已于 2026-10-08 清理中移除：
 * 实机定标出的取值已直接写进实现（LAT 空闲低/正脉冲、相位 −1、band=1 + T43 特例见 vfd_scanpack.h）。 */
#ifndef VFD_PIN_CLKG /* -> CLKg 栅极移位时钟 */
#define VFD_PIN_CLKG 5
#endif
#ifndef VFD_PIN_SIG /* -> SIg 栅极串行数据 */
#define VFD_PIN_SIG 6
#endif
#ifndef VFD_PIN_BK /* -> BKa/BKg 消隐/调光（PWM；PIO 引擎会把它当输入读） */
#define VFD_PIN_BK 7
#endif
#ifndef VFD_PIN_HV_EN /* -> HVEN 升压使能（VDD2 = 45 V） */
#define VFD_PIN_HV_EN 8
#endif
#ifndef VFD_PIN_FL_EN /* -> FLEN 灯丝驱动使能（LM9022 ST） */
#define VFD_PIN_FL_EN 9
#endif
#ifndef VFD_PIN_TEST_MODE /* 测试模式选择：上电时该脚为低 → 渲染内置测试图像 */
#define VFD_PIN_TEST_MODE 16
#endif

/* 板载状态 LED（2026-10-16 需求）：正常工作时闪烁（心跳）、fatal 停泊后常亮。
 * 默认取 SDK 的板卡宏（Pico / Pico 2 = GP25）；**没有该宏的板子**（如 Pico W：LED 挂在 CYW43 上）
 * 自动退化为"不驱动"。可用 -DVFD_PIN_LED=n 覆盖，或用 255 显式关闭。 */
#ifndef VFD_PIN_LED
#if defined(PICO_DEFAULT_LED_PIN)
#define VFD_PIN_LED PICO_DEFAULT_LED_PIN
#else
#define VFD_PIN_LED 255 /* 该板没有 GPIO 板载 LED ⇒ setStatusLed() 空操作 */
#endif
#endif

/* LED 脚不得与已用引脚冲突（这类冲突原本只能上机才发现）。
 * 注：SSD1306 从机的 VFD_EMU_PIN_*（11–15）在另一个头文件里，不在此断言范围内。 */
static_assert(VFD_PIN_LED == 255
        || (VFD_PIN_LED != VFD_PIN_CLKA && VFD_PIN_LED != VFD_PIN_SIA && VFD_PIN_LED != VFD_PIN_LAT
            && VFD_PIN_LED != VFD_PIN_CLKG && VFD_PIN_LED != VFD_PIN_SIG && VFD_PIN_LED != VFD_PIN_BK
            && VFD_PIN_LED != VFD_PIN_HV_EN && VFD_PIN_LED != VFD_PIN_FL_EN
            && VFD_PIN_LED != VFD_PIN_TEST_MODE),
    "VFD_PIN_LED 与扫描/控制引脚冲突：换一个脚号，或设 255 关闭 LED 指示");

/* ---- tick 引擎资源 ---- */
#ifndef VFD_DMA_TX_CH
#define VFD_DMA_TX_CH 0
#endif
#ifndef VFD_DMA_RX_CH
#define VFD_DMA_RX_CH 1
#endif
/* RX 垃圾桶 DMA 的初始计数（也是每次补满的值）。2^32-1 个字节在 48 B/扫描 × 5291 次/s
 * 下约 4.7 h 跑完；service() 会在剩余不足一半时**提前补满**，所以正常永不跑完。
 * 验收/排故可把它调小（例如 -DVFD_TICK_RX_DMA_COUNT=200000 ⇒ 约 0.8 s 耗尽）
 * 来复现"计数耗尽"这条路径，见 docs/13。 */
#ifndef VFD_TICK_RX_DMA_COUNT
#define VFD_TICK_RX_DMA_COUNT 0xFFFFFFFFu
#endif
/* 1 = service() 提前补计数（默认，修复后的行为）；
 * 0 = 不补计数（只保留 CPU 兜底排空）——**仅供实机 A/B 对照实验**，别用于发布。 */
#ifndef VFD_TICK_RX_TOPUP
#define VFD_TICK_RX_TOPUP 1
#endif
/* 1 = service() 里那条"保险性清理"（CPU 轮询读走 SPI RX FIFO 残留）保留（默认）；
 * 0 = 关掉它。与 VFD_TICK_RX_TOPUP=0 合用 = **两条 RX 排空路径都关**，用来判定仓库早期
 * "陷阱 4：PL022 在 RX FIFO 满时不再发起传输"到底是事实还是传说（见 docs/13 §4.4）。
 * ⚠️ 仅供实验：真停摆时阳极数据会停（栅极扫描仍在跑，不违反 Note 14）。 */
#ifndef VFD_TICK_SERVICE_DRAIN
#define VFD_TICK_SERVICE_DRAIN 1
#endif

/* ---- pio 引擎资源 ---- */
#ifndef VFD_PIO_INST /* 0 = pio0, 1 = pio1 */
#define VFD_PIO_INST 0
#endif
#ifndef VFD_PIO_SM /* 0..3 */
#define VFD_PIO_SM 0
#endif
#ifndef VFD_DMA_CH /* 整帧数据搬运通道 */
#define VFD_DMA_CH 0
#endif
#ifndef VFD_PIO_CLK_HZ /* CLKa 频率（手册 fCLK <= 5 MHz） */
#define VFD_PIO_CLK_HZ 4500000
#endif

namespace vfd {

struct Rp2040Config {
    /* 公共引脚 */
    uint8_t pin_clka;
    uint8_t pin_sia;
    uint8_t pin_lat;
    uint8_t pin_clkg;
    uint8_t pin_sig;
    uint8_t pin_bk;
    uint8_t pin_hv_en;
    uint8_t pin_fl_en;

#if VFD_SCAN_ENGINE_PIO
    PIO pio;
    uint8_t sm;
    int8_t dma_ch;       /* PIO TX FIFO 的数据搬运通道 */
    uint32_t pio_clk_hz; /* CLKa 频率（SM 时钟 = 2 × 它） */
#else
    spi_inst_t *spi;
    int8_t dma_tx;
    int8_t dma_rx;
    uint32_t spi_hz; /* 阳极移位时钟，必须 <= 5 MHz */
#endif

    uint32_t scan_period_us; /* 每个时序的周期；189 us -> 123.1 Hz 帧率 */
    uint32_t blank_guard_us; /* 数据传输期间强制消隐的时间 */
    uint32_t preheat_ms;     /* 灯丝预热时间 */
};

/* ⚠️ 状态 LED 的脚号**不进 Rp2040Config**：它是编译期常量（VFD_PIN_LED，默认取板卡宏）。
 *    塞进结构体会让整个 Rp2040Platform 对象布局后移，引擎/打包这些"不该动"的函数
 *    指令编码也跟着变 ⇒ 就没法用"扫描关键函数逐指令相同"来证明改动没碰时序（S1，见 docs/07）。 */

Rp2040Config defaultRp2040Config();

/* 每个扫描周期内引擎需要完成的工作量、以及 PIO 播种请求的时序窗口，
 * 全部集中在 vfd_pio_seed_timing.h（无平台依赖，宿主机时序测试也会用它）。
 * 数值必须与 src/vfd_scan.pio 的结构一致（改 .pio 时同步改那里）。 */

/* 内部工具：微秒级忙等（只读 1 MHz 计时器，不访问 Flash，可在中断里使用） */
void rp2040ShortDelayUs(uint32_t us);

/* BK 通道电平换算（**两个引擎唯一来源**）：PWM 输出高 = 消隐、低 = 点亮 ⇒
 * 比较值 = wrap + 1 − 点亮窗口。定义成 inline 是因为 tick 的扫描中断里要改亮度
 * （挂起的亮度变更在扫描边界生效，避免"新亮度 + 在途搬运"跨过 BK 边沿 ⇒ Note 7②），
 * 而中断路径必须整体编译进 .time_critical（不能有一次 Flash 取指）。 */
static inline void rp2040ApplyPwmLevel(int slice, uint channel, uint32_t wrap, uint32_t litUs)
{
    pwm_set_chan_level(static_cast<uint>(slice), channel, static_cast<uint16_t>(wrap + 1u - litUs));
}

/* 上电采样"测试模式"引脚（VFD_PIN_TEST_MODE）：内部上拉，读到**低电平**表示进入测试模式。
 * 默认（高/悬空）不渲染任何内置测试图像，设备只作为 SSD1306 从机等待主机送画面。 */
bool rp2040TestModeSelected(uint8_t pin = VFD_PIN_TEST_MODE);

class Rp2040Platform : public Platform {
public:
    explicit Rp2040Platform(const Rp2040Config &cfg = defaultRp2040Config());
    ~Rp2040Platform() override;

    void init() override;
    void powerUp(uint32_t preheat_ms) override;
    void powerUpBegin(uint32_t preheat_ms) override;
    bool powerUpPoll() override;
    void emergencyOff() override;
    /* 板载状态 LED（见 VFD_PIN_LED）：驱动层只在心跳翻转与 fatal 停泊时调用 */
    void setStatusLed(bool on) override;
    void setBrightness(uint8_t brightness) override;
    void publishFrame(const uint8_t *frame) override;
    bool wireReversesByteBits() const override;
    uint32_t frameStartCount() const override { return _frameStarts; }
    uint32_t scanCount() const override { return _scans; }
    void service() override;
    void recover() override;
    uint32_t millis() const override { return to_ms_since_boot(get_absolute_time()); }
    void delayMs(uint32_t ms) override { sleep_ms(ms); }

    /* ---- 诊断 ---- */
    const char *engineName() const; /* 当前扫描引擎名 */
    uint32_t clockHz() const;       /* 阳极移位时钟实际频率 */
    uint32_t litWindowUs() const { return _litWindowUs; }
    uint32_t litWindowMaxUs() const { return _maxLitUs; }
    uint32_t guardMarginUs() const;  /* 消隐保护相对工作量的余量（us） */
    bool engineStalled() const;      /* 引擎当前是否卡住 */
    bool engineError() const { return _engineError; }
    uint32_t dmaBusyErrors() const { return _dmaBusyErrors; }
#if !VFD_SCAN_ENGINE_PIO
    /* RX 垃圾桶 DMA 的重新武装次数（tick 专用）。正常约每 2.35 h +1；
     * 用 VFD_TICK_RX_DMA_COUNT 调小做加速验收时会看到它持续增长。 */
    uint32_t rxRearmCount() const { return _rxRearms; }
#endif
    /* 扫描中断累计忙时（µs，单调递增；主循环按秒取差分算占用率）。仅 VFD_DEBUG_DIAG 下更新。 */
    uint32_t irqBusyUs() const { return _irqUs; }

private:
    /* ---- 引擎钩子：由 vfd_platform_rp2040_{tick,pio}.cpp 实现 ---- */
    void engineInit();   /* 引脚/外设/DMA 初始化（可重复调用，用于 recover） */
    void engineStart();  /* 启动扫描 */
    void engineStop();   /* 停止扫描（不改 BK/HV 状态） */
    void engineDeinit(); /* 释放引擎资源（析构用） */
    /* 搬运落点切换（选项 C，**两个引擎各自实现**，落点判据同源）：
     *   · pio：把 .pio 里那条搬运闸门在 `wait 0 pin BK`（高亮度：搬运落在点亮窗口内）与
     *     `nop`（低亮度：搬运落在消隐窗口内）之间切换 —— applyXferGate()；
     *   · tick：把"本扫描的落点模式"挂起，等下一个扫描边界再与 PWM 一起生效
     *     （低亮度 = 紧跟边界触发 DMA；高亮度 = 一次性闹钟等 BK 变低再触发）。
     * 亮度 → 点亮窗口的映射与落点边界都在 vfd_pio_seed_timing.h（唯一来源）。 */
    void applyXferGate();
    void applyBrightnessMode();

    Rp2040Config _cfg;

    /* BK（消隐/调光）PWM */
    int _pwmSlice;
    uint _pwmChannel;
    uint32_t _pwmWrap;
    uint32_t _maxLitUs;
    uint32_t _litWindowUs;

    /* 帧缓冲发布 / 心跳 */
    volatile const uint8_t *_published;
    volatile const uint8_t *_active;
    /* 发布帧的**平台侧稳定副本**（双缓冲）：
     * publishFrame() 把上层缓冲拷进来再把 _published 指过来，DMA/引擎只读这块副本，
     * 上层就能自由复用它的两块 ping-pong 缓冲 —— 否则"引擎正在读、上层同时在写"
     * 会让帧首/帧尾那几列的内容抖动（实机：最后一个 grid 内容抖动）。 */
    uint8_t _frameCopy[2][vfd::FRAME_SIZE];
    volatile uint8_t _copyLatest;
    volatile uint32_t _frameStarts;
    volatile uint32_t _scans;
    volatile uint32_t _irqUs; /* 扫描中断累计忙时（µs） */
    volatile uint32_t _dmaBusyErrors;
    volatile int _scan;

    bool _engineError;

#if VFD_SCAN_ENGINE_PIO
    int _pioOffset;
    bool _dmaIrqReady;
    volatile uint32_t _lastFrameIrqUs = 0; /* 上次帧完成中断时刻（µs）：每帧只播一次防护 */
    dma_channel_config _dmaConfig;
    uint32_t _pioClkHz;    /* 实际 CLKa 频率 */
    uint32_t _guardMarginUs;

    /* 搬运闸门补丁状态（选项 C）：闸门下标在 engineInit 里按编码定位（.pio 结构变了也不会打错地方）*/
    int _gateOffset = -1;         /* 程序里第 2 条 `wait 0 pin BK` 的下标；<0 = 未定位 */
    uint16_t _gateWaitWord = 0;   /* 高亮度形态：等点亮起点再搬 */
    uint16_t _gateNopWord = 0;    /* 低亮度形态：紧跟边界四步就搬 */
    bool _gateLowApplied = false; /* 指令存储里当前是不是低亮度形态 */

    uint32_t _blankFrame[vfd::FRAME_SIZE / 4]; /* _published 为空时的占位帧（48 字节/扫描 布局） */
    static Rp2040Platform *_instance;          /* 中断里需要实例指针 */
    static void dmaIrqThunk();

    void onFrameDmaDone();
    void seedGrid();
    void patchProgramPins(uint16_t *words, uint length);
#else
    repeating_timer_t _timer;
    bool _timerRunning;
    uint32_t _spiBaudrate;
    uint32_t _rxSink; /* RX DMA 的垃圾桶（不递增写入） */
    volatile uint32_t _rxRearms = 0; /* RX 垃圾桶 DMA 补计数次数（诊断；见 docs/13） */
    uint32_t _guardMarginUs;
    static bool scanTimerThunk(repeating_timer_t *t);
    bool scanTick();
    void configureDma();
    void startScanTimer();
    void stopScanTimer();

    /* ---- 选项 C：搬运落点两选一（与 pio 同一形态，见 vfd_pio_seed_timing.h）---- */
    volatile uint32_t _appliedLitUs = 0;    /* 引擎**正在用**的点亮窗口（PWM 与落点都以它为准）*/
    volatile uint32_t _pendingLitUs = 0;    /* setBrightness 挂起的新点亮窗口（下一个扫描边界生效）*/
    volatile bool _pendingApply = false;    /* 有挂起的亮度变更；见 applyBrightnessMode() */
    volatile const uint8_t *_pendingSrc = nullptr; /* 高亮度落点：等一下要触发的那一扫描数据 */
    volatile bool _alarmDoesXfer = false;   /* 闹钟到点要不要触发搬运（= 高亮度落点）*/
    volatile bool _seedPending = false;     /* 闹钟到点要不要播种（= 帧末扫描）*/
    int32_t _trigAlarm = -1;                /* "点亮起点"一次性闹钟 id（<0 = 无）*/
    static int64_t triggerThunk(alarm_id_t id, void *user_data);
    void seedGrid();          /* 栅极链播种（帧末扫描边界之后；engineStart 先播一次）*/
    void triggerScanTransfer(); /* 触发本扫描的 48 字节搬运（低亮度=中断里直接 / 高亮度=闹钟）*/
#endif

    /* ---- 非阻塞上电时序的状态（powerUpBegin/powerUpPoll；0 = 未开始/已完成）---- */
    uint32_t _preheatMs;          /* 本次预热的毫秒数 */
    uint32_t _powerUpDeadlineUs;  /* 当前阶段的截止时刻（time_us_32 基准，回绕安全） */
    uint8_t _powerUpPhase;
};

} /* namespace vfd */

#endif /* VFD_PLATFORM_RP2040_H */
