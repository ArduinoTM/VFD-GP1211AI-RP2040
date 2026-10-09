#include "ssd1306_slave_rp2040.h"

#include <string.h>

#include "hardware/gpio.h"
#include "ssd1306_pio_wire.h"
#include "ssd1306_spi_slave.pio.h"

namespace vfd {

namespace {
constexpr uint32_t RING_BITS = 14; /* 4096 字 = 16 KB 的**字节**指数（channel_config_set_ring 要的是字节数） */
static_assert((1u << RING_BITS) == SSD1306_SLAVE_RING_WORDS * 4u, "环形缓冲与 RING_BITS 不一致");
constexpr uint32_t RING_MASK = SSD1306_SLAVE_RING_WORDS - 1u;

/* DMA 起始计数：**大而有限**（0xFFFFFFFF 字 ≈ 4.29e9 字；4 MHz 满速约 2.4 小时）。
 *
 * ⚠️ 这里**不能**改成"一圈 = 4096 字的有限计数、搬满就停下等 CPU 续装"：DMA 一停，
 * PIO RX FIFO（8 字）在 ~16 µs 内就顶满，状态机随即卡在 `push` 上**丢时钟沿**
 * ⇒ 之后整条流的**位相位**永久错位（未知命令开始增长、画面错乱）。
 * 2026-10-06 第二轮现场日志就是每圈停一次 ⇒ 每圈丢一次位：
 *   `slave-dbg: … dma_left=4096 … laps=0/1/2…` + `stall=1` + 未知命令持续增长。
 *
 * 现在的取舍：**DMA 连续搬运（绝不停在半路）**，而计数精度由"起始计数 − remaining" + `_base`
 * 保证（见 updatePending()）；计数快用完时只在**总线空闲**（无在途数据）时补满（见 task()）。 */
constexpr uint32_t DMA_COUNT_FULL = 0xFFFFFFFFu;
constexpr uint32_t DMA_COUNT_TOPUP = 0x40000000u;

/* PINCTRL / EXECCTRL 里两个引脚字段的位置（RP2040 数据手册 3.7） */
constexpr uint32_t PINCTRL_IN_BASE_LSB = 15;
constexpr uint32_t EXECCTRL_JMP_PIN_LSB = 24;
} // namespace

/* 环形缓冲的定义（见头文件注释：16 KB 对齐不能放在对象里，否则 sizeof 被撑到 48 KB） */
alignas(SSD1306_SLAVE_RING_WORDS * 4) uint32_t Ssd1306SpiSlave::_ring[SSD1306_SLAVE_RING_WORDS];

Ssd1306SpiSlaveConfig defaultSsd1306SpiSlaveConfig()
{
    Ssd1306SpiSlaveConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.pin_sck = VFD_EMU_PIN_SCK;
    cfg.pin_mosi = VFD_EMU_PIN_MOSI;
    cfg.pin_dc = VFD_EMU_PIN_DC; /* 必须 = pin_mosi + 1 */
    cfg.pin_cs = VFD_EMU_PIN_CS;
    cfg.pin_reset = VFD_EMU_PIN_RESET; /* 0xFF = 不接 RESET */
    cfg.pio = VFD_EMU_PIO_INST ? pio1 : pio0;
    cfg.sm = VFD_EMU_PIO_SM;
    cfg.dma_ch = VFD_EMU_DMA_CH;
    return cfg;
}

Ssd1306SpiSlave::Ssd1306SpiSlave(const Ssd1306SpiSlaveConfig &cfg)
    : _cfg(cfg)
    , _offset(-1)
    , _wordCount(0)
    , _dmaConfig(dma_channel_get_default_config(static_cast<uint>(cfg.dma_ch < 0 ? 0 : cfg.dma_ch)))
    , _popped(0)
    , _laps(0)
    , _pending(0)
    , _received(0)
    , _dropped(0)
    , _overruns(0)
    , _resetEvents(0)
    , _resetAsserted(false)
    , _pendingResetEvent(false)
    , _running(false)
    , _error("")
{
    memset(_words, 0, sizeof(_words));
    memset(_ring, 0, sizeof(_ring));
}

Ssd1306SpiSlave::~Ssd1306SpiSlave()
{
    if (_running) {
        pio_sm_set_enabled(_cfg.pio, _cfg.sm, false);
        dma_channel_abort(static_cast<uint>(_cfg.dma_ch));
    }
    if (_offset >= 0) {
        pio_program_t prog = ssd1306_spi_slave_program;
        pio_remove_program(_cfg.pio, &prog, static_cast<uint>(_offset));
        _offset = -1;
    }
}

/* 把 .pio 里的占位引脚号（CS=0 / SCLK=1）换成实际引脚。
 *
 * ⚠️ 必须用 `wait gpio`（**绝对**引脚号）：`wait pin N` 等的是
 * GPIO (PINCTRL.IN_BASE + N) mod 32（pico-sdk 原文 "relative to the executing SM's
 * input pin mapping"），本 SM 的 IN_BASE = MOSI(GP12)，写成 `wait pin` 就会去等
 * GP13(=DC，命令字节期间恒低) / GP26(悬空) ⇒ 状态机永远停在第二条 wait 上、
 * 一个字节都收不到（2026-10-06 现场故障的根因，见 docs/09）。
 * patchProgramPins() 只负责改写，检查交给 verifyWaitPins()。 */
void Ssd1306SpiSlave::patchProgramPins(uint16_t *words, uint length)
{
    /* 实现在 ssd1306_pio_wire.h（纯逻辑，宿主机测试跑的就是这一份） */
    patchSsd1306WaitPins(words, length, _cfg.pin_cs, _cfg.pin_sck);
}

/* 装载后自检（"回读校验"的可行形式：RP2040 的 INSTR_MEM 只写，读回恒 0，只能查镜像）：
 *   ① 一条 wait pin 都不许有（相对 IN 基址 ⇒ 等的脚不对）
 *   ② 恰好 1 条"等 CS 低"、2 条"等 SCLK 高"、2 条"等 SCLK 低"，且不再有别的 wait */
bool Ssd1306SpiSlave::verifyWaitPins(const uint16_t *words, uint length)
{
    const Ssd1306WaitStats st = inspectSsd1306WaitPins(words, length, _cfg.pin_cs, _cfg.pin_sck);
    if (st.relativePin != 0) {
        _error = "PIO 程序含 wait pin（相对 IN 基址）：必须全部改成 wait gpio（绝对引脚号）";
        return false;
    }
    if (!ssd1306WaitPinsOk(st)) {
        _error = "PIO 程序的 wait 指令与引脚约定不一致（装载自检失败）";
        return false;
    }
    return true;
}

void __not_in_flash_func(Ssd1306SpiSlave::updatePending)()
{
    /* ---- 精确的"累计写入字数"----
     *        累计写入 = _base + (DMA_COUNT_FULL - remaining)
     * remaining 只在 DMA 搬运时递减，所以上式在两次补计数之间是**单调**的；
     * 补计数只发生在"无在途数据"时（见 task()），因此累计值不会跳变。
     * 与"写地址 - 缓冲基址 &(N-1)"相比，它**不会**因为 DMA 跑满整圈而丢掉一圈
     * （旧写法：差分 = 0 ⇒ 整圈静默丢弃，见 docs/10）。
     * 纯逻辑实现见 ssd1306_pio_wire.h 的 ringWrittenFromBase()（宿主机有回归）。 */
    const uint32_t remaining = dma_hw->ch[static_cast<uint>(_cfg.dma_ch)].transfer_count;
    const uint32_t written = ringWrittenFromBase(_base, DMA_COUNT_FULL, remaining);
    _pending = ringPending(written, _popped);
    _laps = written / SSD1306_SLAVE_RING_WORDS; /* 诊断用：累计搬了多少整圈 */

    /* 安全网：真被覆盖（CPU 落后超过一整圈）→ 计数并丢弃最旧的未读数据、重新对齐。
     * 修好计数后这个分支才真正可靠：正常收流时不应出现。 */
    const int32_t cap = static_cast<int32_t>(SSD1306_SLAVE_RING_WORDS - 2);
    if (_pending > cap) {
        _overruns++;
        _dropped += static_cast<uint32_t>(_pending - cap);
        _popped = written - static_cast<uint32_t>(cap);
        _pending = cap;
    }
}

bool Ssd1306SpiSlave::begin()
{
    _error = "";

    if (_cfg.pin_dc != static_cast<uint8_t>(_cfg.pin_mosi + 1)) {
        _error = "DC 必须接在 MOSI+1（PIO 用 in pins,2 同时采这两位）";
        return false;
    }
    if (_cfg.pin_sck > 29 || _cfg.pin_mosi > 29 || _cfg.pin_dc > 29 || _cfg.pin_cs > 29
        || (_cfg.pin_reset != 0xFF && _cfg.pin_reset > 29)) {
        _error = "引脚号超出 GPIO 范围";
        return false;
    }
    if (_cfg.dma_ch < 0 || _cfg.dma_ch > 11) {
        _error = "DMA 通道号非法";
        return false;
    }
    /* WAIT 引脚用的是 5 位索引：wait gpio 只能寻址 GPIO 0..31 */
    if (_cfg.pin_sck > 31 || _cfg.pin_cs > 31) {
        _error = "wait gpio 只支持 GPIO 0..31";
        return false;
    }

    /* ---- 1. 引脚：SCLK/MOSI/DC/CS/RESET 都是输入 ---- */
    const uint8_t inPins[] = { _cfg.pin_sck, _cfg.pin_mosi, _cfg.pin_dc, _cfg.pin_cs };
    for (uint8_t pin : inPins) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_IN);
        gpio_set_input_enabled(pin, true);
    }
    gpio_pull_up(_cfg.pin_cs); /* 主机未接/悬空时保持"未选中" */
    if (_cfg.pin_reset != 0xFF) {
        gpio_init(_cfg.pin_reset);
        gpio_set_dir(_cfg.pin_reset, GPIO_IN);
        gpio_set_input_enabled(_cfg.pin_reset, true);
        gpio_pull_up(_cfg.pin_reset); /* 不接时读高（不复位） */
    }

    /* ---- 2. 装载 PIO 程序（替换占位引脚号 + 装载后自检）---- */
    /* 先自检"本文件用的纯逻辑编码 == pico-sdk 的编码"，防位域理解偏差 */
    if (waitGpioEncode(false, 3) != static_cast<uint16_t>(pio_encode_wait_gpio(false, 3))
        || waitPinEncode(true, 5) != static_cast<uint16_t>(pio_encode_wait_pin(true, 5))) {
        _error = "WAIT 指令编码自检失败（ssd1306_pio_wire.h 与 pico-sdk 不一致）";
        return false;
    }

    const uint length = ssd1306_spi_slave_program.length;
    if (length == 0 || length > 32) {
        _error = "PIO 程序长度异常";
        return false;
    }
    uint16_t words[32];
    memset(words, 0, sizeof(words));
    memcpy(words, ssd1306_spi_slave_program_instructions, length * sizeof(uint16_t));
    patchProgramPins(words, length);
    if (!verifyWaitPins(words, length))
        return false;

    pio_program_t prog = ssd1306_spi_slave_program;
    prog.instructions = words;
    if (!pio_can_add_program(_cfg.pio, &prog)) {
        _error = "PIO 指令空间不足（扫描引擎与模拟器需用不同 PIO 块）";
        return false;
    }
    _offset = static_cast<int>(pio_add_program(_cfg.pio, &prog));
    /* 留一份装载镜像：RP2040 的 INSTR_MEM 是只写（WO）的，读回恒为 0，诊断只能看这份 */
    memcpy(_words, words, length * sizeof(uint16_t));
    _wordCount = length;

    /* ---- 3. 状态机：in_base=MOSI、jmp_pin=CS、左移(MSB first)、关自动推送 ---- */
    pio_sm_config c = ssd1306_spi_slave_program_get_default_config(static_cast<uint>(_offset));
    sm_config_set_in_pins(&c, _cfg.pin_mosi);
    sm_config_set_jmp_pin(&c, _cfg.pin_cs);
    sm_config_set_in_shift(&c, false /*shift_right=false → 左移*/, false /*autopush off*/,
        32 /*push_threshold 由显式 push 控制*/);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX); /* RX FIFO 加深到 8，给 DMA 更多余量 */
    sm_config_set_clkdiv(&c, 1.0f);                /* 125 MHz：每个 SCLK 边沿都能跟上 */
    pio_sm_init(_cfg.pio, _cfg.sm, static_cast<uint>(_offset), &c);

    /* ---- 4. DMA：PIO RX FIFO → 环形缓冲（32bit，环形写地址，无限计数）---- */
    dma_channel_config dc = dma_channel_get_default_config(static_cast<uint>(_cfg.dma_ch));
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, true);
    channel_config_set_ring(&dc, true /*写地址环形回绕*/, RING_BITS);
    channel_config_set_dreq(&dc, pio_get_dreq(_cfg.pio, _cfg.sm, false /*RX*/));
    _dmaConfig = dc;

    dma_channel_abort(static_cast<uint>(_cfg.dma_ch));
    /* **连续搬运**（大而有限的计数，见 DMA_COUNT_FULL 注释）：DMA 绝不停在半路，
     * 否则 PIO FIFO 顶满 → SM 卡在 push 上丢时钟沿 → 位相位永久错位（第二轮现场）。 */
    dma_channel_configure(static_cast<uint>(_cfg.dma_ch), &_dmaConfig,
        _ring, &_cfg.pio->rxf[_cfg.sm], DMA_COUNT_FULL, false);

    /* ---- 5. 状态复位并启动 ---- */
    _popped = 0;
    _base = 0;
    _laps = 0;
    _pending = 0;
    _received = 0;
    _dropped = 0;
    _overruns = 0;
    _resetAsserted = false;
    _pendingResetEvent = false;

    pio_sm_clear_fifos(_cfg.pio, _cfg.sm);
    pio_sm_exec(_cfg.pio, _cfg.sm, pio_encode_jmp(static_cast<uint>(_offset)));
    /* 先让 DMA 就绪（填满计数并触发），再放状态机，避免第一个 push 无处可去 */
    dma_channel_configure(static_cast<uint>(_cfg.dma_ch), &_dmaConfig,
        _ring, &_cfg.pio->rxf[_cfg.sm], DMA_COUNT_FULL, true);
    pio_sm_set_enabled(_cfg.pio, _cfg.sm, true);

    _running = true;
    return true;
}

