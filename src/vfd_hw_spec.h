#ifndef VFD_HW_SPEC_H
#define VFD_HW_SPEC_H

#include <stdint.h>

/* ==================================================================== vfd_hw_spec
 * 面板 / 扫描规格常量 —— **唯一来源**（第三方工程评审 §4.3「硬件相关常量集中」）
 *
 * 这些值原先分散在三处、只靠注释维系一致，并且真的漂移过（"扫描周期预算"在源码与文档里
 * 同时存在 802 / 795 / 786 / 784 四个数字）：
 *   · src/vfd_platform_rp2040.cpp  defaultRp2040Config()  → 189 / 100
 *   · src/vfd_scanpack.h                                  → 43 / 48 / FRAME_SIZE
 *   · src/vfd_pio_seed_timing.h                           → 43 / 12 / 516（另一套副本）
 * 现在都从这里派生，并由 static_assert 互锁（见 vfd_scanpack.h / vfd_pio_seed_timing.h）。
 *
 * ⚠️ 这些是**已实机定标**的硬件参数，不是调参旋钮（docs/08 §2「已稳定清单」）：
 *    手册：fCLK ≤ 5 MHz、桁间消隐 ≥ 5 µs、刷新率 ≥ 120 Hz（Note 14：扫描不可停）；
 *    亮度靠缩短"点亮窗口"（BK 低）实现 —— 消隐窗口 = SCAN_PERIOD_US − 点亮窗口，
 *    默认亮度 200 时实测 119.03 / 70.02 µs（见 tests/captures/README.md）。
 *    要改必须重做一遍实机定标。
 *
 * 本头文件**无平台依赖**（只 <stdint.h>）：宿主机测试、QEMU 指令级台架都能包含。
 * ============================================================================= */
namespace vfd {

/* 一个"扫描时序"（= 一个 BK PWM 周期）的时长，µs。
 * 43 × 189 µs = 8.127 ms ⇒ 123.0 Hz（手册要求 ≥ 120 Hz）。 */
constexpr int SCAN_PERIOD_US = 189;

/* 数据搬运期间的**最短**强制消隐时间，µs（手册 Note 7②「写入期间 BK 不得变化」）。
 * 实际消隐 = SCAN_PERIOD_US − 点亮窗口；点亮窗口由亮度决定（全亮时 = 89 µs）。 */
constexpr int BLANK_GUARD_US = 100;

/* 面板几何：一帧 = 43 个时序（手册第 5 页扫描顺序 T43,T1,…,T42）；
 * 每个时序 384 bit = 48 字节阳极数据。 */
constexpr int SCANS_PER_FRAME = 43;
constexpr int SCAN_BYTES = 48;
constexpr int FRAME_SIZE = SCANS_PER_FRAME * SCAN_BYTES; /* 2064 B */

static_assert(SCAN_PERIOD_US > 0 && BLANK_GUARD_US > 0 && BLANK_GUARD_US < SCAN_PERIOD_US,
    "消隐下限必须小于扫描周期（否则没有点亮窗口）");
static_assert(SCAN_BYTES * 8 == 384, "一条扫描对应 384 位栅极链，改了要同步改移位链宽度");
static_assert(FRAME_SIZE == 2064, "帧大小变了要同步改 _frameCopy / 发布协议 / DMA 计数");

} /* namespace vfd */

#endif /* VFD_HW_SPEC_H */
