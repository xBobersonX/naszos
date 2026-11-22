#ifndef KEYBOARD_H
#define KEYBOARD_H

/* Klawisze specjalne - kody powyzej zakresu ASCII */
enum {
    KEY_UP = 0x100,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_HOME,
    KEY_END,
    KEY_DELETE,
    KEY_PGUP,
    KEY_PGDN,
    KEY_MOUSE_LEFT,     /* klikniecie lewym przyciskiem - pozycja: mouse_last_click() */
    KEY_REFRESH,        /* "przerysuj sie" - np. okno terminala zmienilo rozmiar */
};

/* Ctrl + litera daje kod sterujacy 1-26, np. KEY_CTRL('s') = 19 */
#define KEY_CTRL(letter) ((letter) - 'a' + 1)

void keyboard_init(void);
void keyboard_push(int key);    /* dla innych sterownikow (np. myszy) - tylko z przerwania */
int  keyboard_poll(void);       /* znak ASCII, KEY_* albo -1, gdy bufor pusty */
int  keyboard_getkey(void);     /* czeka na klawisz */

#endif
