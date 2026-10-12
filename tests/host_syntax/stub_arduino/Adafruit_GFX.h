/*
 * Adafruit_GFX 形状桩 —— 仅用于 examples 下 .ino 的**宿主机语法检查**（同 stub_arduino/Arduino.h）。
 *
 * 签名对照真实库 Adafruit-GFX-Library 1.12.6 的 Adafruit_GFX.h，只保留 sketch 用到的部分。
 * ⚠️ 这里只做声明（-fsyntax-only 不链接），所以不需要实现体；但签名/默认参数必须与真库一致，
 *    否则会把"真库能编过"的写法误判成错误。
 */
#ifndef VFD_STUB_ADAFRUIT_GFX_H
#define VFD_STUB_ADAFRUIT_GFX_H

#include "Arduino.h"

/* 真库里这两个宏同时存在于 Adafruit_GFX.h（GFX 几何基元）与 Adafruit_SSD1306.h（同值） */
#ifndef SSD1306_BLACK
#define SSD1306_BLACK 0
#endif
#ifndef SSD1306_WHITE
#define SSD1306_WHITE 1
#endif
#ifndef SSD1306_INVERSE
#define SSD1306_INVERSE 2
#endif

class Adafruit_GFX : public Print {
public:
    Adafruit_GFX(int16_t w, int16_t h) : WIDTH(w), HEIGHT(h) { }
    virtual ~Adafruit_GFX() { }

    /* 纯虚：由具体驱动实现 */
    virtual void drawPixel(int16_t x, int16_t y, uint16_t color) = 0;

    /* 事务式写入（SPITFT 驱动用，sketch 里不直接调） */
    virtual void startWrite(void);
    virtual void endWrite(void);
    virtual void writePixel(int16_t x, int16_t y, uint16_t color);
    virtual void writeFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color);
    virtual void writeFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color);
    virtual void writeFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);

    /* 几何基元 */
    void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color);
    void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color);
    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);
    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
    void drawCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color);
    void fillCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color);
    void drawBitmap(int16_t x, int16_t y, const uint8_t bitmap[], int16_t w, int16_t h, uint16_t color);
    void fillScreen(uint16_t color);

    /* 文本 */
    void setCursor(int16_t x, int16_t y);
    void setTextColor(uint16_t c);
    void setTextColor(uint16_t c, uint16_t bg);
    void setTextSize(uint8_t s);
    void setTextSize(uint8_t sx, uint8_t sy);
    void setTextWrap(bool w);
    int16_t getCursorX(void) const;
    int16_t getCursorY(void) const;

    int16_t width(void) const { return WIDTH; }
    int16_t height(void) const { return HEIGHT; }

protected:
    int16_t WIDTH;
    int16_t HEIGHT;
    int16_t cursor_x;
    int16_t cursor_y;
    uint8_t textsize_x;
    uint8_t textsize_y;
    uint16_t textcolor;
    uint16_t textbgcolor;
    bool wrap;
};

#endif /* VFD_STUB_ADAFRUIT_GFX_H */
