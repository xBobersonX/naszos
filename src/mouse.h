#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

#define MOUSE_LEFT    0x01
#define MOUSE_RIGHT   0x02
#define MOUSE_MIDDLE  0x04
#define MOUSE_BUTTONS 0x07

/* Zmiana stanu przyciskow: pozycja w pikselach i przyciski wcisniete PO zmianie */
struct mouse_event {
    int x, y;
    uint8_t buttons;
};

int  mouse_init(void);                             /* 1 = mysz odpowiada */
void mouse_last_click(unsigned *x, unsigned *y);   /* pole ekranu ostatniego klikniecia (konsola) */
void mouse_position(int *x, int *y);               /* biezaca pozycja w pikselach */
int  mouse_buttons(void);                          /* wcisniete przyciski (MOUSE_*) */
int  mouse_poll_event(struct mouse_event *e);      /* 1 = bylo zdarzenie przycisku */
int  mouse_has_wheel(void);
int  mouse_wheel(void);                            /* obroty kolka od ostatniego razu (+ = w dol) */

#endif
