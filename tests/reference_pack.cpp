#include "reference_pack.h"

namespace ref {

#define R(page, col) readFb(fb, send, (page), (col))

void packFrame(const uint8_t *fb, uint8_t *send)
{
    uint8_t i = 0;
    uint8_t grid;
    uint8_t grid_tmp = 0;
    uint8_t ram_tmp = 0;
    uint8_t ram_base_addr = 0;
    uint8_t *buf_ptr;

    buf_ptr = send;

    for (grid = 0; grid < 43; grid++) {
        grid_tmp = 42 - grid;

        ram_base_addr = (grid_tmp >> 1) * 6;
        for (i = 0; i < 8; i++) {
            if (grid_tmp & 0x01) // Even number series
            {
                ram_tmp = ((R(i, ram_base_addr + 5) << 1) & 0x02);
                ram_tmp |= ((R(i, ram_base_addr + 4) << 3) & 0x08);
                ram_tmp |= ((R(i, ram_base_addr + 3) << 5) & 0x20);
                ram_tmp |= ((R(i, ram_base_addr + 5) << 6) & 0x80);
                *buf_ptr = ram_tmp;
                ++buf_ptr;
                ram_tmp = R(i, ram_base_addr + 4) & 0x02;
                ram_tmp |= ((R(i, ram_base_addr + 3) << 2) & 0x08);
                ram_tmp |= ((R(i, ram_base_addr + 5) << 3) & 0x20);
                ram_tmp |= ((R(i, ram_base_addr + 4) << 5) & 0x80);
                *buf_ptr = ram_tmp;
                ++buf_ptr;
                ram_tmp = ((R(i, ram_base_addr + 3) >> 1) & 0x02);
                ram_tmp |= R(i, ram_base_addr + 5) & 0x08;
                ram_tmp |= ((R(i, ram_base_addr + 4) << 2) & 0x20);
                ram_tmp |= ((R(i, ram_base_addr + 3) << 4) & 0x80);
                *buf_ptr = ram_tmp;
                ++buf_ptr;
                ram_tmp = ((R(i, ram_base_addr + 5) >> 3) & 0x02);
                ram_tmp |= ((R(i, ram_base_addr + 4) >> 1) & 0x08);
                ram_tmp |= ((R(i, ram_base_addr + 3) << 1) & 0x20);
                ram_tmp |= ((R(i, ram_base_addr + 5) << 2) & 0x80);
                *buf_ptr = ram_tmp;
                ++buf_ptr;
                ram_tmp = ((R(i, ram_base_addr + 4) >> 4) & 0x02);
                ram_tmp |= ((R(i, ram_base_addr + 3) >> 2) & 0x08);
                ram_tmp |= ((R(i, ram_base_addr + 5) >> 1) & 0x20);
                ram_tmp |= ((R(i, ram_base_addr + 4) << 1) & 0x80);
                *buf_ptr = ram_tmp;
                ++buf_ptr;
                ram_tmp = ((R(i, ram_base_addr + 3) >> 5) & 0x02);
                ram_tmp |= ((R(i, ram_base_addr + 5) >> 4) & 0x08);
                ram_tmp |= ((R(i, ram_base_addr + 4) >> 2) & 0x20);
                ram_tmp |= R(i, ram_base_addr + 3) & 0x80;
                *buf_ptr = ram_tmp;
                ++buf_ptr;
            } else // Odd number series
            {
                ram_tmp = R(i, ram_base_addr + 0) & 0x01;
                ram_tmp |= (R(i, ram_base_addr + 1) << 2 & 0x04);
                ram_tmp |= (R(i, ram_base_addr + 2) << 4 & 0x10);
                ram_tmp |= (R(i, ram_base_addr + 0) << 5 & 0x40);
                *buf_ptr = ram_tmp;
                ++buf_ptr;
                ram_tmp = (R(i, ram_base_addr + 1) >> 1 & 0x01);
                ram_tmp |= (R(i, ram_base_addr + 2) << 1 & 0x04);
                ram_tmp |= (R(i, ram_base_addr + 0) << 2 & 0x10);
                ram_tmp |= (R(i, ram_base_addr + 1) << 4 & 0x40);
                *buf_ptr = ram_tmp;
                ++buf_ptr;
                ram_tmp = (R(i, ram_base_addr + 2) >> 2 & 0x01);
                ram_tmp |= (R(i, ram_base_addr + 0) >> 1 & 0x04);
                ram_tmp |= (R(i, ram_base_addr + 1) << 1 & 0x10);
                ram_tmp |= (R(i, ram_base_addr + 2) << 3 & 0x40);
                *buf_ptr = ram_tmp;
                ++buf_ptr;
                ram_tmp = (R(i, ram_base_addr + 0) >> 4 & 0x01);
                ram_tmp |= (R(i, ram_base_addr + 1) >> 2 & 0x04);
                ram_tmp |= R(i, ram_base_addr + 2) & 0x10;
                ram_tmp |= (R(i, ram_base_addr + 0) << 1 & 0x40);
                *buf_ptr = ram_tmp;
                ++buf_ptr;
                ram_tmp = (R(i, ram_base_addr + 1) >> 5 & 0x01);
                ram_tmp |= (R(i, ram_base_addr + 2) >> 3 & 0x04);
                ram_tmp |= (R(i, ram_base_addr + 0) >> 2 & 0x10);
                ram_tmp |= R(i, ram_base_addr + 1) & 0x40;
                *buf_ptr = ram_tmp;
                ++buf_ptr;
                ram_tmp = (R(i, ram_base_addr + 2) >> 6 & 0x01);
                ram_tmp |= (R(i, ram_base_addr + 0) >> 5 & 0x04);
                ram_tmp |= (R(i, ram_base_addr + 1) >> 3 & 0x10);
                ram_tmp |= (R(i, ram_base_addr + 2) >> 1 & 0x40);
                *buf_ptr = ram_tmp;
                ++buf_ptr;
            }
        }
    }
}

/* 每个 6 槽组按位反转（原驱动槽序 <-> 新默认槽序；见头文件说明） */
void mirrorWithinHalf(uint8_t *frame)
{
    /* 每 6 槽组（k = 48*page + 6*m + off）内做位置换：off 5↔1、4↔0（2、3 不动）。
     * 组按 k % 6 == 0 对齐，因此对每 6 个连续 bit 处理一次。 */
    const int totalBits = 43 * 48 * 8;
    for (int base = 0; base < totalBits; base += 6) {
        const int pairs[][2] = { { 5, 1 }, { 4, 0 } };
        for (const auto &pr : pairs) {
            const int i = base + pr[0];
            const int j = base + pr[1];
            const int bi = i >> 3, bj = j >> 3;
            const uint8_t mi = static_cast<uint8_t>(1u << (i & 7));
            const uint8_t mj = static_cast<uint8_t>(1u << (j & 7));
            const bool a = (frame[bi] & mi) != 0;
            const bool b = (frame[bj] & mj) != 0;
            if (a) frame[bj] |= mj; else frame[bj] = static_cast<uint8_t>(frame[bj] & ~mj);
            if (b) frame[bi] |= mi; else frame[bi] = static_cast<uint8_t>(frame[bi] & ~mi);
        }
    }
}

void mirrorBandSlots(uint8_t *frame)
{
    /* 本文件刻意不依赖 vfd_scanpack.h（保持"原驱动 1:1 转录"的独立性），所以这里写字面量：
     * 43 个扫描 × 48 字节/扫描 × 8 页，每页 8 个 m、每个 m 占 6 个槽位（共 384 位/扫描）。 */
    const int SCANS = 43;
    const int BYTES = 48;
    const int PAGES_N = 8;
    for (int s = 0; s < SCANS; ++s) {
        uint8_t *scan = frame + s * BYTES;
        for (int page = 0; page < PAGES_N; ++page) {
            for (int m = 0; m < 8; ++m) {
                const int base = 48 * page + 6 * m;
                for (int b = 0; b < 3; ++b) {
                    const int k1 = base + b, k2 = base + (5 - b);
                    const uint8_t bit1 = static_cast<uint8_t>((scan[k1 >> 3] >> (k1 & 7)) & 1u);
                    const uint8_t bit2 = static_cast<uint8_t>((scan[k2 >> 3] >> (k2 & 7)) & 1u);
                    scan[k1 >> 3] = static_cast<uint8_t>((scan[k1 >> 3] & ~(1u << (k1 & 7))) | (bit2 << (k1 & 7)));
                    scan[k2 >> 3] = static_cast<uint8_t>((scan[k2 >> 3] & ~(1u << (k2 & 7))) | (bit1 << (k2 & 7)));
                }
            }
        }
    }
}

} /* namespace ref */
