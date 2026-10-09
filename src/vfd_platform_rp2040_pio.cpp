/*
 * vfd_platform_rp2040_pio.cpp —— 可选扫描引擎："PIO 状态机 + DMA"。
 *
 * 与 tick 引擎（定时器 + SPI）的区别：
 *   · CLKa / SIa / CLKg / LAT / SIg 全部由**一个 PIO 状态机**产生；
 *   · 阳极数据由 DMA 连续灌入 PIO TX FIFO（TX DREQ 节流），PIO 自己数 48 字节/扫描；
 *   · 扫描周期直接由 BK(PWM) 的消隐边沿同步 —— 数据传输天然落在消隐窗口内，
 *     且不需要任何 CPU 定时器；
 *   · CPU 只在每帧结束时进一次中断（≈123 Hz）：换帧缓冲 + 重启整帧 DMA。
 *
 * 时序（SM 时钟 = 2 × CLKa；默认 CLKa = 4.5 MHz → SM 9 MHz，周期 111 ns）：
 *   PWM wrap → BK 变高（消隐）→ PIO 的 `wait 1 pin BK` 立即通过 → 开始移位
 *   48 字节 × 17 周期 + 边界 6 周期 ≈ 824 周期 ≈ 91.6 µs（帧末再 + 34 周期播种 ≈ 3.8 µs）
 *   → 100 µs 时 BK 变低开始点亮，189 µs 进入下一扫描。
 *
 * 说明：PIO 读取 BK 引脚需要该 pad 的输入缓冲使能（gpio_set_input_enabled）。
 */
#include "vfd_platform_rp2040.h"

#include <string.h>

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/pwm.h"
#include "pico/time.h"

#include "vfd_scan.pio.h"
#include "vfd_scan_pio_bits.h"

