/*
 * vfd_platform_rp2040_tick.cpp —— 默认扫描引擎："定时器 + SPI + DMA"。
 *
 * 每个扫描周期（189 µs）由 repeating_timer 中断驱动：
 *   ① BK 消隐（PWM 计数器归零，锁定点亮窗口相位）
 *   ② CLKg 前进一位 + LAT 锁存（此时 BK 为消隐）
 *   ③ 帧边界：切换帧缓冲、把 1,1,0,0,0 播种进栅极链
 *   ④ 启动 DMA：48 字节 帧缓冲 -> SPI0 TX FIFO（TX DREQ 节流），CPU 不搬数据
 * 阳极数据在 SPI 移位期间（约 86 µs）完成，全部落在消隐窗口内。
 */
#include "vfd_platform_rp2040.h"

#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/spi.h"
#include "pico/time.h"

namespace vfd {

/* 手工 strobe：拉低 → 保持 → 拉高 → **再保持**（空闲高）。
 *
 * 手册 Figure 3（MN12864K 第 3 页）要求：CLK 高电平宽度 tWCLK ≥ 80 ns、
 * LAT 脉冲宽度 tWL ≥ 300 ns、数据建立/保持 tDS/tDH ≥ 40/30 ns。
 * 栅极驱动是同一族 48 bit 移位寄存器，其时钟同样需要足够的高电平宽度。
 *
 * ⚠️ 2026-10-05 逻辑分析仪（50 MHz）实测发现：原实现只保证了"低电平 1 µs"，
 *    拉高后**紧接着就发下一个 strobe**，于是帧首那 5 个播种脉冲的高电平只有
 *    两条指令的时间（实测 ≤100 ns，实际约 10–30 ns）——面板栅极移位寄存器数不清时钟，
 *    每帧拿不到 5 个播种移位（抓包里 6 连发只剩 3–5 个、SIg 序列变成 0100/01100），
 *    栅极图案因此逐帧漂移，开屏画面出现重影/错列。
 *    现在两个电平都显式保持：用 2 µs 的等待（1 MHz 定时器量化 → 实际 ≥1 µs）。 */
static inline void __not_in_flash_func(vfdStrobe)(uint8_t pin)
{
    gpio_put(pin, 0);
    rp2040ShortDelayUs(2); /* 低电平 ≥1 µs（tWL/tWCLK ≥300/80 ns，留量化余量） */
    gpio_put(pin, 1);
    rp2040ShortDelayUs(2); /* 高电平 ≥1 µs —— 关键修复，原先只有十几 ns */
}

/* LAT 锁存脉冲：先把 LAT 拉到**有效电平**并保持 ≥1 µs，再回到空闲电平。
 * 有效电平 = 高（驱动电路里没有反相器，MCU 直连 LATa/LATg）：
 * MCU 直连 LATa/LATg，所以必须发**正脉冲**（见 vfd_platform_rp2040.h / vfd_scan_pio_bits.h）。
 * ⚠️ 锁存发生在"进入有效电平"的那个沿 ⇒ 本函数必须**先于** CLKg 的 vfdStrobe() 调用，
 *    与 PIO 引擎的 `set pins 0x1 → 0x2` 一致：先锁存上一扫描移入的图案，再前进一格。 */
static inline void __not_in_flash_func(vfdLatPulse)(uint8_t pin)
{
    const uint32_t active = 1, idle = 0; /* 高有效：正脉冲锁存，空闲低 */
    gpio_put(pin, active);
    rp2040ShortDelayUs(2); /* 有效电平 ≥1 µs（手册 tWL ≥ 300 ns，留量化余量） */
    gpio_put(pin, idle);
    rp2040ShortDelayUs(2); /* 撤掉后保持 ≥1 µs 再抬 CLKg（手册 tLH ≥ 120 ns） */
}

/* SIg 的**数据保持时间**（手册 tDH >= 30 ns）：
 * 第 2 个播种脉冲的上升沿采样到 SIg=1 之后，必须再保持一会儿才能拉低。
 * 逻辑分析仪实测原实现里"拉低"与"上升沿"几乎同时（≤100 ns），余量偏小；
 * 这里插入约 0.4 µs 的忙等（不访问 Flash，可安全用于 RAM 中断）。 */
static inline void __not_in_flash_func(vfdSigHoldDelay)()
{
    for (volatile int i = 0; i < 16; ++i)
        tight_loop_contents();
}

/* ------------------------------------------------------------------ 引擎实现 */

void Rp2040Platform::engineInit()
{
    /* ---- 1. LAT / CLKg / SIg：GPIO 软件翻转 ---- */
    const uint8_t outPins[] = { _cfg.pin_lat, _cfg.pin_clkg, _cfg.pin_sig };
    for (uint8_t pin : outPins) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_OUT);
        gpio_put(pin, 0);
    }
    gpio_put(_cfg.pin_lat, 0); /* LAT 空闲低（高有效，正脉冲锁存） */
    gpio_put(_cfg.pin_clkg, 1); /* 空闲高 */
    gpio_put(_cfg.pin_sig, 0);

    /* ---- 2. SPI：MODE3(CPOL=1 空闲高，满足 Note 7①) + LSB-first ---- */
    gpio_set_function(_cfg.pin_clka, GPIO_FUNC_SPI);
    gpio_set_function(_cfg.pin_sia, GPIO_FUNC_SPI);
    spi_init(_cfg.spi, _cfg.spi_hz);
    spi_set_format(_cfg.spi, 8, SPI_CPOL_1, SPI_CPHA_1, SPI_LSB_FIRST);
    _spiBaudrate = spi_set_baudrate(_cfg.spi, _cfg.spi_hz);

    /* ---- 3. DMA ---- */
    configureDma();

    /* ---- 4. 消隐裕量自检：48 字节的移位时间 + 中断内 GPIO/触发开销 ---- */
    const uint32_t transferUs = (48u * 8u * 1000000u) / (_spiBaudrate ? _spiBaudrate : 1u) + 4u;
    if (_cfg.blank_guard_us <= transferUs) {
        _cfg.blank_guard_us = transferUs + 4u; /* 自动抬高，避免违反 Note 7② */
        _guardMarginUs = 4u;
    } else {
        _guardMarginUs = _cfg.blank_guard_us - transferUs;
    }
}

