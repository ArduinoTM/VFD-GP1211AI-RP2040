/*
 * vfd_pio_seed_timing.h —— 扫描引擎的"搬运落点 + 亮度映射"常量（**无平台依赖**）
 *
 * ⚠️ 文件名里的 pio 是历史（2026-10-17 之前只有 pio 引擎用）：**选项 C 之后 tick 与 pio
 *    两个引擎共用本头**，边界（98 / 102 µs）与亮度映射（pioLitWindowUs）只有这一份来源，
 *    别再抄第二份常量（tick 侧新增的量只是"本引擎的搬运/边界耗时"，见文末 TICK_* 一节）。
 *
 * 为什么单独一个头文件：这些常量同时被几处使用，必须只有一个来源：
 *   1. src/vfd_platform_rp2040.h（固件：引擎参数与自检）
 *   2. src/vfd_platform_rp2040_pio.cpp（固件：搬运闸门补丁 + 帧末播种）
 *   3. src/vfd_platform_rp2040_tick.cpp（固件：落点两选一 + 帧末播种 + 亮度映射）
 *   4. src/vfd_platform_rp2040.cpp（固件：setBrightness → 两引擎共用的亮度映射）
 *   5. tests/test_pio_seed_timing.cpp（宿主机时序模型验证）
 * 而 vfd_platform_rp2040.h 依赖 Pico SDK，宿主机测试不能包含它。
 *
 * ============================ 现行播种机制（2026-10-08 起）============================
 * PIO 程序（src/vfd_scan.pio，**26/32 条**）每个扫描周期做：
 *   [等 BK 变低 = 点亮起点] → [等 BK 变高 = 消隐起点 = PWM wrap = 扫描周期起点]
 *   → [边界四步：① CLKg↓ ② CLKg↑ 前进 ③ LAT↑ 锁存 ④ LAT↓ 释放]
 *   → [搬运闸门：高亮度 = `wait 0 pin BK`（等点亮起点）/ 低亮度 = `nop`（CPU 打补丁）]
 *   → [移出 48 字节阳极数据] → 回绕
 * **程序里既没有播种块、也没有 `jmp pin`**：帧首那 5 个 (SIg = 1,1,0,0,0) 脉冲由 CPU 在整帧
 * DMA 完成中断里调 seedGrid() 直接翻 GPIO 完成（与 tick 引擎同一套写法）。**位置由构造保证，
 * 与任何 µs 常量无关**：
 *   · 中断发生时 SM 正在本帧最后一次扫描的数据阶段（消隐中）；
 *   · 中断里自旋等 SM 停在 `wait 0`（`pio_sm_get_pc() - base <= 1`）—— 那一刻说明它已完成
 *     本次移位、正等下一次点亮 ⇒ 正处于"**上一次扫描的边界之后、帧首扫描的边界之前**"；
 *   · 播种落在"搬运结束 → 帧首 wrap"的停等窗口内（2026-10-17 起；搬运排在点亮窗口内，
 *     且全程 LAT 保持低 ⇒ 既不与 PIO 的边界抢写 CLKg，屏幕上也看不到这次链搬运。
 * 详见 src/vfd_platform_rp2040_pio.cpp 的 onFrameDmaDone()。
 *
 * ============================ tick 引擎的等价形态（2026-10-17，选项 C 跟进）============================
 * tick 引擎（定时器 + SPI + DMA）每个扫描周期由 189 µs 中断驱动，结构改成与 pio 逐条对应：
 *   [PWM 计数器归零 = BK 变高 = 消隐起点] → [边界四步] →
 *   [低亮度：立即触发 DMA ⇒ 搬运整段在**消隐**窗口内 // 高亮度：等 BK 变低（点亮起点）
 *    再触发 DMA ⇒ 搬运整段在**点亮**窗口内（用一次性闹钟等，**不忙等**）]
 *   → [整帧最后一次扫描：紧跟边界之后播种 1,1,0,0,0（消隐窗口内；低亮度时与搬运并行，
 *      两者引脚互不相干；LAT 全程低 ⇒ 链搬运不可见）] → 回到 PWM wrap
 * **播种从"帧首扫描开头"挪到"帧末扫描边界之后"**：两者都落在帧首那条 BK 上升沿之前、
 * 且都紧跟 A42 —— 栅极链的移位顺序仍是 [A41, A42, 播种×5, A0]（与"先播种后前进"等价，
 * 见 tests/test_pio_seed_timing.cpp §7 的链位回归），因此**画面相位不变**。之所以必须挪：
 * 低亮度落点要求"边界四步 → 搬运紧跟"，搬运 87 µs + 边界 4 µs 已经吃满 91 µs 的消隐窗口，
 * 帧首再塞 20 µs 播种就会越过点亮起点（Note 7②）。挪到帧末后，播种与搬运并行 ⇒ 两者都不越界。
 * ⚠️ tick 的引脚空闲电平已一并核对：播种末步与边界 ④ 都把 CLKg 留在**高**（空闲电平），
 *    正是 pio 版栽过的地方（播种末步停在 CLKg 低 ⇒ 每帧 1 次 Note 7③，见提交 bc3acfc）。
 *
 * 历史：2026-10-08 之前，播种由 PIO 在程序内执行，靠一根"播种请求脚" + `jmp pin` 每个扫描周期
 * 在固定一点回读（那时用 `PIO_SEED_RAISE_US` / `PIO_SEED_HOLD_US` 这套窗口模型）。该机制（请求脚宏
 * `VFD_PIN_SEED_FLAG`、定时器闹钟、`seedRaiseThunk/seedLowerThunk`）已在 2026-10-08 ~ 10-11 之间
 * **连同相关常量与 `engineInit()` 的自检一起删除**；旧窗口的推导与实测记录留在 git 历史与
 * `docs/00` §6.5（该节已标注为历史）。真机判据改为 `tests/tools/check_seed_window.js`：
 * 从抓包直接量"播种组是否整组落在 BK 消隐窗口内、期间 LAT 是否为低"。
 *
 * ⚠️ 另：别把「检查点」与「扫描边界脉冲」混为一谈：旧程序的两个 wait 之后是"数据段 + 边界"，
 *    而**现程序的两个 wait 之后立即是边界四步** ⇒ 边界落在扫描起点之后 ≈0.2–2 µs
 *    （2026-10-16 LA 实测：② CLKg↑ 前进沿在 +0.58…0.70 µs）。`PIO_SCAN_WORK_CYCLES` = 802 是
 *    **整扫描周期预算**（含数据段），不是边界位置；旧文档里的「≈86.9 µs / 782 周期」是
 *    「边界在数据段之后」的早期结构，勿再引用。
 */
