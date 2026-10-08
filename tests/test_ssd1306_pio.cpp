/*
 * 宿主机测试：SSD1306 从机 PIO 的"线上格式"（src/ssd1306_pio_wire.h + 真实 .pio.h）
 *
 * 传输层（ssd1306_slave_rp2040.cpp）需要 Pico SDK，不能在 PC 上跑；但它最容易出错的两件事
 * 都是纯逻辑，而且正是 2026-10-06 "一个字节都收不到" 的现场故障点，所以单独拉出来测：
 *
 *   1. `wait gpio`（绝对引脚号）与 `wait pin`（相对 PINCTRL.IN_BASE）的区别；
 *   2. 真实生成头里的 5 条 wait 经引脚补丁后，等的确实是 CS / SCLK，且一条 wait pin 都没有；
 *   3. RX FIFO 里 9 位字的数据位布局：用"数据手册模型"（in pins 取源数据最低 N 位、
 *      位序与移位方向无关）逐位模拟一次 SPI 模式 0 的字节传输，再要求生产解码器还原出
 *      原始 byte + DC，全部 256×2 组合都要过。
 *
 * 这里跑的就是生产代码（ssd1306_pio_wire.h 的纯函数），不是复制品。
 */
#include <stdio.h>
#include <string.h>

#include "ssd1306_pio_wire.h"
#include "ssd1306_spi_slave.pio.h" /* pioasm 生成的真实指针（随仓库提供） */

/* ------------------------------------------------------------ 小测试框架 */

static int g_checks = 0;
static int g_failures = 0;

static void check(bool ok, const char *what, const char *detail = "")
{
    g_checks++;
    if (!ok) {
        g_failures++;
        printf("  [FAIL] %s %s\n", what, detail);
    }
}

static void section(const char *title)
{
    printf("\n=== %s ===\n", title);
}

/* ------------------------------------------- 数据手册模型（独立实现，用于验证解码器） */

/* in pins 的数据源：把 32 位 GPIO 快照按 IN_BASE 循环右移 ⇒ bit k = pin(IN_BASE + k)
 * （RP2040 数据手册 3.4.4：IN 取源数据最低 N 位，且位序与移位方向无关） */
static uint32_t modelInPins(uint32_t gpioLevels, uint8_t inBase)
{
    if (inBase == 0)
        return gpioLevels;
    return (gpioLevels << (32 - inBase)) | (gpioLevels >> inBase);
}

/* 按本从机的程序形态（7 × `in pins,1` + 1 × `in pins,2`，左移，每字节 push 一次）
 * 模拟一个字节，返回 PIO 会 push 出去的那个 32 位字。 */
static uint32_t modelRepeatByte(uint8_t byte, bool dc, uint8_t inBase, uint8_t pinMosi)
{
    const uint32_t dcBit = dc ? (1u << (pinMosi + 1)) : 0u;
    uint32_t isr = 0;
    for (int i = 0; i < 7; ++i) { /* b7..b1：SPI 模式 0，MSB first */
        const uint32_t mosi = ((byte >> (7 - i)) & 1u) << pinMosi;
        isr = (isr << 1) | (modelInPins(mosi | dcBit, inBase) & 0x1u);
    }
    const uint32_t mosi = (byte & 1u) << pinMosi; /* 第 8 位与 DC 一起采 */
    isr = (isr << 2) | (modelInPins(mosi | dcBit, inBase) & 0x3u);
    return isr; /* push（PUSH 清空 ISR）⇒ 该字的低 9 位就是本字节 */
}

/* ------------------------------------------------------------------ main */

