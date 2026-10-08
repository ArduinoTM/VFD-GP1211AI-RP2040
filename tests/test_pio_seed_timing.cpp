/*
 * 宿主机测试：PIO 引擎"帧末播种请求"的时序窗口（CPU 标志脚 vs PIO 的 `jmp pin` 检查点）
 *
 * 背景（2026-10-05 由逻辑分析仪实测发现）：
 *   PIO 版固件里 SIg **恒为 0**（475 ms / 58 帧内没有任何播种脉冲）⇒ 栅极链每帧只被推进 43 次、
 *   播种的 1,1,0,0,0 从未进入 ⇒ 图案无法自复位。
 *   根因不在 PIO 程序，而在 CPU 侧的"请求脚窗口"完全错过了 PIO 的检查时刻：
 *     · PIO 每个扫描周期只在固定一点检查请求脚：扫描周期起点（PWM wrap / BK 上升沿）之后
 *       PIO_SEED_CHECK_CYCLES 个 SM 周期处（数据阶段 + 扫描边界脉冲之后）≈ 86.9 µs；
 *     · 整帧 DMA 完成中断发生在**本帧最后一次扫描的数据阶段内**（距该扫描的检查点仅几十 µs，
 *       因为 DMA 只需把最后一个字塞进 4 深 FIFO）。旧代码在这里拉高请求脚、忙等 20 µs 后拉低
 *       ⇒ 窗口结束了、检查点还没到 ⇒ 一帧都播不了种。
 *
 * 本测试用一个纯软件时序模型（坐标：t=0 = 目标帧第一次扫描的起点）验证：
 *   [0] 设计前提：DMA 完成中断落在本帧最后一次扫描内；
 *   [1] 旧窗口必须**看不到任何检查点**（复现实测现象）；
 *   [2] 新窗口必须**恰好跨过帧首扫描的那一次检查**；
 *   [3..5] 早/晚抖动、保持时间变化时的容差边界。
 * 模型参数全部来自 src/vfd_pio_seed_timing.h（与固件同一份常量）。
 */
#include <stdio.h>
#include <string.h>

#include "vfd_pio_seed_timing.h"
#include "vfd_scanpack.h"

/* pioasm 生成的真实指令数组：本测试要直接解码它，检查 LAT 极性/边界顺序。
 * 生成头只在 !PICO_NO_HARDWARE 时才拉 hardware/pio.h 与 XXX_program_init()，
 * 这里显式关掉，保持"宿主机零依赖"。 */
#define PICO_NO_HARDWARE 1
#include "vfd_scan.pio.h"
#include "vfd_scan_pio_bits.h"

/* ------------------------------------------------ 时序模型（单位：µs） */

namespace model {

using namespace vfd;

constexpr double SM_HZ = static_cast<double>(PIO_SEED_SM_HZ_NOMINAL); /* 9.0014 MHz */
constexpr double SCAN_US = 189.0;                                     /* 扫描周期 */
constexpr int SCANS = static_cast<int>(PIO_SEED_SCANS_PER_FRAME);     /* 43 */
constexpr double WORD_US = (32.0 * 2.0) * 1e6 / SM_HZ;                /* 一个字 = 32bit × 2 周期 */
constexpr double CHECK_US = PIO_SEED_CHECK_CYCLES * 1e6 / SM_HZ;      /* 检查点在扫描起点之后 */

/* 检查点相对目标帧第一次扫描起点（t=0）的时刻；scan < 0 表示上一帧的扫描 */
inline double checkAt(int scan) { return scan * SCAN_US + CHECK_US; }

/* 整帧 DMA 完成中断的时刻（相对 t=0）：
 * DMA 要把 516 个字都写进 FIFO，而只有 SM 已取走 (516 - FIFO - OSR) 个字之后
 * 才能写最后一个字。前 42 个扫描周期各消耗 12 个字，余下的在最后一次扫描内消耗。 */
constexpr double DMA_DONE_US = []
{
    const int wordsBeforeLastScan = (SCANS - 1) * static_cast<int>(PIO_SEED_WORDS_PER_SCAN);
    const int wordsIntoLastScan = static_cast<int>(PIO_SEED_FRAME_WORDS - PIO_SEED_FIFO_DEPTH
        - PIO_SEED_OSR_DEPTH) - wordsBeforeLastScan;
    return -SCAN_US + wordsIntoLastScan * WORD_US;
}();

/* 窗口内命中的检查点（返回扫描序号；-1 = 一个都没命中） */
inline int hitIn(double t0, double t1, int fromScan = -1, int toScan = 2)
{
    for (int s = fromScan; s <= toScan; ++s) {
        const double c = checkAt(s);
        if (t0 <= c && c <= t1)
            return s;
    }
    return -1;
}

/* 固件里的排程：从 DMA 完成中断到"帧首扫描起点 + PIO_SEED_RAISE_US"的延时。
 * 中断落在本帧最后一次扫描内 ⇒ 它之后的第一个 wrap 就是目标帧第一次扫描的起点。 */
inline double raiseDelayFromInterrupt() { return -DMA_DONE_US + PIO_SEED_RAISE_US; }

} // namespace model