void Ssd1306SpiSlave::resetStream()
{
    pio_sm_set_enabled(_cfg.pio, _cfg.sm, false);
    dma_channel_abort(static_cast<uint>(_cfg.dma_ch));

    _popped = 0;
    _base = 0;
    _laps = 0;
    _pending = 0;
    memset(_ring, 0, sizeof(_ring));

    pio_sm_clear_fifos(_cfg.pio, _cfg.sm);
    pio_sm_exec(_cfg.pio, _cfg.sm, pio_encode_jmp(static_cast<uint>(_offset)));
    dma_channel_configure(static_cast<uint>(_cfg.dma_ch), &_dmaConfig,
        _ring, &_cfg.pio->rxf[_cfg.sm], DMA_COUNT_FULL, true);
    pio_sm_set_enabled(_cfg.pio, _cfg.sm, true);
}

bool __not_in_flash_func(Ssd1306SpiSlave::popByte)(uint8_t &value, bool &dc)
{
    if (!_running)
        return false;

    updatePending();
    if (_pending <= 0)
        return false;

    const uint32_t word = _ring[_popped & RING_MASK];
    _popped++;
    _pending--;
    _received++;

    /* 9 位字布局由 PIO 的 in pins 语义决定，见 ssd1306_pio_wire.h 与 docs/09：
     *   bit0 = byte 的 LSB，bit1 = DC，bit8..2 = byte 的 bit7..1 */
    unpackRxWord(word, value, dc);
    return true;
}