void Rp2040Platform::engineStart()
{
    startScanTimer();
}

void Rp2040Platform::engineStop()
{
    stopScanTimer();
    dma_channel_abort(static_cast<uint>(_cfg.dma_tx));
    dma_channel_abort(static_cast<uint>(_cfg.dma_rx));
}

void Rp2040Platform::engineDeinit()
{
    engineStop();
    spi_deinit(_cfg.spi);
}

void Rp2040Platform::service()
{
    /* RX 已由 DMA 持续排空，这里只做保险性清理，避免任何残留把 FIFO 顶满 */
    while (spi_get_hw(_cfg.spi)->sr & SPI_SSPSR_RNE_BITS)
        (void)spi_get_hw(_cfg.spi)->dr;
}

void Rp2040Platform::configureDma()
{
    /* 先确保两个通道都处于停止状态（recover() 会重复调用本函数） */
    dma_channel_abort(static_cast<uint>(_cfg.dma_tx));
    dma_channel_abort(static_cast<uint>(_cfg.dma_rx));

    /* TX：每次扫描把 48 字节搬到 SPI TX FIFO，由 SPI TX DREQ 节流 */
    dma_channel_config tx = dma_channel_get_default_config(static_cast<uint>(_cfg.dma_tx));
    channel_config_set_transfer_data_size(&tx, DMA_SIZE_8);
    channel_config_set_read_increment(&tx, true);
    channel_config_set_write_increment(&tx, false);
    channel_config_set_dreq(&tx, spi_get_dreq(_cfg.spi, true));
    dma_channel_configure(static_cast<uint>(_cfg.dma_tx), &tx,
        &spi_get_hw(_cfg.spi)->dr, nullptr, 0, false);

    /* RX：PL022 主机在 RX FIFO 满时不会再发起新的传输，只写不读会让 SPI 卡死。
     * 用一个"永不结束"的 DMA 把收到的字节丢进 _rxSink，CPU 零开销。 */
    dma_channel_config rx = dma_channel_get_default_config(static_cast<uint>(_cfg.dma_rx));
    channel_config_set_transfer_data_size(&rx, DMA_SIZE_8);
    channel_config_set_read_increment(&rx, false);
    channel_config_set_write_increment(&rx, false);
    channel_config_set_dreq(&rx, spi_get_dreq(_cfg.spi, false));
    dma_channel_configure(static_cast<uint>(_cfg.dma_rx), &rx,
        &_rxSink, &spi_get_hw(_cfg.spi)->dr, 0xFFFFFFFFu, true);
}

void Rp2040Platform::startScanTimer()
{
    if (_timerRunning)
        return;
    /* 负数 delay = 固定周期，与回调耗时无关 */
    _timerRunning = add_repeating_timer_us(-static_cast<int64_t>(_cfg.scan_period_us),
        scanTimerThunk, this, &_timer);
}

void Rp2040Platform::stopScanTimer()
{
    if (!_timerRunning)
        return;
    cancel_repeating_timer(&_timer);
    _timerRunning = false;
}

/* ------------------------------------------------------------------ 扫描中断 */

/* 扫描中断入口：与 scanTick 一起放在 RAM（.time_critical），避免 XIP/Flash 抖动 */
bool __not_in_flash_func(Rp2040Platform::scanTimerThunk)(repeating_timer_t *t)
{
    return static_cast<Rp2040Platform *>(t->user_data)->scanTick();
}

