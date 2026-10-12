/*
 * Adafruit_SSD1306 形状桩 —— 仅用于 examples 下 .ino 的**宿主机语法检查**。
 *
 * 签名对照真实库 Adafruit_SSD1306 2.5.17（Adafruit_SSD1306.h）+ Adafruit_BusIO 的 SPITFT 用法，
 * 只保留 examples/pico2_adafruit_test 用到的部分：
 *   · 7 参数构造（最后一个就是 SPI bitrate，库默认 8 MHz —— 本工程要求显式给 4 MHz）
 *   · begin()/display()/clearDisplay()/getBuffer()
 *   · protected 的 spiSettings（子类改时钟要用，真库在 Adafruit_SPITFT 里）
 */
#ifndef VFD_STUB_ADAFRUIT_SSD1306_H
#define VFD_STUB_ADAFRUIT_SSD1306_H

#include "Adafruit_GFX.h"
#include "SPI.h"

#define SSD1306_SWITCHCAPVCC 0x2
#define SSD1306_EXTERNALVCC 0x1

#define SSD1306_LCDWIDTH 128
#define SSD1306_LCDHEIGHT 64

class Adafruit_SSD1306 : public Adafruit_GFX {
public:
    Adafruit_SSD1306(uint16_t w, uint16_t h, SPIClass *spi, int8_t dc_pin,
        int8_t rst_pin = -1, int8_t cs_pin = -1, uint32_t bitrate = 8000000UL);
    Adafruit_SSD1306(uint16_t w, uint16_t h, SPIClass *spi, int8_t dc_pin,
        int8_t rst_pin, int8_t cs_pin, uint32_t bitrate, bool reset);
    virtual ~Adafruit_SSD1306() { }

    bool begin(uint8_t switchvcc = SSD1306_SWITCHCAPVCC, uint8_t i2caddr = 0, bool reset = true);

    /* 真库在这里实现 GFX 的纯虚 drawPixel（写 buffer），所以子类才是具体类 */
    void drawPixel(int16_t x, int16_t y, uint16_t color) override;

    void display(void);
    void clearDisplay(void);
    void dim(bool dim);
    void invertDisplay(bool i);
    void startscrollright(uint8_t start, uint8_t stop);
    void startscrollleft(uint8_t start, uint8_t stop);
    void stopscroll(void);
    void ssd1306_command(uint8_t c);

    uint8_t *getBuffer(void) { return buffer; }

protected:
    uint8_t buffer[SSD1306_LCDWIDTH * SSD1306_LCDHEIGHT / 8];
    SPISettings spiSettings; /* 真库在 Adafruit_SPITFT 里；子类靠它改运行期时钟 */
};

#endif /* VFD_STUB_ADAFRUIT_SSD1306_H */
