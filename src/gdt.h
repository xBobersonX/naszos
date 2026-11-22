#ifndef GDT_H
#define GDT_H

#include <stdint.h>

#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_CODE   (0x18 | 3)   /* | 3 = poziom uprzywilejowania ring 3 */
#define GDT_USER_DATA   (0x20 | 3)
#define GDT_TSS         0x28

void gdt_init(void);
/* Stos kernela uzywany, gdy program (ring 3) przechodzi do systemu */
void gdt_set_kernel_stack(uint64_t rsp0);

#endif