bool __not_in_flash_func(Rp2040Platform::scanTick)()
{
#if VFD_DEBUG_DIAG
    const uint32_t irqT0 = time_us_32(); /* 诊断：ISR 忙时累计起点（VFD_DEBUG_DIAG） */
#endif
    /* (1) PWM 计数器归零：BK 立即回到消隐，锁定点亮窗口相位在扫描周期尾部 */
    pwm_set_counter(static_cast<uint>(_pwmSlice), 0);

    /* (2) 帧首：**先播种 5 个移位，再前进/锁存**。
     *
     * ⚠️ 顺序很关键（实物照片定位出来的 bug）：原驱动 `VFD_GP1211AI::timerHandler()`
     *    在帧首是"先 5 个播种脉冲（SIg=1,1,0,0,0），再发前进脉冲"。
     *    本移植原来写成"先前进一步、再播种 5 个"，当时以为"移位可交换"——**错了**：
     *    一帧共 6 次帧首移位，送进链的**位序列**不同：
     *      先播种：1,1,0,0,0,0  ⇒ 链里那对 1 落在位置 (47,48)
     *      先前进：0,1,1,0,0,0  ⇒ 落在 (46,47)
     *    差一个位置 = 每帧所有栅极对整体错开一格 = **画面水平错 3 列**
     *    （左起 3 列永远不亮、其余内容整体右移 3 列——与实测照片完全一致）。
     */
    if (_scan == 0) {
        /* 帧边界：切到最新发布的帧缓冲 */
        _active = _published;
        _frameStarts++;

        /* 栅极链播种：SIg 依次移入 1,1,0,0,0（5 个时钟，必须在前进脉冲之前） */
        gpio_put(_cfg.pin_sig, 1);
        vfdStrobe(_cfg.pin_clkg);
        vfdStrobe(_cfg.pin_clkg);
        vfdSigHoldDelay();
        gpio_put(_cfg.pin_sig, 0);
        vfdStrobe(_cfg.pin_clkg);
        vfdStrobe(_cfg.pin_clkg);
        vfdStrobe(_cfg.pin_clkg);
    }

    /* (3) 先 LAT 锁存、后 CLKg 前进——顺序必须与 PIO 引擎一致。
     *     PIO 的边界脉冲是 `set pins 0x1`（LAT 拉高 = 锁存）→ `set pins 0x2`（LAT 拉低 + CLKg 拉高）：
     *     LAT 的有效沿在**前**、CLKg 的上升沿（链移位）在**后**，即"锁存旧图案、再走一格"。
     *     tick 原来写成"先 CLKg 再 LAT" ⇒ 锁存的是**已经走过一格**的图案 ⇒
     *     每个时序选通的栅极对整体偏一格 = 画面水平错 3 列
     *     （实测：tick 画面与 pio 最佳对齐 dx=-3）。
     *     此期间 BK 为消隐，满足手册 Note 7③④，且桁间消隐远大于 Note 16 要求的 5 us。 */
    /* ⚠️ 边界四步（与 PIO 的 set pins 0x0/0x2/0x3/0x2 一一对应，见 src/vfd_scan.pio）：
     *  ① CLKg 拉低（LAT 上一步已释放为低）——为空出上升沿
     *  ② CLKg↑ = 栅极链前进一位
     *  ③ LAT↑ = 锁存（此刻栅极链已是前进后的状态）
     *  ④ LAT↓ = 释放；回到空闲（LAT 低、CLKg 高，与手册 Figure 3/5 一致）
     * 顺序不能反：先锁存后前进会锁到"前进之前"的栅极对 ⇒ 画面恒定平移 3 像素
     * （PIO 版抓包实测：LAT 上升沿曾比 CLKg 前进沿早 440 ns）。 */
    gpio_put(_cfg.pin_clkg, 0); /* ① */
    rp2040ShortDelayUs(1);
    gpio_put(_cfg.pin_clkg, 1); /* ② 上升沿 = 前进（LAT 仍低） */
    rp2040ShortDelayUs(1);
    gpio_put(_cfg.pin_lat, 1);  /* ③ 锁存 */
    rp2040ShortDelayUs(2);
    gpio_put(_cfg.pin_lat, 0);  /* ④ 释放，回到空闲 */

    /* (4) 启动本次扫描的 48 字节搬运 */
    const volatile uint8_t *src = _active;
    if (src != nullptr) {
        if (dma_channel_is_busy(static_cast<uint>(_cfg.dma_tx))) {
            _dmaBusyErrors++; /* 理论上不会发生：周期远大于传输时间 */
        } else {
            dma_channel_transfer_from_buffer_now(static_cast<uint>(_cfg.dma_tx),
                src + _scan * SCAN_BYTES, SCAN_BYTES);
        }
    }

    if (++_scan >= SCANS_PER_FRAME)
        _scan = 0;
    _scans++;
#if VFD_DEBUG_DIAG
    _irqUs += time_us_32() - irqT0; /* 诊断：累计本次扫描中断的忙时 */
#endif
    return true;
}

/* ------------------------------------------------------------------ 诊断 */

const char *Rp2040Platform::engineName() const
{
    return "tick(SPI+DMA+timer)";
}

uint32_t Rp2040Platform::clockHz() const
{
    return _spiBaudrate;
}

uint32_t Rp2040Platform::guardMarginUs() const
{
    return _guardMarginUs;
}

bool Rp2040Platform::engineStalled() const
{
    /* tick 引擎由中断驱动：只要中断在跑就不断扫描；
     * 唯一可报告的异常是"扫描边界到了但 DMA 还没搬完"。 */
    return _dmaBusyErrors != 0;
}

} /* namespace vfd */
