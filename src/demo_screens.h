/*
 * demo_screens —— 示例画面（不含任何平台相关代码，宿主机测试也能编译运行）。
 */
#ifndef VFD_DEMO_SCREENS_H
#define VFD_DEMO_SCREENS_H

#include <stdint.h>

#include "vfd_gp1211ai.h"

namespace vfd_demo {

/* 开机自检画面 */
void drawBootScreen(VFD_GP1211AI &display);

/* 第 frame 帧动画：文字 + 弹跳方块 + 圆环 + 底部台阶 */
void drawAnimationFrame(VFD_GP1211AI &display, uint32_t frame);

} /* namespace vfd_demo */

#endif /* VFD_DEMO_SCREENS_H */
