/*
 * vfd_scan_pio_bits.h —— PIO 扫描程序（src/vfd_scan.pio）的指令级解码与 LAT 极性处理
 *
 * 为什么单独一个头文件：`set pins` 的指令编码、以及"LAT 是高有效还是低有效"这件事，
 * 同时被固件与宿主测试使用，必须只有一个来源：
 *   1. src/vfd_platform_rp2040_pio.cpp —— 装载程序时按配置替换引脚号（极性不再需要补丁）；
 *   2. tests/test_pio_seed_timing.cpp  —— 直接解码 pioasm 生成的真实指令做回归；
 *   3. 文档（docs/00 §2.1/§8、docs/03）—— 解释 set 组取值。
 * vfd_platform_rp2040.h 依赖 Pico SDK，宿主机测试不能包含它，所以这些纯逻辑放这里（零依赖）。
 *
 * ---------------------------------------------------------------- LAT 极性（2026-10-06 更正）
 * 手册 Figure 3/5 画的是**面板侧** LAT：正脉冲有效（tWL ≥ 300 ns 对应高电平）。
 * 2026-10-05 的实现假设"MCU 与面板之间还有一级反相"，于是发"空闲高、低脉冲锁存"。
 * 用户核对驱动电路后确认：**电路里没有反相器**（MCU 直连 LATa/LATg）⇒
 * MCU 侧必须"**空闲低、高脉冲锁存**"。
 *
 * 因此 src/vfd_scan.pio 的 set 组取值按更正后的极性书写：
 *   bit0 = LAT（1 = 锁存有效）· bit1 = CLKg · bit2 = SIg
 *   0x0 = 全低（空闲）  0x1 = LAT 锁存脉冲  0x2 = CLKg 前进一位
 *   0x4 = SIg=1        0x6 = CLKg+SIg
 * 极性旋钮已在 2026-10-08 清理中移除：`.pio` 里写的就是最终极性（bit0 = LAT，高 = 锁存有效）。
 */
#ifndef VFD_SCAN_PIO_BITS_H
#define VFD_SCAN_PIO_BITS_H

#include <stdint.h>

namespace vfd {

/* RP2040 指令编码（数据手册 §3.4）：SET = 111 | 延时 5 位 | 目标 3 位 | 数据 5 位 */
constexpr uint16_t PIO_OPCODE_SET = 0x7u;
constexpr uint16_t PIO_DEST_PINS = 0x0u;

/* 是不是一条 `set pins, ...` */
inline bool scanPioIsSetPins(uint16_t instr)
{
    return ((instr >> 13) & 0x7u) == PIO_OPCODE_SET && ((instr >> 5) & 0x7u) == PIO_DEST_PINS;
}

/* `set pins` 的 5 位数据（低 3 位依次是 LAT / CLKg / SIg） */
inline uint8_t scanPioSetValue(uint16_t instr)
{
    return static_cast<uint8_t>(instr & 0x1Fu);
}

/* 指令自带的延时（SM 周期数；1 周期 ≈ 111 ns @ 9 MHz） */
inline uint8_t scanPioDelay(uint16_t instr)
{
    return static_cast<uint8_t>((instr >> 8) & 0x1Fu);
}

/* LAT 位（数据位 0）：true = 锁存有效（更正后的极性 = 高电平） */
inline bool scanPioLatActive(uint16_t instr)
{
    return (instr & 0x1u) != 0;
}

} /* namespace vfd */

#endif /* VFD_SCAN_PIO_BITS_H */
