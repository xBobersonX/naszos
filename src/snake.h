#ifndef SNAKE_H
#define SNAKE_H

#include <stdint.h>

/* Gra Snake w konsoli (tekstowa); wraca do powloki po Q/Esc */
void snake_run(void);

/* Rekord w pliku /snake_rekord.txt - wspolny dla wersji tekstowej i okienkowej */
uint32_t snake_load_record(void);
void snake_save_record(uint32_t record);

#endif
