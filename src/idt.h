#ifndef IDT_H
#define IDT_H

#include <stdint.h>

/* Stan procesora zapisany przez isr_common (src/interrupts.S) - kolejnosc
 * odwrotna do odkladania na stos, na koncu ramka, ktora odklada procesor. */
struct regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;                 /* odklada procesor */
};

typedef void (*irq_handler_t)(struct regs *r);

#define IRQ_BASE 32   /* IRQ 0-15 z PIC trafiaja na wektory 32-47 */

void idt_init(void);
void interrupt_register(uint8_t vector, irq_handler_t handler);
void irq_unmask(uint8_t irq);

#endif
