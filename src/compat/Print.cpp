#include "Print.h"

#include <math.h>
#include <string.h>

size_t Print::write(const uint8_t *buffer, size_t size)
{
    size_t n = 0;
    if (buffer == NULL)
        return 0;
    while (size--) {
        if (write(*buffer++) != 1)
            break;
        n++;
    }
    return n;
}

size_t Print::write(const char *str)
{
    if (str == NULL)
        return 0;
    return write(reinterpret_cast<const uint8_t *>(str), strlen(str));
}

size_t Print::write(const char *buffer, size_t size)
{
    return write(reinterpret_cast<const uint8_t *>(buffer), size);
}

size_t Print::printNumber(unsigned long n, uint8_t base)
{
    char buf[8 * sizeof(unsigned long) + 1];
    char *str = &buf[sizeof(buf) - 1];
    *str = '\0';

    if (base < 2)
        base = 10;

    do {
        char c = static_cast<char>(n % base);
        n /= base;
        *--str = (c < 10) ? static_cast<char>(c + '0') : static_cast<char>(c + 'A' - 10);
    } while (n);

    return write(str);
}

size_t Print::printSigned(long n, uint8_t base)
{
    if (base == 10 && n < 0) {
        size_t t = write(static_cast<uint8_t>('-'));
        return printNumber(static_cast<unsigned long>(-n), base) + t;
    }
    return printNumber(static_cast<unsigned long>(n), base);
}

size_t Print::printFloat(double number, uint8_t digits)
{
    if (isnan(number))
        return print("nan");
    if (isinf(number))
        return print("inf");
    if (number > 4294967040.0)
        return print("ovf");
    if (number < -4294967040.0)
        return print("ovf");

    size_t n = 0;
    if (number < 0.0) {
        n += write(static_cast<uint8_t>('-'));
        number = -number;
    }

    /* 四舍五入到指定小数位 */
    double rounding = 0.5;
    for (uint8_t i = 0; i < digits; ++i)
        rounding /= 10.0;
    number += rounding;

    unsigned long int_part = static_cast<unsigned long>(number);
    double remainder = number - static_cast<double>(int_part);
    n += printNumber(int_part, 10);

    if (digits > 0) {
        n += write(static_cast<uint8_t>('.'));
        while (digits-- > 0) {
            remainder *= 10.0;
            unsigned int digit = static_cast<unsigned int>(remainder);
            n += write(static_cast<uint8_t>('0' + digit));
            remainder -= digit;
        }
    }
    return n;
}
