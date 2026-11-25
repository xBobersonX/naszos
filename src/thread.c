/* Zadania (watki) kernela - patrz thread.h. */

#include <stdint.h>
#include "thread.h"
#include "vga.h"
#include "heap.h"
#include "io.h"
#include "gdt.h"
#include "paging.h"

#define MAX_THREADS 12              /* glowne + strona + fetch + watki obrazkow (2 paczki po 3) */
#define STACK_SIZE  (256 * 1024)       /* rekurencyjny uklad stron i CSS potrzebuja miejsca */
#define TIME_SLICE  2               /* tykniec zegara dla programu, zanim odda procesor */

enum { T_FREE, T_READY, T_DEAD };

struct thread {
    int state;
    uint64_t rsp;               /* zapamietany wskaznik stosu, gdy zadanie nie dziala */
    void *stack;
    void (*fn)(void *);
    void *arg;
    struct console *con;
    uint64_t dir;               /* tablica stron PML4 (0 = kernela) */
    void *process;              /* program uzytkownika dzialajacy w tym zadaniu */
    uint64_t rsp0;              /* stos kernela dla przerwan z programu (TSS) */
    /* Stan koprocesora i rejestrow SSE (fxsave) - liczby zmiennoprzecinkowe
     * JavaScriptu. Musi byc wyrownany do 16 bajtow. */
    uint8_t fpu[512] __attribute__((aligned(16)));
};

static struct thread threads[MAX_THREADS];
static int current;
static int slice;

/* switch.S: zapisuje rejestry na stosie, zapamietuje rsp w *save, przelacza
 * na stos next_rsp i zdejmuje z niego rejestry nowego zadania */
extern void context_switch(uint64_t *save, uint64_t next_rsp);

void thread_init(void)
{
    threads[0].state = T_READY;
    threads[0].con = &con_screen;
    current = 0;
}

/* Stosy zakonczonych zadan zwalniamy dopiero z innego zadania - wlasnego
 * stosu nie mozna zwolnic, stojac na nim */
static void reap(void)
{
    for (int i = 1; i < MAX_THREADS; i++)
        if (threads[i].state == T_DEAD && i != current) {
            kfree(threads[i].stack);
            threads[i].stack = 0;
            threads[i].state = T_FREE;
        }
}

static void thread_entry(void)
{
    struct thread *t = &threads[current];
    /* Nowe zadanie zaczyna z czystym FPU/SSE (wszystkie wyjatki zamaskowane) */
    uint32_t mxcsr = 0x1F80;
    __asm__ volatile ("fninit; ldmxcsr %0" : : "m"(mxcsr));
    __asm__ volatile ("sti");
    t->fn(t->arg);
    thread_exit();
}

int thread_create(void (*fn)(void *), void *arg, struct console *con)
{
    reap();
    int id = -1;
    for (int i = 1; i < MAX_THREADS; i++)
        if (threads[i].state == T_FREE) {
            id = i;
            break;
        }
    if (id < 0)
        return -1;
    void *stack = kmalloc(STACK_SIZE);
    if (!stack)
        return -1;

    /* Stos nowego zadania wyglada tak, jakby juz raz wywolalo context_switch:
     * 6 rejestrow (rbx, rbp, r12-r15) i adres powrotu = thread_entry.
     * Po "powrocie" do thread_entry rsp musi dawac 8 mod 16 (jak po call). */
    uint64_t *sp = (uint64_t *)((uint8_t *)stack + STACK_SIZE);
    *--sp = 0;                          /* thread_entry nigdy nie wraca */
    *--sp = (uint64_t)thread_entry;
    for (int i = 0; i < 6; i++)
        *--sp = 0;

    struct thread *t = &threads[id];
    t->stack = stack;
    t->rsp = (uint64_t)sp;
    t->fn = fn;
    t->arg = arg;
    t->con = con;
    t->dir = 0;
    t->process = 0;
    t->rsp0 = 0;
    t->state = T_READY;
    return id;
}

void thread_yield(void)
{
    /* Przelaczenie z wylaczonymi przerwaniami; kazde zadanie po powrocie
     * przywraca swoj wlasny stan przerwan (moglo oddac procesor z przerwania) */
    unsigned long flags = irq_save();
    int next = current;
    for (int n = 1; n <= MAX_THREADS; n++) {
        int j = (current + n) % MAX_THREADS;
        if (threads[j].state == T_READY) {
            next = j;
            break;
        }
    }
    if (next != current) {
        int prev = current;
        current = next;
        slice = 0;
        con_cur = threads[next].con;
        paging_switch(threads[next].dir);
        if (threads[next].rsp0)             /* stos kernela dla przerwan z ring 3 */
            gdt_set_kernel_stack(threads[next].rsp0);
        /* Rejestry FPU/SSE nie sa zapisywane przez context_switch - zapisujemy
         * je tu, a po powrocie do tego zadania przywracamy. Przerwania nie
         * uzywaja SSE (kernel kompilowany z -mno-sse), wiec to wystarcza. */
        __asm__ volatile ("fxsave %0" : "=m"(threads[prev].fpu));
        context_switch(&threads[prev].rsp, threads[next].rsp);
        __asm__ volatile ("fxrstor %0" : : "m"(threads[current].fpu));
        reap();
    }
    irq_restore(flags);
}

void thread_preempt(void)
{
    if (++slice >= TIME_SLICE)
        thread_yield();
}

void thread_exit(void)
{
    __asm__ volatile ("cli");
    threads[current].state = T_DEAD;
    thread_yield();                     /* zadanie glowne jest zawsze gotowe - tu nie wrocimy */
    for (;;)
        __asm__ volatile ("hlt");
}

int thread_alive(int id)
{
    return id > 0 && id < MAX_THREADS && threads[id].state == T_READY;
}

int thread_is_main(void)
{
    return current == 0;
}

void thread_set_address_space(uint64_t dir, void *process)
{
    threads[current].dir = dir;
    threads[current].process = process;
    paging_switch(dir);
}

void thread_set_user_kernel_stack(uint64_t rsp0)
{
    threads[current].rsp0 = rsp0;
    if (rsp0)
        gdt_set_kernel_stack(rsp0);
}

void *thread_process(void)
{
    return threads[current].process;
}

void cpu_idle(void)
{
    thread_yield();
    if (current == 0)
        __asm__ volatile ("hlt");
}