namespace vfd {

static_assert(FRAME_SIZE % 4 == 0, "整帧必须能按 32bit 字搬运");
static constexpr uint32_t FRAME_WORDS = FRAME_SIZE / 4; /* 516 个字/帧（48 字节/扫描 布局） */

Rp2040Platform *Rp2040Platform::_instance = nullptr;

/* 把 .pio 里两条 `wait ... pin 7` 的占位引脚号改写成实际的 BK 引脚。
 * 通过"编码后逐字比对"定位，因此 .pio 里指令顺序变化也不会失效。 */
void Rp2040Platform::patchProgramPins(uint16_t *words, uint length)
{
    const uint16_t placeholderLow = static_cast<uint16_t>(pio_encode_wait_pin(false, 7));
    const uint16_t placeholderHigh = static_cast<uint16_t>(pio_encode_wait_pin(true, 7));
    const uint16_t realLow = static_cast<uint16_t>(pio_encode_wait_pin(false, _cfg.pin_bk));
    const uint16_t realHigh = static_cast<uint16_t>(pio_encode_wait_pin(true, _cfg.pin_bk));

    uint patched = 0;
    for (uint i = 0; i < length; ++i) {
        if (words[i] == placeholderLow) {
            words[i] = realLow;
            patched++;
        } else if (words[i] == placeholderHigh) {
            words[i] = realHigh;
            patched++;
        }
    }
    /* 应当恰好找到"等 BK 变低"和"等 BK 变高"各一条 */
    if (patched != 2)
        _engineError = true;
}

/* ------------------------------------------------------------------ 引擎实现 */

void Rp2040Platform::engineInit()
{
    _instance = this;
    _engineError = false;

    /* ---- 0. 引脚约束：SET 组必须是连续三个引脚（LAT, CLKg, SIg）---- */
    if (_cfg.pin_clkg != static_cast<uint8_t>(_cfg.pin_lat + 1)
        || _cfg.pin_sig != static_cast<uint8_t>(_cfg.pin_lat + 2)) {
        _engineError = true;
        return;
    }
    if (_cfg.pin_clka > 29 || _cfg.pin_sia > 29 || _cfg.pin_bk > 29) {
        _engineError = true;
        return;
    }

    /* ---- 1. 装载 PIO 程序 ---- */
    if (_pioOffset >= 0) { /* recover 场景：先释放再重装 */
        pio_program_t old = vfd_scan_program;
        pio_remove_program(_cfg.pio, &old, static_cast<uint>(_pioOffset));
        _pioOffset = -1;
    }

    const uint length = vfd_scan_program.length;
    if (length == 0 || length > 32) {
        _engineError = true;
        return;
    }
    uint16_t words[32];
    memset(words, 0, sizeof(words));
    memcpy(words, vfd_scan_program_instructions, length * sizeof(uint16_t));
    patchProgramPins(words, length);
    if (_engineError)
        return;

    pio_program_t prog = vfd_scan_program;
    prog.instructions = words;
    if (!pio_can_add_program(_cfg.pio, &prog)) {
        _engineError = true;
        return;
    }
    _pioOffset = static_cast<int>(pio_add_program(_cfg.pio, &prog));

    /* ---- 2. 引脚交给 PIO；BK 仍是 PWM 输出，但要让 PIO 能读它 ---- */
    pio_gpio_init(_cfg.pio, _cfg.pin_clka);
    pio_gpio_init(_cfg.pio, _cfg.pin_sia);
    pio_gpio_init(_cfg.pio, _cfg.pin_lat);
    pio_gpio_init(_cfg.pio, _cfg.pin_clkg);
    pio_gpio_init(_cfg.pio, _cfg.pin_sig);
    gpio_set_input_enabled(_cfg.pin_bk, true);

    /* ---- 3. 状态机配置 ---- */
    const float clkSys = static_cast<float>(clock_get_hz(clk_sys));
    float div = clkSys / (2.0f * static_cast<float>(_cfg.pio_clk_hz));
    if (div < 1.0f)
        div = 1.0f;

    pio_sm_config c = vfd_scan_program_get_default_config(static_cast<uint>(_pioOffset));
    sm_config_set_out_pins(&c, _cfg.pin_sia, 1);      /* SIa：out 组（1 个引脚） */
    sm_config_set_set_pins(&c, _cfg.pin_lat, 3);      /* LAT/CLKg/SIg：set 组（连续 3 个） */
    sm_config_set_sideset_pins(&c, _cfg.pin_clka);    /* CLKa：sideset */
    sm_config_set_out_shift(&c, true, true, 32);      /* 右移 = LSB first；autopull 32bit */
    sm_config_set_clkdiv(&c, div);
    pio_sm_init(_cfg.pio, _cfg.sm, static_cast<uint>(_pioOffset), &c);
    _pioClkHz = static_cast<uint32_t>(clkSys / (2.0f * div) + 0.5f);

    /* ---- 4. 引脚方向 + 初始电平（CLKa/CLKg 高；LAT 空闲低；SIa/SIg 低）----
     * LAT：高有效（驱动电路无反相器）⇒ 空闲电平为**低**。 */
    const uint32_t pinMask = (1u << _cfg.pin_clka) | (1u << _cfg.pin_sia)
        | (1u << _cfg.pin_lat) | (1u << _cfg.pin_clkg) | (1u << _cfg.pin_sig);
    const uint32_t pinHigh = (1u << _cfg.pin_clka) | (1u << _cfg.pin_clkg);
    /* 这两个 SDK 函数是往 SM 指令端口逐条塞 `set pins/pindirs` 实现的，
     * 连续调用间没有等待，因此做两遍并留出间隔，避免指令端口竞争导致丢写。 */
    pio_sm_set_pindirs_with_mask(_cfg.pio, _cfg.sm, pinMask, pinMask);
    pio_sm_set_pins_with_mask(_cfg.pio, _cfg.sm, pinHigh, pinMask);
    rp2040ShortDelayUs(10);
    pio_sm_set_pindirs_with_mask(_cfg.pio, _cfg.sm, pinMask, pinMask);
    pio_sm_set_pins_with_mask(_cfg.pio, _cfg.sm, pinHigh, pinMask);

    /* ---- 5. DMA：整帧 516 个 32bit 字 -> PIO TX FIFO，由 TX DREQ 节流 ---- */
    dma_channel_config dc = dma_channel_get_default_config(static_cast<uint>(_cfg.dma_ch));
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, true);
    channel_config_set_write_increment(&dc, false);
    channel_config_set_dreq(&dc, pio_get_dreq(_cfg.pio, _cfg.sm, true));
    _dmaConfig = dc;