#ifndef VFD_PIO_SEED_TIMING_H
#define VFD_PIO_SEED_TIMING_H

#include <stdint.h>

#include "vfd_hw_spec.h" /* 帧结构 / 扫描周期 / 消隐下限的唯一来源 */

namespace vfd {

/* 每个扫描周期内 PIO 的工作量（周期数），必须与 src/vfd_scan.pio 的结构一致：
 *   2 (两个 wait：先等 BK 低、再等 BK 高 = 下一个 PWM wrap)
 *   + 4×4 (扫描边界四条 set pins，带 [3] 延时 = 444 ns：
 *          ① CLKg 拉低 ② CLKg↑ 前进 ③ LAT↑ 锁存 ④ LAT↓ 释放)
 *   + 1 (set y) + 3×(1 (set x) + 16 字节 × 16 周期) + 3×4 (组循环 jmp y-- [3]) = 802
 * 播种块已从程序里移除（改由 CPU 在帧末中断直接翻引脚，见 seedGrid()）⇒ 无帧首额外开销。
 * ⚠️ 两条 wait 都不能省：一次扫描 ≈89 µs，结束时 BK 仍在消隐（高）⇒ 只留 `wait 1` 会立即穿过、
 *    状态机以 ~2 倍速跑飞（数据被抽干后停在 out 上，实机表现为"只有 BK 动、其余全 0"）。
 * 边界脉冲用 [3] 而不是 [2]：CLKg/LAT 电平 4 个 SM 周期 = 444 ns，
 * 相对手册 tWL ≥ 300 ns 留 48% 余量（[2] 只有 333 ns / 11%）。 */
constexpr uint32_t PIO_SCAN_WORK_CYCLES = 2 + 4 * 4 + 1 + 3 * (1 + 16 * 16) + 3 * 4;
/* 注：组循环的 [3] 是手册 Figure 3 的 tLS 要求 —— tLS = 最后一个 CLK 上升沿 → LAT 有效沿
 *     ≥ 250 ns。原来 LAT 紧跟最后一个 CLK 沿（实测仅 ≈200 ns，违规）；给组循环加 [3] 后
 *     LAT 晚 444 ns 出现 ⇒ 实测将变为 ≈644 ns，无论面板侧 LAT 是高有效还是低有效都满足。 */

/* 程序里已没有播种块（改由 CPU 直接翻引脚）⇒ 帧首没有额外的程序周期。 */
constexpr uint32_t PIO_SEED_CYCLES = 0;

/* 扫描起点之后到**边界四步开始**的 SM 周期数（= 两条 wait 之后立刻是边界① CLKg 拉低）：
 * 供宿主机时序模型与 check_seed_window.js 的落点换算使用；程序结构改了就改这里。 */
constexpr uint32_t PIO_BOUNDARY_CYCLES = 2;


/* 时基与帧结构（供时序模型使用）—— **全部从 src/vfd_hw_spec.h 派生**，不再各留一份副本
 * （第三方工程评审 §4.3：此前这里与 vfd_scanpack.h / Rp2040Config 是三次手抄） */
constexpr uint32_t PIO_SEED_SM_HZ_NOMINAL = 9001400; /* 2 × 4.5007 MHz（clkdiv 定点化后的实际 CLKa） */
constexpr uint32_t PIO_SEED_SCAN_PERIOD_US = static_cast<uint32_t>(SCAN_PERIOD_US);
constexpr uint32_t PIO_SEED_SCANS_PER_FRAME = static_cast<uint32_t>(SCANS_PER_FRAME);
constexpr uint32_t PIO_SEED_WORDS_PER_SCAN = static_cast<uint32_t>(SCAN_BYTES / 4); /* 48 B = 12 个字 */
constexpr uint32_t PIO_SEED_FRAME_WORDS = PIO_SEED_SCANS_PER_FRAME * PIO_SEED_WORDS_PER_SCAN; /* 516 */

/* ============ 搬运落点"两选一" → 亮度 20%–80%（2026-10-17 落地选项 C）============
 * 手册 Figure 5 的形态是"搬运整段落在**点亮**窗口内"，但点亮窗口的长度就是亮度：
 * 低亮度时点亮（20% 档 38 µs）根本装不下 88 µs 的搬运。于是按亮度在两种落点间切换 ——
 * .pio 里那条 `wait 0 pin BK` 闸门由 CPU 打补丁换成 `nop`（见 applyXferGate()）：
 *   落点一（低亮度，lit ≤ PIO_LIT_LOW_MAX_US）：闸门 = nop ⇒ 搬运紧跟边界四步，
 *          整段落在**消隐**窗口内（搬运期间 BK 不变 ⇒ 仍满足 Note 7②）；
 *          约束：lit + 边界 + 搬运 + 余量 ≤ 扫描周期。
 *   落点二（高亮度，lit ≥ PIO_LIT_HIGH_MIN_US）：闸门 = wait 0 pin BK ⇒ 搬运整段落在
 *          **点亮**窗口内（手册 Figure 5 形态），帧末播种紧随搬运之后；
 *          约束：lit ≥ 搬运 + 播种起点延迟 + 播种总时长 + 余量。
 * 两者之间的 lit（死区）由 pioLitWindowUs() **跳过**（切换点两侧各留 ~2 µs 实测余量，
 * 死区 4 µs ≈ 2% 占空比，扫亮度时只是一个小台阶）。
 * 实测依据（2026-10-17 抓包 Timing-PIO-brightness_0.csv，lit 108.02 / 消隐 81.02 µs）：
 *   搬运首沿在点亮起点 +0.52 µs、末沿 +86.78 µs；播种首沿在搬运末沿 +1.88 µs；
 *   播种 11 个 CLKg 沿、步距 1.02 µs、SIg 高 5.10 µs；播种末沿距下一条 BK 上升沿 9.48 µs。
 * 改程序/改时钟后这些值**自动重算**，并由下面的 static_assert 在编译期拦住不自洽的组合。
 * 功耗侧依据（手册 Sheet 2/9 电气包线）：80% 档整机实测 2.5 W @5 V，已占手册包线 ~89%，
 *   因此上限保持 80%，不再上调。 */
/* 搬运段（784 周期）与边界四步（16 周期 = 1.78 µs，进位取 2 µs）*/
constexpr uint32_t PIO_XFER_CYCLES = 1 + 3 * (1 + 16 * 16) + 3 * 4; /* 搬运段 784 周期 */
constexpr uint32_t PIO_XFER_US =
    (PIO_XFER_CYCLES * 1000000u + PIO_SEED_SM_HZ_NOMINAL - 1u) / PIO_SEED_SM_HZ_NOMINAL; /* ≈88 µs */
constexpr uint32_t PIO_BOUNDARY_US = 2;

/* 帧末播种：CPU 在中断里 pio_sm_exec 注入 12 条 `set pins`（实测步距 1.02 µs ⇒ 12.24 µs）*/
constexpr uint32_t PIO_SEED_TOTAL_US = 13;   /* 12 步 × 1.02 µs 实测 → 进位取整 */
constexpr uint32_t PIO_SEED_START_US = 1;    /* 搬运末沿 → 播种首次注入（实测 0.9 µs）*/

/* 两个落点的亮度边界；中间的死区由 pioLitWindowUs() 跳过 */
constexpr uint32_t PIO_LIT_LOW_SLACK_US = 1; /* 搬运末沿到点亮起点至少留这么多 */
constexpr uint32_t PIO_LIT_LOW_MAX_US =
    PIO_SEED_SCAN_PERIOD_US - (PIO_BOUNDARY_US + PIO_XFER_US + PIO_LIT_LOW_SLACK_US); /* 98 µs ≈52% */
constexpr uint32_t PIO_LIT_HIGH_MIN_US =
    PIO_XFER_US + PIO_SEED_START_US + PIO_SEED_TOTAL_US;                              /* 102 µs ≈54% */

constexpr uint32_t PIO_LIT_MIN_US = (PIO_SEED_SCAN_PERIOD_US * 20u + 50u) / 100u;     /* 38 µs = 20% 档 */
constexpr uint32_t PIO_BLANK_MIN_US = 23;    /* 消隐下限（落点二里消隐只需装边界 + Note 16 ≥5 µs）*/
constexpr uint32_t PIO_LIT_MAX_PHYS_US = PIO_SEED_SCAN_PERIOD_US - PIO_BLANK_MIN_US;  /* 166 µs ≈88% */
constexpr uint32_t PIO_LIT_MAX_PRODUCT_US = (PIO_SEED_SCAN_PERIOD_US * 80u) / 100u;   /* 80% 档 = 151 µs */
constexpr uint32_t PIO_LIT_MAX_US = PIO_LIT_MAX_PHYS_US < PIO_LIT_MAX_PRODUCT_US
    ? PIO_LIT_MAX_PHYS_US : PIO_LIT_MAX_PRODUCT_US;                                   /* 取小 ⇒ 151 µs */
static_assert(PIO_LIT_LOW_MAX_US + PIO_BOUNDARY_US + PIO_XFER_US <= PIO_SEED_SCAN_PERIOD_US,
    "落点一：一个扫描周期的消隐装不下边界 + 搬运");
static_assert(PIO_LIT_HIGH_MIN_US >= PIO_XFER_US + PIO_SEED_START_US + PIO_SEED_TOTAL_US,
    "落点二：点亮窗口装不下搬运 + 播种");
static_assert(PIO_LIT_MIN_US <= PIO_LIT_LOW_MAX_US, "20% 档不落在低亮度形态里");
static_assert(PIO_LIT_HIGH_MIN_US < PIO_LIT_MAX_US, "两个落点之间的死区吃掉了整个亮度区间");
static_assert(PIO_LIT_MAX_US + PIO_BLANK_MIN_US <= PIO_SEED_SCAN_PERIOD_US,
    "点亮上限 + 消隐下限 装不进一个扫描周期");

/* 亮度 → 点亮窗口（µs）：**两段线性 + 跳过死区**（整数、无浮点、无 gamma）。
 * 两段斜率取等（60/140 ≈ 49/114）⇒ 扫亮度时步进均匀；切换点只跳 4 µs（≈2% 占空比）。
 * ⚠️ 这段同时被固件与宿主机测试使用（tests/test_pio_seed_timing.cpp 逐点验证端点/单调性），
 *    改映射必须两边一起过。 */
constexpr uint32_t PIO_LIT_SPLIT_B = 140; /* b ≤ 140 → 落点一；b ≥ 141 → 落点二 */
constexpr uint32_t pioLitWindowUs(uint32_t brightness)
{
    return brightness <= PIO_LIT_SPLIT_B
        ? PIO_LIT_MIN_US
            + (brightness * (PIO_LIT_LOW_MAX_US - PIO_LIT_MIN_US)) / PIO_LIT_SPLIT_B
        : PIO_LIT_HIGH_MIN_US
            + ((brightness - PIO_LIT_SPLIT_B - 1u) * (PIO_LIT_MAX_US - PIO_LIT_HIGH_MIN_US))
                / (255u - PIO_LIT_SPLIT_B - 1u);
}
/* 落点判据（= 闸门该不该打补丁）：死区里没有合法 lit ⇒"小于高亮度下限"就是落点一 */
constexpr bool pioLowBrightnessMode(uint32_t litUs)
{
    return litUs < PIO_LIT_HIGH_MIN_US;
}
static_assert(PIO_LIT_MIN_US == (PIO_SEED_SCAN_PERIOD_US * 20u + 50u) / 100u, "亮度 0 必须是 20% 档");
static_assert(PIO_LIT_MAX_US == (PIO_SEED_SCAN_PERIOD_US * 80u) / 100u, "亮度 255 必须是 80% 档");
static_assert(pioLitWindowUs(0) == PIO_LIT_MIN_US, "b=0 不是 20% 档端点");
static_assert(pioLitWindowUs(PIO_LIT_SPLIT_B) == PIO_LIT_LOW_MAX_US, "低段顶 ≠ 落点一上限");
static_assert(pioLitWindowUs(PIO_LIT_SPLIT_B + 1u) == PIO_LIT_HIGH_MIN_US, "高段底 ≠ 落点二下限");
static_assert(pioLitWindowUs(255) == PIO_LIT_MAX_US, "b=255 不是 80% 档端点");
static_assert(pioLowBrightnessMode(pioLitWindowUs(PIO_LIT_SPLIT_B))
        && !pioLowBrightnessMode(pioLitWindowUs(PIO_LIT_SPLIT_B + 1u)),
    "切换点两侧的落点判据必须分开");
static_assert(pioLitWindowUs(PIO_LIT_SPLIT_B + 1u) > pioLitWindowUs(PIO_LIT_SPLIT_B)
        && pioLitWindowUs(1) >= pioLitWindowUs(0)
        && pioLitWindowUs(255) > pioLitWindowUs(254),
    "亮度映射必须单调（死区的跳变只能是向上的一步）");

/* ==================== tick 引擎（定时器 + SPI + DMA）的对应量 ====================
 * 上面那套（亮度映射 + 落点边界 98/102）是**两个引擎共用**的；这里只补 tick 自己的
 * 两个耗时量，再由它们**编译期**证明"tick 也扛得住同一组落点边界"：
 *   · TICK_XFER_US     = 一扫描 384 bit（48 B）@ SPI 4.464 MHz 的 CLKa 活动时长；
 *   · TICK_BOUNDARY_US = 从 PWM 归零到**第一个 CLKa 沿**：等 BK 在**引脚上**变高
 *                        （PWM 输出比较跟着它自己的计数时钟走，写 CTR 到引脚有 ≈0.3–0.5 µs 滞后；
 *                         不等的后果 = 引脚上"CLKg 先低、BK 后高" = 每扫描 1 次 Note 7③，
 *                         见 vfd_platform_rp2040_tick.cpp 的 vfdWaitBkHigh()）
 *                        + 让位 ≈2.2 µs（vfdBkSettleDelay()，实测）
 *                        + 边界四步（3 个 ≥1 µs 电平保持）+ DMA 触发延迟。
 * 实测复核（2026-10-17 tick 四档抓包 golden-tick-89ad18a.csv.gz，§10 有断言）：
 *   引脚上 BK↑ → 首个 CLKa 沿 = 6.2…6.6 µs（进位 7）；低亮度落点"搬运末沿 → BK↓"余量
 *   = 58.6（20%）/ 4.6（49%）µs；高亮度落点搬运整段在点亮窗口内（余量 23 / 63 µs）。
 * ⚠️ 这两个值是"保守进位"（宁大不小）：低亮度落点要求"搬运整段落在消隐窗口内"，
 *    即 TICK_BOUNDARY_US + TICK_XFER_US ≤ 消隐窗口 = 扫描周期 − 点亮窗口。
 *    SPI 实际波特率随 clk_sys 变（125 MHz / 28 ≈ 4.464 MHz），这里取标称值。 */
constexpr uint32_t TICK_SPI_HZ_NOMINAL = 4464287; /* clk_sys 125 MHz ÷ prescale 28（SDK 取偶）*/
constexpr uint32_t TICK_XFER_US =
    (static_cast<uint32_t>(SCAN_BYTES) * 8u * 1000000u + TICK_SPI_HZ_NOMINAL - 1u) / TICK_SPI_HZ_NOMINAL; /* 87 */
constexpr uint32_t TICK_BOUNDARY_US = 7;  /* 边界四步 + DMA→首个 CLKa 沿（实测 ≈6.2–6.6 µs）*/
/* tick 的低亮度落点上限：消隐窗口至少要装下"边界 + 搬运"。
 * 实测 189 − (6.6 + 86.0) ≈ 96.4 ⇒ lit ≥ 96 µs 的档（b ≥ 136）装不下，改走高亮度落点
 * （搬运 87 µs 塞进 ≥96 µs 的点亮窗口，仍有 ≥9 µs 余量）—— 两种落点都满足 Note 7②，
 * 只是 lit ≥ 96 这几档 tick 与 pio 的落点选择不同（四档验收 38/92/110/151 不含它们）。
 * ⚠️ 2026-10-17 四档抓包把这里量实了：原以为上限 97/98，实测"让位 ≈2.2 µs"把边界推到
 *    6.2–6.6 µs ⇒ 上限 95（本值）；lit ≥ 96 若仍走低亮度落点，搬运末沿会越过 BK 下降沿。*/
constexpr uint32_t TICK_LIT_LOW_MAX_US =
    PIO_SEED_SCAN_PERIOD_US - (TICK_BOUNDARY_US + TICK_XFER_US); /* 95 µs ≈50% */
static_assert(TICK_LIT_LOW_MAX_US < PIO_LIT_HIGH_MIN_US,
    "tick 的低亮度上限必须仍落在死区之下（否则与高亮度落点重叠）");
static_assert(TICK_LIT_LOW_MAX_US + 3u >= PIO_LIT_LOW_MAX_US,
    "tick 的低亮度落点上限比共享边界低太多：亮度区间会出现'两种落点都不合规'的空档");
/* 高亮度落点：搬运必须在**下一次边界**（= 本扫描周期结束）之前搬完 ——
 * 触发点最多晚到点亮起点 + TICK_TRIG_DELAY_US，因此要求 点亮 ≥ 搬运 + 触发延迟 + 收尾余量。*/
constexpr uint32_t TICK_TRIG_DELAY_US = 1; /* BK 变低之后再过这么久触发（避开边沿本身）*/
constexpr uint32_t TICK_XFER_TAIL_US = 3;  /* 搬运末沿 → 下一次边界（锁存）的收尾余量 */
constexpr uint32_t TICK_LIT_HIGH_MIN_US =
    TICK_XFER_US + TICK_TRIG_DELAY_US + TICK_XFER_TAIL_US; /* 91 µs，< 共享下限 102 ⇒ 一定装得下 */
static_assert(TICK_LIT_HIGH_MIN_US <= PIO_LIT_HIGH_MIN_US,
    "tick 的高亮度落点装不下共享的落点二下限");

/* 帧末播种的时长（tick）：5 个 vfdStrobe（低/高各 ≥2 µs 电平保持）+ SIg 保持 ≈20.4 µs。
 * 播种在"点亮起点 + TICK_TRIG_DELAY_US"之后发生 —— 与 pio 引擎同形（播种组要紧跟帧首那 1 个
 * 前进脉冲：analyze_la_csv.js 的"帧首 SIg 序列"判 1,1,0,0,0,0；见 tick 引擎 (4) 段的教训），
 * 因此要求它整段落在**点亮窗口**内 —— 最紧的是 20% 档（点亮 38 µs）。*/
constexpr uint32_t TICK_SEED_TOTAL_US = 21;
static_assert(TICK_TRIG_DELAY_US + TICK_SEED_TOTAL_US <= PIO_LIT_MIN_US,
    "帧末播种装不进最窄的点亮窗口（20% 档 38 µs）⇒ 会跨过下一次 BK 上升沿");

/* tick 的落点判据（= 搬运该不该等到点亮窗口再触发）：低亮度落点 = 消隐窗口装得下搬运 */
constexpr bool tickLowBrightnessPlacement(uint32_t litUs)
{
    return litUs <= TICK_LIT_LOW_MAX_US;
}
static_assert(tickLowBrightnessPlacement(pioLitWindowUs(134u)) /* = 95 µs，低段最高档 */
        && !tickLowBrightnessPlacement(pioLitWindowUs(136u))    /* = 96 µs，已改走点亮落点 */
        && !tickLowBrightnessPlacement(PIO_LIT_HIGH_MIN_US),
    "tick 落点判据：低亮度段走消隐落点、高亮度段走点亮落点");
static_assert(!tickLowBrightnessPlacement(PIO_LIT_LOW_MAX_US),
    "lit ≥ 96 µs（b ≥ 136）tick 装不进消隐窗口 ⇒ 走点亮落点（见 TICK_LIT_LOW_MAX_US 注释）");

/* ---------------- 每帧只允许播种一次（2026-10-17，修双播种）----------------
 * 背景：DMA_IRQ_0 是**所有通道共享**的中断线；实测抓到 1 s 内 46 次"两次 seedGrid()
 * 相隔 12 µs"（SIg 两段、对应 46 个帧只有 42 个扫描）⇒ 栅极链多前进 5 格 ⇒ 画面错位。
 * 两道防护：① 中断入口先判"是不是扫描通道的完成标志"（见 dmaIrqThunk）；
 * ② 这里的最小间隔判据 —— 帧周期 8.13 ms，两次帧中断若近于 2 ms 一律判为杂散/重复，整段跳过。
 * 纯 constexpr ⇒ 固件与宿主机测试（都包含本头文件）在**编译期**验证其行为。 */
constexpr uint32_t PIO_SEED_MIN_INTERVAL_US = 2000;
constexpr bool seedAllowedByInterval(uint32_t nowUs, uint32_t lastUs)
{
    return static_cast<uint32_t>(nowUs - lastUs) >= PIO_SEED_MIN_INTERVAL_US;
}
static_assert(!seedAllowedByInterval(10000u, 9988u), "相隔 12 µs 的第二次播种必须被拒");
static_assert(!seedAllowedByInterval(10000u, 10000u), "同一时刻重复进入必须被拒");
static_assert(seedAllowedByInterval(20000u, 10000u), "相隔 10 ms（≈一帧）必须允许");
static_assert(seedAllowedByInterval(3000u, 0u), "开机后第一次帧中断必须允许");

/* 互锁：任何一处改了数值，这里立刻编译失败（而不是等到实机画面出错） */
static_assert(SCAN_BYTES % 4 == 0, "整扫描字节数必须能按 32bit 字搬运");
static_assert(PIO_SEED_WORDS_PER_SCAN * 4u == static_cast<uint32_t>(SCAN_BYTES),
    "每扫描字数 × 4 必须等于 SCAN_BYTES（DMA 按字搬运的前提）");
static_assert(PIO_SEED_FRAME_WORDS * 4u == static_cast<uint32_t>(FRAME_SIZE),
    "帧字数 × 4 必须等于 FRAME_SIZE");
/* TX FIFO 深度：扫描引擎没有 join FIFO ⇒ TX 4 深（OSR 另可容纳 1 个字） */
constexpr uint32_t PIO_SEED_FIFO_DEPTH = 4;
constexpr uint32_t PIO_SEED_OSR_DEPTH = 1;

} /* namespace vfd */

#endif /* VFD_PIO_SEED_TIMING_H */
