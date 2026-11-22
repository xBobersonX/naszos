/* UTF-8 - patrz utf8.h */

#include "utf8.h"

uint32_t utf8_next(const char **s, const char *end)
{
    const uint8_t *b = (const uint8_t *)*s;
    uint32_t cp = b[0];
    unsigned extra = 0;
    if (cp >= 0xF0 && cp < 0xF8)  { cp &= 0x07; extra = 3; }
    else if (cp >= 0xE0)          { cp &= 0x0F; extra = 2; }
    else if (cp >= 0xC0)          { cp &= 0x1F; extra = 1; }
    else if (cp >= 0x80)          { (*s)++; return 0xFFFD; }
    if ((const char *)b + extra >= end) {
        *s = end;
        return 0xFFFD;
    }
    for (unsigned i = 1; i <= extra; i++) {
        if ((b[i] & 0xC0) != 0x80) {
            *s += i;
            return 0xFFFD;
        }
        cp = cp << 6 | (b[i] & 0x3F);
    }
    *s += 1 + extra;
    return cp;
}

unsigned utf8_put(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | cp >> 6);
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | cp >> 12);
        out[1] = (char)(0x80 | (cp >> 6 & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | cp >> 18);
    out[1] = (char)(0x80 | (cp >> 12 & 0x3F));
    out[2] = (char)(0x80 | (cp >> 6 & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

char unicode_to_ascii(uint32_t cp)
{
    static const struct { uint16_t cp; char c; } map[] = {
        { 0x105, 'a' }, { 0x104, 'A' }, { 0x107, 'c' }, { 0x106, 'C' }, { 0x119, 'e' },
        { 0x118, 'E' }, { 0x142, 'l' }, { 0x141, 'L' }, { 0x144, 'n' }, { 0x143, 'N' },
        { 0x0F3, 'o' }, { 0x0D3, 'O' }, { 0x15B, 's' }, { 0x15A, 'S' }, { 0x17A, 'z' },
        { 0x179, 'Z' }, { 0x17C, 'z' }, { 0x17B, 'Z' }, { 0x0E9, 'e' }, { 0x0E8, 'e' },
        { 0x0E1, 'a' }, { 0x0E0, 'a' }, { 0x0F6, 'o' }, { 0x0FC, 'u' }, { 0x0E4, 'a' },
        { 0x0ED, 'i' }, { 0x0FA, 'u' }, { 0x0F1, 'n' }, { 0x0E7, 'c' }, { 0x0DF, 's' },
        { 0x0C9, 'E' }, { 0x0D6, 'O' }, { 0x0DC, 'U' }, { 0x0C4, 'A' }, { 0x10D, 'c' },
        { 0x161, 's' }, { 0x17E, 'z' }, { 0x159, 'r' }, { 0x11B, 'e' },
        { 0x0A0, ' ' }, { 0x2009, ' ' }, { 0x2019, '\'' }, { 0x2018, '\'' },
        { 0x201C, '"' }, { 0x201D, '"' }, { 0x201E, '"' }, { 0x2013, '-' }, { 0x2014, '-' },
        { 0x2022, '*' }, { 0x00B7, '*' }, { 0x00AB, '"' }, { 0x00BB, '"' }, { 0x2192, '>' },
        { 0x2190, '<' }, { 0x203A, '>' }, { 0x2039, '<' }, { 0x00D7, 'x' }, { 0x2212, '-' },
        { 0x00B0, 'o' },
    };
    if (cp < 0x80)
        return (char)cp;
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (map[i].cp == cp)
            return map[i].c;
    if (cp == 0x200B || cp == 0xFEFF)          /* znaki niewidoczne */
        return 0;
    return '?';
}

unsigned utf8_to_ascii(const char *in, unsigned len, char *out, unsigned size)
{
    const char *s = in, *end = in + len;
    unsigned n = 0;
    while (s < end && n + 3 < size) {
        uint32_t cp = utf8_next(&s, end);
        if (cp == 0x2026) {                    /* wielokropek */
            out[n++] = '.'; out[n++] = '.'; out[n++] = '.';
            continue;
        }
        char c = unicode_to_ascii(cp);
        if (c)
            out[n++] = c;
    }
    if (size)
        out[n < size ? n : size - 1] = '\0';
    return n;
}
