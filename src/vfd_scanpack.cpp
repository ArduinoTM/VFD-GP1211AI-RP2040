#include "vfd_scanpack.h"

#include <string.h>

namespace vfd {

/* 每 (scan, page) 的组内基准位（相对 48 bit 组的第 dx 列）：见 vfd_scanpack.h 顶部定标。
 *   t 偶 → off {0,2,4}（d,e,f）；t 奇 → off {5,3,1}（a,b,c）；
 *   T43（t == 42，Note 12 只用 a,b）→ off {2,4} 且 k 整体 −2 ⇒ 相对位 {0,2}，只两列。 */
static inline void gatherBases(int t, int base[3], int &ncols)
{
    if (t == 42) {
        ncols = 2;
        base[0] = 0;
        base[1] = 2;
        base[2] = 0;
    } else if (t & 1) {
        ncols = 3;
        base[0] = 5;
        base[1] = 3;
        base[2] = 1;
    } else {
        ncols = 3;
        base[0] = 0;
        base[1] = 2;
        base[2] = 4;
    }
}

void packScan(const uint8_t *fb, int scan, bool invert, uint8_t *out, bool wireOrder)
{
    memset(out, 0, SCAN_BYTES);

    if (scan < 0 || scan >= SCANS_PER_FRAME)
        return;

    const int t = (SCANS_PER_FRAME - 1) - scan; /* 42 - scan */
    const int x0 = 3 * t;

    int base[3];
    int ncols;
    gatherBases(t, base, ncols);

    for (int page = 0; page < PAGES; ++page) {
        uint32_t lo = 0, hi = 0; /* 48 bit 组：lo = 字节 0..3，hi = 低 16 位 = 字节 4..5 */
        const uint8_t *row = fb + page * WIDTH;

        for (int dx = 0; dx < ncols; ++dx) {
            const int x = x0 + dx;
            if (x < 0 || x >= WIDTH)
                continue; /* 越界列不产生数据 */
            uint8_t src = row[x];
            if (invert)
                src = static_cast<uint8_t>(~src); /* 只翻真实像素位；填充位由查表天然保持 0 */
            if (src == 0)
                continue; /* 稀疏画面：整字节为空就跳过（查表 ×0 无意义） */
            const int b = base[dx];
            lo |= SPREAD.lo[src] << b;
            hi |= SPREAD.hi[src] << b;
        }

        if (wireOrder) { /* 折进打包：每字节 8 位反序（SWAR），省掉整帧 2064 B 的反序遍历 */
            lo = reverseBitsPerByte32(lo);
            hi = reverseBitsPerByte32(hi);
        }

        /* 组内 48 bit 的字节落位：lo 的有效位只有低 3 字节（m=0..3 → 位 6m ≤ 23），
         * hi 的有效位同样只有低 3 字节（m=4..7 → 组内位 24..47）⇒ 不能用 lo>>24 当第 4 字节。 */
        uint8_t *p = out + 6 * page; /* 6 字节组，天然字节对齐（k = 48*page + j） */
        p[0] = static_cast<uint8_t>(lo);
        p[1] = static_cast<uint8_t>(lo >> 8);
        p[2] = static_cast<uint8_t>(lo >> 16);
        p[3] = static_cast<uint8_t>(hi);
        p[4] = static_cast<uint8_t>(hi >> 8);
        p[5] = static_cast<uint8_t>(hi >> 16);
    }
}

void packFrame(const uint8_t *fb, bool invert, uint8_t *out, bool wireOrder)
{
    for (int scan = 0; scan < SCANS_PER_FRAME; ++scan)
        packScan(fb, scan, invert, out + scan * SCAN_BYTES, wireOrder);
}

void wirePrepareFrame(uint8_t *frame, bool reverseBits)
{
    if (reverseBits) {
        for (int i = 0; i < FRAME_SIZE; ++i)
            frame[i] = reverseBits8(frame[i]);
    }

    /* 扫描相位 −1：第 s 个扫描发逻辑扫描 (s + 1) mod 43 的数据 ——
     * 等价于把整块**左移一个扫描**（48 B），移出的首扫描接到末尾。
     *
     * 就地旋转：只暂存首扫描的 48 B，其余整块前移。
     * ⚠️ 旧实现用 2064 B 的函数内 `static` 暂存：既白占 .bss，又让本函数**不可重入**
     *    （将来若从两个核/两个上下文调用会互相踩）。现在的实现无静态状态。
     * 等价性由 tests/test_pio_seed_timing.cpp 的逐字节断言锁死（相位 −1 两个分支都测）。 */
    uint8_t head[SCAN_BYTES];
    memcpy(head, frame, SCAN_BYTES);
    memmove(frame, frame + SCAN_BYTES, (SCANS_PER_FRAME - 1) * SCAN_BYTES);
    memcpy(frame + (SCANS_PER_FRAME - 1) * SCAN_BYTES, head, SCAN_BYTES);
}

} /* namespace vfd */
