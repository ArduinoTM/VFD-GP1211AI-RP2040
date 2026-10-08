/*
 * Print —— Arduino Print 类的极简实现（Adafruit_GFX 的基类）。
 * 仅实现 Adafruit_GFX / 示例代码用得到的接口。
 */
#ifndef VFD_COMPAT_PRINT_H
#define VFD_COMPAT_PRINT_H

#include <stdint.h>
#include <stddef.h>

/* Arduino 在 WString.h 中前置声明，这里同样声明，供 getTextBounds(F()) 使用 */
class __FlashStringHelper;

#define DEC 10
#define HEX 16
#define OCT 8
#define BIN 2

class Print {
public:
    virtual ~Print() {}

    /* 子类必须实现的最小接口 */
    virtual size_t write(uint8_t c) = 0;
    virtual size_t write(const uint8_t *buffer, size_t size);

    size_t write(const char *str);
    size_t write(const char *buffer, size_t size);

    size_t print(const char *s) { return write(s); }
    size_t print(const __FlashStringHelper *s) { return write(reinterpret_cast<const char *>(s)); }
    size_t print(char c) { return write(static_cast<uint8_t>(c)); }
    size_t print(unsigned char c, int base = DEC)
    {
        return printNumber(static_cast<unsigned long>(c), static_cast<uint8_t>(base));
    }
    size_t print(int n, int base = DEC) { return printSigned(n, base); }
    size_t print(unsigned int n, int base = DEC)
    {
        return printNumber(static_cast<unsigned long>(n), static_cast<uint8_t>(base));
    }
    size_t print(long n, int base = DEC) { return printSigned(n, base); }
    size_t print(unsigned long n, int base = DEC)
    {
        return printNumber(n, static_cast<uint8_t>(base));
    }
    size_t print(double n, int digits = 2) { return printFloat(n, static_cast<uint8_t>(digits)); }

    size_t println(const char *s) { return print(s) + println(); }
    size_t println(const __FlashStringHelper *s) { return print(s) + println(); }
    size_t println(char c) { return print(c) + println(); }
    size_t println(int n, int base = DEC) { return print(n, base) + println(); }
    size_t println(unsigned int n, int base = DEC) { return print(n, base) + println(); }
    size_t println(long n, int base = DEC) { return print(n, base) + println(); }
    size_t println(unsigned long n, int base = DEC) { return print(n, base) + println(); }
    size_t println(double n, int digits = 2) { return print(n, digits) + println(); }
    size_t println(void) { return write("\r\n"); }

    virtual void flush() {}

protected:
    size_t printNumber(unsigned long n, uint8_t base);
    size_t printSigned(long n, uint8_t base);
    size_t printFloat(double number, uint8_t digits);
};

#endif /* VFD_COMPAT_PRINT_H */