void __not_in_flash_func(Ssd1306SpiSlave::task)()
{
    if (!_running)
        return;

    updatePending();

    /* RESET 引脚（低有效）：由低变高的时刻通知应用复位模拟器状态 */
    if (_cfg.pin_reset != 0xFF) {
        const bool asserted = gpio_get(_cfg.pin_reset) == 0;
        if (asserted != _resetAsserted) {
            _resetAsserted = asserted;
            if (!asserted) { /* 上升沿：复位完成 */
                _resetEvents++;
                _pendingResetEvent = true;
                resetStream();
            }
        }
    }

    /* 计数快用完时补满 —— **只在总线空闲时做**（无在途数据：_pending==0 且 RX FIFO 空）。
     * 这样"起始计数 − remaining"的累计不会因为补计数而跳变；反之若在搬运中途补，
     * 读 remaining 与写计数之间发生的那几次搬运就会从累计里漏掉（丢字）。 */
    if (_pending == 0 && pio_sm_is_rx_fifo_empty(_cfg.pio, _cfg.sm)) {
        const uint32_t remaining = dma_hw->ch[static_cast<uint>(_cfg.dma_ch)].transfer_count;
        if (remaining < DMA_COUNT_TOPUP) {
            _base = ringWrittenFromBase(_base, DMA_COUNT_FULL, remaining);
            dma_channel_set_trans_count(static_cast<uint>(_cfg.dma_ch), DMA_COUNT_FULL, false);
            _overruns += 0; /* （补计数不丢数据；这里留空避免误读为丢包） */
        }
    }
}

