/* SPI 形状桩（arduino-pico 5.5.1 的 SPIClassRP2040 / HardwareSPI.h） */
#ifndef VFD_STUB_SPI_H
#define VFD_STUB_SPI_H

#include "Arduino.h"

#define MSBFIRST 1
#define LSBFIRST 0
#define SPI_MODE0 0x00
#define SPI_MODE1 0x01
#define SPI_MODE2 0x02
#define SPI_MODE3 0x03

class SPISettings {
public:
    SPISettings() { }
    SPISettings(uint32_t clock, uint8_t bitOrder, uint8_t dataMode)
    {
        (void)clock;
        (void)bitOrder;
        (void)dataMode;
    }
};

class SPIClass {
public:
    void begin();
    void end();
    void beginTransaction(SPISettings settings);
    void endTransaction(void);
    uint8_t transfer(uint8_t data);
    uint16_t transfer16(uint16_t data);
    void transfer(void *buf, size_t count);                 /* 双向：会把 RX 写回 buf（官方如此） */
    void transfer(const void *txbuf, void *rxbuf, size_t count); /* 3 参数版：rxbuf 可为 nullptr = 只发不收 */
    void setSCK(uint8_t pin);
    void setTX(uint8_t pin);
    void setRX(uint8_t pin);
    void setCS(uint8_t pin);
};

extern SPIClass SPI;

#endif /* VFD_STUB_SPI_H */
