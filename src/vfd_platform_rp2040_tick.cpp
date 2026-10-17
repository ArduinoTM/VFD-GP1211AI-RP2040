/*
 * vfd_platform_rp2040_tick.cpp —— 默认扫描引擎："定时器 + SPI + DMA"。
 *
 * 每个扫描周期（189 µs）由 repeating_timer 中断驱动，**选项 C（2026-10-17）之后与 pio
 * 引擎逐条同形**（亮度映射与落点边界共用 src/vfd_pio_seed_timing.h，别抄第二份）：
 *   ① BK 消隐（PWM 计数器归零 = 消隐起点，锁定点亮窗口相位在周期尾部）
 *   ② CLKg 前进一位 + LAT 锁存（边界四步，此时 BK 为消隐）
 *   ③ 帧边界：切换帧缓冲（在整帧第一次扫描，先于边界四步）
 *   ④ 搬运落点两选一（按亮度）：
 *        · 低亮度（点亮 ≤ 97 µs）：立即触发 DMA ⇒ 48 字节整段落在**消隐**窗口内；
 *        · 高亮度（点亮 ≥ 102 µs）：消隐窗口装不下 87 µs 的搬运 ⇒ 排一个**一次性闹钟**，
 *          等 BK 变低（点亮起点）再触发 ⇒ 整段落在**点亮**窗口内（手册 Figure 5 形态）。
 *          ⚠️ 不用中断里忙等：高亮度档的等待是 38…87 µs/扫描（20%–45% CPU）。
 *   ⑤ 整帧最后一次扫描：紧跟边界之后把 1,1,0,0,0 播种进栅极链（消隐窗口内；低亮度时与
 *      搬运并行 —— CLKg/SIg 与 CLKa/SIa 是两套互不相干的移位链，LAT 全程低 ⇒ 不可见）。
 *      播种位置与"帧首边界之前"逐位等价（两者之间没有别的 CLKg 上升沿）⇒ 画面相位不变。
 * 阳极数据由 DMA 搬到 SPI0 TX FIFO（TX DREQ 节流），CPU 不搬数据；SPI 4.464 MHz 下
 * 384 bit ≈ 86 µs。落点是否满足手册 Note 7②（搬运期间 BK 不得变化）由头文件的
 * static_assert 在**编译期**保证：低亮度 边界+搬运 ≤ 消隐窗口；高亮度 搬运+延迟+收尾 ≤ 点亮窗口。
 */
#include "vfd_platform_rp2040.h"

#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/spi.h"
#include "hardware/sync.h"  /* save_and_disable_interrupts（挂起亮度变更时保护那一对值）*/
#include "hardware/timer.h" /* timer_hw：等 BK 变高的超时判据（1 MHz 计时器）*/
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
 * ⚠️ 锁存发生在"进入有效电平"的那个沿 ⇒ 边界顺序是**先前进、再锁存**：
 *    本函数必须在 CLKg 的前进沿（vfdStrobe()）**之后**调用，锁到的才是前进后的栅极对。
 *    与 PIO 引擎逐条对应：`set pins 0x0`（CLKg↓）→ `0x2`（CLKg↑ 前进）→ `0x3`（LAT↑ 锁存）
 *    → `0x2`（LAT↓ 释放），见 src/vfd_scan.pio。
 *    ⚠️ 写反（先锁存、后前进）会锁到"前进之前"的栅极对 ⇒ 画面整体水平平移 3 列 ——
 *       这是项目历史上真实踩过的坑（tick 曾与 PIO 差 dx = −3）。 */
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

/* 等 BK **真的在引脚上**变高。
 *
 * ⚠️ 2026-10-17 tick 四档抓包揪出的坑：pwm_set_counter(slice, 0) 只是把 PWM 的计数寄存器写 0，
 *    而 PWM 的输出比较跟着**它自己的计数时钟**走（本项目 clkdiv 让 PWM 计数 1 µs 一拍）
 *    ⇒ 写寄存器到引脚翻转有 ≈0.3–1 µs 的滞后；而 gpio_put(CLKg, …) 走 SIO、几乎立刻到引脚。
 *    于是"先写 CTR、再翻 CLKg"在**引脚上**变成"CLKg 先变低、BK 后变高"—— 实测每扫描 1 次
 *    Note 7③（4 份抓包各 2115/2116 次，CLKg↓ 比 BK↑ 早 0.18–0.30 µs）。
 *    手册 Note 7③ 要求"CLK 为低时不得改变 BK"⇒ 必须**先看到 BK 变高**再动 CLKg。
 *    超时 3 µs：正常 0.3–0.7 µs 就返回；真超时（引脚/输入通路坏了）也不在这里停摆
 *    —— 扫描不能停（手册 Note 14），照旧往下走，让抓包去暴露问题。 */
