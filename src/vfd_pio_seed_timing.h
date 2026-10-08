/*
 * vfd_pio_seed_timing.h —— PIO 扫描引擎的"帧末播种请求"时序常量（**无平台依赖**）
 *
 * 为什么单独一个头文件：这些常量同时被三处使用，必须只有一个来源：
 *   1. src/vfd_platform_rp2040.h（固件：引擎参数与自检）
 *   2. src/vfd_platform_rp2040_pio.cpp（固件：闹钟排程）
 *   3. tests/test_pio_seed_timing.cpp（宿主机时序模型验证）
 * 而 vfd_platform_rp2040.h 依赖 Pico SDK，宿主机测试不能包含它。
 *
 * ---------------------------------------------------------------- 时序背景
 * PIO 程序（src/vfd_scan.pio）每个扫描周期做：
 *   [等 BK 点亮结束] → [等 BK 消隐开始 = PWM wrap = 扫描周期起点]
 *   → [移出 48 字节阳极数据] → [扫描边界脉冲：CLKg 前进 + LAT 锁存]
 *   → **`jmp pin` 检查播种请求脚** → 回绕
 * 也就是说，请求脚每个扫描周期只在**一个固定时刻**被检查一次：
 *
 *     检查点 = 扫描周期起点 + PIO_SEED_CHECK_CYCLES / (2 × CLKa)
 *            = wrap + 782 / (2 × 4.5007 MHz) ≈ wrap + 86.9 µs   （逻辑分析仪实测 86.67 µs ✔）
 *
 * 而整帧 DMA 完成中断（CPU 唯一的每帧事件）发生在**本帧最后一次扫描的数据阶段内**，
 * 距该扫描的检查点只有几十 µs（取决于 TX FIFO 深度）⇒ 若在这里直接把请求脚拉高、
 * 再等一个固定短时间拉低，窗口既过不了本帧的检查点，也到不了帧首扫描的检查点。
 * （2026-10-05 实测：PIO 版 SIg 在 475 ms / 58 帧内恒为 0 —— 一帧都没播上种。）
 *
 * 因此请求脚必须按"以 PWM 计数器为准的相对时刻"排程，让窗口**恰好跨过帧首扫描的检查点**：
 *   DMA 完成中断必定落在本帧第 43 次扫描内 ⇒ 它之后的第一个 PWM wrap 就是**新帧第一次扫描**起点。
 *   在该 wrap 之后 PIO_SEED_RAISE_US 拉高、保持 PIO_SEED_HOLD_US 后拉低。
 *
 * 约束（tests/test_pio_seed_timing.cpp 会把它们全部验证一遍）：
 *   (a) PIO_SEED_RAISE_US < 检查点偏移        —— 拉高必须早于帧首扫描的检查点；
 *   (b) PIO_SEED_RAISE_US + PIO_SEED_HOLD_US > 检查点偏移
 *                                            —— 窗口必须跨过该检查点；
 *   (c) PIO_SEED_RAISE_US > 0 且（因 DMA 中断落在第 43 次扫描内）窗口不会覆盖上一次检查点；
 *   (d) PIO_SEED_RAISE_US + PIO_SEED_HOLD_US < 扫描周期 + 检查点偏移
 *                                            —— 下一次检查点（帧首 + 189 µs）之前必须撤掉，
 *                                               否则会重复播种（每帧多 5 次移位 → 画面错位）。
 * 容差：两侧各 ≈±60 µs（闹钟基于 1 MHz 计时器，抖动 µs 级）。
 */
#ifndef VFD_PIO_SEED_TIMING_H
#define VFD_PIO_SEED_TIMING_H

#include <stdint.h>

