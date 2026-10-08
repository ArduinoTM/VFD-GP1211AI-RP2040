/*
 * VFD-GP1211AI-RP2040 示例程序（RP2040 / Raspberry Pi Pico，Pico SDK）。
 *
 * 两种运行模式（上电时采样 VFD_PIN_TEST_MODE，默认 GP16，内部上拉）：
 *
 *   ① 测试模式（该脚为**低电平**）：
 *        渲染内置测试图像（开机自检画面 + 动画），用于点亮/时序自检。
 *   ② 默认模式（该脚为高/悬空）：
 *        **不渲染任何测试图像**，本设备作为"一片 SSD1306 OLED"工作：
 *        主机 MCU 通过 4 线 SPI（SCLK/MOSI/CS/DC）发送 SSD1306 命令与 GDDRAM 数据，
 *        本机解析后把画面渲染到 VFD 上（见 src/ssd1306_emulator.*）。
 *
 * 接线（VFD 侧，默认，见 src/vfd_platform_rp2040.h 的 VFD_PIN_* 宏）：
 *
 *   Pico GPIO | 驱动板 J12 | 说明
 *   ----------+------------+--------------------------------------------
 *   GP2       | CLKA       | 阳极移位时钟（tick: SPI0 SCK；pio: PIO sideset）
 *   GP3       | SIA        | 阳极串行数据（tick: SPI0 TX；pio: PIO out）
 *   GP4       | LAT        | LATa+LATg，数据锁存
 *   GP5       | CLKG       | 栅极移位时钟
 *   GP6       | SIG        | SIg 栅极串行数据
 *   GP7       | BK         | BKa+BKg 消隐/调光（PWM）
 *   GP8       | HVEN       | 升压使能（VDD2 ≈ 45 V）
 *   GP9       | FLEN       | 灯丝驱动使能（LM9022 ST）
 *   GND       | GND        | 必须共地
 *
 * 接线（SSD1306 从机侧，默认 GP11–GP15；对端是另一块 MCU）：
 *
 *   主机 MCU        | 本机 Pico | 说明
 *   ----------------+-----------+------------------------------------------
 *   SCK  / SCL      | GP11      | SPI 时钟（模式 0：CPOL=0, CPHA=0）
 *   MOSI / SDA      | GP12      | 主机输出 → 本机输入（PIO in_base）
 *   DC   / A0       | GP13      | 命令/数据选择（**必须紧邻 MOSI 且为其 +1**）
 *   CS   / SS       | GP14      | 片选，低有效（内部上拉）
 *   RESET（可选）   | GP15      | 低有效复位（内部上拉；不接则读高，不影响使用）
 *   GND             | GND       | 必须共地
 *
 * 测试模式选择：GP16 内部上拉；上电时被拉低 → 测试模式（例如跳线帽/按键对地）。
 *
 * 注意：VDD2 = 45 V，灯丝为 2.9 Vac，接线/上电前请务必确认驱动板状态。
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include "demo_screens.h"
#include "ssd1306_emulator.h"
#include "ssd1306_pio_wire.h"
#include "ssd1306_slave_rp2040.h"
#include "vfd_gp1211ai.h"
#include "vfd_platform_rp2040.h"

/* ---------------------------------------------------------------- 诊断输出开关
 * CMake 会用 -DVFD_DEBUG_DIAG=<0|1> 传进来（见 CMakeLists.txt 的 VFD_DEBUG_DIAG 缓存变量，
 * 取值 auto/on/off，默认 auto = Debug 构建开、其余关）。单独编译本文件时按 Release（0）处理。
 *
 *   1 = 逐秒打印 rx/cmd/data/gdram_crc/pinmon/slave-dbg/pc_hist/waits/判读结论，
 *       并采样引脚跳变与 PIO PC 直方图（排故用；打印本身会占 UART 与少量 CPU）；
 *   0 = 只保留启动横幅 + "出问题才打印"的告警（fatal / 初始化失败 / 接线未通），
 *       诊断采样与统计打印全部编译掉。
 *
 * ⚠️ 关了诊断不代表丢了能力：从机固件里 PIO 程序仍然会 `irq set 0`（1 条指令、不占 CPU），
 *    想排故时把本开关打开重编即可，不需要改 .pio。 */
