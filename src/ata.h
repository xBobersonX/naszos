#ifndef ATA_H
#define ATA_H

#include <stdint.h>

#define ATA_SECTOR_SIZE 512

int  ata_init(void);                  /* 1 = znaleziono dysk */
uint32_t ata_sectors(void);           /* pojemnosc w sektorach po 512 B */
const char *ata_model(void);

/* 0 = sukces, -1 = blad lub przekroczony zakres */
int ata_read(uint32_t lba, uint32_t count, void *buf);
int ata_write(uint32_t lba, uint32_t count, const void *buf);

#endif
