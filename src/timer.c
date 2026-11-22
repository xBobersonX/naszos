/* Programmable Interval Timer (Intel 8253/8254). Kanal 0 generuje IRQ 0
 * z czestotliwoscia 1193182 Hz / dzielnik - ustawiamy go na TIMER_HZ. */

#include "crypto.h"
#include "timer.h"
#include "idt.h"
#include "io.h"
#include "thread.h"
#include "speaker.h"

#define PIT_BASE_HZ  1193182
#define PIT_CHANNEL0 0x40
#define PIT_CMD      0x43

#define REFRESH_TICKS (TIMER_HZ / TIMER_REFRESH_HZ)

static volatile uint32_t ticks;
static void (*refresh_callback)(uint32_t);

static void on_tick(struct regs *r)
{
    (void)r;
    ticks++;
    sound_tick();                        /* kolejna nuta z kolejki dzwiekow */
    if (ticks % 8 == 0)
        random_mix(ticks);               /* "szum" dla liczb losowych (TLS) */
    if (refresh_callback && ticks % REFRESH_TICKS == 0)
        refresh_callback(ticks / TIMER_HZ);
}

void timer_on_refresh(void (*callback)(uint32_t uptime_secs))
{
    refresh_callback = callback;
}

void timer_init(void)
{
    uint16_t divisor = PIT_BASE_HZ / TIMER_HZ;

    outb(PIT_CMD, 0x36);                 /* kanal 0, lo/hi bajt, tryb 3 (fala prostokatna) */
    outb(PIT_CHANNEL0, divisor & 0xFF);
    outb(PIT_CHANNEL0, divisor >> 8);

    interrupt_register(IRQ_BASE + 0, on_tick);
    irq_unmask(0);
}

uint32_t timer_ticks(void)
{
    return ticks;
}

uint32_t timer_uptime_ms(void)
{
    return ticks * (1000 / TIMER_HZ);
}

void timer_sleep(uint32_t ms)
{
    uint32_t end = ticks + (ms * TIMER_HZ + 999) / 1000;
    while ((int32_t)(end - ticks) > 0)
        cpu_idle();                      /* inne zadania dzialaja, gdy czekamy */
}
