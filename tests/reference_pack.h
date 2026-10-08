/*
 * reference_pack —— 原 STM32 驱动 display() 的 1:1 转录，仅用于宿主机对照测试。
 *
 * 来源：VFD-GP1211AI/Libraries/VFD_GP1211AI/VFD_GP1211AI.cpp，函数 display()。
 * 唯一的改写是把 _framebuffer[i][col] 换成 readFb(...)，以便显式复现原版在
 * col == 128 时的越界读行为（i < 7 读到下一页首字节，i == 7 读到 _sendBuffer[0]），
 * 而不是依赖真实的未定义行为。
 */
#ifndef VFD_TEST_REFERENCE_PACK_H
#define VFD_TEST_REFERENCE_PACK_H

#include <stdint.h>

namespace ref {

/* 复现原版对 _framebuffer[i][col] 的访问；col 只会是 128 这种越界值 */
inline uint8_t readFb(const uint8_t *fb, const uint8_t *send, int page, int col)
{
    if (col < 128)
        return fb[page * 128 + col];
    if (page < 7)
        return fb[(page + 1) * 128]; /* 原版：_framebuffer[page][128] == 下一页 [0] */
    return send[0]; /* 原版：读到紧随 _framebuffer 之后的 _sendBuffer[0] */
}

/* 与原版 display() 完全等价的整帧重排；send 必须 >= 2064 字节 */
void packFrame(const uint8_t *fb, uint8_t *send);

/* 把"原驱动槽序"转成"新默认槽序"：每个 6 槽组（k = 48*page + 6*m + off）按位反转。
 * 依据：两套约定的差别恰好是"每 6 槽组镜像"——t 偶 0,2,4 ↔ 5,3,1、t 奇 5,3,1 ↔ 0,2,4
 * ⇒ bit b ↔ bit 5−b。见 src/vfd_scanpack.cpp 的 mode 表与 docs/00 §14.12。 */
void mirrorBandSlots(uint8_t *frame);

/* 把"半带内顺序"镜像：a↔c、d↔f（即 off 5↔1、4↔0；b/e、c… 中位不动）。
 * 依据：手动/Sheet 6 的字母顺序（a,f,b,e,c,d ⇒ off 5,4,3,2,1,0）与面板实际空间顺序相反，
 * 实机症状 = 每个字在原位左右镜像（「（）」→「）（」）⇒ 需要这个置换。 */
void mirrorWithinHalf(uint8_t *frame);

} /* namespace ref */

#endif /* VFD_TEST_REFERENCE_PACK_H */
