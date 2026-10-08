#include "demo_screens.h"

namespace vfd_demo {

void drawBootScreen(VFD_GP1211AI &display)
{
    display.clearDisplay();

    /* 双线边框 */
    display.drawRect(0, 0, vfd::WIDTH, vfd::HEIGHT, WHITE);
    display.drawRect(2, 2, vfd::WIDTH - 4, vfd::HEIGHT - 4, WHITE);

    display.setTextSize(1);
    display.setTextColor(WHITE);

    display.setCursor(8, 8);
    display.println(F("GP1211AI  VFD"));

    display.setCursor(8, 20);
    display.println(F("128x64 / 44grid"));
    display.setCursor(8, 30);
    display.println(F("RP2040 + Pico SDK"));

    display.drawFastHLine(6, 42, vfd::WIDTH - 12, WHITE);

    display.setCursor(8, 48);
    display.print(F("scan ok  "));
    display.drawCircle(vfd::WIDTH - 14, 52, 5, WHITE);
    display.fillCircle(vfd::WIDTH - 14, 52, 2, WHITE);

    /* ---- 校准刻度（用于实物照片核对：任何"列重复/列交换/行错位"都会破坏刻度） ----
     * 底部：每 8 列一个点，每 32 列一个 3 点高的长刻度 ⇒ 照片上可直接数列。
     * 左侧：每 8 行一个点，每 32 行一个 3 点宽的长刻度 ⇒ 可直接数行。 */
    for (int x = 4; x < vfd::WIDTH - 4; x += 8)
        display.drawPixel(x, 57, WHITE);
    for (int x = 4; x < vfd::WIDTH - 4; x += 32)
        for (int y = 55; y <= 57; ++y)
            display.drawPixel(x, y, WHITE);
    for (int y = 4; y < vfd::HEIGHT - 4; y += 8)
        display.drawPixel(4, y, WHITE);
    for (int y = 4; y < vfd::HEIGHT - 4; y += 32)
        for (int x = 4; x <= 6; ++x)
            display.drawPixel(x, y, WHITE);
}

void drawAnimationFrame(VFD_GP1211AI &display, uint32_t frame)
{
    display.clearDisplay();

    display.drawRect(0, 0, vfd::WIDTH, vfd::HEIGHT, WHITE);

    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(4, 4);
    display.print(F("frame "));
    display.print(static_cast<unsigned long>(frame));

    /* 弹跳方块（左右往返） */
    const int span = vfd::WIDTH - 24;
    const int pos = static_cast<int>(frame % static_cast<uint32_t>(2 * span));
    const int x = (pos < span) ? pos : (2 * span - pos);
    display.fillRect(x, 18, 20, 14, WHITE);
    display.drawRect(x, 18, 20, 14, BLACK);

    /* 右上角呼吸圆环 */
    const int radius = 3 + static_cast<int>(frame % 7);
    display.drawCircle(vfd::WIDTH - 16, 16, radius, WHITE);

    /* 底部台阶波形 */
    for (int i = 2; i < vfd::WIDTH - 2; ++i) {
        const int y = 52 + (static_cast<int>(frame) + i / 4) % 8;
        display.drawPixel(i, y, WHITE);
        display.drawPixel(i, y + 1, WHITE);
    }
}

} /* namespace vfd_demo */
