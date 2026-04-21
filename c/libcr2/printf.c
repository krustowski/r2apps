#include "printf.h"

uint8_t _printf_buf[512];
uint16_t _printf_pos;

static void _putc(uint8_t c) {
    if (_printf_pos < sizeof(_printf_buf) - 1)
        _printf_buf[_printf_pos++] = c;
}

void print_char(uint8_t c) {
    uint8_t buf[2];
    buf[0] = c;
    buf[1] = '\0';
    print((const uint8_t *)buf);
}

static void print_string(const uint8_t *s) {
    while (*s)
        _putc(*s++);
}

static void print_decimal(int val) {
    uint8_t buf[12];
    int i = 0;
    if (val < 0) {
        _putc('-');
        val = -val;
    }
    do {
        buf[i++] = '0' + (val % 10);
        val /= 10;
    } while (val > 0);
    while (i--)
        _putc(buf[i]);
}

static void print_unsigned(unsigned int val) {
    uint8_t buf[10];
    int i = 0;
    do {
        buf[i++] = '0' + (val % 10);
        val /= 10;
    } while (val > 0);
    while (i--)
        _putc(buf[i]);
}

static void print_hex(unsigned int val) {
    const uint8_t *digits = (const uint8_t *)"0123456789abcdef";
    uint8_t buf[8];
    int i = 0;
    do {
        buf[i++] = digits[val % 16];
        val /= 16;
    } while (val > 0);
    while (i--)
        _putc(buf[i]);
}

void printf(const uint8_t *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    _printf_pos = 0;

    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            switch (*fmt) {
            case 's':
                print_string(va_arg(args, const uint8_t *));
                break;
            case 'c':
                _putc((uint8_t)va_arg(args, int));
                break;
            case 'd':
                print_decimal(va_arg(args, int));
                break;
            case 'u':
                print_unsigned(va_arg(args, unsigned int));
                break;
            case 'x':
                print_hex(va_arg(args, unsigned int));
                break;
            case '%':
                _putc('%');
                break;
            default:
                _putc('%');
                _putc(*fmt);
                break;
            }
        } else {
            _putc(*fmt);
        }
        fmt++;
    }

    _printf_buf[_printf_pos] = '\0';
    print(_printf_buf);
    va_end(args);
}