static inline void __not_in_flash_func(vfdWaitBkHigh)(uint8_t pin)
{
    const uint32_t t0 = timer_hw->timerawl;
    while (!gpio_get(pin) && static_cast<uint32_t>(timer_hw->timerawl - t0) < 3u) {
    }
}

/* BK↑ 与随后的 CLKg↓ 之间再拉开一点：物理上让 BK 先稳下来，同时保证任何采样率下这两个沿
 * 都能被分开看见。与 vfdSigHoldDelay() 同一套忙等，只是目的不同（那个是 SIg 的保持时间）。
 * ⚠️ 实测（2026-10-17 四档抓包）：24 次循环 ≈2.2 µs（含轮询尾巴）—— 比原先注释里估的 0.4 µs
 *    长得多，已按实测写进 TICK_BOUNDARY_US 与 TICK_LIT_LOW_MAX_US（见 vfd_pio_seed_timing.h）。
 *    不再加长：它已经是低亮度落点余量（0x80 档实测 4.6 µs）的主要开销。 */
static inline void __not_in_flash_func(vfdBkSettleDelay)()
{
    for (volatile int i = 0; i < 24; ++i)
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
    /* BK 是 PWM 输出，但扫描中断要**读回**它的引脚电平（等 BK↑ 再动 CLKg，见 vfdWaitBkHigh()）
     * ⇒ 打开该 pad 的输入缓冲。SDK 的 gpio_set_function() 默认就把 IE 置 1，这里显式再来一次，
     * 与 pio 引擎的做法一致（那边读 BK 当 wait 条件）。 */
    gpio_set_input_enabled(_cfg.pin_bk, true);

    /* ---- 2. SPI：MODE3(CPOL=1 空闲高，满足 Note 7①) ----
     * ⚠️ 这里**必须**传 SPI_MSB_FIRST：PL022 只支持 MSB-first；传 SPI_LSB_FIRST 时该参数是
     *    `__unused`（被忽略），SDK 里 `invalid_params_if(… order != SPI_MSB_FIRST)` 那行**在**，
     *    只是 `PARAM_ASSERTIONS_ENABLED_HARDWARE_SPI` 默认为 0 才没炸 —— 一旦有人开启断言
     *    （`PARAM_ASSERTIONS_ENABLE_ALL=1`）就会在 init() 直接 panic。
     *    本驱动要的 LSB-first 是**逻辑位序**，由打包阶段逐字节反序产出
     *    （`packFrame(..., wireOrder=true)`，见 vfd_scanpack.h 的"传输层适配"）。 */
    gpio_set_function(_cfg.pin_clka, GPIO_FUNC_SPI);
    gpio_set_function(_cfg.pin_sia, GPIO_FUNC_SPI);
    spi_init(_cfg.spi, _cfg.spi_hz);
    spi_set_format(_cfg.spi, 8, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST);
    _spiBaudrate = spi_set_baudrate(_cfg.spi, _cfg.spi_hz);

    /* ---- 3. DMA ---- */
    configureDma();

    /* ---- 4. 落点自洽性：**在编译期**由 vfd_pio_seed_timing.h 的 static_assert 保证
     *（低亮度：边界 + 搬运 ≤ 消隐窗口；高亮度：搬运 + 触发延迟 + 收尾 ≤ 点亮窗口），
     * 这里只算一个供诊断口播的"余量"，形态与 pio 引擎的 guardMarginUs() 一致。
     * ⚠️ 只算数、**不新增 _engineError 条件**：任何新判错都会让 engineStart() 直接 return、
     *    状态机/定时器不启动而表现为"只有 BK 跳变"（2026-10-16 已踩两次）。 */
    /* 让"落点判据"与"PWM 电平"从第一刻起就一致：recover() 可能在"亮度变更还挂起"时重入，
     * 这里把挂起项一并落地（宁可丢一次 189 µs 的延后，也不要出现"落点按新亮度、PWM 按旧亮度"的错配）。*/
    _pendingApply = false;
    _appliedLitUs = _litWindowUs; /* 引擎当前生效的点亮窗口（落点判据与 PWM 电平都以它为准）*/
    rp2040ApplyPwmLevel(_pwmSlice, _pwmChannel, _pwmWrap, _appliedLitUs);
    const uint32_t litUs = _litWindowUs ? _litWindowUs : static_cast<uint32_t>(PIO_LIT_MIN_US);
    _guardMarginUs = tickLowBrightnessPlacement(litUs)
        ? ((PIO_SEED_SCAN_PERIOD_US - litUs > TICK_BOUNDARY_US + TICK_XFER_US)
              ? (PIO_SEED_SCAN_PERIOD_US - litUs) - (TICK_BOUNDARY_US + TICK_XFER_US) : 0u)
        : (litUs > TICK_XFER_US + TICK_TRIG_DELAY_US
              ? litUs - (TICK_XFER_US + TICK_TRIG_DELAY_US) : 0u);
}

