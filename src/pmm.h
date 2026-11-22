#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include "multiboot.h"

#define FRAME_SIZE 4096

void     pmm_init(struct multiboot_info *mbi);
uintptr_t pmm_alloc(void);            /* adres wolnej ramki albo 0 */
void     pmm_free(uintptr_t addr);
int      pmm_claim(uintptr_t addr);   /* zajmij konkretna ramke; 1 = udalo sie */

uint32_t pmm_total_frames(void);
uint32_t pmm_used_frames(void);
uint64_t pmm_top(void);              /* koniec najwyzszego obszaru RAM */
uintptr_t pmm_first_free_addr(void);   /* pierwszy adres za kernelem i mapa bitowa */
void     pmm_print_map(void);

#endif
