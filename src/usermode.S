/* Przejscie do trybu uzytkownika (ring 3) i "awaryjny powrot" do kernela. */

.section .text
.code64

/* void enter_user(uint64_t entry, uint64_t user_stack)
 * Procesor nie ma rozkazu "skocz do ring 3" - udajemy powrot z przerwania:
 * odkladamy na stos ramke taka, jaka zostawiloby przerwanie z ring 3
 * (SS, RSP, RFLAGS, CS, RIP) i robimy iretq. */
.global enter_user
enter_user:
    mov $0x23, %ax              /* segment danych uzytkownika (0x20 | ring 3) */
    mov %ax, %ds
    mov %ax, %es
    pushq $0x23                 /* SS */
    pushq %rsi                  /* RSP */
    pushfq
    popq %rax
    or $0x200, %rax             /* IF: przerwania wlaczone w programie */
    pushq %rax                  /* RFLAGS */
    pushq $0x1B                 /* CS: kod uzytkownika (0x18 | ring 3), 64-bitowy */
    pushq %rdi                  /* RIP */
    iretq

/* int kjmp_set(uint64_t buf[8]) - zapamietuje miejsce w kernelu (jak setjmp).
 * Zwraca 0; po kjmp_go "wraca drugi raz" z podana wartoscia. */
.global kjmp_set
kjmp_set:
    mov %rbx, 0(%rdi)
    mov %rbp, 8(%rdi)
    mov %r12, 16(%rdi)
    mov %r13, 24(%rdi)
    mov %r14, 32(%rdi)
    mov %r15, 40(%rdi)
    lea 8(%rsp), %rax           /* rsp po powrocie z tej funkcji */
    mov %rax, 48(%rdi)
    mov (%rsp), %rax            /* adres powrotu */
    mov %rax, 56(%rdi)
    xor %eax, %eax
    ret

/* void kjmp_go(uint64_t buf[8], int value) - wraca do miejsca z kjmp_set
 * (jak longjmp). Uzywane, gdy program sie konczy albo zostaje zamkniety:
 * porzucamy stos wywolania systemowego/wyjatku i wracamy do process_run. */
.global kjmp_go
kjmp_go:
    mov %esi, %eax
    mov 0(%rdi), %rbx
    mov 8(%rdi), %rbp
    mov 16(%rdi), %r12
    mov 24(%rdi), %r13
    mov 32(%rdi), %r14
    mov 40(%rdi), %r15
    mov 48(%rdi), %rsp
    jmp *56(%rdi)
