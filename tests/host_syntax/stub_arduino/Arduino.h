/*
 * Arduino API 形状桩 —— 仅用于**主控测试程序**（examples 下的 .ino）的宿主机语法检查。
 *
 * 本机没装 arduino-cli（见 machine-environment skill），所以用一个"形状正确"的桩把
 * sketch 编译一遍，提前抓拼写/API 用法错误，避免用户烧到 Pico 2 上才发现编不过。
 *
 * ⚠️ 这里的签名必须与 arduino-pico 核心（5.5.1）+ U8g2（olikraus）真实头文件一致；
 *    改动 stub 时请对照：
 *      %LOCALAPPDATA%\Arduino15\packages\rp2040\hardware\rp2040\<ver>\...
 *      %USERPROFILE%\Documents\Arduino\libraries\U8g2\src\U8g2lib.h
 */
#ifndef VFD_STUB_ARDUINO_H
#define VFD_STUB_ARDUINO_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2

#define F(x) (x)

typedef uint8_t byte;

/* arduino-pico 的 variants/generic/common.h 会预定义这几个脚（Pico/Pico 2 的 SPI0）。
 * 本桩照抄，原因有二：
 *   ① examples 里的 sketch 直接用 SCK/MOSI，没有它们就编不过；
 *   ② 它们是**对象名**（static const uint8_t），sketch 里再定义同名变量会 conflicting
 *      declaration —— 这正是 examples/pico2_bitbang_wiring_test 注释里踩过的坑。 */
static const uint8_t PIN_SPI0_MISO = 16;
static const uint8_t PIN_SPI0_MOSI = 19;
static const uint8_t PIN_SPI0_SCK = 18;
static const uint8_t SS = 17;
static const uint8_t MISO = PIN_SPI0_MISO;
static const uint8_t MOSI = PIN_SPI0_MOSI;
static const uint8_t SCK = PIN_SPI0_SCK;

void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t value);
int digitalRead(uint8_t pin);
void delay(uint32_t ms);
void delayMicroseconds(uint32_t us);
uint32_t millis(void);
uint32_t micros(void);

class Print {
public:
    void print(const char *s);
    void print(int v);
    void print(unsigned int v);
    void print(long v);
    void print(unsigned long v);
    void print(char c);
    void println(void);
    void println(const char *s);
    void println(int v);
    void println(unsigned int v);
    void println(long v);
    void println(unsigned long v);
};

class Stream : public Print {
public:
    int available();
    int read();
    int peek();
};

class SerialClass : public Stream {
public:
    void begin(unsigned long baud);
    void end();
    void flush();
};

extern SerialClass Serial;

#endif /* VFD_STUB_ARDUINO_H */
