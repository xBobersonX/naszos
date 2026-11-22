/* Test silnika JavaScript NaszOS na Windowsie: "atrapy" funkcji kernela
 * (pamiec, zegar, watki) zrobione na Windows API (kernel32.dll).
 * Dzieki temu ten sam kod js_*.c, dom.c, css.c mozna sprawdzac w
 * sekundy, bez uruchamiania QEMU. */

#include <stdint.h>
#include <stddef.h>
#include "heap.h"
#include "string.h"
#include "rtc.h"
#include "timer.h"
#include "thread.h"

typedef void *HANDLE;
typedef struct {
    uint16_t wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME;

__declspec(dllimport) HANDLE __stdcall GetStdHandle(uint32_t);
__declspec(dllimport) int __stdcall WriteFile(HANDLE, const void *, uint32_t, uint32_t *, void *);
__declspec(dllimport) int __stdcall ReadFile(HANDLE, void *, uint32_t, uint32_t *, void *);
__declspec(dllimport) HANDLE __stdcall CreateFileA(const char *, uint32_t, uint32_t, void *, uint32_t, uint32_t, HANDLE);
__declspec(dllimport) uint32_t __stdcall GetFileSize(HANDLE, uint32_t *);
__declspec(dllimport) int __stdcall CloseHandle(HANDLE);
__declspec(dllimport) void *__stdcall VirtualAlloc(void *, size_t, uint32_t, uint32_t);
__declspec(dllimport) void __stdcall ExitProcess(uint32_t);
__declspec(dllimport) uint32_t __stdcall GetTickCount(void);
__declspec(dllimport) void __stdcall GetLocalTime(SYSTEMTIME *);
__declspec(dllimport) char *__stdcall GetCommandLineA(void);
__declspec(dllimport) void __stdcall Sleep(uint32_t);

int _fltused = 1;

/* ---- pamiec: prosty przydzial "na przod" z duzego obszaru ---- */

static uint8_t *heap_base, *heap_pos, *heap_committed;
#define HEAP_RESERVE (16ULL << 30)
#define COMMIT_STEP  (64u << 20)

void *kmalloc(size_t size)
{
    if (!heap_base) {
        heap_base = VirtualAlloc(0, HEAP_RESERVE, 0x2000 /* MEM_RESERVE */, 0x04);
        heap_pos = heap_committed = heap_base;
        if (!heap_base)
            return 0;
    }
    size = (size + 15) & ~(size_t)15;
    uint8_t *p = heap_pos + 16;                      /* naglowek: rozmiar */
    uint8_t *end = p + size;
    while (end > heap_committed) {
        if ((size_t)(heap_committed - heap_base) + COMMIT_STEP > HEAP_RESERVE)
            return 0;
        if (!VirtualAlloc(heap_committed, COMMIT_STEP, 0x1000 /* MEM_COMMIT */, 0x04))
            return 0;
        heap_committed += COMMIT_STEP;
    }
    *(size_t *)(p - 16) = size;
    heap_pos = end;
    return p;
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);
    if (p)
        memset(p, 0, size);
    return p;
}

void kfree(void *ptr)
{
    (void)ptr;                                       /* test - nie oddajemy pamieci */
}

/* ---- zegar, watki ---- */

static uint32_t start_tick;

uint32_t timer_uptime_ms(void)
{
    if (!start_tick)
        start_tick = GetTickCount() - 1000;
    return GetTickCount() - start_tick;
}

uint32_t timer_ticks(void) { return timer_uptime_ms() / 10; }
void thread_yield(void) {}

void rtc_read(struct rtc_time *t)
{
    SYSTEMTIME s;
    GetLocalTime(&s);
    t->year = s.wYear;
    t->month = (uint8_t)s.wMonth;
    t->day = (uint8_t)s.wDay;
    t->hour = (uint8_t)s.wHour;
    t->minute = (uint8_t)s.wMinute;
    t->second = (uint8_t)s.wSecond;
    t->weekday = (uint8_t)s.wDayOfWeek;
}

/* ---- siec: w testach nie ma (http.c jest potrzebny tylko dla adresow URL) ---- */

struct http_response;
int net_lock_dummy;

/* ---- wejscie/wyjscie ---- */

void out(const char *s, unsigned n)
{
    uint32_t w;
    WriteFile(GetStdHandle((uint32_t)-11), s, n, &w, 0);
}

void outs(const char *s) { out(s, (unsigned)strlen(s)); }

char *read_file(const char *path, unsigned *len)
{
    HANDLE h = CreateFileA(path, 0x80000000 /* GENERIC_READ */, 1, 0, 3 /* OPEN_EXISTING */, 0x80, 0);
    if (h == (HANDLE)(intptr_t)-1)
        return 0;
    uint32_t size = GetFileSize(h, 0), got = 0;
    char *buf = kmalloc(size + 1);
    ReadFile(h, buf, size, &got, 0);
    CloseHandle(h);
    buf[got] = '\0';
    *len = got;
    return buf;
}

void sleep_ms(uint32_t ms) { Sleep(ms); }
char *command_line(void) { return GetCommandLineA(); }
void exit_process(uint32_t code) { ExitProcess(code); }
