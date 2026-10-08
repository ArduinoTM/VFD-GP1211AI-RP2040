/*
 * vfd_crc32.h —— CRC32（IEEE 802.3 / zlib 同款：poly 0xEDB88320、初值/末值取反）
 *
 * 用途：**两端对照**的端到端判据。主控发完一帧后算出"它写进 GDDRAM 的 1024 B 的 CRC32"，
 * 从机每秒打印自己 GDDRAM 的 CRC32 —— 数字一样就说明**每一个字节**都落对了位置，
 * 不需要相机、不需要示波器（见 docs/05 §8.7 与 examples/pico2_full_test）。
 *
 * 同一份算法在三个地方各有一份拷贝（不能共享头文件）：
 *   · 从机固件：本文件（ssd1306_emulator 的 gdramCrc32()）
 *   · 主控测试程序：examples/pico2_full_test/pico2_full_test.ino 内的同名函数
 *   · 宿主机测试：tests/test_ssd1306.cpp（用标准向量与外部参考值校验）
 * 三处都用标准向量 CRC32("123456789") == 0xCBF43926 自检，任何一处不一致都会立刻暴露。
 */
#ifndef VFD_CRC32_H
#define VFD_CRC32_H

#include <stdint.h>

namespace vfd {

constexpr uint32_t CRC32_POLY = 0xEDB88320u;

/* 标准向量（用外部工具算过：Node zlib.crc32 / zlib crc32 均为这两个值）：
 *   CRC32("")          = 0x00000000
 *   CRC32("123456789") = 0xCBF43926
 *   CRC32(1024 B 参考图) = 0xCCE79D14（见 examples/pico2_full_test 的 buildImage()） */
constexpr uint32_t CRC32_CHECK_EMPTY = 0x00000000u;
constexpr uint32_t CRC32_CHECK_123456789 = 0xCBF43926u;
constexpr uint32_t CRC32_CHECK_REF_IMAGE = 0xCCE79D14u;

inline uint32_t crc32Update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (CRC32_POLY & (0u - (crc & 1u)));
    }
    return crc;
}

inline uint32_t crc32(const uint8_t *data, uint32_t len)
{
    return crc32Update(0xFFFFFFFFu, data, len) ^ 0xFFFFFFFFu;
}

} /* namespace vfd */

#endif /* VFD_CRC32_H */