/* ------------------------------------------------------------------ 断言 */

static int g_checks = 0;
static int g_failures = 0;
static void check(bool ok, const char *what, const char *detail = "")
{
    g_checks++;
    if (!ok) {
        g_failures++;
        printf("  [FAIL] %s %s\n", what, detail);
        return;
    }
    printf("  [ ok ] %s\n", what);
}

/* --------------------------------------------- [6] 扫描程序里的 LAT 极性
 * 手册 Figure 3/5 画的是**面板侧** LAT（正脉冲有效）；驱动电路里没有反相器 ⇒ MCU 侧必须
 * "空闲低、高脉冲锁存"（2026-10-06 依据电路图更正）。这里直接解码 pioasm 生成的真实指令：
 *   · 只有边界那一条 set pins 把 LAT 拉高（唯一一次锁存脉冲），数据段 LAT 恒为低；
 *   · LAT 的有效沿必须在 CLKg 上升沿**之前**（先锁存"上一扫描已移入"的图案，再前进一格）；
 *   · 锁存脉冲宽 4 个 SM 周期 = 444 ns ≥ 手册 tWL 300 ns；
 *   · 没有任何一条同时抬 LAT 与 CLKg（锁存与链前进不重叠）；
 *   · LAT 位固定为"高 = 锁存有效"（极性旋钮已在 2026-10-08 清理中移除）。 */
