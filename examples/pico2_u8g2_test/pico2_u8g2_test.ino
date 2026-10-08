/*
 * pico2_u8g2_test.ino —— 用 Raspberry Pi Pico 2（RP2350）+ u8g2 测试本工程的
 *                        "SSD1306 行为模拟"（另一块 RP2040 把自己伪装成一片
 *                        128×64、4 线 SPI、只写的 SSD1306）
 *
 * 环境
 *   Arduino IDE + Earle Philhower 的 "Raspberry Pi Pico/RP2040" 核心（支持 Pico 2 / RP2350）
 *   开发板选 "Raspberry Pi Pico 2"，库管理器装 "U8g2"（olikraus）
 *
 * 接线（主控 Pico 2  →  模拟器 RP2040）
 *   GP18 (SPI0 SCK)  →  GP11  SCLK
 *   GP19 (SPI0 TX)   →  GP12  MOSI
 *   GP14 (普通 GPIO) →  GP13  DC     ⚠️ 必须是模拟器的 GP13（= 它的 MOSI+1）
 *   GP15 (普通 GPIO) →  GP14  CS
 *   GP20 (普通 GPIO) →  GP15  RESET  （可省：不接就把 reset 参数改成 U8X8_PIN_NONE）
 *   GND              →  GND          （必须共地）
 *
 *   模拟器侧：GP16 上电保持高/悬空 ⇒ 进入 SSD1306 模拟模式（串口会打印 "mode: SSD1306 emulator"）
 *
 * 说明
 *   · u8g2 的 SSD1306 4 线硬件 SPI 就是 SPI mode 0（CPOL=0/CPHA=0），与模拟器要求一致；
 *   · 时钟先给 4 MHz（模拟器是 PIO 从机，跟得上；杜邦线长时别硬拉高）；
 *   · 本程序每 2 秒轮换一幅图，专门用几幅"可判读"的图来核对列/行映射、
 *     缺口与左右方向（不对称字形、刻度、对角线、棋盘）。
 */

#include <Arduino.h>
#include <SPI.h>
#include <U8g2lib.h>

/* 全缓冲 4 线硬件 SPI。参数：旋转、CS 脚、DC 脚、RESET 脚 */
U8G2_SSD1306_128X64_NONAME_F_4W_HW_SPI u8g2(U8G2_R0, /*cs=*/15, /*dc=*/14, /*reset=*/20);

/* 若不想接 RESET：改成
 * U8G2_SSD1306_128X64_NONAME_F_4W_HW_SPI u8g2(U8G2_R0, 15, 14, U8X8_PIN_NONE);
 */

static uint32_t screenIndex = 0;
static uint32_t frameCount  = 0;

/* 一幅图内部用到的通用小工具：底部显示第几幅 + 帧号 */
static void drawFooter(const char *name)
{
    u8g2.setFont(u8g2_font_5x7_tf);
    char buf[48];
    snprintf(buf, sizeof(buf), "#%lu %s f%lu", (unsigned long)screenIndex, name,
             (unsigned long)frameCount);
    u8g2.drawStr(2, 63, buf);
}

/* 0. 常规自检：边框、文字、圆、分隔线 */
static void screenBasic()
{
    u8g2.drawFrame(0, 0, 128, 64);
    u8g2.drawFrame(2, 2, 124, 60);
    u8g2.setFont(u8g2_font_7x14_tf);
    u8g2.drawStr(6, 20, "Pico2 + u8g2");
    u8g2.drawStr(6, 36, "SSD1306 emu");
    u8g2.drawDisc(108, 26, 9);
    u8g2.drawLine(6, 44, 121, 44);
    drawFooter("basic");
}

/* 1. 列刻度：每 8 列一个点（y=32），每 32 列一个 3 点高的长刻度
 *    ⇒ 可在屏上直接读出"哪一列不亮 / 是否整体偏 3 列" */
static void screenColumnRuler()
{
    u8g2.setFont(u8g2_font_6x12_tf);
    u8g2.drawStr(2, 12, "column ruler");
    u8g2.drawLine(0, 16, 127, 16);
    for (int x = 0; x < 128; x += 8) {
        if (x % 32 == 0)
            u8g2.drawLine(x, 28, x, 36);   /* 长刻度 */
        else
            u8g2.drawPixel(x, 32);         /* 短刻度 */
    }
    u8g2.drawLine(0, 44, 127, 44);
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(2, 52, "0    32    64    96  127");
    drawFooter("cols");
}