void Rp2040Platform::engineStart()
{
    /* 先给栅极链播一次种（engineStart 时扫描还没跑起来，这里不占用任何窗口）：
     * 播种已挪到"整帧最后一次扫描"（见 seedGrid()），不先播的话第一帧要跑满 8.1 ms
     * 才能让栅极图案成形。pio 引擎的 engineStart() 同样先播一次。 */
    seedGrid();
    startScanTimer();
}

void Rp2040Platform::engineStop()
{
    stopScanTimer();
    /* 点亮起点的一次性闹钟不能留到停扫之后（它会去触发一次搬运 / 多播一次种）*/
    if (_trigAlarm >= 0) {
        cancel_alarm(_trigAlarm);
        _trigAlarm = -1;
    }
    _alarmDoesXfer = false;
    _seedPending = false;
    dma_channel_abort(static_cast<uint>(_cfg.dma_tx));
    dma_channel_abort(static_cast<uint>(_cfg.dma_rx));
    /* P2-6（v1.5.1）：停扫后把栅极输入钉回手册规定的空闲电平 —— CLKg 空闲**高**（与 Note 7① 同理）、
     * LAT 空闲低、SIg 空闲低。CLKa 是 SPI 的 SCK（MODE3 ⇒ CPOL=1，空闲电平就是高），停扫后本来就
     * 停在高电平，不需要（也不能）用 SIO 去写它。 */
    gpio_put(_cfg.pin_clkg, 1);
    gpio_put(_cfg.pin_lat, 0);
    gpio_put(_cfg.pin_sig, 0);
}

void Rp2040Platform::engineDeinit()
{
    engineStop();
    spi_deinit(_cfg.spi);
}