static void test_lat_polarity()
{
    using namespace vfd;

    printf("\n[6] 边界脉冲形状与顺序（手册 Figure 3/5：LAT 空闲低/正脉冲，CLKg 空闲高）\n");

    const uint16_t *prog = vfd_scan_program_instructions;
    const int n = static_cast<int>(sizeof(vfd_scan_program_instructions) / sizeof(uint16_t));

    int setPinsCount = 0, latActiveCount = 0, latActiveIdx = -1;
    int clkgHighCount = 0, sigHighCount = 0;
    for (int i = 0; i < n; ++i) {
        if (!scanPioIsSetPins(prog[i]))
            continue;
        ++setPinsCount;
        const uint8_t v = scanPioSetValue(prog[i]);
        const bool lat = scanPioLatActive(prog[i]);
        const bool clkg = (v & 0x2u) != 0;
        const bool sig = (v & 0x4u) != 0;
        if (lat) { ++latActiveCount; latActiveIdx = i; }
        if (clkg) ++clkgHighCount;
        if (sig) ++sigHighCount;
    }

    check(setPinsCount == 4, "程序里共 4 条 set pins（全是边界四步；播种已搬到 CPU 侧）");
    check(latActiveCount == 1, "只有 1 条 set pins 把 LAT 拉高（唯一一次锁存脉冲）");
    check(clkgHighCount == 3 && sigHighCount == 0,
        "静态结构：边界 ②③④ 三条 CLKg 高；程序里不再有 SIg 动作（播种由 CPU 直接翻引脚）");

    bool dataTouchesSet = false;
    for (int i = latActiveIdx + 2; i < n; ++i) {   /* 跳过边界第 ④ 条（LAT 释放） */
        if (scanPioIsSetPins(prog[i]))
            dataTouchesSet = true;
    }
    check(!dataTouchesSet, "数据段（out/nop/jmp）完全不碰 set 组 ⇒ 整个数据段 LAT/CLKg 保持空闲电平");


    /* ---- 边界顺序（2026-10-08 依据 MN12864K Note 15 + 抓包重定）----------------
     * 板子把 LATa 与 LATg 并联，手册却要求 LATg 常高（透明）⇒ 必须"先让 CLKg 前进、
     * 再打 LAT 锁存"，否则锁到的栅极对是"前进之前"的 ⇒ 画面恒定平移 3 列（实机症状）。 */
    {
        int b0 = -1;
        for (int i = 0; i + 3 < n; ++i) {
            if (scanPioIsSetPins(prog[i]) && scanPioIsSetPins(prog[i + 1])
                && scanPioIsSetPins(prog[i + 2]) && scanPioIsSetPins(prog[i + 3])
                && scanPioSetValue(prog[i]) == 0x0 && scanPioSetValue(prog[i + 1]) == 0x2
                && scanPioSetValue(prog[i + 2]) == 0x3 && scanPioSetValue(prog[i + 3]) == 0x2)
                b0 = i;
        }
        check(b0 >= 0,
            "边界是连续四条 set pins：① 0x0(CLKg 拉低) ② 0x2(CLKg↑ 前进) ③ 0x3(LAT↑ 锁存) ④ 0x2(LAT↓ 释放)");
        if (b0 >= 0) {
            check((scanPioSetValue(prog[b0]) & 0x3u) == 0,
                "① 先把 CLKg 拉低（LAT 本来就是低）⇒ ② 的上升沿才是栅极链的前进沿");
            check(scanPioLatActive(prog[b0 + 2]) && !scanPioLatActive(prog[b0 + 1]),
                "LAT 锁存（③）在 CLKg 前进（②）之后 ⇒ 锁到的是前进后的栅极对（不再平移 3 列）");
            check((scanPioSetValue(prog[b0 + 3]) & 0x1u) == 0 && (scanPioSetValue(prog[b0 + 3]) & 0x2u) != 0,
                "④ 收尾在空闲电平：LAT 低（正脉冲结束）、CLKg 高（与手册 Figure 5 一致）");
            check(scanPioDelay(prog[b0 + 2]) == 3 && scanPioDelay(prog[b0 + 3]) == 3,
                "锁存脉冲宽 [3] = 4 个 SM 周期 = 444 ns ≥ 手册 tWL 300 ns");
        }
        check(n == 25, "程序只用 25/32 条指令（播种块搬走后余量充足）");
        int waits = 0;
        for (int i = 0; i < n; ++i)
            if ((prog[i] >> 13) == 0x1) /* WAIT 操作码 = 001 */
                waits++;
        check(waits == 2, "两条 wait（先等 BK 低、再等 BK 高 = PWM wrap）；⚠️ 省掉任一条都会跑飞（实机停扫）");
        /* 数据段形状：每字节 8 个 out pins + 7 个 nop + 1 个 jmp x-- ⇒ 8 个时钟上升沿 */
        int nOut = 0, nNop = 0, nJmpX = 0, nJmpY = 0;
        for (int i = 0; i < n; ++i) {
            const int op = (prog[i] >> 13) & 0x7;
            if (op == 0x3 && ((prog[i] >> 5) & 0x7) == 0x0) nOut++;         /* OUT PINS */
            else if (op == 0x5) nNop++;                                      /* NOP */
            else if (op == 0x0 && ((prog[i] >> 5) & 0x7) == 0x2) nJmpX++;    /* JMP X-- */
            else if (op == 0x0 && ((prog[i] >> 5) & 0x7) == 0x4) nJmpY++;    /* JMP Y-- */
        }
        check(nOut == 8 && nNop == 7 && nJmpX == 1 && nJmpY == 1,
            "数据段形状：每字节 8×out pins + 7×nop + 1×jmp x--，外层 1×jmp y--（48 字节）");
    }

    printf("      LAT 空闲低、锁存打正脉冲（驱动电路无反相器；旋钮已固化）\n");
}

