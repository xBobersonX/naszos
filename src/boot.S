/* Punkt wejscia systemu. Bootloader (QEMU -kernel, Multiboot 1) laduje kernel
 * pod 1 MiB i skacze do _start w 32-bitowym trybie chronionym: EAX = magic,
 * EBX = wskaznik na multiboot_info. Stad sami przechodzimy do trybu 64-bitowego:
 *   1. sprawdzamy, czy procesor go ma (CPUID),
 *   2. budujemy tablice stron: pierwsze 4 GiB "jeden do jednego" (strony 2 MiB),
 *   3. wlaczamy PAE, tryb dlugi (EFER.LME) i stronicowanie,
 *   4. ladujemy 64-bitowa GDT i skaczemy do kodu 64-bitowego. */

.set MB_ALIGN,    1 << 0              /* moduly wyrownane do stron */
.set MB_MEMINFO,  1 << 1              /* prosimy o mape pamieci */
.set MB_FLAGS,    MB_ALIGN | MB_MEMINFO
.set MB_MAGIC,    0x1BADB002
.set MB_CHECKSUM, -(MB_MAGIC + MB_FLAGS)

.section .multiboot, "a"
.align 4
.long MB_MAGIC
.long MB_FLAGS
.long MB_CHECKSUM

.section .bss
.align 4096
boot_pml4:  .skip 4096
boot_pdpt:  .skip 4096
boot_pd:    .skip 4096 * 4            /* 4 katalogi po 1 GiB = 0..4 GiB */
.align 16
stack_bottom:
.skip 262144                          /* 256 KiB stosu (uklad stron WWW jest rekurencyjny) */
.global stack_top
stack_top:
boot_magic: .skip 4
boot_info:  .skip 4

.section .rodata
.align 8
gdt64:
    .quad 0                           /* pusty deskryptor */
    .quad 0x00AF9A000000FFFF          /* 0x08: kod 64-bit (L=1) */
    .quad 0x00CF92000000FFFF          /* 0x10: dane */
gdt64_ptr:
    .word gdt64_ptr - gdt64 - 1
    .long gdt64

no64_msg: .ascii "Ten procesor nie ma trybu 64-bitowego - NaszOS nie moze wystartowac."
no64_end:

.section .text
.code32
.global _start
.type _start, @function
_start:
    mov $stack_top, %esp
    mov %eax, boot_magic
    mov %ebx, boot_info

    /* 1. Czy procesor zna tryb 64-bitowy? (CPUID 0x80000001, EDX bit 29) */
    mov $0x80000000, %eax
    cpuid
    cmp $0x80000001, %eax
    jb no_long_mode
    mov $0x80000001, %eax
    cpuid
    test $(1 << 29), %edx
    jz no_long_mode

    /* 2. Tablice stron: PML4[0] -> PDPT, PDPT[0..3] -> 4 katalogi,
     *    kazdy katalog: 512 stron po 2 MiB (bit 7 = "duza strona") */
    mov $boot_pdpt, %eax
    or $0x3, %eax                     /* obecna + zapis */
    mov %eax, boot_pml4
    xor %ecx, %ecx
1:  mov %ecx, %eax
    shl $12, %eax
    add $boot_pd, %eax
    or $0x3, %eax
    mov %eax, boot_pdpt(, %ecx, 8)
    inc %ecx
    cmp $4, %ecx
    jne 1b
    xor %ecx, %ecx                    /* 2048 wpisow = 4 GiB */
2:  mov %ecx, %eax
    shl $21, %eax                     /* adres = numer * 2 MiB */
    or $0x83, %eax                    /* obecna + zapis + duza strona */
    mov %eax, boot_pd(, %ecx, 8)      /* gorna polowa wpisu zostaje 0 (.bss) */
    inc %ecx
    cmp $2048, %ecx
    jne 2b

    /* 3. PAE, tryb dlugi, stronicowanie */
    mov $boot_pml4, %eax
    mov %eax, %cr3
    mov %cr4, %eax
    or $(1 << 5), %eax                /* PAE */
    mov %eax, %cr4
    mov $0xC0000080, %ecx             /* EFER */
    rdmsr
    or $(1 << 8), %eax                /* LME: tryb dlugi */
    wrmsr
    mov %cr0, %eax
    or $0x80000000, %eax              /* PG: stronicowanie */
    mov %eax, %cr0

    /* 4. 64-bitowa GDT i skok do kodu 64-bitowego */
    lgdt gdt64_ptr
    ljmp $0x08, $long_mode_start

no_long_mode:
    mov $no64_msg, %esi
    mov $0xB8000, %edi
3:  cmp $no64_end, %esi
    je 4f
    movb (%esi), %al
    movb %al, (%edi)
    movb $0x4F, 1(%edi)               /* bialy na czerwonym */
    inc %esi
    add $2, %edi
    jmp 3b
4:  cli
    hlt
    jmp 4b

.code64
long_mode_start:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    xor %ax, %ax
    mov %ax, %fs
    mov %ax, %gs
    mov $stack_top, %rsp
    xor %rbp, %rbp

    /* Koprocesor i SSE (liczby zmiennoprzecinkowe dla JavaScriptu):
     * CR0: EM=0 (instrukcje FPU wykonuje procesor), MP=1;
     * CR4: OSFXSR (system zapisuje stan przez fxsave), OSXMMEXCPT.
     * Stan FPU/SSE kazdego zadania zapisuje thread_yield (thread.c). */
    mov %cr0, %rax
    and $~(1 << 2), %rax
    or $(1 << 1), %rax
    mov %rax, %cr0
    mov %cr4, %rax
    or $(3 << 9), %rax
    mov %rax, %cr4
    fninit

    mov boot_magic, %edi              /* kernel_main(magic, multiboot_info*) */
    mov boot_info, %esi
    call kernel_main

    cli                               /* kernel_main nie powinno wrocic */
5:  hlt
    jmp 5b
.size _start, . - _start
