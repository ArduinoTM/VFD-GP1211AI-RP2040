/*
 * ssd1306_pio_wire.h —— SSD1306 从机 PIO 的"线上格式"约定（纯逻辑，可宿主机测试）
 *
 * 这个头文件只放两件最容易搞错、又完全可以在 PC 上验证的事：
 *
 *   ① WAIT 指令等的到底是哪个 GPIO（wait gpio 与 wait pin 的区别）
 *   ② RX FIFO 里 9 位字的数据位布局（in pins,N 的位序）
 *
 * 两件事都曾经按错误的假设实现过，导致 2026-10-06 现场"一个字节都收不到"。下面是依据，
 * 改代码前请先读完（完整排故见 docs/09-从机收不到字节-根因与修复.md）：
 *
 * ① WAIT
 *    pico-sdk hardware/pio_instructions.h 明确写着：
 *      pio_encode_wait_gpio(polarity, gpio)  —— "The real GPIO number 0-31"
 *      pio_encode_wait_pin (polarity, pin)   —— "The pin number 0-31 **relative to the
 *                                               executing SM's input pin mapping**"
 *    即 `wait pin N` 等的是 GPIO (PINCTRL.IN_BASE + N) mod 32，**不是**绝对 GPIO N。
 *    本从机 IN_BASE = MOSI = GP12（in pins 需要），所以 `wait 1 pin 1` 实际等的是
 *    GP13 = DC（命令字节期间恒为低）⇒ 状态机永远停在第二条指令，一个字节都收不到。
 *    结论：**本程序的所有 wait 必须用 `wait gpio`（绝对引脚号）**。
 *
 * ② 9 位字布局
 *    数据手册 3.4.4 IN：`IN` 永远取源数据的**最低** N 位；IN_BASE=5 时 `IN 3,PINS`
 *    读的是引脚 5、6、7，**最低位引脚在最低位**，且"输入数据的数据位顺序与移位方向无关"。
 *    因此本从机第 8 位执行 `in pins,2`（IN_BASE=MOSI）时，2 位数据是
 *        bit0 = MOSI 的第 8 位（= byte 的 LSB）   bit1 = DC
 *    再叠加前面 7 条 `in pins,1`（b7..b1，左移，PUSH 会清空 ISR）后，每个 push 出去的字
 *    低 9 位是：
 *
 *        bit:  8   7   6   5   4   3   2   1   0
 *              b7  b6  b5  b4  b3  b2  b1  DC  b0
 *
 *    即 `word = ((byte >> 1) << 2) | (dc << 1) | (byte & 1)`。
 *    （早期代码假设的是 `(byte << 1) | dc`，解码会得到 value = (byte >> 1) | (dc << 7)、
 *      dc = byte 的 LSB —— 全错。见 unpackRxWord()。）
 *
 * ⚠️ 另一条同源坑：RP2040 的 PIO 指令内存（INSTR_MEM0..31）是**只写**的（RP2040.svd：
 *    PIO_INSTR_MEM0_ACCESS "WO"），读回恒为 0。想"回读校验装载结果"只能查软件镜像，
 *    见 Ssd1306SpiSlave::debugLoadedWord()。
 *
 * 本文件不依赖 pico-sdk，因此 tests/test_ssd1306_pio.cpp 可以直接把"数据手册模型"跑一遍。
 */
#ifndef VFD_SSD1306_PIO_WIRE_H
#define VFD_SSD1306_PIO_WIRE_H

#include <stdbool.h>
#include <stdint.h>