/* 2. 行刻度：每 8 行一个点（x=64），每 32 行一个 3 点宽的长刻度 */
static void screenRowRuler()
{
    u8g2.setFont(u8g2_font_6x12_tf);
    u8g2.drawStr(2, 10, "row ruler");
    for (int y = 0; y < 64; y += 8) {
        if (y % 32 == 0)
            u8g2.drawLine(60, y, 68, y);
        else
            u8g2.drawPixel(64, y);
    }
    u8g2.drawLine(80, 0, 80, 63);
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(84, 12, "8 rows/step");
    u8g2.drawStr(84, 26, "long=32");
    drawFooter("rows");
}

/* 3. 不对称字形：检验"左右是否镜像/交换"最直接的一幅
 *    （'(' ')' '[' ']' '<' '>' 与 b/d、p/q 一旦镜像极易看出来） */
static void screenAsymmetric()
{
    u8g2.setFont(u8g2_font_10x20_tf);
    u8g2.drawStr(4, 22, "()[]<>");
    u8g2.drawStr(4, 46, "bd pq 47");
    u8g2.setFont(u8g2_font_6x12_tf);
    u8g2.drawStr(78, 22, "left/right");
    u8g2.drawStr(78, 38, "must be");
    u8g2.drawStr(78, 52, "readable");
    drawFooter("asym");
}

/* 4. 对角线与网格交叉：核对行列同时映射（错位/交换会立刻歪） */
static void screenDiagonal()
{
    u8g2.drawLine(0, 0, 127, 63);
    u8g2.drawLine(127, 0, 0, 63);
    u8g2.drawFrame(0, 0, 128, 64);
    for (int x = 16; x < 128; x += 32) u8g2.drawLine(x, 0, x, 63);
    for (int y = 16; y < 64; y += 16) u8g2.drawLine(0, y, 127, y);
    drawFooter("diag");
}

/* 5. 棋盘格：8×8 点阵，位序/相邻列问题一眼可见 */
static void screenChecker()
{
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 128; x++)
            if (((x >> 3) + (y >> 3)) & 1)
                u8g2.drawPixel(x, y);
    u8g2.setDrawColor(0);
    u8g2.drawBox(16, 16, 96, 32);
    u8g2.setDrawColor(1);
    u8g2.setFont(u8g2_font_7x14_tf);
    u8g2.drawStr(22, 38, "checker 8px");
    drawFooter("checker");
}

/* 6. 反显块（前景/背景反转）+ 计数，确认 GDDRAM 全写与刷新都正常 */
static void screenInvert()
{
    u8g2.drawBox(0, 0, 128, 64);
    u8g2.setDrawColor(0);
    u8g2.drawFrame(4, 4, 120, 56);
    u8g2.setFont(u8g2_font_7x14_tf);
    u8g2.drawStr(10, 28, "inverted");
    char buf[24];
    snprintf(buf, sizeof(buf), "frame %lu", (unsigned long)frameCount);
    u8g2.drawStr(10, 46, buf);
    u8g2.setDrawColor(1);
    drawFooter("invert");
}

void setup()
{
    Serial.begin(115200);
    delay(1500);
    Serial.println();
    Serial.println(F("=== Pico 2 -> GP1211AI SSD1306 emulator (u8g2) ==="));
    Serial.println(F("wiring: GP18->SCK GP19->MOSI GP14->DC GP15->CS GP20->RESET GND->GND"));
    Serial.println(F("emulator side: GP16 must be HIGH at power-up (SSD1306 mode)"));

    /* 默认就是 SPI0 的 GP18(SCK)/GP19(TX)；若要换脚，取消下面两行注释即可：
     * 注意用到的必须是 SPI0 的合法脚（SCK: 2/6/18/22，TX: 3/7/19/23） */
    // SPI.setSCK(18);
    // SPI.setTX(19);

    u8g2.begin();
    u8g2.setBusClock(4000000);   /* 4 MHz 起步；稳定后可试 8000000 */
    Serial.println(F("u8g2 ready, SPI mode 0 @4 MHz"));
}

void loop()
{
    u8g2.clearBuffer();
    switch (screenIndex % 7) {
        case 0: screenBasic();         break;
        case 1: screenColumnRuler();   break;
        case 2: screenRowRuler();      break;
        case 3: screenAsymmetric();    break;
        case 4: screenDiagonal();      break;
        case 5: screenChecker();       break;
        default: screenInvert();       break;
    }
    u8g2.sendBuffer();             /* 1 KB GDDRAM 一次性写入模拟器 */

    static const char *names[] = {"basic", "cols", "rows", "asym", "diag", "checker", "invert"};
    Serial.print(F("screen #"));
    Serial.print(screenIndex);
    Serial.print(F(" ("));
    Serial.print(names[screenIndex % 7]);
    Serial.println(F(") sent"));

    frameCount++;
    screenIndex++;
    delay(2000);                   /* 每 2 秒换一幅，方便肉眼与拍照核对 */
}
