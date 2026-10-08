/*
 * 极简 Arduino 兼容层 —— 让 Adafruit_GFX v1.2.3 能在"裸 Pico SDK"（或宿主机 g++）
 * 上编译，而不需要引入完整 Arduino 框架。
 *
 * 只提供 Adafruit_GFX 与 VFD_GP1211AI 实际用到的部分：
 *   类型（boolean/byte）、PROGMEM/pgm_read_*、F()、HIGH/LOW/OUTPUT、min/max/abs、Print。
 *
 * 注意：
 *   1. min/max/abs 用内联模板实现而不是 Arduino 的宏，避免污染 Pico SDK 的
 *      第三方头文件（宏会破坏 std::numeric_limits<T>::max() 之类的写法）。
 *   2. 在同时使用 Pico SDK 头文件的 .cpp 里，请先 include SDK 头、再 include 本文件。
 */
#ifndef VFD_COMPAT_ARDUINO_H
#define VFD_COMPAT_ARDUINO_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ---------------------------------------------------------------- 基础类型 */
typedef bool    boolean;
typedef uint8_t byte;

/* Adafruit_GFX v1.2.3 用 ARDUINO >= 100 选择 write() 的签名，必须定义 */
#ifndef ARDUINO
#define ARDUINO 10800
#endif

/* ------------------------------------------------- RP2040 上 Flash 是内存映射的 */
#ifndef PROGMEM
#define PROGMEM
#endif
/* pgm_read_* ：Flash 已内存映射，直接解引用即可。
 * 用 may_alias 的 typedef 而不是裸指针转换：Adafruit_GFX 的字体表会拿 GFXglyph 指针
 * 去读 dword，裸转换会触发 -Wstrict-aliasing（严格别名 UB），加 may_alias 后语义正确且零开销。 */
#ifndef pgm_read_byte
typedef unsigned char __attribute__((may_alias)) pgm_byte_t;
#define pgm_read_byte(addr) (*(const pgm_byte_t *)(addr))
#endif
#ifndef pgm_read_word
typedef unsigned short __attribute__((may_alias)) pgm_word_t;
#define pgm_read_word(addr) (*(const pgm_word_t *)(addr))
#endif
#ifndef pgm_read_dword
typedef unsigned long __attribute__((may_alias)) pgm_dword_t;
#define pgm_read_dword(addr) (*(const pgm_dword_t *)(addr))
#endif

/* --------------------------------------------------------------- 引脚电平 */
#ifndef HIGH
#define HIGH 0x1
#define LOW 0x0
#define INPUT 0x0
#define OUTPUT 0x1
#define INPUT_PULLUP 0x2
#endif

#ifndef _BV
#define _BV(bit) (1UL << (bit))
#endif

/* ------------------------------------------------------------------ min/max */
/* Arduino 用宏；这里用内联模板，语义一致但不会污染其它头文件 */
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

template <typename T, typename U>
constexpr auto min(T a, U b) -> decltype(a < b ? a : b) { return (a < b) ? a : b; }
template <typename T, typename U>
constexpr auto max(T a, U b) -> decltype(a > b ? a : b) { return (a > b) ? a : b; }

inline long map(long x, long in_min, long in_max, long out_min, long out_max)
{
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

/* --------------------------------------------------- F() / Flash 字符串辅助 */
class __FlashStringHelper;
#define F(string_literal) (reinterpret_cast<const __FlashStringHelper *>(string_literal))
#define PSTR(string_literal) (string_literal)

/* -------------------------------------------------------------------- Print */
#include "Print.h"

#endif /* VFD_COMPAT_ARDUINO_H */