    dma_channel_abort(static_cast<uint>(_cfg.dma_ch));
    dma_channel_configure(static_cast<uint>(_cfg.dma_ch), &_dmaConfig,
        &_cfg.pio->txf[_cfg.sm], _blankFrame, FRAME_WORDS, false);

    if (!_dmaIrqReady) {
        dma_channel_set_irq0_enabled(static_cast<uint>(_cfg.dma_ch), true);
        irq_set_exclusive_handler(DMA_IRQ_0, &Rp2040Platform::dmaIrqThunk);
        irq_set_enabled(DMA_IRQ_0, true);
        _dmaIrqReady = true;
    }

    /* ---- 6. 消隐裕量自检（按最坏情况：帧末还要播种）---- */
    const uint32_t smHz = _pioClkHz * 2u;
    const uint32_t workUs = ((PIO_SCAN_WORK_CYCLES + PIO_SEED_CYCLES) * 1000000u)
            / (smHz ? smHz : 1u)
        + 1u;
    if (_cfg.blank_guard_us <= workUs) {
        _cfg.blank_guard_us = workUs + 4u; /* 自动抬高，避免违反 Note 7② */
        _guardMarginUs = 4u;
    } else {
        _guardMarginUs = _cfg.blank_guard_us - workUs;
    }

    /* ---- 7. 播种请求窗口自检 ----
     * 检查点在扫描起点之后 checkUs；请求脚窗口 [raise, raise+hold] 必须
     *   (a) 跨过 checkUs（否则 PIO 看不到请求 → 完全不播种）；
     *   (b) 在下一次检查点（扫描周期 + checkUs）之前结束（否则每帧多播一次 → 画面错位）。
     * 若 CLKa 取得很低（checkUs 接近甚至超过扫描周期），这套握手就不成立，直接报错。 */
    const uint32_t checkUs = (PIO_SEED_CHECK_CYCLES * 1000000u) / (smHz ? smHz : 1u);
    const uint32_t nextCheckUs = _cfg.scan_period_us + checkUs;
    if (checkUs >= _cfg.scan_period_us
        || static_cast<int32_t>(PIO_SEED_RAISE_US) > 0
        || (PIO_SEED_RAISE_US + static_cast<int32_t>(PIO_SEED_HOLD_US)) <= static_cast<int32_t>(checkUs)
        || (PIO_SEED_RAISE_US + PIO_SEED_HOLD_US) <= checkUs
        || (PIO_SEED_RAISE_US + PIO_SEED_HOLD_US) >= nextCheckUs) {
        _engineError = true;
    }
}

void Rp2040Platform::engineStart()
{
    if (_engineError)
        return;

    _active = _published ? _published : reinterpret_cast<const uint8_t *>(_blankFrame);

    seedGrid(); /* 先给栅极链播一次种，让第一帧就正确 */

    pio_sm_clear_fifos(_cfg.pio, _cfg.sm);
    /* 让程序从 .wrap_target（scan:）重新开始：复位 PC + 循环计数，
     * 保证 PIO 的 43 扫描计数与 DMA 的整帧指针始终对齐。 */
    pio_sm_exec(_cfg.pio, _cfg.sm, pio_encode_jmp(static_cast<uint>(_pioOffset)));

    /* 先把 FIFO 喂上数据，再启动状态机，避免第一个 out 就等数据 */
    dma_channel_configure(static_cast<uint>(_cfg.dma_ch), &_dmaConfig,
        &_cfg.pio->txf[_cfg.sm], _active, FRAME_WORDS, true);
    pio_sm_set_enabled(_cfg.pio, _cfg.sm, true);

}

void Rp2040Platform::engineStop()
{
    cancelSeedRequest(); /* 撤掉可能还在排队的播种闹钟，并让请求脚回到低 */
    pio_sm_set_enabled(_cfg.pio, _cfg.sm, false);
    dma_channel_abort(static_cast<uint>(_cfg.dma_ch));
}

