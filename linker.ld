ENTRY(_start)

SECTIONS
{
    /* Kernel ladowany pod 1 MiB - klasyczne miejsce dla Multiboot */
    . = 1M;
    _kernel_start = .;

    .text : ALIGN(4K)
    {
        KEEP(*(.multiboot))   /* naglowek musi byc w pierwszych 8 KiB */
        *(.text*)
    }

    .rodata : ALIGN(4K) { *(.rodata*) }
    .data   : ALIGN(4K) { *(.data*) }
    .bss    : ALIGN(4K) { *(COMMON) *(.bss*) }

    _kernel_end = .;
}
