/*
 * dualcore_handoff.h —— core1(生产者) → core0(消费者) 的帧缓冲握手（纯逻辑，可在宿主机测试）。
 *
 * 协议（SPSC、2 槽、无锁，配合 volatile + 内存屏障使用）：
 *   · 生产者（core1，SSD1306 数据面）把渲染结果写进槽 handoffSlot(seq)，然后 seq++；
 *   · 消费者（core0，帧发布）看到 seq 变化后读槽 handoffSlot(seq-1)，读完后 ack = seq-1；
 *   · 生产者只有在 handoffCanProduce(seq, ack) 为真时才写，否则丢弃本帧
 *     （宁可丢帧也不覆盖消费者还没读走的槽 ⇒ 不撕裂）。
 *
 * 槽位索引：帧 k 放在槽 k % 2；2 个槽 ⇒ 帧 k 与帧 k-2 共用同一槽。
 * 帧 k 可安全写入当且仅当帧 k-2 已被消费（ack >= k-2），即 seq - ack <= 2。
 *
 * 该文件不依赖任何平台头（只含 <stdint.h>），宿主机 tests/test_dualcore_handoff.cpp
 * 用它做"永不覆盖未读帧"的不变量回归。
 */
#ifndef VFD_DUALCORE_HANDOFF_H
#define VFD_DUALCORE_HANDOFF_H

#include <stdint.h>

namespace vfd {

/* 槽数量（必须是 2 的幂，槽位索引用取模/按位与） */
constexpr uint32_t DUALCORE_HANDOFF_SLOTS = 2;

/* 帧 k 所在的槽位 */
inline uint32_t handoffSlot(uint32_t frameIndex)
{
    return frameIndex % DUALCORE_HANDOFF_SLOTS;
}

/* 生产者当前 seq（= 已生产帧数，即"下一帧的编号"）、消费者已确认 ack（= 最近完整消费的帧号）：
 * 返回 true 表示可以写帧 seq，false 表示必须丢弃（否则会覆盖未消费的槽）。 */
inline bool handoffCanProduce(uint32_t seq, uint32_t ack)
{
    /* 用 int32 减法天然处理 uint32 回绕（只要 seq/ack 差距远小于 2^31）。 */
    return static_cast<int32_t>(seq - ack) <= static_cast<int32_t>(DUALCORE_HANDOFF_SLOTS);
}

/* 消费者看到 seq（前提 seq > ack）时，应读的最新帧号 = seq - 1。 */
inline uint32_t handoffLatestFrame(uint32_t seq)
{
    return seq - 1u;
}

} /* namespace vfd */

#endif /* VFD_DUALCORE_HANDOFF_H */
