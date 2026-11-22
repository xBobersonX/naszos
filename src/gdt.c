/* Global Descriptor Table - opisuje segmenty pamieci. W trybie 64-bitowym
 * segmenty prawie nie maja znaczenia (baza 0, bez limitu), ale procesor dalej
 * wymaga deskryptorow kodu/danych dla kernela (ring 0) i programow (ring 3),
 * z bitem L = "kod 64-bitowy", oraz deskryptora TSS (16 bajtow). */

#include <stdint.h>
#include "gdt.h"

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* Task State Segment (64 bity) - procesor bierze z niego stos kernela (rsp0),
 * gdy program w ring 3 wywola system albo przyjdzie przerwanie */
struct tss {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

static uint64_t gdt[7];                 /* 5 zwyklych + TSS (2 wpisy) */
static struct gdt_ptr gdtp;
static struct tss tss;

extern void gdt_flush(struct gdt_ptr *p);

/* access: typ i uprawnienia; flags: gorne 4 bity bajtu "granularity" (G, D/B, L) */
static uint64_t descriptor(uint32_t base, uint32_t limit, uint8_t access, uint8_t flags)
{
    return (uint64_t)(limit & 0xFFFF)
         | (uint64_t)(base & 0xFFFFFF) << 16
         | (uint64_t)access << 40
         | (uint64_t)((limit >> 16) & 0x0F) << 48
         | (uint64_t)(flags & 0xF0) << 48
         | (uint64_t)(base >> 24) << 56;
}

void gdt_init(void)
{
    gdt[0] = 0;                                        /* wymagany pusty deskryptor */
    gdt[1] = descriptor(0, 0xFFFFF, 0x9A, 0xA0);       /* 0x08: kod kernela (L=1) */
    gdt[2] = descriptor(0, 0xFFFFF, 0x92, 0xC0);       /* 0x10: dane kernela   */
    gdt[3] = descriptor(0, 0xFFFFF, 0xFA, 0xA0);       /* 0x18: kod programow (L=1) */
    gdt[4] = descriptor(0, 0xFFFFF, 0xF2, 0xC0);       /* 0x20: dane programow */

    /* TSS: 16-bajtowy deskryptor, gorne 32 bity adresu w drugim wpisie */
    uint64_t base = (uint64_t)&tss;
    tss.iomap_base = sizeof(tss);                      /* bez mapy portow - programy nie maja do nich dostepu */
    gdt[5] = descriptor((uint32_t)base, sizeof(tss) - 1, 0x89, 0x00);
    gdt[6] = base >> 32;

    gdtp.limit = sizeof(gdt) - 1;
    gdtp.base  = (uint64_t)&gdt;
    gdt_flush(&gdtp);
    __asm__ volatile ("ltr %w0" : : "r"(GDT_TSS));
}

void gdt_set_kernel_stack(uint64_t rsp0)
{
    tss.rsp0 = rsp0;
}