int main()
{
    const uint8_t PIN_SCK = 11;
    const uint8_t PIN_MOSI = 12;
    const uint8_t PIN_DC = 13;
    const uint8_t PIN_CS = 14;

    printf("SSD1306 从机 PIO 线上格式测试（SCK=GP%u MOSI=GP%u DC=GP%u CS=GP%u）\n",
        PIN_SCK, PIN_MOSI, PIN_DC, PIN_CS);

    /* ---------------------------------------------------------------- §1 */
    section("§1 WAIT 指令语义：wait gpio 绝对 / wait pin 相对 IN_BASE");

    {
        const vfd::PioWaitInfo g = vfd::decodeWait(vfd::waitGpioEncode(true, PIN_SCK), PIN_MOSI);
        check(g.isWait, "wait gpio 被识别为 WAIT");
        check(g.source == vfd::PIO_WAIT_SRC_GPIO, "wait gpio 的源是 GPIO");
        check(g.polarity, "wait 1 gpio 极性是高");
        check(g.index == 11, "wait gpio 索引 = 指令里的 11");
        check(g.pin == 11, "wait gpio 11 实际等的就是 GP11（绝对）");

        /* 旧写法：wait 1 pin 1 —— 索引是相对量 */
        const vfd::PioWaitInfo p = vfd::decodeWait(vfd::waitPinEncode(true, 1), PIN_MOSI);
        check(p.source == vfd::PIO_WAIT_SRC_PIN, "wait pin 的源是 PIN");
        check(p.pin == 13, "wait 1 pin 1 且 IN_BASE=12 ⇒ 实际等 GP13(=DC) ★旧 bug");
        const vfd::PioWaitInfo p2 = vfd::decodeWait(vfd::waitPinEncode(false, PIN_CS), PIN_MOSI);
        check(p2.pin == 26, "wait 0 pin 14 且 IN_BASE=12 ⇒ 实际等 GP26(悬空) ★旧 bug");

        /* 旧的 5 条 wait 若是 wait pin，解出来是这些脚（现场故障签名） */
        check(vfd::decodeWait(vfd::waitPinEncode(true, PIN_SCK), PIN_MOSI).pin == 23,
            "wait 1 pin 11 且 IN_BASE=12 ⇒ 实际等 GP23（永远不高）★旧 bug");

        /* 与 pico-sdk 的编码字对齐（pio_encode_wait_* 的结果，见 ssd1306_slave_rp2040.cpp 自检） */
        check(vfd::waitGpioEncode(false, PIN_CS) == 0x200Eu, "wait 0 gpio 14 = 0x200E");
        check(vfd::waitGpioEncode(true, PIN_SCK) == 0x208Bu, "wait 1 gpio 11 = 0x208B");
        check(vfd::waitGpioEncode(false, PIN_SCK) == 0x200Bu, "wait 0 gpio 11 = 0x200B");
        check(vfd::waitPinEncode(true, 1) == 0x20A1u, "wait 1 pin 1 = 0x20A1（旧 .pio.h 的原值）");

        /* 解码/编码对整个 0..31 × 极性 × 源 互逆 */
        int rt = 0;
        for (uint16_t pin = 0; pin < 32; ++pin) {
            for (int pol = 0; pol < 2; ++pol) {
                const vfd::PioWaitInfo a = vfd::decodeWait(vfd::waitGpioEncode(pol != 0, (uint8_t)pin), 5);
                const vfd::PioWaitInfo b = vfd::decodeWait(vfd::waitPinEncode(pol != 0, (uint8_t)pin), 5);
                if (a.source == vfd::PIO_WAIT_SRC_GPIO && a.pin == pin && a.polarity == (pol != 0)
                    && b.source == vfd::PIO_WAIT_SRC_PIN && b.pin == ((5 + pin) & 0x1F)
                    && b.polarity == (pol != 0))
                    rt++;
            }
        }
        check(rt == 64, "wait 编码/解码 64 种组合全部互逆");

        /* 非 WAIT 指令不能被当成 wait */
        check(!vfd::decodeWait(0x0000u, PIN_MOSI).isWait, "0x0000(jmp 0) 不是 WAIT");
        check(!vfd::decodeWait(0x4001u, PIN_MOSI).isWait, "0x4001(in pins,1) 不是 WAIT");
        check(!vfd::decodeWait(0xA0C3u, PIN_MOSI).isWait, "0xA0C3(mov isr,null) 不是 WAIT");
    }

    /* ---------------------------------------------------------------- §2 */
    section("§2 真实 .pio.h 程序：引脚补丁 + 装载自检");

    const uint32_t length = ssd1306_spi_slave_program.length;
    check(length == 15, "程序长度 15 条");
    check(ssd1306_spi_slave_wrap_target == 0 && ssd1306_spi_slave_wrap == 14, "wrap 0..14");

    {
        /* 占位值统计：CS 低 1 条、SCLK 高 2 条、SCLK 低 2 条 */
        int phCs = 0, phSckHigh = 0, phSckLow = 0;
        for (uint32_t i = 0; i < length; ++i) {
            const uint16_t w = ssd1306_spi_slave_program_instructions[i];
            if (w == vfd::waitGpioEncode(false, vfd::SSD1306_PIO_PLACEHOLDER_CS)) phCs++;
            if (w == vfd::waitGpioEncode(true, vfd::SSD1306_PIO_PLACEHOLDER_SCK)) phSckHigh++;
            if (w == vfd::waitGpioEncode(false, vfd::SSD1306_PIO_PLACEHOLDER_SCK)) phSckLow++;
        }
        check(phCs == 1, "占位：1 条 wait 0 gpio 0（CS）");
        check(phSckHigh == 2, "占位：2 条 wait 1 gpio 1（SCLK 高）");
        check(phSckLow == 2, "占位：2 条 wait 0 gpio 1（SCLK 低）");
    }

    {
        uint16_t words[32];
        memcpy(words, ssd1306_spi_slave_program_instructions, length * sizeof(uint16_t));
        const uint32_t patched = vfd::patchSsd1306WaitPins(words, length, PIN_CS, PIN_SCK);
        check(patched == 5, "补丁改写 5 条 wait");

        const vfd::Ssd1306WaitStats st = vfd::inspectSsd1306WaitPins(words, length, PIN_CS, PIN_SCK);
        check(st.relativePin == 0, "补丁后一条 wait pin 都没有（本次修复的核心）");
        check(st.other == 0, "补丁后没有多余的 wait");
        check(st.csLow == 1 && st.sckHigh == 2 && st.sckLow == 2, "1×CS低 + 2×SCLK高 + 2×SCLK低");
        check(vfd::ssd1306WaitPinsOk(st), "装载自检通过（上机走的就是这个判定）");

        /* 逐条核对索引 → 实际等待的脚 */
        const uint16_t expected[15] = {
            0x200E, /* +0 wait 0 gpio 14 (CS) */
            0xe026, /* +1 set x,6 */
            0x208B, /* +2 wait 1 gpio 11 (SCK 高) */
            0x4001, /* +3 in pins,1 */
            0x200B, /* +4 wait 0 gpio 11 (SCK 低) */
            0x00ce, /* +5 jmp pin,14 */
            0x0042, /* +6 jmp x--,2 */
            0x208B, /* +7 wait 1 gpio 11 (第 8 位) */
            0x4002, /* +8 in pins,2 */
            0x200B, /* +9 wait 0 gpio 11 */
            0x8020, /* +10 push */
            0xc000, /* +11 irq set 0（诊断） */
            0x00ce, /* +12 jmp pin,14 */
            0x0001, /* +13 jmp 1 */
            0xa0c3, /* +14 mov isr,null */
        };
        int diff = 0, same = 0;
        for (uint32_t i = 0; i < length; ++i) {
            if (words[i] == expected[i]) same++; else { diff++; printf("    [+%u] got 0x%04x want 0x%04x\n", i, words[i], expected[i]); }
        }
        check(diff == 0, "补丁后 15 条指令与期望逐条一致", "");
        check(same == 15, "含 5 处改写 + 10 处原样");

        const int idx[5] = { 0, 2, 4, 7, 9 };
        const uint8_t wantPin[5] = { PIN_CS, PIN_SCK, PIN_SCK, PIN_SCK, PIN_SCK };
        const bool wantPol[5] = { false, true, false, true, false };
        int okWaits = 0;
        for (int k = 0; k < 5; ++k) {
            const vfd::PioWaitInfo w = vfd::decodeWait(words[idx[k]], PIN_MOSI);
            if (w.source == vfd::PIO_WAIT_SRC_GPIO && w.pin == wantPin[k] && w.polarity == wantPol[k])
                okWaits++;
        }
        check(okWaits == 5, "5 条 wait 全部落在 GP14(CS)/GP11(SCK) 上");
    }

    {
        /* 反例：把 5 条 wait 按旧写法改成 `wait pin`（相对 IN_BASE），装载自检必须拦住。
         * 注意占位值与生产值在**源位**上就已经不同：wait gpio 0 = 0x2000，wait pin 0 = 0x2020，
         * 所以这里直接按索引写入旧编码，模拟"当年那份 .pio"。 */
        uint16_t words[32];
        memcpy(words, ssd1306_spi_slave_program_instructions, length * sizeof(uint16_t));
        const int oldIdx[5] = { 0, 2, 4, 7, 9 };
        const uint8_t oldPin[5] = { PIN_CS, PIN_SCK, PIN_SCK, PIN_SCK, PIN_SCK };
        const bool oldPol[5] = { false, true, false, true, false };
        for (int k = 0; k < 5; ++k)
            words[oldIdx[k]] = vfd::waitPinEncode(oldPol[k], oldPin[k]);
        const vfd::Ssd1306WaitStats st = vfd::inspectSsd1306WaitPins(words, length, PIN_CS, PIN_SCK);
        check(st.relativePin == 5, "旧写法（wait pin）会被统计出 5 条相对 wait");
        check(!vfd::ssd1306WaitPinsOk(st), "旧写法无法通过装载自检（不会再静默上机）");
    }

    {
        /* 换一套引脚（CS=GP20, SCK=GP5）补丁同样成立 */
        uint16_t words[32];
        memcpy(words, ssd1306_spi_slave_program_instructions, length * sizeof(uint16_t));
        vfd::patchSsd1306WaitPins(words, length, 20, 5);
        const vfd::Ssd1306WaitStats st = vfd::inspectSsd1306WaitPins(words, length, 20, 5);
        check(vfd::ssd1306WaitPinsOk(st), "改引脚（CS=GP20 SCK=GP5）后自检仍通过");
        check(vfd::decodeWait(words[0], PIN_MOSI).pin == 20, "CS wait 落在 GP20");
        check(vfd::decodeWait(words[2], PIN_MOSI).pin == 5, "SCK wait 落在 GP5");
    }

    /* ---------------------------------------------------------------- §3 */
    section("§3 9 位字布局：数据手册模型 vs 生产解码器");

    {
        /* 位图：bit0 = byte 的 LSB、bit1 = DC、bit8..2 = byte 的 bit7..1 */
        int badBitmap = 0, badRoundTrip = 0, badNegCtrl = 0;
        for (uint32_t v = 0; v < 256; ++v) {
            for (int d = 0; d < 2; ++d) {
                const uint8_t byte = (uint8_t)v;
                const bool dc = d != 0;
                const uint32_t word = modelRepeatByte(byte, dc, PIN_MOSI, PIN_MOSI);

                if ((word & 0x1u) != (byte & 0x1u)) badBitmap++;
                if (((word >> 1) & 0x1u) != (dc ? 1u : 0u)) badBitmap++;
                if (((word >> 2) & 0x7Fu) != static_cast<uint32_t>(byte >> 1)) badBitmap++;
                if ((word & ~vfd::SSD1306_PIO_WORD_MASK) != 0) badBitmap++;

                uint8_t gotValue = 0;
                bool gotDc = false;
                vfd::unpackRxWord(word, gotValue, gotDc);
                if (gotValue != byte || gotDc != dc) {
                    badRoundTrip++;
                    if (badRoundTrip <= 3)
                        printf("    byte=0x%02X dc=%d word=0x%08X -> 0x%02X/%d\n", byte, d, word, gotValue, gotDc);
                }

                /* 负控：旧假设 word=(byte<<1)|dc 的解码（value=(word>>1)&0xFF, dc=word&1）
                 * 只在 dc == byte 的 LSB 时才对 —— 正是"恰好一半"的错误率。 */
                const uint8_t oldValue = (uint8_t)((word >> 1) & 0xFFu);
                const bool oldDc = (word & 0x1u) != 0;
                const uint32_t repacked = vfd::packRxWord(byte, dc);
                if (repacked != word) badRoundTrip++;
                if ((oldValue != byte || oldDc != dc) && byte != 0x00 && byte != 0xFF)
                    badNegCtrl++;
            }
        }
        check(badBitmap == 0, "9 位字位图（bit0=byte LSB / bit1=DC / bit8..2=byte 7..1）512 组合全对");
        check(badRoundTrip == 0, "pack/unpack 对 512 组合互逆且与硬件模型一致");
        /* 逐字节算：byte 取 1..254 时，dc 两种取值里恰有一种能让旧解码"蒙对" ⇒ 254 次失败 */
        check(badNegCtrl == 254, "旧假设 (byte<<1)|dc 的解码恰好错一半（254 组）⇒ 必须改解码器");
    }

    {
        /* 端到端：位翻转测试发 4 条命令（DC=0）→ 解码回来必须一模一样 */
        const uint8_t cmds[4] = { 0xAE, 0xAF, 0xA7, 0xA6 };
        int ok = 0;
        for (int i = 0; i < 4; ++i) {
            const uint32_t word = modelRepeatByte(cmds[i], false, PIN_MOSI, PIN_MOSI);
            uint8_t v = 0;
            bool dc = true;
            vfd::unpackRxWord(word, v, dc);
            if (v == cmds[i] && !dc)
                ok++;
        }
        check(ok == 4, "位翻转测试的 4 条命令字节（0xAE/0xAF/0xA7/0xA6, DC=0）可无损还原");
    }

    {
        /* IN_BASE 必须 = MOSI：换成别的基址，模型产出的字就不再是本字节 */
        const uint32_t w12 = modelRepeatByte(0xA5, true, 12, 12);
        const uint32_t w16 = modelRepeatByte(0xA5, true, 16, 12);
        uint8_t v12 = 0, v16 = 0;
        bool d12 = false, d16 = false;
        vfd::unpackRxWord(w12, v12, d12);
        vfd::unpackRxWord(w16, v16, d16);
        check(v12 == 0xA5 && d12, "IN_BASE=MOSI(12) 时模型字可正确解码");
        check(!(v16 == 0xA5 && d16), "IN_BASE 换成 16 后同一份字解不出原字节（基址确实参与运算）");
    }

    /* ---------------------------------------------------------------- §4 */
    section("§4 环形 DMA 的精确计数（大突发不再整圈丢失）");

    {
        /* ── 现场故障的机制 ──────────────────────────────────────────────
         * 旧实现用"写地址 - 缓冲基址"再 &(N-1) 求差分：DMA 只要在 CPU 两次轮询之间
         * 跑满 k 整圈，差分就是 w mod N ⇒ 整圈的字被静默丢弃。
         * 现场（docs/captures/Logout-*.txt）：主机一次 CS 内连写整屏（1024 B）时，
         * 从机的 data 计数只 +256，overrun/drop 还都是 0 —— 正是这个 bug。
         * 新实现（有限整圈计数 + _laps 累计）在同样条件下必须精确。 */
        const uint32_t N = 256; /* 旧环 = 256 字；这里用小环把"整圈"演示出来 */
        const uint32_t cases[] = { 1, 100, 255, 256, 257, 511, 512, 768, 1024, 4096 };
        const uint32_t caseCount = (uint32_t)(sizeof(cases) / sizeof(cases[0]));

        uint32_t oldLostTotal = 0;
        int oldExact = 0, newExact = 0;
        for (uint32_t i = 0; i < caseCount; ++i) {
            const uint32_t w = cases[i];
            /* 旧：只能看到回绕后的位置差 */
            const uint32_t oldCounted = w % N;
            /* 新：整圈数 + 圈内偏移（与 ssd1306_slave_rp2040.cpp 的 task()/updatePending() 同约定） */
            const uint32_t laps = w / N;
            const uint32_t remaining = N - (w % N);
            const uint32_t newCounted = vfd::ringWrittenTotal(laps, N, remaining);

            if (oldCounted == w) oldExact++;
            if (newCounted == w) newExact++;
            oldLostTotal += (w - oldCounted);
        }
        check(newExact == (int)caseCount, "新计数对任意写入量都精确（含整圈边界）");
        check(oldExact == 3, "旧写法只在 w < N 时精确（1/100/255 三例）");
        check(oldLostTotal > 6000, "旧写法在这组用例里累计丢掉 6000+ 字（整圈丢失）★本次修复的 bug");

        /* 具体到现场：1024 B 突发 ⇒ 旧写法只看见 1024 mod 256 = 0，新写法精确 1024 */
        check(vfd::ringWrittenTotal(1024 / N, N, N - (1024 % N)) == 1024, "1024 字突发新计数 = 1024");
        check((1024 % N) == 0, "1024 字突发在 256 字环上正好 4 整圈 ⇒ 旧差分 = 0（全丢）");

        /* 连续搬运（大有限计数 + 只在空闲时补计数）也必须精确 —— 这是最终采用的方案：
         * DMA 绝不停在半路（否则 PIO FIFO 顶满、SM 停摆丢位），计数靠 "base + (FULL - remaining)"。 */
        {
            const uint32_t FULL = 0xFFFFFFFFu;
            const uint32_t total = 3 * N + 7;  /* 跨 3 圈还多 7 字 */
            const uint32_t topupAt = N + 3;    /* 第 1 圈多一点时补一次计数 */

            uint32_t base = 0, remaining = FULL - topupAt;
            check(vfd::ringWrittenFromBase(base, FULL, remaining) == topupAt, "补计数前累计写入精确");
            base = vfd::ringWrittenFromBase(base, FULL, remaining); /* 补计数：把已写量挪进 base */
            remaining = FULL - (total - topupAt);
            check(vfd::ringWrittenFromBase(base, FULL, remaining) == total,
                "补计数后累计写入仍精确（跨 3 圈、中途补过计数，无丢失）");
            check((total % N) == 7, "对照：旧写法（写地址 mod N 差分）在同样情形下只看得到 7 字，整圈被丢");
        }

        /* 取走量用"累计"表示（模 2^32 减法；这也是 popByte 用 _popped & MASK 定位置的原因） */
        check(vfd::ringPending(1024, 0) == 1024, "pending = 累计写入 - 累计取走");
        check(vfd::ringPending(0, 0) == 0, "无数据时 pending = 0");
        check(vfd::ringPending(5, 10) == -5, "uint32 回绕下 pending 仍正确（负数=取多了）");
        check(vfd::ringPending(0x00000005u, 0xFFFFFFF0u) == 21, "跨 2^32 回绕时 pending 仍正确");
    }

    printf("\n%d 项检查，%d 项失败\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
