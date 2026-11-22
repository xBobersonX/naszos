/* Interrupt Descriptor Table - mowi procesorowi, jaka funkcje wywolac dla
 * kazdego z 256 przerwan. Wektory 0-31 to wyjatki procesora (bledy),
 * 32-47 to przerwania sprzetowe z kontrolera PIC (zegar, klawiatura...). */

#include <stdint.h>
#include "idt.h"
#include "gdt.h"
#include "io.h"
#include "vga.h"
#include "thread.h"
#include "process.h"

/* W trybie 64-bitowym wpis ma 16 bajtow - adres funkcji ma 64 bity */
struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20

static struct idt_entry idt[256];
static struct idt_ptr idtp;
/* Kilka urzadzen PCI moze dzielic jedna linie IRQ (np. karta sieciowa
 * i dzwiekowa) - wtedy wolamy wszystkie ich funkcje, a kazda sprawdza,
 * czy to jej urzadzenie zglosilo przerwanie. */
#define SHARED_MAX 4
static irq_handler_t handlers[256][SHARED_MAX];

extern const uint64_t isr_stub_table[48];
extern void isr128(void);

static const char *const exception_names[32] = {
    "Dzielenie przez zero", "Debug", "NMI", "Breakpoint",
    "Przepelnienie (INTO)", "Przekroczenie zakresu (BOUND)",
    "Niepoprawna instrukcja", "Brak koprocesora",
    "Double fault", "Coprocessor segment overrun", "Niepoprawny TSS",
    "Brak segmentu", "Blad segmentu stosu", "General protection fault",
    "Page fault", "Zarezerwowany", "Blad FPU x87", "Blad wyrownania",
    "Machine check", "Blad SIMD", "Blad wirtualizacji", "Control protection",
    "Zarezerwowany", "Zarezerwowany", "Zarezerwowany", "Zarezerwowany",
    "Zarezerwowany", "Zarezerwowany", "Hypervisor injection",
    "VMM communication", "Blad bezpieczenstwa", "Zarezerwowany",
};

static void idt_set(uint8_t vec, uint64_t handler)
{
    idt[vec].offset_low  = handler & 0xFFFF;
    idt[vec].offset_mid  = (handler >> 16) & 0xFFFF;
    idt[vec].offset_high = handler >> 32;
    idt[vec].selector    = GDT_KERNEL_CODE;
    idt[vec].ist         = 0;
    idt[vec].reserved    = 0;
    idt[vec].type_attr   = 0x8E;   /* obecny, ring 0, 64-bit interrupt gate */
}

static inline void io_wait(void)
{
    outb(0x80, 0);
}

/* Domyslnie PIC wysyla IRQ na wektory 8-15, ktore koliduja z wyjatkami
 * procesora - przesuwamy je na 32-47. Wszystkie IRQ zostaja zamaskowane,
 * sterowniki wlaczaja swoje przez irq_unmask(). */
static void pic_remap(void)
{
    outb(PIC1_CMD, 0x11);  io_wait();   /* ICW1: inicjalizacja + ICW4 */
    outb(PIC2_CMD, 0x11);  io_wait();
    outb(PIC1_DATA, IRQ_BASE);     io_wait();   /* ICW2: offset wektorow */
    outb(PIC2_DATA, IRQ_BASE + 8); io_wait();
    outb(PIC1_DATA, 0x04); io_wait();   /* ICW3: slave na IRQ2 */
    outb(PIC2_DATA, 0x02); io_wait();
    outb(PIC1_DATA, 0x01); io_wait();   /* ICW4: tryb 8086 */
    outb(PIC2_DATA, 0x01); io_wait();

    outb(PIC1_DATA, 0xFF & ~(1 << 2));  /* wszystko wylaczone poza kaskada */
    outb(PIC2_DATA, 0xFF);
}

void irq_unmask(uint8_t irq)
{
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    outb(port, inb(port) & ~(1 << (irq % 8)));
}

void interrupt_register(uint8_t vector, irq_handler_t handler)
{
    for (int i = 0; i < SHARED_MAX; i++)
        if (!handlers[vector][i] || handlers[vector][i] == handler) {
            handlers[vector][i] = handler;
            return;
        }
}

void idt_init(void)
{
    for (int i = 0; i < 48; i++)
        idt_set((uint8_t)i, isr_stub_table[i]);
    idt_set(0x80, (uint64_t)isr128);
    idt[0x80].type_attr = 0xEE;            /* jak wyzej, ale dostepne z ring 3 (int 0x80) */

    pic_remap();

    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint64_t)&idt;
    __asm__ volatile ("lidt %0" : : "m"(idtp));
}

static void __attribute__((noreturn)) kernel_panic(struct regs *r)
{
    vga_emergency();
    vga_set_color(VGA_WHITE, VGA_RED);
    kprintf("\n\n *** KERNEL PANIC: %s (wyjatek %lu, kod bledu 0x%lx) *** \n",
            exception_names[r->int_no], r->int_no, r->err_code);
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kprintf("  RIP=%lx  CS=%lx  RFLAGS=%lx\n", r->rip, r->cs, r->rflags);
    kprintf("  RAX=%lx  RBX=%lx  RCX=%lx  RDX=%lx\n", r->rax, r->rbx, r->rcx, r->rdx);
    kprintf("  RSI=%lx  RDI=%lx  RBP=%lx  RSP=%lx\n", r->rsi, r->rdi, r->rbp, r->rsp);
    kprintf("  R8=%lx  R9=%lx  R10=%lx  R11=%lx\n", r->r8, r->r9, r->r10, r->r11);
    if (r->int_no == 14) {
        uint64_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        kprintf("  Adres bledu stronicowania (CR2)=%lx\n", cr2);
    }
    vga_write("\nSystem zatrzymany.");
    for (;;)
        __asm__ volatile ("cli; hlt");
}

/* Wywolywane z isr_common dla kazdego przerwania. */
void isr_handler(struct regs *r)
{
    int from_user = (r->cs & 3) == 3;      /* przerwany zostal program (ring 3) */

    if (r->int_no >= IRQ_BASE && r->int_no < IRQ_BASE + 16) {
        /* Potwierdzenie (EOI) dla PIC - bez tego nie przysle kolejnych IRQ */
        if (r->int_no >= IRQ_BASE + 8)
            outb(PIC2_CMD, PIC_EOI);
        outb(PIC1_CMD, PIC_EOI);
    }

    /* Blad w programie zamyka tylko ten program - system dziala dalej */
    if (r->int_no < 32 && from_user)
        process_fault(r, exception_names[r->int_no]);

    if (handlers[r->int_no][0]) {
        for (int i = 0; i < SHARED_MAX && handlers[r->int_no][i]; i++)
            handlers[r->int_no][i](r);
    } else if (r->int_no < 32) {
        kernel_panic(r);
    }

    /* Tykniecie zegara w trakcie programu: mozna oddac procesor innym
     * zadaniom, nawet jesli program sam tego nie robi (petla bez konca) */
    if (r->int_no == IRQ_BASE && from_user)
        process_tick();
}
