#include "printf.h"

void print_char(uint8_t c)
{
	uint8_t buf[2];
	buf[0] = c;
	buf[1] = '\0';
	print((const uint8_t *) buf);
}

static void print_string(const uint8_t *s) {
	print(s);
}

static void print_decimal(int val) {
	uint8_t buf[12]; 
	int i = 0;
	if (val < 0) {
		print_char('-');
		val = -val;
	}
	do {
		buf[i++] = '0' + (val % 10);
		val /= 10;
	} while (val > 0);
	while (i--) {
		print_char(buf[i]);
	}
}

static void print_unsigned(unsigned int val) {
	uint8_t buf[10];
	int i = 0;
	do {
		buf[i++] = '0' + (val % 10);
		val /= 10;
	} while (val > 0);
	while (i--) {
		print_char(buf[i]);
	}
}

static void print_hex(unsigned int val) {
	const uint8_t *digits = (const uint8_t *)"0123456789abcdef";
	uint8_t buf[8];
	int i = 0;
	do {
		buf[i++] = digits[val % 16];
		val /= 16;
	} while (val > 0);
	while (i--) {
		print_char(buf[i]);
	}
}

void printf(const uint8_t *fmt, ...) {
	va_list args;
	va_start(args, fmt);

	while (*fmt) {
		if (*fmt == '%') {
			fmt++;
			switch (*fmt) {
				case 's':
					print_string(va_arg(args, const uint8_t *));
					break;
				case 'c':
					print_char((uint8_t) va_arg(args, int));
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
					print_char('%');
					break;
				default:
					print_char('%');
					print_char(*fmt);
					break;
			}
		} else {
			print_char(*fmt);
		}
		fmt++;
	}

	va_end(args);
}

