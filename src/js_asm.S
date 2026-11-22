/* JavaScript - wstawki w asemblerze (konwencja System V, 64 bity).
 *
 * js_setjmp / js_longjmp: wyjatki (throw). setjmp zapamietuje rejestry,
 * ktore funkcja w C musi zachowac, wskaznik stosu i adres powrotu;
 * longjmp je przywraca - program "wraca" jeszcze raz z setjmp, tym razem
 * z wynikiem 1 (zamiast 0) i trafia do bloku catch.
 *
 * js_on_stack: wywoluje fn(arg) na osobnym, duzym stosie silnika JS
 * (rekurencja w skryptach nie zjada stosu zadania kernela). */

.section .text
.code64

/* int js_setjmp(struct js_jmp *j)  -  rdi = j */
.global js_setjmp
js_setjmp:
    mov %rbx, 0(%rdi)
    mov %rbp, 8(%rdi)
    mov %r12, 16(%rdi)
    mov %r13, 24(%rdi)
    mov %r14, 32(%rdi)
    mov %r15, 40(%rdi)
    lea 8(%rsp), %rdx                 /* rsp po powrocie z setjmp */
    mov %rdx, 48(%rdi)
    mov (%rsp), %rdx                  /* adres powrotu */
    mov %rdx, 56(%rdi)
    xor %eax, %eax
    ret

/* void js_longjmp(struct js_jmp *j, int v)  -  rdi = j, esi = v */
.global js_longjmp
js_longjmp:
    mov 0(%rdi), %rbx
    mov 8(%rdi), %rbp
    mov 16(%rdi), %r12
    mov 24(%rdi), %r13
    mov 32(%rdi), %r14
    mov 40(%rdi), %r15
    mov 48(%rdi), %rsp
    mov %esi, %eax
    test %eax, %eax
    jnz 1f
    inc %eax
1:  jmp *56(%rdi)

/* void js_on_stack(void (*fn)(void *), void *arg, void *stack_top)
 * rdi = fn, rsi = arg, rdx = gorny koniec nowego stosu */
.global js_on_stack
js_on_stack:
    push %rbp
    mov %rsp, %rbp                    /* stary stos zapamietany w rbp (zachowywany przez fn) */
    and $-16, %rdx
    mov %rdx, %rsp
    sub $32, %rsp                     /* miejsce, ktore wg konwencji Windows moze zajac fn (test na PC) */
    mov %rdi, %rax
    mov %rsi, %rdi
    mov %rsi, %rcx                    /* argument tez w rcx - konwencja Windows (test na PC) */
    call *%rax
    mov %rbp, %rsp
    pop %rbp
    ret