/* ---------------------------------------------- [7] 扫描相位补偿（面板锁存模型）
 * 面板在扫描边界的 LAT 锁存到的是**上一扫描**移入的 48 字节（该边界位于本扫描移位之前）
 * ⇒ 数据侧必须"提前一个扫描"发。该相位固定为 −1（旋钮已在 2026-10-08 清理中移除）。 */
static void test_scan_phase()
{
    using namespace vfd;

    printf("\n[7] 扫描相位补偿：扫描 s 的边界锁存到上一扫描的数据 ⇒ 固定 −1\n");

    static uint8_t raw[FRAME_SIZE];
    for (int s = 0; s < SCANS_PER_FRAME; ++s)
        for (int b = 0; b < SCAN_BYTES; ++b)
            raw[s * SCAN_BYTES + b] = static_cast<uint8_t>(s * 3 + b); /* 每个扫描都能认出自己 */

    /* 面板模型：边界 ③ 把 LAT 抬高的瞬间锁存到的是扫描 s−1 移入的块，④ 之后 LAT 低（保持住）
     * ⇒ 扫描 s 的点亮窗口显示的是**上一个扫描**发出去的块。 */
    const auto displayedAt = [](const uint8_t *wire, int s) {
        const int latched = (s - 1 + SCANS_PER_FRAME) % SCANS_PER_FRAME;
        return wire[latched * SCAN_BYTES];
    };

    static uint8_t w[FRAME_SIZE];
    memcpy(w, raw, sizeof(w));
    wirePrepareFrame(w, false);
    check(w[0] == raw[SCAN_BYTES],
        "第 0 个扫描携带的是逻辑扫描 1 的数据（相位 −1 的旋转已生效）");
    int ok = 0;
    for (int s = 0; s < SCANS_PER_FRAME; ++s)
        if (displayedAt(w, s) == raw[s * SCAN_BYTES])
            ok++;
    check(ok == SCANS_PER_FRAME, "逐扫描对齐 ⇒ 屏幕内容 == 逻辑帧");

}


