#include "vfd_scanpack.h"

#include <string.h>

namespace vfd {

void packScan(const uint8_t *fb, int scan, bool invert, uint8_t *out)
{
    memset(out, 0, SCAN_BYTES);

    if (scan < 0 || scan >= SCANS_PER_FRAME)
        return;

    const int t = (SCANS_PER_FRAME - 1) - scan; /* 42 - scan */
    const int x0 = 3 * t;

    /* 见 vfd_scanpack.h 顶部的槽位/定标说明：
     *   一般时序 t 偶 → off 0,2,4；t 奇 → off 5,3,1
     *   T43（t == 42，Note 12 只用 a,b）→ off 2,4 且 k 整体 −2、只写两个槽 */
    const bool t43 = (t == 42);
    const int off0 = t43 ? 2 : ((t & 1) ? 5 : 0);
    const int offStep = t43 ? 2 : ((t & 1) ? -2 : 2);
    const int kShift = t43 ? -2 : 0;
    const int maxDx = t43 ? 2 : 3;

    for (int page = 0; page < PAGES; ++page) {
        for (int dx = 0; dx < maxDx; ++dx) {
            const int x = x0 + dx;
            if (x < 0 || x >= WIDTH)
                continue; /* 越界列不产生数据 */
            uint8_t src = fb[page * WIDTH + x];
            if (invert)
                src = static_cast<uint8_t>(~src);
            if (src == 0)
                continue;

            const int off = off0 + offStep * dx;
            for (int m = 0; m < 8; ++m) {
                if (((src >> m) & 0x01u) == 0)
                    continue;
                const int k = 48 * page + 6 * m + off + kShift;
                if (k < 0 || k >= SCAN_BYTES * 8)
                    continue; /* 越界（仅 T43 的 kShift 可能）不产生数据 */
                out[k >> 3] |= static_cast<uint8_t>(1u << (k & 7));
            }
        }
    }
}

void packFrame(const uint8_t *fb, bool invert, uint8_t *out)
{
    for (int scan = 0; scan < SCANS_PER_FRAME; ++scan)
        packScan(fb, scan, invert, out + scan * SCAN_BYTES);
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