bool Ssd1306SpiSlave::consumeResetEvent()
{
    const bool e = _pendingResetEvent;
    _pendingResetEvent = false;
    return e;
}

bool Ssd1306SpiSlave::stalled() const
{
    /* FDEBUG 的 RXSTALL 位（SM 因 RX FIFO 满而卡住） */
    const uint32_t bit = 1u << (PIO_FDEBUG_RXSTALL_LSB + _cfg.sm);
    return (_cfg.pio->fdebug & bit) != 0;
}

} /* namespace vfd */
namespace vfd {

/* ------------------------------------------------------------------ 诊断
 * 只读状态机与 DMA 的硬件状态，用来区分"PIO 没产出"与"DMA 没搬运"。
 * 程序共 15 条指令（见 ssd1306_spi_slave.pio），PC 减去程序起点即为下表索引。 */
int Ssd1306SpiSlave::debugPcOffset() const
{
    if (!_running)
        return -1;
    const int pc = static_cast<int>(pio_sm_get_pc(_cfg.pio, _cfg.sm)) - _offset;
    return (pc >= 0 && pc <= 14) ? pc : -1;
}

const char *Ssd1306SpiSlave::debugPcName() const
{
    static const char *const names[15] = {
        "wait_cs(等CS低)",   /* +0  */
        "set x,6",           /* +1  */
        "wait_sck_hi",       /* +2  */
        "in pins,1",         /* +3  */
        "wait_sck_lo",       /* +4  */
        "jmp CS->end",       /* +5  */
        "jmp x--",           /* +6  */
        "wait_sck_hi(bit8)", /* +7  */
        "in pins,2",         /* +8  */
        "wait_sck_lo",       /* +9  */
        "push",              /* +10 */
        "irq set 0(诊断)",    /* +11 */
        "jmp CS->end",       /* +12 */
        "jmp byte_loop",     /* +13 */
        "txn_end(清ISR)"     /* +14 */
    };
    const int pc = debugPcOffset();
    return (pc >= 0) ? names[pc] : "?";
}

int Ssd1306SpiSlave::debugRxFifo() const
{
    return _running ? static_cast<int>(pio_sm_get_rx_fifo_level(_cfg.pio, _cfg.sm)) : -1;
}

uint32_t Ssd1306SpiSlave::debugDmaWriteAddr() const
{
    return static_cast<uint32_t>(dma_channel_hw_addr(static_cast<uint>(_cfg.dma_ch))->write_addr);
}

uint32_t Ssd1306SpiSlave::debugDmaRemaining() const
{
    return static_cast<uint32_t>(dma_channel_hw_addr(static_cast<uint>(_cfg.dma_ch))->transfer_count);
}

int Ssd1306SpiSlave::debugDmaBusy() const
{
    return dma_channel_is_busy(static_cast<uint>(_cfg.dma_ch)) ? 1 : 0;
}

/* 装载镜像：**不是** INSTR_MEM 回读。RP2040 的 PIO INSTR_MEM 是只写（WO）的，
 * 读回恒为 0（RP2040.svd：PIO_INSTR_MEM0_ACCESS "WO"），所以旧版打印的 instr[] 全 0
 * 是"读法"问题、不是装载问题。这里返回的是真正写进指令内存、且已自检通过的那份字。 */
uint16_t Ssd1306SpiSlave::debugLoadedWord(int i) const
{
    if (i < 0 || static_cast<uint>(i) >= _wordCount)
        return 0;
    return _words[i];
}

uint32_t Ssd1306SpiSlave::debugLoadedWords() const
{
    return _wordCount;
}

uint32_t Ssd1306SpiSlave::debugPinCtrl() const
{
    return _running ? _cfg.pio->sm[_cfg.sm].pinctrl : 0;
}

uint8_t Ssd1306SpiSlave::debugInBase() const
{
    return static_cast<uint8_t>((debugPinCtrl() >> PINCTRL_IN_BASE_LSB) & 0x1Fu);
}

uint8_t Ssd1306SpiSlave::debugJmpPin() const
{
    return static_cast<uint8_t>((debugExecCtrl() >> EXECCTRL_JMP_PIN_LSB) & 0x1Fu);
}

uint32_t Ssd1306SpiSlave::debugExecCtrl() const
{
    return _running ? _cfg.pio->sm[_cfg.sm].execctrl : 0;
}

uint32_t Ssd1306SpiSlave::debugShiftCtrl() const
{
    return _running ? _cfg.pio->sm[_cfg.sm].shiftctrl : 0;
}

bool Ssd1306SpiSlave::debugTakeIrqFlag()
{
    if (!_running)
        return false;
    if (!pio_interrupt_get(_cfg.pio, 0))
        return false;
    pio_interrupt_clear(_cfg.pio, 0);
    return true;
}

} /* namespace vfd */
