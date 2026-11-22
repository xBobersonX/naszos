/* Nie mamy biblioteki standardowej C, wiec podstawowe funkcje piszemy sami.
 * memset/memcpy sa tez potrzebne kompilatorowi, ktory potrafi je wywolac
 * niejawnie (np. przy zerowaniu wiekszych tablic). */

#include <stdarg.h>
#include <stdint.h>
#include "string.h"

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}

/* Jak memcpy, ale obszary moga na siebie nachodzic */
void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else if (d > s) {
        while (n--)
            d[n] = s[n];
    }
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

/* ---- ksnprintf: jak kprintf, ale do bufora (%s %c %u %d %x, szerokosc np. %02u) ---- */

struct out {
    char *buf;
    size_t size, len;
};

static void out_char(struct out *o, char c)
{
    if (o->len + 1 < o->size)
        o->buf[o->len] = c;
    o->len++;
}

static void out_uint(struct out *o, uint64_t v, unsigned base, int width, char pad)
{
    char tmp[21];
    int i = 0;
    do {
        tmp[i++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    for (int n = i; n < width; n++)
        out_char(o, pad);
    while (i--)
        out_char(o, tmp[i]);
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct out o = { buf, size, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            out_char(&o, *fmt);
            continue;
        }
        char pad = ' ';
        int width = 0;
        if (*++fmt == '0')
            pad = '0';
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        int is_long = 0;                    /* %lu / %lx / %ld - 64 bity */
        if (*fmt == 'l') {
            is_long = 1;
            fmt++;
        }
        switch (*fmt) {
        case 's':
            for (const char *s = va_arg(ap, const char *); *s; s++)
                out_char(&o, *s);
            break;
        case 'c': out_char(&o, (char)va_arg(ap, int)); break;
        case 'u':
        case 'x': {
            uint64_t v = is_long ? va_arg(ap, uint64_t) : va_arg(ap, uint32_t);
            out_uint(&o, v, *fmt == 'x' ? 16 : 10, width, pad);
            break;
        }
        case 'd': {
            int64_t v = is_long ? va_arg(ap, int64_t) : va_arg(ap, int32_t);
            if (v < 0) {
                out_char(&o, '-');
                out_uint(&o, 0u - (uint64_t)v, 10, width - 1, pad);
            } else {
                out_uint(&o, (uint64_t)v, 10, width, pad);
            }
            break;
        }
        case '%': out_char(&o, '%'); break;
        case '\0': fmt--; break;
        default: out_char(&o, '%'); out_char(&o, *fmt); break;
        }
    }
    if (size)
        buf[o.len < size ? o.len : size - 1] = '\0';
    return (int)o.len;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

/* Szuka needle w hay bez rozrozniania wielkosci liter (ASCII). NULL = nie ma */
const char *strstr_ci(const char *hay, const char *needle)
{
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n) {
            char a = *h, b = *n;
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b)
                break;
            h++;
            n++;
        }
        if (!*n)
            return hay;
    }
    return 0;
}