#ifndef VFD_DEBUG_DIAG
#define VFD_DEBUG_DIAG 0
#endif

/* 动画帧间延时（20 fps 左右；显示刷新本身固定 123 Hz，与它无关） */
static constexpr uint32_t ANIM_FRAME_MS = 50;
/* 统计打印周期 */
static constexpr uint32_t STATS_PERIOD_MS = 1000;
/* SSD1306 模拟模式：把收到的画面刷到 VFD 的最小间隔。
 * 取 12 ms（≈83 fps）以便最快档滚动（2 帧/步 = 16 ms）能被完整呈现。 */
static constexpr uint32_t EMU_RENDER_MIN_MS = 12;

static uint8_t gRenderBuffer[vfd::FRAMEBUFFER_SIZE];

/* SSD1306 模拟模式的常驻对象（16 KB 环形缓冲要求对齐，放 .bss；构造函数不访问硬件） */
static vfd::Ssd1306SpiSlave gSlave;
static vfd::Ssd1306Emulator gEmu;

/* ---------------------------------------------------------------- 打印 + 边打边取
 * 诊断行又长又多，而 UART 115200 下一行 90 字符要 ~8 ms；模拟器的 DMA 环是 16 KB，
 * 在 4 MHz 时钟下 **8.2 ms 就被 DMA 绕满一圈**。打印期间没人 pop 环形缓冲 ⇒ DMA 搬满
 * 一圈就停下 → PIO RX FIFO 顶住 → SM 停摆，正在传输的字节就丢了（stall 计数会涨）。
 * 所以：整行先格式化到内存，再按 **32 字节一块**写出，**块与块之间把环形缓冲取空**
 * （~2.8 ms/块，4/8 MHz 都安全）。见 docs/10。 */
#if VFD_DEBUG_DIAG
static void printDrained(vfd::Ssd1306SpiSlave &slave, vfd::Ssd1306Emulator &emu,
    const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    const size_t n = strlen(buf);
    for (size_t off = 0; off < n; off += 32) {
        const size_t chunk = (n - off > 32) ? 32 : (n - off);
        fwrite(buf + off, 1, chunk, stdout);
        uint8_t v;
        bool d;
        while (slave.popByte(v, d))
            emu.pushByte(v, d);
    }
    fflush(stdout);
}
#endif /* VFD_DEBUG_DIAG */

/* ------------------------------------------------------------------ 模式 ① */

/* 测试模式：渲染内置测试图像（点亮 / 时序 / 图形自检） */
static void runTestImageLoop(VFD_GP1211AI &display, vfd::Rp2040Platform &platform)
{
    printf("  mode: TEST IMAGE (VFD_PIN_TEST_MODE 上电为低)\n");

    display.setBrightness(200);
    vfd_demo::drawBootScreen(display);
    display.display();
    /* 自检画面（含列/行校准刻度）刻意停 10 s：便于用相机短曝光拍"静态"照片核对显示，
     * 避免动画画面在长曝光下被拍成拖影而误判成"错位"。*/
    sleep_ms(10000);

    uint32_t frame = 0;
#if VFD_DEBUG_DIAG
    uint32_t framesSinceStats = 0;
    absolute_time_t statsAt = get_absolute_time();
#endif
    bool fatalReported = false;

    for (;;) {
        /* 主循环看护：清 FIFO + 扫描心跳检查（停摆会自动恢复，连续失败则紧急关断） */
        if (!display.task() && display.isFatal() && !fatalReported) {
            fatalReported = true;
            printf("!! scan engine fatal: faults=%lu, HV disabled\n",
                static_cast<unsigned long>(display.faultCount()));
        }

        vfd_demo::drawAnimationFrame(display, frame++);
        display.display();
#if VFD_DEBUG_DIAG
        framesSinceStats++;

        if (absolute_time_diff_us(statsAt, get_absolute_time()) >= STATS_PERIOD_MS * 1000) {
            printf("fps=%lu  scans=%lu  faults=%lu  sync_err=%lu  stalled=%d  brightness(u8)=%u  seed_req=%lu\n",
                static_cast<unsigned long>(framesSinceStats),
                static_cast<unsigned long>(platform.scanCount()),
                static_cast<unsigned long>(display.faultCount()),
                static_cast<unsigned long>(display.frameErrorCount()),
                platform.engineStalled() ? 1 : 0,
                static_cast<unsigned>(display.getBrightness()),
                0UL); /* 播种由 CPU 在帧末直接翻引脚，无请求计数 */
            framesSinceStats = 0;
            statsAt = get_absolute_time();
        }
#endif /* VFD_DEBUG_DIAG */

        sleep_ms(ANIM_FRAME_MS);
    }
}