void Rp2040Platform::engineDeinit()
{
    engineStop();
    if (_dmaIrqReady) {
        dma_channel_set_irq0_enabled(static_cast<uint>(_cfg.dma_ch), false);
        _dmaIrqReady = false;
    }
    if (_pioOffset >= 0) {
        pio_program_t prog = vfd_scan_program;
        pio_remove_program(_cfg.pio, &prog, static_cast<uint>(_pioOffset));
        _pioOffset = -1;
    }
    if (_instance == this)
        _instance = nullptr;
}

/* CPU 侧播种：SIg 依次 1,1,0,0,0，每个电平给一个 CLKg 低→高脉冲。
 * 用于（a）engineStart 的第一帧、（b）帧末 DMA 完成中断里给下一帧播种。
 *
 * ⚠️ **全程保持 LAT 低**（= 栅极锁存"保持"，不透明）：这样这 5 次链前进只在锁存器后面发生，
 *    屏幕上不会看到"线框左右抖动"；等帧首扫描的边界 ③ 才把结果锁存出来。
 *    早先这里把 LAT 也抬高了（照 Note 15 的"LATg 常高"），一旦播种时刻落进点亮窗口，
 *    栅极链的搬动会**可见**地显示出来 —— 实机症状正是"最右 3 列闪烁、上半部线框左右抖动"。
 *    （tick 引擎的内联播种一直是保持 LAT 低的，所以它没有这个现象。） */
void Rp2040Platform::seedGrid()
{
    const uint32_t lat = 1u << _cfg.pin_lat;
    const uint32_t clkg = 1u << _cfg.pin_clkg;
    const uint32_t sig = 1u << _cfg.pin_sig;
    const uint32_t mask = lat | clkg | sig;

    const uint32_t idle = 0; /* LAT 低（保持）+ CLKg 低 + SIg 低 */
    const uint32_t steps[] = {
        clkg | sig, /* SIg=1, CLKg 高（空闲） */
        sig,        /* CLKg 低 */
        clkg | sig, /* CLKg 高 -> 第 1 个脉冲 */
        sig,
        clkg | sig, /* 第 2 个脉冲 */
        idle,       /* SIg=0, CLKg 低 */
        clkg,       /* 第 3 个脉冲 */
        idle,
        clkg, /* 第 4 个脉冲 */
        idle,
        clkg, /* 第 5 个脉冲 */
        idle, /* 收尾回空闲 */
    };
    for (uint32_t v : steps) {
        pio_sm_set_pins_with_mask(_cfg.pio, _cfg.sm, v, mask);
        rp2040ShortDelayUs(1); /* 满足 tWL >= 300 ns，同时让指令端口不被抢写 */
    }
    /* 最后一个 CLKg 上升沿（第 5 个脉冲）之后，SIg 已为 0；此处再等一拍，
     * 顺便把请求脚状态复位 */
    cancelSeedRequest();
}

/* -------------------------------------------------------------- 帧完成中断 */

void __not_in_flash_func(Rp2040Platform::dmaIrqThunk)()
{
    if (_instance != nullptr)
        _instance->onFrameDmaDone();
}

/* 帧首扫描内拉高播种请求脚（由定时器闹钟在 wrap + PIO_SEED_RAISE_US 触发） */
int64_t __not_in_flash_func(Rp2040Platform::seedRaiseThunk)(alarm_id_t id, void *user_data)
{
    (void)id;
    Rp2040Platform *self = static_cast<Rp2040Platform *>(user_data);
    if (self == nullptr)
        return 0;

    self->_seedRaiseAlarm = -1;
    /* 保持到跨过本扫描周期的 `jmp pin` 检查点，并在下一次检查点之前撤掉 */
    self->_seedLowerAlarm = add_alarm_in_us(PIO_SEED_HOLD_US,
        &Rp2040Platform::seedLowerThunk, self, true);
    return 0;
}

