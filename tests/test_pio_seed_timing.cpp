/*
 * 宿主机测试：PIO 引擎"帧末播种"的时序模型 —— 含 2026-10-08 之前请求脚窗口的**历史模型**，
 * 以及现行 25 条 PIO 程序的结构断言（后者才是防回归的活判据）。
 *
 * 背景（2026-10-05 由逻辑分析仪实测发现）：
 *   PIO 版固件里 SIg **恒为 0**（475 ms / 58 帧内没有任何播种脉冲）⇒ 栅极链每帧只被推进 43 次、
 *   播种的 1,1,0,0,0 从未进入 ⇒ 图案无法自复位。
 *   根因不在 PIO 程序，而在 CPU 侧的"请求脚窗口"完全错过了 PIO 的检查时刻：
 *     · PIO 每个扫描周期只在固定一点检查请求脚：扫描周期起点（PWM wrap / BK 上升沿）之后
 *       PIO_SEED_CHECK_CYCLES 个 SM 周期处（= 两个 wait 之后）≈ 0.22 µs；
 *       ⚠️ 该 jmp pin 检查点已随「播种搬回 CPU」（2026-10-11）删除，此处只描述历史机制；
 *       「≈86.9 µs（数据阶段 + 边界之后）」是**边界在数据段之后**的旧结构，勿再引用。
 *     · 整帧 DMA 完成中断发生在**本帧最后一次扫描的数据阶段内**（距该扫描的检查点仅几十 µs，
 *       因为 DMA 只需把最后一个字塞进 4 深 FIFO）。旧代码在这里拉高请求脚、忙等 20 µs 后拉低
 *       ⇒ 窗口结束了、检查点还没到 ⇒ 一帧都播不了种。
 *
 * 本测试用一个纯软件时序模型（坐标：t=0 = 目标帧第一次扫描的起点）验证：
 *   [0] 设计前提：DMA 完成中断落在本帧最后一次扫描内；
 *   [1] 现行播种落点：CPU 自旋等 SM 停进 wait 之后再播种 ⇒ 整组必须在本扫描边界之后、
 *       帧首扫描起点之前完成，且装得进消隐下限。真机判据另见 tests/tools/check_seed_window.js：
 *       从 LA 抓包量"播种组是否整组落在 BK 消隐窗口内、期间 LAT 是否为低"。
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
constexpr double SCAN_US = static_cast<double>(PIO_SEED_SCAN_PERIOD_US); /* 扫描周期（源自 vfd_hw_spec.h） */
constexpr int SCANS = static_cast<int>(PIO_SEED_SCANS_PER_FRAME);     /* 43 */
constexpr double WORD_US = (32.0 * 2.0) * 1e6 / SM_HZ;                /* 一个字 = 32bit × 2 周期 */
constexpr double BOUNDARY_US = PIO_BOUNDARY_CYCLES * 1e6 / SM_HZ;    /* 边界四步距扫描起点 */



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
 *   · LAT 的有效沿必须在 CLKg 上升沿**之后**（**先前进一格、再锁存**前进后的栅极对）；
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
        check(n == 26, "程序 26/32 条（第 1 步：搬运前多一条 wait，等消隐结束再搬）");
        int waits = 0;
        for (int i = 0; i < n; ++i)
            if ((prog[i] >> 13) == 0x1) /* WAIT 操作码 = 001 */
                waits++;
        check(waits == 3, "三条 wait：等点亮 → 等 wrap → 搬运闸门");
        /* 搬运闸门 = 程序里**第 2 条** `wait 0 pin`（第 1 条是扫描起点），位置必须紧跟边界四步：
         * CPU 侧（applyXferGate()）就是按这个位置打补丁（低亮度改成 nop）⇒ 结构一动就要在这里拦下。 */
        {
            int gateAt = -1, nLow = 0;
            for (int i = 0; i < n; ++i) {
                if ((prog[i] >> 13) != 0x1) continue;          /* WAIT 操作码 = 001 */
                if (((prog[i] >> 7) & 0x1u) != 0u) continue;   /* WAIT 极性在 bit 7（bit 12 属 delay 字段）*/
                ++nLow;
                if (nLow == 2) gateAt = i;
            }
            check(nLow == 2 && gateAt == b0 + 4,
                "搬运闸门 = 第 2 条 `wait 0 pin`，紧跟边界四步（下标 b0+4 = 6）⇒ 补丁位置与结构一致");
        }
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
    printf("[0] 播种时机：DMA 完成中断 → seedGrid()（11 步 × 1.02 µs ≈ 11.2 µs；末步停在 CLKg 高）\n");
    check(DMA_DONE_US > -SCAN_US && DMA_DONE_US < 0.0,
        "DMA 完成中断落在本帧最后一次扫描周期内（-189 < t < 0）");
    check(DMA_DONE_US + 12.3 < 0.0,
        "播种（≈12.2 µs）在帧首扫描的边界（t=0）之前完成 ⇒ 顺序 = 先播种、后前进");
    check(DMA_DONE_US > -SCAN_US + 3.0,
        "播种开始于最后一次扫描的边界之后（四步边界只占 ~2 µs）⇒ 不会与边界抢写引脚");

    /* ---- [1] 搬运落点"两选一"（选项 C）：播种永远紧跟"搬运用完"之后，差别只在搬运落在
     *          点亮窗口（高亮度，手册 Figure 5）还是消隐窗口（低亮度）。
     *          坐标：t=0 = 目标帧第一次扫描起点；本帧最后一次扫描 = [-SCAN_US, 0)。
     *          模型与实测依据见 src/vfd_pio_seed_timing.h 的落点说明。 ---- */
    printf("  落点模型：搬运 %u µs（%u 周期）· 边界 %u µs · 播种 %u µs · 落点边界 %u / %u µs\n",
        static_cast<unsigned>(vfd::PIO_XFER_US), static_cast<unsigned>(vfd::PIO_XFER_CYCLES),
        static_cast<unsigned>(vfd::PIO_BOUNDARY_US), static_cast<unsigned>(vfd::PIO_SEED_TOTAL_US),
        static_cast<unsigned>(vfd::PIO_LIT_LOW_MAX_US), static_cast<unsigned>(vfd::PIO_LIT_HIGH_MIN_US));
    {
        const double periodUs = static_cast<double>(vfd::PIO_SEED_SCAN_PERIOD_US);
        const double xferUs = static_cast<double>(vfd::PIO_XFER_US);
        check(BOUNDARY_US < 3.0, "边界四步在扫描起点之后 ≈0.22 µs 开始、≈2 µs 内结束");
        check(static_cast<double>(vfd::PIO_LIT_LOW_MAX_US) + vfd::PIO_BOUNDARY_US + xferUs <= periodUs,
            "落点一：点亮上限 + 边界 + 搬运 ≤ 扫描周期 ⇒ 搬运不越进点亮窗口");
        check(static_cast<double>(vfd::PIO_LIT_HIGH_MIN_US)
                >= xferUs + vfd::PIO_SEED_START_US + vfd::PIO_SEED_TOTAL_US,
            "落点二：点亮下限 ≥ 搬运 + 播种起点延迟 + 播种总时长 ⇒ 播种不被 wrap 切断");
        /* 实测复核（2026-10-17 抓包 Timing-PIO-brightness_0.csv：点亮 108.02 / 消隐 81.02 µs）：
         *   搬运末沿 = 点亮起点 + 86.78 µs；播种首沿 = 搬运末沿（CLKa 末沿）+ 1.88 µs；
         *   播种末沿距下一条 BK 上升沿 9.48 µs。 */
        const double measuredSeedEnd = 86.78 + 1.88 + 11.22; /* 相对点亮起点（11 步 × 1.02 µs）*/
        check(measuredSeedEnd < static_cast<double>(vfd::PIO_LIT_HIGH_MIN_US),
            "实测播种末沿 ≈100.9 µs < 落点二下限 102 µs ⇒ 下限不是纸上数字");
        check(static_cast<double>(vfd::PIO_LIT_LOW_MAX_US) + 88.6 < 189.04,
            "实测低亮度落点：上限 98 µs 时搬运 ≈88.6 µs 仍早于点亮起点（余量 ≈2.4 µs）");
    }

    /* ---- [9] 选项 C：亮度 20%–80% 映射（两段线性 + 跳过死区）---- */
    {
        printf("[9] 亮度映射：20%%–80%%（%u–%u µs），两段线性 + 死区跳过（切换点 b=%u）\n",
            static_cast<unsigned>(vfd::PIO_LIT_MIN_US), static_cast<unsigned>(vfd::PIO_LIT_MAX_US),
            static_cast<unsigned>(vfd::PIO_LIT_SPLIT_B));
        check(vfd::PIO_LIT_MIN_US == 38u && vfd::PIO_LIT_MAX_US == 151u,
            "点亮区间 = 38–151 µs（正好 20% / 80% 档）");
        check(vfd::PIO_LIT_LOW_MAX_US == 98u && vfd::PIO_LIT_HIGH_MIN_US == 102u,
            "两个落点边界 = 98 / 102 µs（死区 4 µs ≈2% 占空比）");
        check(vfd::pioLitWindowUs(0) == 38u && vfd::pioLitWindowUs(255) == 151u,
            "端点：b=0 → 20%、b=255 → 80%");
        bool mono = true, oneJump = false;
        unsigned distinct = 0, prev = 0;
        for (unsigned b = 0; b <= 255u; ++b) {
            const unsigned lit = vfd::pioLitWindowUs(b);
            if (b != 0u) {
                if (lit < prev) mono = false;
                if (lit > prev + 1u) oneJump = true;
                if (lit != prev) ++distinct;
            }
            prev = lit;
        }
        check(mono, "映射单调不减（255 档逐点检查）");
        check(oneJump, "死区只表现为一次向上跳变（98 → 102 µs）");
        check(distinct >= 105u, "可分辨档位 ≥105（低段 61 + 高段 50）");
        check(vfd::pioLowBrightnessMode(vfd::pioLitWindowUs(140u))
                && !vfd::pioLowBrightnessMode(vfd::pioLitWindowUs(141u)),
            "落点切换点 = b 140/141（低亮度段 ≤52%、高亮度段 ≥54%）");
    }

    /* ---- [10] tick 引擎跟进选项 C（2026-10-17）：落点两选一 + 帧末播种，与 pio 同形 ----
     * 纯软件模型（坐标：t=0 = 本扫描周期起点 = BK 变高 = 消隐起点）：
     *   ① 低亮度（点亮 ≤ TICK_LIT_LOW_MAX_US）：搬运紧跟边界 ⇒ 末沿必须早于点亮的起点；
     *   ② 高亮度（点亮 ≥ PIO_LIT_HIGH_MIN_US）：一次性闹钟等 BK 变低再触发 ⇒ 搬运整段在
     *      点亮窗口内、且在下一次边界（= 扫描周期末）之前搬完；
     *   ③ 帧末播种（边界之后 ≈5 µs 起、≈21 µs 长）必须整段落在消隐窗口内 —— 两种落点都要成立；
     *   ④ 四档验收（0x00/0x80/0xA0/0xFF）的落点必须正好是 消隐/消隐/点亮/点亮。 */
    printf("[10] tick 引擎（定时器+SPI+DMA）：选项 C 落点模型（搬运 %u µs · 边界 %u µs · 播种 %u µs）\n",
        static_cast<unsigned>(vfd::TICK_XFER_US), static_cast<unsigned>(vfd::TICK_BOUNDARY_US),
        static_cast<unsigned>(vfd::TICK_SEED_TOTAL_US));
    {
        const double periodUs = static_cast<double>(vfd::PIO_SEED_SCAN_PERIOD_US);
        const double xferUs = static_cast<double>(vfd::TICK_XFER_US);
        const double boundaryUs = static_cast<double>(vfd::TICK_BOUNDARY_US);
        const double seedUs = static_cast<double>(vfd::TICK_SEED_TOTAL_US);
        /* 搬运真实耗时（头文件的常量是保守进位）：384 bit @ 4.464 MHz */
        const double realXferUs = 384.0 * 1e6 / (125.0e6 / 28.0);
        check(vfd::TICK_XFER_US >= realXferUs + 0.9,
            "搬运常量是保守进位（≥ 实测 384 bit @ 4.464 MHz ≈86.0 µs）");

        bool lowOk = true, highOk = true, seedOk = true;
        unsigned lowCount = 0, highCount = 0;
        for (unsigned b = 0; b <= 255u; ++b) {
            const unsigned lit = vfd::pioLitWindowUs(b);
            const double litUs = static_cast<double>(lit);
            /* 帧末播种（等"点亮起点 + 触发延迟"之后播）整段在**点亮窗口**内 —— 两种落点都查 */
            if (litUs < vfd::TICK_TRIG_DELAY_US + seedUs)
                seedOk = false;
            if (vfd::tickLowBrightnessPlacement(lit)) {
                ++lowCount;
                /* 落点一：搬运紧跟边界 ⇒ 末沿必须早于点亮起点 */
                if (boundaryUs + xferUs > periodUs - litUs)
                    lowOk = false;
            } else {
                ++highCount;
                /* 落点二：点亮窗口装得下 搬运 + 触发延迟 + 收尾；且触发点在本周期内 */
                if (litUs < xferUs + vfd::TICK_TRIG_DELAY_US + vfd::TICK_XFER_TAIL_US)
                    highOk = false;
                if ((periodUs - litUs) + vfd::TICK_TRIG_DELAY_US + xferUs > periodUs)
                    highOk = false;
            }
        }
        check(lowOk, "低亮度落点：255 档逐点检查，边界 + 搬运 ≤ 消隐窗口（搬运不越进点亮窗口）");
        check(highOk, "高亮度落点：255 档逐点检查，搬运 + 触发延迟 + 收尾 ≤ 点亮窗口");
        check(seedOk, "帧末播种整段落在点亮窗口内（最紧 20% 档：点亮 38 µs ≥ 触发延迟+播种 22 µs）");
        check(lowCount + highCount == 256u && lowCount > 100u && highCount > 100u,
            "两段亮度都非空（低/高亮度档位数量合理）");

        /* 四档验收：与 pio 的四档表逐项对齐（tests/captures/README.md）*/
        const unsigned probe[4] = { 0x00u, 0x80u, 0xA0u, 0xFFu };
        const unsigned wantLit[4] = { 38u, 92u, 110u, 151u };
        const bool wantLow[4] = { true, true, false, false };
        bool probesOk = true;
        for (int i = 0; i < 4; ++i) {
            const unsigned lit = vfd::pioLitWindowUs(probe[i]);
            const bool low = vfd::tickLowBrightnessPlacement(lit);
            if (lit != wantLit[i] || low != wantLow[i])
                probesOk = false;
            printf("     0x%02X → 点亮 %3u µs（消隐 %3u）· 落点 %s\n", probe[i], lit,
                static_cast<unsigned>(vfd::PIO_SEED_SCAN_PERIOD_US) - lit,
                low ? "消隐窗口内" : "点亮窗口内");
        }
        check(probesOk, "四档验收（0x00/0x80/0xA0/0xFF）⇒ 点亮 38/92/110/151 µs、落点 消隐/消隐/点亮/点亮");

        /* 实测复核（2026-10-17 tick 四档抓包，样本 golden-tick-89ad18a.csv.gz，固件 89ad18a）：
         *   · 引脚上 BK↑ → 首个 CLKa 沿 = 6.2…6.6 µs（0x00/0x80；高亮度档是闹钟触发，另算）；
         *   · 低亮度落点"最后一次 CLKa 沿 → BK↓"余量 = 58.64（0x00）/ 4.64（0x80）µs；
         *   · 点亮窗口 = 38.02 / 92.02 / 110.02 / 151.04 µs（与映射逐档吻合）。
         * 注意：这个 6.6 µs 里含"等 BK 在引脚上变高"（≈0.4 µs）与让位（实测 ≈2.2 µs）——
         * 后者把低亮度落点上限从 97 压到 95（TICK_LIT_LOW_MAX_US 就是这么来的）。 */
        const double measuredBoundaryUs = 6.6; /* 抓包实测（引脚上 BK↑ → 首个 CLKa 沿） */
        check(measuredBoundaryUs <= static_cast<double>(vfd::TICK_BOUNDARY_US),
            "实测复核：BK↑ → 首个 CLKa 沿 6.6 µs ≤ 保守常量 7 µs");
        const double litLowBlank = periodUs - static_cast<double>(vfd::TICK_LIT_LOW_MAX_US); /* 94 µs */
        check(litLowBlank - (measuredBoundaryUs + realXferUs) > 1.0,
            "实测复核：tick 低亮度上限（95 µs）档搬运末沿仍比点亮起点早 >1 µs");
        const double lit92Blank = periodUs - 92.0; /* 0x80 档消隐 97 µs */
        check(lit92Blank - (measuredBoundaryUs + realXferUs) > 4.0,
            "实测复核：0x80 档（点亮 92 µs）余量 >4 µs（抓包实测 4.64 µs）");
    }

    /* ---- [11] 帧末播种与"帧首先播种"**逐位等价**（画面相位不变的证明）----
     * 栅极链 = 48 位移位寄存器，每帧正好 48 次 CLKg 上升沿（43 次边界前进 + 5 次播种）。
     * 链在某个锁存点的"内容"就是**最近 48 个 SIg 位**（先入链的走得更远）。
     *   A 方案（原 tick）：帧首 = 播种 5 拍，再 A0…A42 —— 帧内第 0…4 拍是播种；
     *   B 方案（现行 tick/pio）：A0…A42，帧末（A42 之后）播种 —— 帧内第 43…47 拍是播种。
     * 断言：两种方案在 A0…A42 这 43 个锁存点上的 48 位链内容**逐位相同**。
     * （历史 bug 的边界情形另见 [7]：帧首"先前进攻再播种"会整体错一格 = 画面水平错 3 列。）*/
    printf("[11] 帧末播种 vs 帧首先播种：43 个锁存点的链内容逐位比对\n");
    {
        const unsigned kScans = static_cast<unsigned>(vfd::PIO_SEED_SCANS_PER_FRAME); /* 43 */
        const unsigned kSeed = 5;
        /* 每帧 48 拍：第 i 拍对应的 SIg 位 + 是不是锁存点（A0…A42）*/
        auto chainAtLatches = [&](bool seedAtFrameEnd, uint64_t out[kScans + 8]) {
            unsigned latches = 0;
            uint64_t history = 0; /* 最近 48 个 SIg 位（新位进低位）*/
            for (unsigned frame = 0; frame < 3; ++frame) {
                for (unsigned k = 0; k < kScans + kSeed; ++k) {
                    unsigned sig = 0;
                    bool isLatch = false;
                    if (!seedAtFrameEnd) {
                        /* 帧首：第 0…4 拍是播种（1,1,0,0,0），第 5…47 拍是 A0…A42 */
                        if (k < kSeed)
                            sig = (k < 2u) ? 1u : 0u;
                        else
                            isLatch = true;
                    } else {
                        /* 帧末：第 0…42 拍是 A0…A42，第 43…47 拍是播种 */
                        if (k < kScans)
                            isLatch = true;
                        else
                            sig = ((k - kScans) < 2u) ? 1u : 0u;
                    }
                    history = ((history << 1) | sig) & 0xFFFFFFFFFFFFull; /* 48 位 */
                    if (isLatch && frame > 0 && latches < kScans)
                        out[latches++] = history;
                }
            }
            return latches;
        };
        static uint64_t ha[kScans + 8], hb[kScans + 8];
        const unsigned na = chainAtLatches(false, ha);
        const unsigned nb = chainAtLatches(true, hb);
        check(na == kScans && nb == kScans, "两种方案各取到 43 个锁存点的链快照");
        unsigned same = 0;
        for (unsigned i = 0; i < kScans && i < na && i < nb; ++i)
            if (ha[i] == hb[i])
                ++same;
        check(same == kScans, "帧末播种与帧首先播种：43/43 个锁存点的 48 位链内容完全相同（画面相位不变）");
    }

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
    /* 注：[9] 亮度映射块紧跟在 [1] 落点块之后（见上）。 */

    test_lat_polarity();
    test_scan_phase();

    printf("\n----------------------------------------\n");
    printf("检查项: %d, 失败: %d\n", g_checks, g_failures);
    printf("%s\n", g_failures == 0 ? "全部通过" : "存在失败项");
    return g_failures == 0 ? 0 : 1;
}
