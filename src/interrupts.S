/* Niskopoziomowa czesc przerwan (64 bity): przeladowanie GDT oraz "stuby"
 * wejscia dla wektorow 0-47 i 0x80. Kazdy stub odklada numer przerwania
 * (i sztuczny kod bledu tam, gdzie procesor go nie daje), po czym skacze do
 * isr_common, ktory zapisuje wszystkie rejestry i wola isr_handler(regs*). */

.section .text
.code64

/* void gdt_flush(struct gdt_ptr *p) */
.global gdt_flush
gdt_flush:
    lgdt (%rdi)
    mov $0x10, %ax              /* segment danych kernela */
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    xor %ax, %ax
    mov %ax, %fs
    mov %ax, %gs
    /* CS przeladowujemy "dalekim powrotem": selektor i adres na stos, lretq */
    pushq $0x08
    lea 1f(%rip), %rax
    pushq %rax
    lretq
1:  ret

.macro ISR_NOERR n
isr\n:
    pushq $0
    pushq $\n
    jmp isr_common
.endm

.macro ISR_ERR n
isr\n:
    pushq $\n
    jmp isr_common
.endm

/* Wyjatki 8, 10-14, 17, 21, 29, 30 dostaja od procesora kod bledu */
.irp n, 0,1,2,3,4,5,6,7,9,15,16,18,19,20,22,23,24,25,26,27,28,31
ISR_NOERR \n
.endr
.irp n, 8,10,11,12,13,14,17,21,29,30
ISR_ERR \n
.endr
/* IRQ 0-15 z PIC */
.irp n, 32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
ISR_NOERR \n
.endr
/* int 0x80 - wywolania systemowe z programow */
.global isr128
ISR_NOERR 128

/* Kolejnosc odkladania musi zgadzac sie ze struct regs w idt.h */
isr_common:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    cld

    mov %rsp, %rdi              /* struct regs * */
    call isr_handler

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    add $16, %rsp               /* numer przerwania + kod bledu */
    iretq

.section .rodata
.global isr_stub_table
isr_stub_table:
.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
    .quad isr\n
.endr