/* 撤掉播种请求脚 */
int64_t __not_in_flash_func(Rp2040Platform::seedLowerThunk)(alarm_id_t id, void *user_data)
{
    (void)id;
    Rp2040Platform *self = static_cast<Rp2040Platform *>(user_data);
    if (self == nullptr)
        return 0;

    self->_seedLowerAlarm = -1;
    return 0;
}

/* 预约"帧首扫描"的播种请求。
 *
 * 为什么不能像原来那样在中到这里立刻拉高、等固定时间再拉低：
 *   整帧 DMA 完成中断发生在**最后一次扫描的数据阶段内**（距该扫描的 jmp pin 检查点
 *   约 50 µs，取决于 FIFO 深度），而 PIO 每个扫描周期只在数据阶段结束后查一次请求脚。
 *   原来"拉高 → 忙等 20 µs → 拉低"的窗口既错过了本帧最后一次扫描的检查点，
 *   也早在帧首扫描的检查点之前就结束了 ⇒ **一帧都播不了种**。
 *   （2026-10-05 逻辑分析仪实测：PIO 版 475 ms / 58 帧内 SIg 恒为 0，无任何播种脉冲。）
 *
 * 现在：以 PWM 计数器为准，把请求脚安排在**下一个扫描周期起点**（= 新帧第一次扫描，
 * 因为 DMA 完成中断必定落在本帧第 43 次扫描内）之后的 PIO_SEED_RAISE_US 处拉高，
 * 保持 PIO_SEED_HOLD_US（≫ 检查点偏移，且 < 扫描周期 + 检查点偏移），
 * 使请求脚**恰好跨过帧首扫描的那一次检查**：
 *   · 早于上一次检查（第 43 次扫描）结束 ⇒ 不会播到错误扫描；
 *   · 晚于下一次检查（帧首 + 189 µs）开始 ⇒ 不会重复播种。
 * 容差：两侧各 ≈±60 µs（闹钟用 1 MHz 计时器，抖动 µs 级）——见 tests/test_pio_seed_timing.cpp。 */

void __not_in_flash_func(Rp2040Platform::cancelSeedRequest)()
{
    if (_seedRaiseAlarm >= 0) {
        cancel_alarm(_seedRaiseAlarm);
        _seedRaiseAlarm = -1;
    }
    if (_seedLowerAlarm >= 0) {
        cancel_alarm(_seedLowerAlarm);
        _seedLowerAlarm = -1;
    }
}

