#ifndef STORAGE_H
#define STORAGE_H

#include <stdint.h>

enum {
    STORAGE_OK       =  0,
    STORAGE_EMPTY    = -1,   /* na dysku nie ma jeszcze zapisanego systemu plikow */
    STORAGE_ENODISK  = -2,
    STORAGE_EIO      = -3,
    STORAGE_ENOSPC   = -4,
    STORAGE_ENOMEM   = -5,
    STORAGE_ECORRUPT = -6,
};

int  storage_init(void);                 /* 1 = jest dysk */
int  storage_load(uint32_t *entries);    /* wczytuje drzewo plikow z dysku */
int  storage_save(void);                 /* zapisuje cale drzewo na dysk */
int  storage_sync_if_dirty(void);        /* zapisuje, jesli cos sie zmienilo */
void storage_print_info(void);
const char *storage_strerror(int err);

#endif
