/* void context_switch(uint64_t *save, uint64_t next_rsp)
 * Przelaczenie zadan: rejestry, ktore wedlug konwencji C (System V, 64 bity)
 * funkcja musi zachowac - rbx, rbp, r12-r15 - odkladamy na stos biezacego
 * zadania, zapamietujemy jego rsp i przechodzimy na stos nastepnego.
 * "ret" wraca tam, gdzie nastepne zadanie samo wywolalo context_switch. */

.section .text
.code64
.global context_switch
context_switch:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    mov %rsp, (%rdi)
    mov %rsi, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    ret
