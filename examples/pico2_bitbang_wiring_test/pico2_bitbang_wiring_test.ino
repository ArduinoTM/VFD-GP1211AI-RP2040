/*
 * 位翻转接线测试（Pico 2 → SSD1306 模拟器）
 *
 * 目的：完全绕开 u8g2 与硬件 SPI，用最慢的速度手工发命令字节，
 *       判定"主控 → 模拟器"的四根线到底通不通。
 *
 * 判读（看模拟器的串口统计行）：
 *   发完这些命令后 cmd 应 ≥4、disp=1；若始终 cmd=0 ⇒ 是接线 / 共地问题。
 *
 * ⚠️ 命名：arduino-pico 核心已在 variants/generic/common.h 预定义了
 *    SCK / MOSI / MISO / SS / SDA / SCL / LED_BUILTIN 等常量，
 *    所以这里**不能**再定义同名的 SCK/MOSI/CS/DC（上一版报的正是这个冲突）。
 *    SCK(=18)、MOSI(=19) 直接复用核心的预定义值；CS/DC 改用 csPin/dcPin。
 *
 * 接线：
 *   Pico2 GP18 → 模拟器 GP11 (SCLK)
 *   Pico2 GP19 → 模拟器 GP12 (MOSI)
 *   Pico2 GP15 → 模拟器 GP14 (CS)     ← 悬空会导致从机永不启动（典型 0 字节）
 *   Pico2 GP14 → 模拟器 GP13 (DC)     ← 必须是模拟器的 GP13
 *   Pico2 GND  → 模拟器 GND           ← 必须共地
 *   模拟器 GP16 悬空/上电为高 = SSD1306 模式
 */

#include <Arduino.h>

static const uint8_t csPin = 15;   /* → 模拟器 GP14 */
static const uint8_t dcPin = 14;   /* → 模拟器 GP13 */
/* SCK = 18、MOSI = 19：用核心预定义常量（Pico / Pico 2 的 SPI0 默认脚） */

/* SPI mode 0：SCK 空闲低，数据在 SCK 上升沿被采样，MSB first */
static void sendByte(uint8_t v)
{
    for (int i = 7; i >= 0; --i) {
        digitalWrite(MOSI, (v >> i) & 1u);
        delayMicroseconds(200);
        digitalWrite(SCK, HIGH);
        delayMicroseconds(200);
        digitalWrite(SCK, LOW);
        delayMicroseconds(200);
    }
}

static void sendCmd(uint8_t c)
{
    digitalWrite(dcPin, LOW);      /* DC = 低 → 命令 */
    digitalWrite(csPin, LOW);
    delayMicroseconds(50);
    sendByte(c);
    delayMicroseconds(50);
    digitalWrite(csPin, HIGH);
    delay(2);
}

void setup()
{
    Serial.begin(115200);
    delay(1500);
    Serial.println();
    Serial.println(F("=== bitbang wiring test (Pico 2 -> SSD1306 emulator) ==="));
    Serial.println(F("wiring: GP18->emu GP11(SCK)  GP19->emu GP12(MOSI)"));
    Serial.println(F("        GP15->emu GP14(CS)   GP14->emu GP13(DC)   GND->GND"));

    pinMode(SCK, OUTPUT);
    pinMode(MOSI, OUTPUT);
    pinMode(csPin, OUTPUT);
    pinMode(dcPin, OUTPUT);
    digitalWrite(SCK, LOW);
    digitalWrite(MOSI, LOW);
    digitalWrite(csPin, HIGH);     /* CS 空闲高 */
    digitalWrite(dcPin, LOW);
    delay(200);

    Serial.println(F("cmd 0xAE (display OFF) -> 模拟器 cmd 应 +1"));
    sendCmd(0xAE);
    delay(1000);

    Serial.println(F("cmd 0xAF (display ON)  -> cmd 再 +1, disp=1"));
    sendCmd(0xAF);
    delay(1000);

    Serial.println(F("cmd 0xA7 (invert)      -> 屏应整屏点亮（数据通路也 OK）"));
    sendCmd(0xA7);
    delay(1000);

    Serial.println(F("cmd 0xA6 (normal)"));
    sendCmd(0xA6);
    delay(1000);

    Serial.println(F("done. 若模拟器计数仍为 0：先查 CS->GP14、SCK->GP11、MOSI->GP12 与共地"));
}

void loop()
{
    static uint32_t t = 0;
    if (millis() - t > 5000) {
        t = millis();
        Serial.println(F("(idle) 等待核对模拟器串口的 cmd/data 计数"));
    }
}