void __not_in_flash_func(Rp2040Platform::onFrameDmaDone)()
{
#if VFD_DEBUG_DIAG
    const uint32_t irqT0 = time_us_32(); /* 诊断：ISR 忙时累计起点（VFD_DEBUG_DIAG） */
#endif
    dma_hw->ints0 = 1u << static_cast<uint>(_cfg.dma_ch); /* 清本通道完成标志 */

    /* (1) 帧边界：切到最新发布的帧缓冲并**先**重启整帧搬运。
     *     必须先做：下面要忙等播种窗口，不能让 SM 在这期间断流。
     *     此刻 FIFO 里还剩最后几个字，新数据会紧接其后 —— 不会打断正在移出的扫描。 */
    _active = _published ? _published : reinterpret_cast<const uint8_t *>(_blankFrame);
    _frameStarts++;
    _scans += SCANS_PER_FRAME;

    dma_channel_configure(static_cast<uint>(_cfg.dma_ch), &_dmaConfig,
        &_cfg.pio->txf[_cfg.sm], _active, FRAME_WORDS, true);

    /* (2) 帧首播种：**只在消隐窗口里动手**（2026-10-08 依据实机"低亮度画面出错"定位）。
     *
     * 为什么**不能**在点亮窗口里做：点亮窗口的长度**就是亮度** —— 低亮度时可短到 1 µs，
     * 而 5 个 CLKg 播种脉冲要 ≈15 µs。若从点亮起点开始播，脉冲必然越过"点亮结束"这条边沿，
     * 而那条边沿同时就是**帧首扫描的边界 ①②③④** ⇒ CPU 直接翻引脚的 CLKg 与 PIO 的边界抢写
     * ⇒ 栅极前进次数错乱 ⇒ 画面内容错位。
     * （实机症状：亮度越低越容易出错；全亮时点亮 ≈89 µs，15 µs 脉冲塞得下，所以高亮度看不出来。）
     *
     * 消隐窗口里播种是安全的：
     *   · 本扫描的边界 ①②③④ 刚刚过去（消隐起点 = 边界时刻），不会与之抢 CLKg；
     *   · 消隐长度 = 189 µs − 点亮长度 ≥ 100 µs（全亮时最短）⇒ 5 个脉冲绰绰有余，
     *     而且**永远**在帧首边界之前；
     *   · 播种全程 LAT 保持低（见 seedGrid()）⇒ 屏幕上看不到这次链搬运。
     *
     * DMA 完成中断本来就发生在最后一次扫描的**数据阶段**（正在消隐）⇒ 绝大多数情况直接播即可；
     * 万一中断延迟到点亮窗口才进来，就等点亮结束（下一条 BK 上升沿）、越过边界四步再播：
     * 代价是这一帧的图案整体错一格，但绝不会与边界抢写（宁错位、不脏画面）。 */
    /* 播种是 CPU 用 pio_sm_set_pins_with_mask() **强制 SM 执行** `set pins` 完成的，而那会
     * 顶掉 SM 本该执行的那一条指令（pio_sm_exec 语义：注入的指令取代 PC 处的那条）。
     * 若 SM 正在数据循环里（out pins,1 / nop side 1），被顶掉的可能是某个 `out`
     * ⇒ 阳极数据少移一位 ⇒ 该扫描送出的数据被破坏。
     * （实机症状：把播种改到消隐窗口里"立刻播"之后，**最右 3 列上半部分闪烁** ——
     *   那条被破坏的扫描数据锁存后正好落在帧首 T43 的最右 3 列上。）
     *
     * 扫描周期里 SM 大部分时间停在 `wait` 上：PC==0 在等点亮起点、PC==1 在等消隐起点；
     * 只有约 89 µs 的数据阶段是"忙"的。等它回到 wait 再播，就绝不会破坏移位数据。
     * 这个窗口同时满足位置要求：SM 停在 `wait 0` 说明它已完成本扫描移位、正等下一次点亮，
     * 也就是"最后一次扫描的边界之后、帧首扫描的边界之前"；消隐 ≥100 µs，5 个脉冲绰绰有余，
     * 而且与亮度无关。 */
    {
        /* ⚠️ pio_sm_get_pc() 返回的是**含程序偏移**的地址（与 ssd1306_slave_rp2040.cpp 里
         * 同一处理：要减掉 _pioOffset 才是程序内的 PC）。漏减会让条件恒真 ⇒ 每次帧中断都空转
         * 满整个自旋上限（≈2 ms），把 SM 饿死、扫描停摆（实机症状：上电无显示、串口无输出）。 */
        uint32_t spins = 0;
        const uint32_t base = static_cast<uint32_t>(_pioOffset);
        while ((pio_sm_get_pc(_cfg.pio, _cfg.sm) - base) > 1u && ++spins < 20000u) {
        }
    }
    seedGrid();
#if VFD_DEBUG_DIAG
    _irqUs += time_us_32() - irqT0; /* 诊断：累计本次帧完成中断的忙时 */
#endif
}

void Rp2040Platform::service()
{
    /* PIO 引擎不需要清 RX FIFO（只有 SPI 才有），留空 */
}

/* ------------------------------------------------------------------ 诊断 */

const char *Rp2040Platform::engineName() const
{
    return "pio(SM+DMA)";
}

uint32_t Rp2040Platform::clockHz() const
{
    return _pioClkHz;
}

uint32_t Rp2040Platform::guardMarginUs() const
{
    return _guardMarginUs;
}

bool Rp2040Platform::engineStalled() const
{
    /* FDEBUG 的 TXSTALL 位：SM 因为等 TX FIFO 数据而卡住
     * （DMA 断流 / CPU 长时间关中断时的直观指示） */
    const uint32_t bit = 1u << (PIO_FDEBUG_TXSTALL_LSB + _cfg.sm);
    return (_cfg.pio->fdebug & bit) != 0;
}

} /* namespace vfd */