void Rp2040Platform::service()
{
    /* ---- RX 垃圾桶 DMA 续命（本轮修复，见 docs/13）--------------------------------
     * 该通道在 configureDma() 里被**一次性**武装成 VFD_TICK_RX_DMA_COUNT 个字节，
     * 默认 2^32-1：48 B/扫描 × 5291 次/s ⇒ 约 **4.70 h** 跑完；
     * 跑完之后排空就退化为下面那条 CPU 兜底循环（主循环每轮多几条指令）。
     * ⚠️ 报告曾断言"跑完后 RX FIFO 顶满 ⇒ PL022 不再发起传输 ⇒ 缺帧 / 静默冻结"——
     *    **2026-10-12 实机 A/B 没有复现**：对照固件把计数跑到耗尽后 dma_busy_err 仍恒 0、
     *    画面正常（见 docs/13 §4）。所以本修复的定位是"把 DMA 排空通道恢复成一直活着"
     *    （CPU 零开销、不再依赖 CPU 兜底的时序），**不是**在修一个已观测到的缺帧。
     *
     * 处置：**在剩余不足一半时就补满**（写 TRANS_COUNT 不影响已在途的传输），
     * 通道一直在搬；万一真跑到 0（例如 TOPUP 关了），`!busy` 分支会把计数重新武装并触发。 */
#if VFD_TICK_RX_TOPUP
    {
        const uint dmaRx = static_cast<uint>(_cfg.dma_rx);
        if (!dma_channel_is_busy(dmaRx)) {
            dma_channel_set_trans_count(dmaRx, VFD_TICK_RX_DMA_COUNT, true); /* 通道已停：补计数并触发 */
            _rxRearms++;
        } else if (dma_hw->ch[dmaRx].transfer_count < (VFD_TICK_RX_DMA_COUNT / 2u)) {
            dma_channel_set_trans_count(dmaRx, VFD_TICK_RX_DMA_COUNT, false); /* 在途补满，不重触发 */
            _rxRearms++;
        }
    }
#endif

    /* 保险性清理：任何残留都读走，避免 RX FIFO 顶满把主机（PL022）卡住。
     * ⚠️ 这一步在主循环上下文执行，只兜底；真正的持续排空靠上面的 DMA 通道。
     *    置 VFD_TICK_SERVICE_DRAIN=0 可关掉它（与 TOPUP=0 合用 = 两条排空路径都关），
     *    用来实测"RX FIFO 满会不会让传输停摆"—— 见 docs/13 §4.4。 */
#if VFD_TICK_SERVICE_DRAIN
    while (spi_get_hw(_cfg.spi)->sr & SPI_SSPSR_RNE_BITS)
        (void)spi_get_hw(_cfg.spi)->dr;
#endif
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
     * 用一个**大而有限**的计数把收到的字节丢进 _rxSink，CPU 零开销；计数由 service()
     * 在剩余不足一半时提前补满（见那里的注释与 docs/13），所以正常永不跑完。 */
    dma_channel_config rx = dma_channel_get_default_config(static_cast<uint>(_cfg.dma_rx));
    channel_config_set_transfer_data_size(&rx, DMA_SIZE_8);
    channel_config_set_read_increment(&rx, false);
    channel_config_set_write_increment(&rx, false);
    channel_config_set_dreq(&rx, spi_get_dreq(_cfg.spi, false));
    dma_channel_configure(static_cast<uint>(_cfg.dma_rx), &rx,
        &_rxSink, &spi_get_hw(_cfg.spi)->dr, VFD_TICK_RX_DMA_COUNT, true);
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

/* 触发本扫描的 48 字节搬运。两条调用路径：
 *   · 低亮度落点：scanTick() 在边界四步之后**直接**调用（消隐窗口内，紧跟边界）；
 *   · 高亮度落点：triggerThunk() 在"BK 变低（点亮起点）"之后由一次性闹钟调用。 */
void __not_in_flash_func(Rp2040Platform::triggerScanTransfer)()
{
    const volatile uint8_t *src = _pendingSrc;
    if (src == nullptr)
        return;
    if (dma_channel_is_busy(static_cast<uint>(_cfg.dma_tx))) {
        _dmaBusyErrors++; /* 理论上不会发生：周期远大于传输时间 */
        return;
    }
    dma_channel_transfer_from_buffer_now(static_cast<uint>(_cfg.dma_tx), src, SCAN_BYTES);
}

/* "点亮起点"一次性闹钟的回调（每个扫描周期最多一个，两种用途）：
 *   · 高亮度落点：触发本扫描的 48 字节搬运（等 BK 变低之后再搬 ⇒ 整段落在点亮窗口内）；
 *   · 帧末扫描：紧跟其后做栅极链播种（两种落点都用同一条闹钟）。
 * 为什么用闹钟而不是在扫描中断里忙等：等待 = 消隐窗口 − 边界 ≈ 38…87 µs，每个扫描都忙等
 * 会吃掉 20%–45% CPU（见 docs/17 §6 选项 A 的代价评估）。闹钟的容差很宽：搬运只要落进
 * [点亮起点, 扫描周期 − 搬运 − 收尾]（lit = 102 µs 档最紧 ≈13 µs）；播种只要整段在点亮
 * 窗口内（lit 最小 38 µs，播种 ≈21 µs ⇒ 余量 ≈17 µs）。 */
int64_t __not_in_flash_func(Rp2040Platform::triggerThunk)(alarm_id_t id, void *user_data)
{
    (void)id;
    Rp2040Platform *self = static_cast<Rp2040Platform *>(user_data);
    self->_trigAlarm = -1;
    if (self->_alarmDoesXfer)
        self->triggerScanTransfer();
    if (self->_seedPending) {
        self->_seedPending = false;
        self->seedGrid();
    }
    return 0; /* 一次性：不再排程 */
}

/* 亮度变了：把新点亮窗口挂起，等**下一个扫描边界**与 PWM 电平一起生效。
 * 为什么不能立刻生效：中断里触发的 DMA 可能正在飞（搬运 ≈86 µs），此刻改 PWM 比较值会让
 * BK 边沿落进搬运内部 ⇒ 违反 Note 7②（pio 版用"等状态机停到 wait 再打指令补丁"解决同一问题）。
 * 延到扫描边界只晚 ≤189 µs（人工改亮度察觉不到），而边界处"在途搬运"必定已经结束
 * （搬运末沿 ≤ 边界 + 91 µs < 189 µs）。
 * ⚠️ 关中断写这一对值：中断可能在两句之间读走"新值 + 旧标志"（或反之）。 */
void Rp2040Platform::applyBrightnessMode()
{
    const uint32_t save = save_and_disable_interrupts();
    _pendingLitUs = _litWindowUs;
    _pendingApply = true;
    restore_interrupts(save);
}

bool __not_in_flash_func(Rp2040Platform::scanTick)()
{
#if VFD_DEBUG_DIAG
    const uint32_t irqT0 = time_us_32(); /* 诊断：ISR 忙时累计起点（VFD_DEBUG_DIAG） */
#endif
    /* (0) 兜底清理 + 挂起的亮度变更在**扫描边界**生效：
     *   · 上一次扫描排的高亮度落点闹钟正常早已消费；万一被极端延迟拖过 189 µs，
     *     这里取消掉 —— 否则它会落到本扫描上，等于本扫描触发两次搬运。
     *   · 亮度变更与 PWM 电平必须**同时**生效（见 applyBrightnessMode()）。*/
    if (_trigAlarm >= 0) {
        cancel_alarm(_trigAlarm);
        _trigAlarm = -1;
    }
    _alarmDoesXfer = false;
    _seedPending = false; /* 捞回来的闹钟不会执行，别把标志留给下一帧（否则会多播一次种）*/
    if (_pendingApply) {
        _pendingApply = false;
        _appliedLitUs = _pendingLitUs;
        rp2040ApplyPwmLevel(_pwmSlice, _pwmChannel, _pwmWrap, _appliedLitUs);
    }

    /* (1) PWM 计数器归零：BK 回到消隐（消隐起点 = 扫描周期起点），锁定点亮窗口相位在周期尾部。
     *     ⚠️ 必须**等到 BK 真的在引脚上变高**、再与随后的 CLKg↓ 拉开 ≈0.4 µs，才能走边界四步：
     *        PWM 的输出比较跟着它自己的计数时钟走，写 CTR 到引脚翻转有 ≈0.3–1 µs 滞后，而 SIO
     *        写 CLKg 几乎立刻到引脚 —— 不等就会出现"CLKg 先低、BK 后高"= 每扫描 1 次 Note 7③
     *        （2026-10-17 tick 四档抓包实测 2115/2116）。见 vfdWaitBkHigh() 的注释。 */
    pwm_set_counter(static_cast<uint>(_pwmSlice), 0);
    vfdWaitBkHigh(_cfg.pin_bk); /* 等引脚上的 BK↑（正常 ≈0.3–0.7 µs，超时 3 µs）*/
    vfdBkSettleDelay();         /* 再让位 ≈0.4 µs，让两个沿在任何采样率下都分得开 */
    const uint32_t periodT0 = time_us_32(); /* 本扫描周期起点（≈ 引脚上 BK 变高的时刻）*/

    /* (2) 帧边界：切到最新发布的帧缓冲（先于边界四步；播种已挪到帧末，见 seedGrid()）*/
    if (_scan == 0) {
        _active = _published;
        _frameStarts++;
    }

    /* (3) 边界四步：**先前进、再锁存** —— 顺序必须与 PIO 引擎一致。
     *     手册 Figure 3/5 的硬要求：LAT 空闲低（锁存打正脉冲）、CLKg 空闲高（脉冲低→高，
     *     上升沿 = 栅极链前进）。顺序上必须"先 CLKg↑ 前进、再 LAT↑ 锁存"：本板 LATa 与 LATg
     *     并联（手册 Note 15 要求 LATg 常高/透明），锁存的永远是"此刻的栅极链"，
     *     写成"先锁存后前进"就会锁到**前进之前**的那一对 ⇒ 每个时序选通的栅极对整体偏一格
     *     = **画面恒定水平错 3 列**（PIO 版抓包实测：LAT 上升沿曾比 CLKg 前进沿早 440 ns；
     *     tick 版与 pio 版最佳对齐 dx=-3）。此期间 BK 为消隐，满足手册 Note 7③④，
     *     且桁间消隐远大于 Note 16 要求的 5 us。 */
    /* ⚠️ 四步与 PIO 的 `set pins 0x0/0x2/0x3/0x2` 一一对应（见 src/vfd_scan.pio）：
     *  ① CLKg 拉低（LAT 上一步已释放为低）——为空出上升沿
     *  ② CLKg↑ = 栅极链前进一位
     *  ③ LAT↑ = 锁存（此刻栅极链已是前进后的状态）
     *  ④ LAT↓ = 释放；回到空闲（LAT 低、CLKg 高，与手册 Figure 3/5 一致） */
    gpio_put(_cfg.pin_clkg, 0); /* ① */
    rp2040ShortDelayUs(1);
    gpio_put(_cfg.pin_clkg, 1); /* ② 上升沿 = 前进（LAT 仍低） */
    rp2040ShortDelayUs(1);
    gpio_put(_cfg.pin_lat, 1);  /* ③ 锁存 */
    rp2040ShortDelayUs(2);
    gpio_put(_cfg.pin_lat, 0);  /* ④ 释放，回到空闲 */

    /* (4) 搬运落点两选一 + 帧末播种的时机（选项 C，与 pio 引擎同一形态；判据与边界见
     *     vfd_pio_seed_timing.h）：
     *   · 低亮度（点亮 ≤ 95 µs）：搬运紧跟边界 ⇒ 整段落在**消隐**窗口内
     *     （消隐窗口 = 189 − 点亮 ≥ 94 µs，装得下 边界 ~7 + 搬运 87）；
     *   · 高亮度（点亮 ≥ 102 µs）：消隐窗口装不下 ⇒ 排一次性闹钟、等 BK 变低（点亮起点）
     *     再触发 ⇒ 整段落在**点亮**窗口内（手册 Figure 5 形态）。
     *   · 帧末播种（整帧最后一次扫描）：**也等点亮起点**再播（两种落点共用那条闹钟）——
     *     位置在 A42 之后、A0 之前（= 帧首扫描的边界之前），整段落在同一个 BK 相位内。
     *     ⚠️ 2026-10-17 四档抓包教训：原来"紧跟 A42 之后立刻播"会让播种组与 A42 的前进脉冲
     *        并成一组（SIg 采样 0,1,1,0,0,0），而 pio 引擎（实机验证过）的形态是
     *        "播种 5 个紧跟帧首那 1 个前进"（1,1,0,0,0,0）—— analyze_la_csv.js 的
     *        "帧首 SIg 序列"正是按后者判的（当时 49 个组全报 0,1,1,0,0,0）。
     * ⚠️ 两条路都**不在中断里忙等**：等待 38…87 µs/扫描 = 20%–45% CPU（docs/17 §6 选项 A）。 */
    const volatile uint8_t *src = _active;
    _pendingSrc = (src != nullptr) ? (src + _scan * SCAN_BYTES) : nullptr;
    const bool frameEnd = (_scan == SCANS_PER_FRAME - 1);
    const bool lowPlacement = tickLowBrightnessPlacement(_appliedLitUs);
    if (lowPlacement && _pendingSrc != nullptr)
        triggerScanTransfer(); /* 低亮度：搬运紧跟边界（消隐窗口内）*/

    /* 要排"点亮起点 + TICK_TRIG_DELAY_US"闹钟的两种情况：
     *   · 高亮度档的搬运（等 BK 变低再搬）；· 帧末播种（两种落点都要，等点亮起点再播）。
     * 两者共用同一个闹钟槽（一个扫描周期最多一个）。*/
    const bool alarmForXfer = !lowPlacement && _pendingSrc != nullptr;
    if (alarmForXfer || frameEnd) {
        _alarmDoesXfer = alarmForXfer;
        _seedPending = frameEnd;
        /* 点亮起点（相对本扫描周期起点）= 扫描周期 − 点亮窗口；再留 TICK_TRIG_DELAY_US
         * 避开边沿本身（保证搬运整段在点亮窗口内、播种不压在 BK 下降沿上）。
         * 闹钟延时按"现在"折算（边界四步刚过去 ~5 µs）；万一已经过点，fire_if_past
         * 让它立即触发（仍然落在点亮窗口内，只是余量变小）。*/
        const uint32_t litStartUs =
            static_cast<uint32_t>(PIO_SEED_SCAN_PERIOD_US) - _appliedLitUs;
        const uint32_t targetUs = litStartUs + TICK_TRIG_DELAY_US;
        const uint32_t elapsedUs = time_us_32() - periodT0;
        const int64_t delayUs = static_cast<int64_t>(targetUs) - static_cast<int64_t>(elapsedUs);
        _trigAlarm = add_alarm_in_us(delayUs > 0 ? static_cast<uint64_t>(delayUs) : 0u,
            triggerThunk, this, true);
        if (_trigAlarm < 0) {
            /* 闹钟池满（理论上不会）：退化成忙等 —— 宁可吃这几微秒的 CPU，
             * 也不能让搬运跨过 BK 下降沿（Note 7②）或让播种落错相位。*/
            while (static_cast<int32_t>(time_us_32() - periodT0) < static_cast<int32_t>(targetUs)) {
            }
            if (_alarmDoesXfer)
                triggerScanTransfer();
            if (_seedPending) {
                _seedPending = false;
                seedGrid();
            }
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

/* 栅极链播种：SIg 依次移入 1,1,0,0,0（5 个 CLKg 脉冲）。调用点两处：
 *   · engineStart()：扫描还没跑起来，先给栅极链一个确定的初始图案（第一帧就要正确）；
 *   · triggerThunk()：**整帧最后一次扫描**（_scan == 42）的"点亮起点"闹钟到点时。
 *
 * ⚠️ 顺序很关键（实物照片定位出来的 bug）：原驱动 `VFD_GP1211AI::timerHandler()`
 *    在帧首是"先 5 个播种脉冲（SIg=1,1,0,0,0），再发前进脉冲"。本移植原来写成
 *    "先前进一步、再播种 5 个"，当时以为"移位可交换"——**错了**：一帧共 48 次移位，
 *    送进链的**位序列**不同：先播种 ⇒ 那对 1 落在链位 (43,44)；先前进 ⇒ (44,45)。
 *    差一个位置 = 每帧所有栅极对整体错开一格 = **画面水平错 3 列**。
 *    现在播种放在 A42 之后、A0 之前（与"帧首 A0 之前"是同一个位置：这两点之间没有别的
 *    CLKg 上升沿）⇒ 移位顺序仍是 [A41, A42, 播种×5, A0]，逐位等价、画面相位不变。
 *    为什么必须从帧首挪到帧末：低亮度落点要求"边界四步 → 搬运紧跟"，边界 ~5 + 搬运 87 µs
 *    已经吃满 91 µs 的消隐窗口，帧首再插 20 µs 播种就会越过点亮起点（Note 7②）。
 *    为什么又必须等到"点亮起点"才播（而不是紧跟 A42）：紧跟 A42 会让播种组与 A42 的前进
 *    脉冲并成一组（SIg 采样 0,1,1,0,0,0）—— LA 判读把它读成"先前进攻再播种"（= 画面水平
 *    错 3 列），而 pio 引擎（实机验证过）的形态是"播种 5 个紧跟帧首那 1 个前进"（1,1,0,0,0,0）。
 *
 * ⚠️ 引脚空闲电平（pio 版栽过的地方，见提交 bc3acfc）：播种末步 = 第 5 个脉冲，
 *    vfdStrobe() 的两个电平都显式保持 ⇒ 结束时 **CLKg 停在"高"**（空闲电平）、SIg = 0，
 *    全程 LAT 保持低（这 5 次链搬运不会锁存到屏幕上）。边界 ④ 收尾同样是 CLKg 高、LAT 低。 */
void __not_in_flash_func(Rp2040Platform::seedGrid)()
{
    gpio_put(_cfg.pin_sig, 1);
    vfdStrobe(_cfg.pin_clkg); /* 第 1 个脉冲 */
    vfdStrobe(_cfg.pin_clkg); /* 第 2 个脉冲 */
    vfdSigHoldDelay();        /* SIg 保持时间（手册 tDH >= 30 ns）*/
    gpio_put(_cfg.pin_sig, 0);
    vfdStrobe(_cfg.pin_clkg); /* 第 3 个脉冲 */
    vfdStrobe(_cfg.pin_clkg); /* 第 4 个脉冲 */
    vfdStrobe(_cfg.pin_clkg); /* 第 5 个脉冲；收尾 **CLKg = 高**（空闲电平）*/
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
