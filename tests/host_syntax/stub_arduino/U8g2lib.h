/* U8g2 形状桩（只覆盖本工程 examples 下 .ino 用到的那部分 API） */
#ifndef VFD_STUB_U8G2LIB_H
#define VFD_STUB_U8G2LIB_H

#include "Arduino.h"

#define U8G2_R0 0
#define U8G2_R1 1
#define U8G2_R2 2
#define U8G2_R3 3
#define U8X8_PIN_NONE 255

/* 用到的字体符号（真实库里是 const uint8_t[] 数组） */
extern const uint8_t u8g2_font_5x7_tf[];
extern const uint8_t u8g2_font_6x12_tf[];
extern const uint8_t u8g2_font_7x14_tf[];
extern const uint8_t u8g2_font_10x20_tf[];

class U8G2 {
public:
    void begin(void);
    void setBusClock(uint32_t clock);
    void clearBuffer(void);
    void sendBuffer(void);
    void setFont(const uint8_t *font);
    void setDrawColor(uint8_t color);
    void setCursor(int16_t x, int16_t y);
    void drawPixel(int16_t x, int16_t y);
    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1);
    void drawFrame(int16_t x, int16_t y, int16_t w, int16_t h);
    void drawBox(int16_t x, int16_t y, int16_t w, int16_t h);
    void drawDisc(int16_t x0, int16_t y0, int16_t r);
    void drawStr(int16_t x, int16_t y, const char *s);
    uint8_t *getBufferPtr(void);
    /* 真实 U8g2lib.h（2.35.30）把 getBufferSize() 放在 #ifdef U8G2_USE_DYNAMIC_ALLOC 里，
     * 默认**不存在**；本桩照抄这个条件 —— 否则会出现"桩里能编、真机编不过"的假绿灯
     * （2026-10-06 漏掉 pico2_full_test 里的 getBufferSize() 就是这个原因）。 */
#ifdef U8G2_USE_DYNAMIC_ALLOC
    uint16_t getBufferSize(void);
#endif
    /* 这两个一定存在；可用它自己算缓冲区大小：tile 宽 × 8 × tile 高 */
    uint8_t getBufferTileWidth(void);
    uint8_t getBufferTileHeight(void);
};

/* 真实库里 U8G2_SSD1306_128X64_NONAME_F_4W_HW_SPI 是 U8G2 的派生类；
 * 这里直接给个同名类型，构造参数与真实一致即可（rotation, cs, dc, reset） */
class U8G2_SSD1306_128X64_NONAME_F_4W_HW_SPI : public U8G2 {
public:
    U8G2_SSD1306_128X64_NONAME_F_4W_HW_SPI(uint8_t rotation, uint8_t cs, uint8_t dc, uint8_t reset)
    {
        (void)rotation;
        (void)cs;
        (void)dc;
        (void)reset;
    }
};

#endif /* VFD_STUB_U8G2LIB_H */
