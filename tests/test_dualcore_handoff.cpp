/*
 * test_dualcore_handoff.cpp —— 跨核帧缓冲握手（2 槽 SPSC）的纯逻辑不变量测试。
 *
 * 测 src/dualcore_handoff.h（生产用的同一份头）：槽位映射、可写边界，以及
 * "生产者永不覆盖消费者未读帧"的不变量（随机交错仿真）。
 *
 * 协议要点（见 dualcore_handoff.h）：
 *   · 帧 k 放在槽 k % 2；帧 k 与帧 k-2 共用同一槽；
 *   · 生产者写帧 seq 前必须 handoffCanProduce(seq, ack)（即 seq - ack <= 2），
 *     否则丢弃 —— 这保证"要写的槽只可能装着已被消费（<= ack）的帧"。
 */
#include <cstdio>
#include <cstdint>

#include "dualcore_handoff.h"

static int gChecks = 0;
static int gFail = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        ++gChecks;                                                        \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            ++gFail;                                                      \
        }                                                                 \
    } while (0)

int main()
{
    using namespace vfd;

    /* 1. 槽位映射 */
    for (uint32_t k = 0; k < 4096; ++k)
        CHECK(handoffSlot(k) == k % DUALCORE_HANDOFF_SLOTS);

    /* 2. 可写边界：seq - ack <= 2 才可写（否则会覆盖未读槽） */
    CHECK(handoffCanProduce(0, 0) == true);
    CHECK(handoffCanProduce(1, 0) == true);
    CHECK(handoffCanProduce(2, 0) == true);
    CHECK(handoffCanProduce(3, 0) == false);
    CHECK(handoffCanProduce(5, 3) == true);
    CHECK(handoffCanProduce(6, 3) == false);
    /* 回绕安全：差距用 int32 减法，只要远小于 2^31 就正确 */
    CHECK(handoffCanProduce(0xFFFFFFFAu, 0xFFFFFFF9u) == true);   /* 差 1 */
    CHECK(handoffCanProduce(0xFFFFFFFBu, 0xFFFFFFF9u) == true);   /* 差 2 */
    CHECK(handoffCanProduce(0xFFFFFFFCu, 0xFFFFFFF9u) == false);  /* 差 3 */

    /* 3. 随机交错仿真：生产者只在 handoffCanProduce 为真时写；断言写入槽要么空、
     *    要么装着已被消费（<= ack）的帧，即"永不覆盖未读帧"。 */
    uint32_t seq = 0, ack = 0;
    uint32_t slotOwner[DUALCORE_HANDOFF_SLOTS] = { 0xFFFFFFFFu, 0xFFFFFFFFu }; /* 0xFFFFFFFF = 空 */
    uint32_t rnd = 0x1234567u;
    auto next = [&rnd]() { rnd = rnd * 1103515245u + 12345u; return (rnd >> 16) & 0x7FFFu; };

    for (int step = 0; step < 200000; ++step) {
        if ((next() & 1u) == 0) {
            /* 生产者尝试生产帧 seq */
            if (handoffCanProduce(seq, ack)) {
                const uint32_t slot = handoffSlot(seq);
                CHECK(slotOwner[slot] == 0xFFFFFFFFu || slotOwner[slot] <= ack);
                slotOwner[slot] = seq;
                ++seq;
            }
        } else {
            /* 消费者读最新帧（seq-1，若有未读帧） */
            if (seq > ack)
                ack = handoffLatestFrame(seq);
        }
    }
    CHECK(seq > 0);

    printf("dualcore handoff: %d checks, %d failed\n", gChecks, gFail);
    return gFail == 0 ? 0 : 1;
}