namespace vfd {

/* 每个扫描周期内 PIO 的工作量（周期数），必须与 src/vfd_scan.pio 的结构一致：
 *   2 (两个 wait：先等 BK 低、再等 BK 高 = 下一个 PWM wrap)
 *   + 4×4 (扫描边界四条 set pins，带 [3] 延时 = 444 ns：
 *          ① CLKg 拉低 ② CLKg↑ 前进 ③ LAT↑ 锁存 ④ LAT↓ 释放)
 *   + 1 (set y) + 3×(1 (set x) + 16 字节 × 16 周期) + 3×4 (组循环 jmp y-- [3]) = 802
 * 播种块已从程序里移除（改由 CPU 在帧末中断直接翻引脚，见 seedGrid()）⇒ 无帧首额外开销。
 * ⚠️ 两条 wait 都不能省：一次扫描 ≈88 µs，结束时 BK 仍在消隐（高）⇒ 只留 `wait 1` 会立即穿过、
 *    状态机以 ~2 倍速跑飞（数据被抽干后停在 out 上，实机表现为"只有 BK 动、其余全 0"）。
 * 边界脉冲用 [3] 而不是 [2]：CLKg/LAT 电平 4 个 SM 周期 = 444 ns，
 * 相对手册 tWL ≥ 300 ns 留 48% 余量（[2] 只有 333 ns / 11%）。 */
constexpr uint32_t PIO_SCAN_WORK_CYCLES = 2 + 4 * 4 + 1 + 3 * (1 + 16 * 16) + 3 * 4;
/* 注：组循环的 [3] 是手册 Figure 3 的 tLS 要求 —— tLS = 最后一个 CLK 上升沿 → LAT 有效沿
 *     ≥ 250 ns。原来 LAT 紧跟最后一个 CLK 沿（实测仅 ≈200 ns，违规）；给组循环加 [3] 后
 *     LAT 晚 444 ns 出现 ⇒ 实测将变为 ≈644 ns，无论面板侧 LAT 是高有效还是低有效都满足。 */

/* 程序里已没有播种块（改由 CPU 直接翻引脚）⇒ 帧首没有额外的程序周期。 */
constexpr uint32_t PIO_SEED_CYCLES = 0;

/* 播种请求检查点在扫描周期起点之后的周期数（= 数据阶段 + 扫描边界 + jmp pin） */
constexpr uint32_t PIO_SEED_CHECK_CYCLES = 2;   /* 两个 wait 之后立刻检查 */

/* ---- 请求脚窗口（µs，相对"帧首扫描的起点 = PWM wrap"）---- */
constexpr int32_t PIO_SEED_RAISE_US = -39;  /* 相对**目标扫描起点**：提前 39 µs 拉低
                                             * （= 上一扫描的 +150 µs；帧 DMA 完成中断约在 -50 µs） */ /* 拉高：检查点之前 57 µs */
constexpr uint32_t PIO_SEED_HOLD_US = 100;   /* 保持 100 µs：跨过 +0.22 µs 的检查点，
                                             * 且在下次检查点（+189.2 µs）前撤掉 */ /* 保持：跨过检查点后在下次检查点前 96 µs 撤掉 */

/* 检查点在扫描起点之后的**标称**时间（µs @ CLKa 4.5007 MHz），仅用于文档/日志 */
constexpr uint32_t PIO_SEED_CHECK_US_NOMINAL = 0; /* 检查点就在扫描起点（+0.22 µs） */ /* 795 周期 ÷ 9.0014 MHz = 88.3 µs（实测 ≈86.8） */

/* 时基与帧结构（供时序模型使用；与 vfd_scanpack.h / Rp2040Config 保持一致） */
constexpr uint32_t PIO_SEED_SM_HZ_NOMINAL = 9001400; /* 2 × 4.5007 MHz */
constexpr uint32_t PIO_SEED_SCANS_PER_FRAME = 43;
constexpr uint32_t PIO_SEED_WORDS_PER_SCAN = 12;  /* 48 字节/扫描（DMA 直接喂发布帧） */
constexpr uint32_t PIO_SEED_FRAME_WORDS = 516;    /* 43 × 12 */
/* TX FIFO 深度：扫描引擎没有 join FIFO ⇒ TX 4 深（OSR 另可容纳 1 个字） */
constexpr uint32_t PIO_SEED_FIFO_DEPTH = 4;
constexpr uint32_t PIO_SEED_OSR_DEPTH = 1;

} /* namespace vfd */

#endif /* VFD_PIO_SEED_TIMING_H */
