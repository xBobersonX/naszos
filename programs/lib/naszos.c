/* Biblioteka programow NaszOS: start programu i wywolania systemowe.
 * Numery uslug musza zgadzac sie z enum w src/process.c. */

#include "naszos.h"

enum {
    SYS_EXIT, SYS_WRITE, SYS_GETKEY, SYS_POLLKEY, SYS_SLEEP, SYS_TIME,
    SYS_CANVAS_OPEN, SYS_CANVAS_FILL, SYS_CANVAS_TEXT, SYS_CANVAS_SHOW,
    SYS_RANDOM, SYS_MOUSE, SYS_SETCOLOR, SYS_CLEAR,
};

/* int 0x80: numer uslugi w RAX, argumenty w RDI, RSI, RDX, R10, R8, wynik w RAX
 * (64 bity: kazdy argument miesci caly wskaznik) */
static long syscall(long n, long a, long b, long c, long d, long e)
{
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    long ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
                      : "memory");
    return ret;
}

int main(void);

/* Tu zaczyna sie kazdy program (adres startowy w pliku .exe) */
void _start(void)
{
    exit(main());
}

void exit(int code)
{
    syscall(SYS_EXIT, code, 0, 0, 0, 0);
    for (;;)
        ;
}

int str_len(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

void print(const char *s)            { syscall(SYS_WRITE, (long)s, str_len(s), 0, 0, 0); }
void print_char(char c)              { syscall(SYS_WRITE, (long)&c, 1, 0, 0, 0); }
void set_color(int fg, int bg)       { syscall(SYS_SETCOLOR, fg, bg, 0, 0, 0); }
void clear_screen(void)              { syscall(SYS_CLEAR, 0, 0, 0, 0, 0); }
int  get_key(void)                   { return (int)syscall(SYS_GETKEY, 0, 0, 0, 0, 0); }
int  poll_key(void)                  { return (int)syscall(SYS_POLLKEY, 0, 0, 0, 0, 0); }
void sleep_ms(int ms)                { syscall(SYS_SLEEP, ms, 0, 0, 0, 0); }
int  time_ms(void)                   { return (int)syscall(SYS_TIME, 0, 0, 0, 0, 0); }
int  canvas_open(int w, int h)       { return (int)syscall(SYS_CANVAS_OPEN, w, h, 0, 0, 0); }
void canvas_show(void)               { syscall(SYS_CANVAS_SHOW, 0, 0, 0, 0, 0); }
void get_mouse(struct mouse *m)      { syscall(SYS_MOUSE, (long)m, 0, 0, 0, 0); }

void canvas_fill(int x, int y, int w, int h, unsigned color)
{
    syscall(SYS_CANVAS_FILL, x, y, w, h, color);
}

void canvas_text(int x, int y, const char *s, unsigned color)
{
    syscall(SYS_CANVAS_TEXT, x, y, (long)s, color, 0);
}

int random(int max)
{
    return max > 0 ? (int)(syscall(SYS_RANDOM, 0, 0, 0, 0, 0) % max) : 0;
}

void print_int(int n)
{
    char buf[12];
    int i = 0;
    unsigned u = n < 0 ? 0u - (unsigned)n : (unsigned)n;
    do {
        buf[i++] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (n < 0)
        buf[i++] = '-';
    while (i--)
        print_char(buf[i]);
}

int read_line(char *buf, int size)
{
    int len = 0;
    for (;;) {
        int k = get_key();
        if (k == '\n') {
            print_char('\n');
            break;
        }
        if (k == '\b') {
            if (len > 0) {
                len--;
                print("\b");
            }
        } else if (k >= ' ' && k < 127 && len < size - 1) {
            buf[len++] = (char)k;
            print_char((char)k);
        }
    }
    buf[len] = '\0';
    return len;
}

/* Kompilator moze sam wstawic wywolania memset/memcpy (np. przy kopiowaniu
 * struktur) - programy nie maja biblioteki standardowej, wiec sa tutaj */
void *memset(void *dst, int c, unsigned n)
{
    unsigned char *d = dst;
    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}

void *memcpy(void *dst, const void *src, unsigned n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}
