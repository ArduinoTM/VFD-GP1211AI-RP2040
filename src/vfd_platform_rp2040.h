/*
 * vfd_platform_rp2040.h —— RP2040（Pico SDK）平台实现。
 *
 * 提供两种可选扫描引擎（编译期二选一，见 CMake 选项 VFD_SCAN_ENGINE）：
 *
 * ┌ VFD_SCAN_ENGINE=tick（默认）—— "定时器 + SPI + DMA"
 * │   CLKa/SIa : SPI0（MODE3 + LSB-first，4.46 MHz ≤ 手册 fCLK 上限 5 MHz）
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
 * │     · 帧末的栅极链播种(1,1,0,0,0) 也由 PIO 完成
 * │   CPU 只在一帧结束时进一次中断（123 Hz）：换帧缓冲 + 重启 DMA
 * │   ⇒ 扫描时序不再依赖中断延迟；即使主循环/中断被拖慢，扫描仍按时序进行
 * └
 * 两种引擎的引脚接线完全相同，BK 仍由 PWM 产生消隐/调光。
 *
 * 时序（一个扫描周期 = 189 us，PWM 一个周期也是 189 us）：
 *
 *   0 us        PWM wrap → BK 变高（消隐开始）
 *               PIO/中断 在这里开始把 48 字节送入阳极移位链
 *   ~87/92 us   数据移完 → CLKg 前进一位（选通下两条栅极）+ LAT 锁存
 *               （全程 BK 恒定消隐：满足手册 Note 7②③④，桁间消隐 ≫ Note 16 的 5 us）
 *   100 us      BK 取消消隐 → 本次扫描的 3 列 × 64 行点亮
 *   189 us      进入下一扫描
 *
 * “点亮窗口”= 189 - blank_guard_us（默认 89 us），亮度 = 窗口宽度 / 189，最大约 47%。
 * 若要更亮可以调小 blank_guard_us，但必须大于"数据搬运 + 锁存（+ 帧末播种）"的时间：
 * 引擎会在 init() 里自检，必要时自动抬高 guard 并通过 guardMarginUs() 暴露余量。
 */
#ifndef VFD_PLATFORM_RP2040_H
#define VFD_PLATFORM_RP2040_H

#include "pico/stdlib.h"

#include "hardware/dma.h"

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

/* ---- tick 引擎资源 ---- */
#ifndef VFD_DMA_TX_CH
#define VFD_DMA_TX_CH 0
#endif
#ifndef VFD_DMA_RX_CH
#define VFD_DMA_RX_CH 1
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

Rp2040Config defaultRp2040Config();

/* 每个扫描周期内引擎需要完成的工作量、以及 PIO 播种请求的时序窗口，
 * 全部集中在 vfd_pio_seed_timing.h（无平台依赖，宿主机时序测试也会用它）。
 * 数值必须与 src/vfd_scan.pio 的结构一致（改 .pio 时同步改那里）。 */

/* 内部工具：微秒级忙等（只读 1 MHz 计时器，不访问 Flash，可在中断里使用） */
void rp2040ShortDelayUs(uint32_t us);

/* 上电采样"测试模式"引脚（VFD_PIN_TEST_MODE）：内部上拉，读到**低电平**表示进入测试模式。
 * 默认（高/悬空）不渲染任何内置测试图像，设备只作为 SSD1306 从机等待主机送画面。 */
bool rp2040TestModeSelected(uint8_t pin = VFD_PIN_TEST_MODE);

class Rp2040Platform : public Platform {
public:
    explicit Rp2040Platform(const Rp2040Config &cfg = defaultRp2040Config());
    ~Rp2040Platform() override;

    void init() override;
    void powerUp(uint32_t preheat_ms) override;
    void emergencyOff() override;
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
    /* 扫描中断累计忙时（µs，单调递增；主循环按秒取差分算占用率）。仅 VFD_DEBUG_DIAG 下更新。 */
    uint32_t irqBusyUs() const { return _irqUs; }

private:
    /* ---- 引擎钩子：由 vfd_platform_rp2040_{tick,pio}.cpp 实现 ---- */
    void engineInit();   /* 引脚/外设/DMA 初始化（可重复调用，用于 recover） */
    void engineStart();  /* 启动扫描 */
    void engineStop();   /* 停止扫描（不改 BK/HV 状态） */
    void engineDeinit(); /* 释放引擎资源（析构用） */

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
    dma_channel_config _dmaConfig;
    uint32_t _pioClkHz;    /* 实际 CLKa 频率 */
    uint32_t _guardMarginUs;
    alarm_id_t _seedRaiseAlarm;      /* 帧首扫描内拉高请求脚 */
    alarm_id_t _seedLowerAlarm;      /* 稍后撤掉请求脚 */
    uint32_t _blankFrame[vfd::FRAME_SIZE / 4]; /* _published 为空时的占位帧（48 字节/扫描 布局） */
    static Rp2040Platform *_instance;          /* 中断里需要实例指针 */
    static void dmaIrqThunk();
    static int64_t seedRaiseThunk(alarm_id_t id, void *user_data);
    static int64_t seedLowerThunk(alarm_id_t id, void *user_data);
    void cancelSeedRequest();
    void onFrameDmaDone();
    void seedGrid();
    void patchProgramPins(uint16_t *words, uint length);
#else
    repeating_timer_t _timer;
    bool _timerRunning;
    uint32_t _spiBaudrate;
    uint32_t _rxSink; /* RX DMA 的垃圾桶（不递增写入） */
    uint32_t _guardMarginUs;
    static bool scanTimerThunk(repeating_timer_t *t);
    bool scanTick();
    void configureDma();
    void startScanTimer();
    void stopScanTimer();
#endif
};

} /* namespace vfd */

#endif /* VFD_PLATFORM_RP2040_H */