namespace vfd {

/* ============================================================================
 * ① WAIT 指令
 * ==========================================================================*/

/* 指令编码（RP2040 数据手册 3.4）：15:13 操作码，12:8 延时/侧集，7:0 参数。
 * WAIT 的参数段：bit7 极性，bit6:5 源，bit4:0 索引。 */
constexpr uint16_t PIO_WAIT_OPCODE = 0x1u;   /* bits 15:13 = 001 */
constexpr uint16_t PIO_WAIT_SRC_GPIO = 0x0u; /* 索引 = 绝对 GPIO 号（0..31） */
constexpr uint16_t PIO_WAIT_SRC_PIN = 0x1u;  /* 索引相对 PINCTRL.IN_BASE */
constexpr uint16_t PIO_WAIT_SRC_IRQ = 0x2u;

/* 按"源 + 极性 + 索引"拼出 WAIT 指令字（等价于 pico-sdk 的 pio_encode_wait_*） */
inline constexpr uint16_t waitEncode(uint16_t source, bool polarity, uint16_t index)
{
    return static_cast<uint16_t>((PIO_WAIT_OPCODE << 13)
        | (((polarity ? 4u : 0u) | (source & 0x3u)) << 5) | (index & 0x1Fu));
}

/* wait <polarity> gpio <gpio>：等的是绝对 GPIO（本程序必须用这个） */
inline constexpr uint16_t waitGpioEncode(bool polarity, uint8_t gpio)
{
    return waitEncode(PIO_WAIT_SRC_GPIO, polarity, static_cast<uint16_t>(gpio));
}

/* wait <polarity> pin <pin>：等的是 GPIO (inBase + pin) mod 32（曾经的 bug 来源） */
inline constexpr uint16_t waitPinEncode(bool polarity, uint8_t pin)
{
    return waitEncode(PIO_WAIT_SRC_PIN, polarity, static_cast<uint16_t>(pin));
}

struct PioWaitInfo {
    bool isWait;     /* 该指令字是不是 WAIT */
    uint16_t source; /* PIO_WAIT_SRC_* */
    bool polarity;   /* true = 等"高" */
    uint8_t index;   /* 指令里的原始索引 */
    uint8_t pin;     /* **实际等待的绝对 GPIO**（GPIO/PIN 源；IRQ 源为 0xFF） */
};

/* 解出一条指令；inBase = PINCTRL.IN_BASE（只有 wait pin 需要它） */
inline PioWaitInfo decodeWait(uint16_t word, uint8_t inBase)
{
    PioWaitInfo w;
    w.isWait = ((word >> 13) & 0x7u) == PIO_WAIT_OPCODE;
    w.source = static_cast<uint16_t>((word >> 5) & 0x3u);
    w.polarity = ((word >> 7) & 0x1u) != 0;
    w.index = static_cast<uint8_t>(word & 0x1Fu);
    if (!w.isWait || w.source == PIO_WAIT_SRC_IRQ)
        w.pin = 0xFFu;
    else if (w.source == PIO_WAIT_SRC_GPIO)
        w.pin = w.index; /* 绝对引脚号 */
    else
        w.pin = static_cast<uint8_t>((inBase + w.index) & 0x1Fu); /* 相对 IN 基址 */
    return w;
}

/* ============================================================================
 * ② RX FIFO 的 9 位字（见文件头注释的位图）
 * ==========================================================================*/

/* 有效位宽度 */
constexpr uint32_t SSD1306_PIO_WORD_BITS = 9u;
constexpr uint32_t SSD1306_PIO_WORD_MASK = (1u << SSD1306_PIO_WORD_BITS) - 1u;

/* 硬件产出的字 → (byte, dc) */
inline void unpackRxWord(uint32_t word, uint8_t &value, bool &dc)
{
    value = static_cast<uint8_t>((((word >> 2) & 0x7Fu) << 1) | (word & 0x1u));
    dc = ((word >> 1) & 0x1u) != 0;
}

/* (byte, dc) → 硬件产出的字（宿主机测试/文档用；与 unpackRxWord 互逆） */
inline uint32_t packRxWord(uint8_t value, bool dc)
{
    return (static_cast<uint32_t>(value & 0x1u)
        | (static_cast<uint32_t>(dc ? 1u : 0u) << 1)
        | (static_cast<uint32_t>(value >> 1) << 2));
}

/* ============================================================================
 * ③ 从机 PIO 程序的引脚补丁与自检
 *    纯逻辑，ssd1306_slave_rp2040.cpp 与宿主机测试（tests/test_ssd1306_pio.cpp）共用，
 *    因此"测试跑的东西"和"上机跑的东西"是同一份代码。
 * ==========================================================================*/

/* ssd1306_spi_slave.pio 里的占位引脚号（运行期改写为实际 CS / SCLK） */
constexpr uint8_t SSD1306_PIO_PLACEHOLDER_CS = 0;
constexpr uint8_t SSD1306_PIO_PLACEHOLDER_SCK = 1;

/* 把占位值换成实际引脚；返回改写的指令条数（本程序应为 5） */
inline uint32_t patchSsd1306WaitPins(uint16_t *words, uint32_t length, uint8_t pinCs, uint8_t pinSck)
{
    const uint16_t csPh = waitGpioEncode(false, SSD1306_PIO_PLACEHOLDER_CS);
    const uint16_t sckHighPh = waitGpioEncode(true, SSD1306_PIO_PLACEHOLDER_SCK);
    const uint16_t sckLowPh = waitGpioEncode(false, SSD1306_PIO_PLACEHOLDER_SCK);
    const uint16_t csReal = waitGpioEncode(false, pinCs);
    const uint16_t sckHighReal = waitGpioEncode(true, pinSck);
    const uint16_t sckLowReal = waitGpioEncode(false, pinSck);

    uint32_t patched = 0;
    for (uint32_t i = 0; i < length; ++i) {
        if (words[i] == csPh) {
            words[i] = csReal;
            patched++;
        } else if (words[i] == sckHighPh) {
            words[i] = sckHighReal;
            patched++;
        } else if (words[i] == sckLowPh) {
            words[i] = sckLowReal;
            patched++;
        }
    }
    return patched;
}

struct Ssd1306WaitStats {
    int csLow;       /* 等 CS 低（事务开始） */
    int sckHigh;     /* 等 SCLK 高（采样时刻） */
    int sckLow;      /* 等 SCLK 低 */
    int relativePin; /* ⚠️ wait pin（相对 IN_BASE）—— 必须为 0 */
    int other;       /* 其它 wait（IRQ 源 / 别的引脚） */
};

/* 逐条解出程序里的 wait，统计"等谁、等几条" */
inline Ssd1306WaitStats inspectSsd1306WaitPins(const uint16_t *words, uint32_t length,
    uint8_t pinCs, uint8_t pinSck)
{
    Ssd1306WaitStats s = { 0, 0, 0, 0, 0 };
    for (uint32_t i = 0; i < length; ++i) {
        const PioWaitInfo w = decodeWait(words[i], 0 /* GPIO 源不看 IN_BASE */);
        if (!w.isWait)
            continue;
        if (w.source == PIO_WAIT_SRC_PIN)
            s.relativePin++;
        else if (w.source != PIO_WAIT_SRC_GPIO)
            s.other++;
        else if (w.pin == pinCs && !w.polarity)
            s.csLow++;
        else if (w.pin == pinSck && w.polarity)
            s.sckHigh++;
        else if (w.pin == pinSck && !w.polarity)
            s.sckLow++;
        else
            s.other++;
    }
    return s;
}

/* 约定：1 条等 CS 低 + 2 条等 SCLK 高 + 2 条等 SCLK 低，且一条 wait pin 都不许有 */
inline bool ssd1306WaitPinsOk(const Ssd1306WaitStats &s)
{
    return s.relativePin == 0 && s.other == 0 && s.csLow == 1 && s.sckHigh == 2 && s.sckLow == 2;
}

/* ============================================================================
 * ④ 环形 DMA 的**精确**累计计数（不要再用"写地址 mod N 差分"）
 *
 * DMA 用**有限计数**搬运：一圈 = 环形字数 N，搬满一圈 BUSY 清零、计数回到 N，
 * CPU 在 task() 里续装并把 laps++。于是
 *
 *      累计写入 = laps * N + (N - remaining)
 *
 * 是单调、不受回绕影响的量（uint32 自然回绕，做减法永远安全）。
 *
 * 反例（曾经的 bug）：用"写地址 - 缓冲基址"再 &(N-1) 做差分。DMA 只要在两次轮询之间
 * 跑满一整圈，差分就是 0 ⇒ **这一整圈的 N 个字被静默丢弃**。现场表现：主机一次 CS 内
 * 连写整屏（水平寻址 1024 B）时，从机只收到 256 B，GDDRAM 有 3/4 没更新
 * （见 docs/10-大突发丢字节-环形缓冲计数根因与修复.md）。
 * ==========================================================================*/

inline uint32_t ringWrittenTotal(uint32_t laps, uint32_t ringWords, uint32_t remaining)
{
    return laps * ringWords + (ringWords - remaining);
}

/* **连续搬运**（大而有限的计数，DMA 绝不停在半路）下的累计写入量：
 * base 是历次"总线空闲时补计数"累加出来的基数，initialCount 是补计数时装载的值。
 * 与"写地址 mod N 差分"相比，它不会因为 DMA 跑满整圈而丢掉一圈（docs/10）。 */
inline uint32_t ringWrittenFromBase(uint32_t base, uint32_t initialCount, uint32_t remaining)
{
    return base + (initialCount - remaining);
}

/* 已写入但未取走（模 2^32 减法；只要未取走量远小于 2^31 就永远正确） */
inline int32_t ringPending(uint32_t writtenTotal, uint32_t poppedTotal)
{
    return static_cast<int32_t>(writtenTotal - poppedTotal);
}

} /* namespace vfd */

#endif /* VFD_SSD1306_PIO_WIRE_H */