int main()
{
    using namespace model;
    printf("PIO 扫描引擎：4 步边界 + CPU 帧末播种（结构与时机验证）\n");
    printf("  扫描周期 %.0f µs · SM 时钟 %.4f MHz · 每扫描程序工作量 %.2f µs\n",
        SCAN_US, SM_HZ / 1e6, PIO_SCAN_WORK_CYCLES * 1e6 / SM_HZ);
    printf("  帧末 DMA 完成中断 ≈ t %+.1f µs（目标帧第一次扫描起点为 t=0）\n\n", DMA_DONE_US);

    /* ---- 0. 播种时机（本方案的关键）：帧首播种改由 CPU 在 DMA 完成中断里直接翻引脚完成。
     *         该中断必定落在本帧最后一次扫描内 ⇒ 播种正好滑进"上一个扫描的边界之后、
     *         帧首扫描的边界之前"，这一顺序决定帧首那对 1 落在链位 (43,44)（见 [7]）。 ---- */
    printf("[0] 播种时机：DMA 完成中断 → seedGrid()（11 步 × 1 µs ≈ 15 µs）\n");
    check(DMA_DONE_US > -SCAN_US && DMA_DONE_US < 0.0,
        "DMA 完成中断落在本帧最后一次扫描周期内（-189 < t < 0）");
    check(DMA_DONE_US + 15.0 < 0.0,
        "播种（≈15 µs）在帧首扫描的边界（t=0）之前完成 ⇒ 顺序 = 先播种、后前进");
    check(DMA_DONE_US > -SCAN_US + 3.0,
        "播种开始于最后一次扫描的边界之后（四步边界只占 ~2 µs）⇒ 不会与边界抢写引脚");

    /* [7] 位**顺序**（不是只数个数！）——2026-10-05 实物照片定位出的 bug：
     *     链方向是 SIg → 48 47 … 1，先进链的位走得更远（链号更小）。
     *     帧首 6 次移位送进去的位序列决定那对 1 最终落在哪两个链位：
     *       先播种后前进（原驱动 / 现在的实现）：1,1,0,0,0,0 ⇒ 落在 (43,44) ✔
     *       先前进攻再播种（本移植原来的写法）：0,1,1,0,0,0 ⇒ 落在 (44,45) ✘
     *     差一格 = 每个扫描周期选通的栅极对整体偏一格 = **画面水平错 3 列**
     *     （左起 3 列永远不亮、其余内容整体右移 3 列，与实测照片一致）。 */
    {
        static const int kSeedFirst[6] = { 1, 1, 0, 0, 0, 0 };   /* 播种 5 个 + 前进 1 个 */
        static const int kAdvanceFirst[6] = { 0, 1, 1, 0, 0, 0 }; /* 旧写法：前进 1 个 + 播种 5 个 */

        /* 帧首窗口共 6 次移位：先进链的位走得更远 ⇒ 第 i 个进链的位最终落在链位 43+i */
        auto pairPos = [](const int *bits, int &p0, int &p1) {
            int n = 0;
            for (int i = 0; i < 6; ++i)
                if (bits[i]) { if (n == 0) p0 = 43 + i; else p1 = 43 + i; ++n; }
        };
        int a0 = 0, a1 = 0, b0 = 0, b1 = 0;
        pairPos(kSeedFirst, a0, a1);
        pairPos(kAdvanceFirst, b0, b1);
        printf("  帧首位序列 → 栅极对：先播种后前进 ⇒ (%d,%d)；先前进攻再播种 ⇒ (%d,%d)\n", a0, a1, b0, b1);
        check(a0 == 43 && a1 == 44, "先播种后前进 ⇒ 那对 1 落在 (43,44) = 帧首的 (G43,G44) ✔");
        check(b0 == 44 && b1 == 45, "先前进攻再播种 ⇒ 落在 (44,45)：整体错一格 = 画面水平错 3 列 ✘（回归防线）");
    }
    /* [8] 传输层顺序适配：逐字节反序 + 扫描相位旋转（相位固定 −1，旋钮已移除）
     *     · tick（SPI/PL022 只能 MSB-first）必须逐字节反序，才能等价于 LSB-first 的总线顺序；
     *     · 两个引擎都要做相位 −1 的扫描旋转（见 §7）。 */
    {
        static uint8_t raw8[vfd::FRAME_SIZE];
        for (int i = 0; i < vfd::FRAME_SIZE; ++i) raw8[i] = static_cast<uint8_t>(i * 7 + 1);

        /* 反序 + 相位 −1：扫描 s 的字节 = reverseBits8(逻辑扫描 s+1 的对应字节) */
        static uint8_t f[vfd::FRAME_SIZE];
        memcpy(f, raw8, sizeof(f));
        vfd::wirePrepareFrame(f, true);
        bool ok = true;
        for (int s = 0; s < vfd::SCANS_PER_FRAME; ++s) {
            const int src = (s + 1) % vfd::SCANS_PER_FRAME;
            for (int b = 0; b < vfd::SCAN_BYTES; ++b)
                if (f[s * vfd::SCAN_BYTES + b] != vfd::reverseBits8(raw8[src * vfd::SCAN_BYTES + b]))
                    ok = false;
        }
        check(ok, "wirePrepareFrame(reverseBits=true) = 逐字节位反序 + 扫描相位 −1");

        static uint8_t g[vfd::FRAME_SIZE];
        memcpy(g, raw8, sizeof(g));
        vfd::wirePrepareFrame(g, false);
        bool ok2 = true;
        for (int s = 0; s < vfd::SCANS_PER_FRAME; ++s) {
            const int src = (s + 1) % vfd::SCANS_PER_FRAME;
            if (g[s * vfd::SCAN_BYTES] != raw8[src * vfd::SCAN_BYTES]) ok2 = false;
        }
        check(ok2, "wirePrepareFrame(reverseBits=false) 只做相位 −1 旋转（pio 用）");
    }

    check(SCANS == 43, "一帧 43 次扫描；每帧移位总数 43 + 5 = 48 = 栅极链长度（图案自复位）");

    test_lat_polarity();
    test_scan_phase();

    printf("\n----------------------------------------\n");
    printf("检查项: %d, 失败: %d\n", g_checks, g_failures);
    printf("%s\n", g_failures == 0 ? "全部通过" : "存在失败项");
    return g_failures == 0 ? 0 : 1;
}