/* ------------------------------------------------------------------ 模式 ② */

/* 默认模式：作为 SSD1306 从机 —— 解析主机送来的命令/数据并渲染到 VFD */
static void runSsd1306EmulatorLoop(VFD_GP1211AI &display, vfd::Rp2040Platform &platform)
{
    const vfd::Ssd1306SpiSlaveConfig slaveCfg = vfd::defaultSsd1306SpiSlaveConfig();
    vfd::Ssd1306SpiSlave &slave = gSlave;
    vfd::Ssd1306Emulator &emu = gEmu;

    printf("  mode: SSD1306 emulator (VFD_PIN_TEST_MODE 上电为高)\n");
    printf("  slave pins: SCK=GP%u MOSI=GP%u DC=GP%u CS=GP%u RESET=%d\n",
        slaveCfg.pin_sck, slaveCfg.pin_mosi, slaveCfg.pin_dc, slaveCfg.pin_cs,
        slaveCfg.pin_reset == 0xFF ? -1 : static_cast<int>(slaveCfg.pin_reset));
    printf("  master must use SPI mode 0 (CPOL=0, CPHA=0); DC 必须接在 MOSI+1\n");

    if (!slave.begin()) {
        printf("!! SSD1306 slave init failed: %s\n", slave.lastError());
        display.clearDisplay();
        display.display();
        /* 初始化失败也要保持栅极扫描（手册 Note 14：扫描不可停） */
        for (;;) {
            display.task();
            sleep_ms(100);
        }
    }

    /* 初始全黑（SSD1306 上电默认 Display OFF） */
    emu.renderToFramebuffer(gRenderBuffer);
    display.blitFramebuffer(gRenderBuffer);
    display.display();

    int lastContrast = -1;
#if VFD_DEBUG_DIAG
    uint32_t lastStats = platform.millis();
    uint32_t commandsAtStats = 0, dataAtStats = 0;
    uint32_t overrunsAtStats = 0, droppedAtStats = 0;
#endif

    /* ---- 引脚监控（接线排查）--------------------------------------------
     * 每轮循环采样 SCLK/MOSI/DC/CS 电平，累计"跳变次数 / 是否见过高 / 是否见过低"，
     * 并在统计行末尾给出一句话结论。判读：
     *   · chg=0                ⇒ 该线没有跳变（没接通，或主控没动作）；
     *   · CS 从未变低           ⇒ 从机停在 `wait 0 gpio CS`，收不到任何字节（最典型 0 字节原因）；
     *   · SCLK 无跳变           ⇒ 没有时钟进来（SCK/MOSI 接反也会这样）。
     * 只读引脚，不改方向/功能（PIO 的输入采样不受影响）。 */
#if VFD_DEBUG_DIAG
    const uint8_t monPins[4] = { 11, 12, 13, 14 };   /* SCLK / MOSI / DC / CS */
    uint32_t pcHist[16] = { 0 };                     /* PC 直方图（判定状态机是否真卡死） */
    uint32_t pushIrqCount = 0;                       /* push 次数（PIO IRQ0 轮询计数） */
    uint8_t monLast[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    uint32_t monChg[4] = { 0, 0, 0, 0 };
    bool monHigh[4] = { false, false, false, false };
    bool monLow[4] = { false, false, false, false };
#endif
    bool fatalReported = false;

    for (;;) {
        if (!display.task() && display.isFatal() && !fatalReported) {
            fatalReported = true;
            printf("!! scan engine fatal: faults=%lu, HV disabled\n",
                static_cast<unsigned long>(display.faultCount()));
        }
#if VFD_DEBUG_DIAG
        /* PC 直方图 + push 中断计数（只读；每轮都采） */
        {
            const int pc = slave.debugPcOffset();
            if (pc >= 0 && pc < 16)
                pcHist[pc]++;
            if (slave.debugTakeIrqFlag())
                pushIrqCount++;
        }

        /* 引脚监控采样（只读；模拟器模式下每轮都采） */
        for (int mi = 0; mi < 4; ++mi) {
            const uint8_t lvl = gpio_get(monPins[mi]) ? 1u : 0u;
            if (monLast[mi] == 0xFF) {
                monLast[mi] = lvl;
                if (lvl) monHigh[mi] = true; else monLow[mi] = true;
            } else if (lvl != monLast[mi]) {
                monLast[mi] = lvl;
                monChg[mi]++;
                if (lvl) monHigh[mi] = true; else monLow[mi] = true;
            }
        }
#endif
        slave.task();

        /* 主机拉过 RESET：复位模拟器状态 */
        if (slave.consumeResetEvent())
            emu.reset();

        /* 取走本轮收到的所有字节 */
        uint8_t value;
        bool dc;
        while (slave.popByte(value, dc))
            emu.pushByte(value, dc);

        /* 推进滚动（0x26/0x27/0x29/0x2A 设置 + 0x2F 启动后按时间改写 GDDRAM） */
        emu.tick(platform.millis());

        /* 对比度（0x81）→ VFD 亮度 */
        if (static_cast<int>(emu.contrast()) != lastContrast) {
            lastContrast = emu.contrast();
            display.setBrightness(static_cast<uint8_t>(lastContrast));
        }

        /* 有新数据且达到最小间隔 → 渲染一帧 */
        if (emu.shouldRender(platform.millis(), EMU_RENDER_MIN_MS)) {
            emu.renderToFramebuffer(gRenderBuffer);
            display.blitFramebuffer(gRenderBuffer);
            display.display();
        }

#if VFD_DEBUG_DIAG
        if (static_cast<uint32_t>(platform.millis() - lastStats) >= STATS_PERIOD_MS) {
            const uint32_t cmds = emu.commandCount();
            const uint32_t data = emu.dataCount();
            printDrained(slave, emu, "rx=%lu B  cmd=%lu(+%lu)  data=%lu(+%lu)  unknown=%lu  overrun=%lu(+%lu)  drop=%lu(+%lu)  stall=%d  disp=%d  contrast=%u  gdram_crc=0x%08lx\n",
                static_cast<unsigned long>(slave.receivedCount()),
                static_cast<unsigned long>(cmds), static_cast<unsigned long>(cmds - commandsAtStats),
                static_cast<unsigned long>(data), static_cast<unsigned long>(data - dataAtStats),
                static_cast<unsigned long>(emu.unknownCommandCount()),
                static_cast<unsigned long>(slave.overrunCount()),
                static_cast<unsigned long>(slave.overrunCount() - overrunsAtStats),
                static_cast<unsigned long>(slave.droppedCount()),
                static_cast<unsigned long>(slave.droppedCount() - droppedAtStats),
                slave.stalled() ? 1 : 0, emu.displayOn() ? 1 : 0,
                static_cast<unsigned>(emu.contrast()),
                static_cast<unsigned long>(emu.gdramCrc32())); /* 与主控测试程序打印的图像 CRC 对照 */
            printDrained(slave, emu, "  scroll: active=%d mode=%d %s pages=%u-%u interval=%u(%lu ms/step) v=%u steps=%lu\n",
                emu.scrollActive() ? 1 : 0, static_cast<int>(emu.scrollMode()),
                emu.scrollRight() ? "right" : "left",
                emu.scrollStartPage(), emu.scrollEndPage(), emu.scrollInterval(),
                static_cast<unsigned long>(emu.scrollPeriodMs()),
                emu.scrollVerticalOffset(),
                static_cast<unsigned long>(emu.scrollStepCount()));

            /* ---- 引脚监控结论 ---- */
            {
                char verdict[4][24];
                for (int i = 0; i < 4; ++i) {
                    if (monChg[i] == 0)      snprintf(verdict[i], sizeof(verdict[i]), "%s", "无跳变(疑未接通)");
                    else if (!monLow[i])     snprintf(verdict[i], sizeof(verdict[i]), "%s", "从未变低");
                    else if (!monHigh[i])    snprintf(verdict[i], sizeof(verdict[i]), "%s", "从未变高");
                    else                     snprintf(verdict[i], sizeof(verdict[i]), "%s", "正常");
                }
                printDrained(slave, emu, "pinmon: SCLK(GP%u)=%d chg=%lu %s | MOSI(GP%u)=%d chg=%lu %s | DC(GP%u)=%d chg=%lu %s | CS(GP%u)=%d chg=%lu %s\n",
                    monPins[0], monLast[0], static_cast<unsigned long>(monChg[0]), verdict[0],
                    monPins[1], monLast[1], static_cast<unsigned long>(monChg[1]), verdict[1],
                    monPins[2], monLast[2], static_cast<unsigned long>(monChg[2]), verdict[2],
                    monPins[3], monLast[3], static_cast<unsigned long>(monChg[3]), verdict[3]);
                /* 上电头 3 秒还没开始发数据，别喊"未接通"（那时的 chg=0 是正常的） */
                const bool started = platform.millis() > 3000;
                if (started && (monChg[3] == 0 || !monLow[3]))
                    printDrained(slave, emu, "  !! CS(GP14) 从未变低 => 从机停在 wait 0 gpio CS，收不到任何字节：查 CS 是否接到 GP14、是否共地\n");
                if (started && monChg[0] == 0)
                    printDrained(slave, emu, "  !! SCLK(GP11) 无跳变 => 没有时钟进来：查 SCK 是否接在 GP11（与 MOSI 接反也如此）\n");
            }
            /* ---- 从机诊断：状态机 PC / RX FIFO / DMA ---- */
            printDrained(slave, emu, "slave-dbg: pc=+%d(%s) rx_fifo=%d  dma_busy=%d dma_wr=0x%08lx dma_left=%lu  ring_pending=%ld  laps=%lu\n",
                slave.debugPcOffset(), slave.debugPcName(), slave.debugRxFifo(),
                slave.debugDmaBusy(),
                static_cast<unsigned long>(slave.debugDmaWriteAddr()),
                static_cast<unsigned long>(slave.debugDmaRemaining()),
                static_cast<long>(slave.pendingWords()),
                static_cast<unsigned long>(slave.lapCount()));
            /* ---- PC 直方图 + push 计数（先给数据，再给判读）---- */
            {
                uint32_t total = 0;
                for (int i = 0; i < 16; ++i) total += pcHist[i];
                /* 采样到 +3..+14 里任何一条 ⇒ 状态机确实走过 wait_sck_hi（不是卡死）。
                 * 注意：事务之间停在 +2 是**正常**的 —— 位翻转/库驱动都会在最后一个 SCK 下降沿之后
                 * 约 50 µs 才抬 CS，而 SM 早在那之前就执行完 `jmp pin` 回到 byte_loop 等下一位时钟了
                 * （ISR 已被 push 清空，位对齐不受影响）。判据必须是"从未走到 +3 之后"。 */
                uint32_t beyondFirstWait = 0;
                for (int i = 3; i < 16; ++i) beyondFirstWait += pcHist[i];

                char hist[192];
                int hl = snprintf(hist, sizeof(hist), "pc_hist(共%lu次采样): ", static_cast<unsigned long>(total));
                for (int i = 0; i < 16 && hl > 0 && static_cast<size_t>(hl) < sizeof(hist); ++i)
                    if (pcHist[i])
                        hl += snprintf(hist + hl, sizeof(hist) - static_cast<size_t>(hl), "+%d:%lu ",
                            i, static_cast<unsigned long>(pcHist[i]));
                printDrained(slave, emu, "%s\n  push_irq=%lu 次（PIO 实际走到 push 的次数；0 ⇒ 本窗口从未 push）\n",
                    hist, static_cast<unsigned long>(pushIrqCount));
                /* pin 相关的三个字段决定"状态机到底在看哪个脚"：
                 *   PINCTRL.IN_BASE  —— in pins 的基址，**也是 wait pin 的相对基址**
                 *   EXECCTRL.JMP_PIN —— jmp pin 的绝对 GPIO（jmp pin 用绝对引脚号，与 wait pin 不同）
                 * ⚠️ PIO 的 INSTR_MEM0..31 在 RP2040 上是**只写**的（读回恒 0），所以下面打的是
                 *    "装载镜像"（装载时已按引脚自检通过），不是回读。 */
                printDrained(slave, emu, "  sm: pinctrl=0x%08lx(IN_BASE=%u) execctrl=0x%08lx(JMP_PIN=%u) shiftctrl=0x%08lx\n",
                    static_cast<unsigned long>(slave.debugPinCtrl()),
                    static_cast<unsigned>(slave.debugInBase()),
                    static_cast<unsigned long>(slave.debugExecCtrl()),
                    static_cast<unsigned>(slave.debugJmpPin()),
                    static_cast<unsigned long>(slave.debugShiftCtrl()));
                char waits[256];
                int wl = snprintf(waits, sizeof(waits), "  waits(装载镜像 %lu 条, 装载时已自检):",
                    static_cast<unsigned long>(slave.debugLoadedWords()));
                for (int i = 0; i < static_cast<int>(slave.debugLoadedWords())
                     && wl > 0 && static_cast<size_t>(wl) < sizeof(waits); ++i) {
                    const vfd::PioWaitInfo wi = vfd::decodeWait(slave.debugLoadedWord(i), slave.debugInBase());
                    if (!wi.isWait)
                        continue;
                    wl += snprintf(waits + wl, sizeof(waits) - static_cast<size_t>(wl),
                        " [+%d]wait%u %s%u->GP%u%s", i, static_cast<unsigned>(wi.polarity),
                        wi.source == vfd::PIO_WAIT_SRC_GPIO ? "gpio"
                        : (wi.source == vfd::PIO_WAIT_SRC_PIN ? "pin" : "irq"),
                        static_cast<unsigned>(wi.index), static_cast<unsigned>(wi.pin),
                        wi.pin == slaveCfg.pin_cs ? "(CS)" : (wi.pin == slaveCfg.pin_sck ? "(SCK)" : ""));
                }
                printDrained(slave, emu, "%s\n", waits);

                /* ---- 判读：只在"确实卡住"时才报警，避免把正常的等待当故障 ---- */
                if (slave.debugRxFifo() > 0) {
                    printDrained(slave, emu, "  !! RX FIFO 有字但没进环形缓冲：卡在 DMA（DREQ/通道/写地址）\n");
                } else if (pushIrqCount == 0 && beyondFirstWait == 0 && slave.receivedCount() == 0) {
                    const int pc = slave.debugPcOffset();
                    if (pc == 2 && monChg[0] != 0)
                        printDrained(slave, emu, "  !! SCLK 在跳变、却始终停在 wait_sck_hi（本窗口从未走到 +3 之后）⇒ 等错脚："
                               "确认 wait 用的是 gpio(绝对) 而不是 pin(相对 IN_BASE)\n");
                    else if (pc == 0 && monChg[3] != 0)
                        printDrained(slave, emu, "  !! CS 在跳变、却停在 wait_cs ⇒ 引脚补丁/极性/程序起点\n");
                } else if (beyondFirstWait != 0) {
                    printDrained(slave, emu, "  ok: PIO 正常跑程序（本窗口采样到 +3..+14 共 %lu 次），累计收 %lu 字节 / %lu 次 push\n",
                        static_cast<unsigned long>(beyondFirstWait),
                        static_cast<unsigned long>(slave.receivedCount()),
                        static_cast<unsigned long>(pushIrqCount));
                }
                for (int i = 0; i < 16; ++i) pcHist[i] = 0;
            }
            commandsAtStats = cmds;
            dataAtStats = data;
            overrunsAtStats = slave.overrunCount();
            droppedAtStats = slave.droppedCount();
            lastStats = platform.millis();
        }
#endif /* VFD_DEBUG_DIAG */
    }
}

/* ------------------------------------------------------------------- main */

int main()
{
    stdio_init_all();

    const vfd::Rp2040Config cfg = vfd::defaultRp2040Config();
    vfd::Rp2040Platform platform(cfg);
    VFD_GP1211AI display(platform);

    /* init() + 空白帧 + 上电时序（灯丝预热 preheat_ms，再上高压） */
    display.begin(cfg.preheat_ms);

    /* 上电采样测试模式脚（内部上拉，低 = 测试模式） */
    const bool testMode = vfd::rp2040TestModeSelected();

    printf("\nVFD-GP1211AI-RP2040 demo\n");
    printf("  scan engine: %s\n", platform.engineName());
    /* 启动时打印"总线顺序配置"：用串口即可确认板上跑的到底是哪一版固件 */
    printf("  wire: reverseBits=%d scanPhase=%d bitShift=%d\n",
        platform.wireReversesByteBits() ? 1 : 0);
    printf("  CLKa=GP%u  SIa=GP%u  LAT=GP%u  CLKg=GP%u  SIg=GP%u  BK=GP%u  HVEN=GP%u  FLEN=GP%u\n",
        cfg.pin_clka, cfg.pin_sia, cfg.pin_lat, cfg.pin_clkg,
        cfg.pin_sig, cfg.pin_bk, cfg.pin_hv_en, cfg.pin_fl_en);
    printf("  anode shift clock: %.3f MHz (manual limit 5 MHz)\n",
        static_cast<double>(platform.clockHz()) / 1e6);
    printf("  scan period: %lu us -> %.1f Hz frame rate (%d scans/frame)\n",
        static_cast<unsigned long>(cfg.scan_period_us),
        1e6 / (static_cast<double>(cfg.scan_period_us) * vfd::SCANS_PER_FRAME),
        vfd::SCANS_PER_FRAME);
    printf("  lit window: max %lu us of %lu us, blank guard %lu us (margin %lu us)\n",
        static_cast<unsigned long>(platform.litWindowMaxUs()),
        static_cast<unsigned long>(cfg.scan_period_us),
        static_cast<unsigned long>(cfg.blank_guard_us),
        static_cast<unsigned long>(platform.guardMarginUs()));
    if (platform.engineError())
        printf("  !! scan engine init failed - check pin mapping (LAT/CLKg/SIg must be consecutive)\n");
    /* 栅极链播种：tick 引擎内联在 0 号扫描；pio 引擎用"请求脚 + jmp pin"握手排在帧首扫描 */
    printf("  frame-end grid seed: %s\n",
        platform.engineName()[0] == 'p' ? "seed by CPU @ frame end" : "seed inline @ frame end");

    if (testMode)
        runTestImageLoop(display, platform);
    else
        runSsd1306EmulatorLoop(display, platform);
}
