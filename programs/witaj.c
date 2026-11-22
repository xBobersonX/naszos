/* Pierwszy program dla NaszOS: pyta o imie i sie wita. */
#include "naszos.h"

int main(void)
{
    set_color(LIGHT_CYAN, BLACK);
    print("Witaj! To jest pierwszy program dla NaszOS.\n\n");
    set_color(LIGHT_GREY, BLACK);
    print("Jak masz na imie? ");

    char name[40];
    set_color(WHITE, BLACK);
    read_line(name, sizeof(name));
    set_color(LIGHT_GREY, BLACK);

    print("\nCzesc, ");
    set_color(YELLOW, BLACK);
    print(name[0] ? name : "nieznajomy");
    set_color(LIGHT_GREY, BLACK);
    print("! Milo cie poznac.\n\n");
    print("Ten program dziala w trybie uzytkownika, we wlasnej pamieci -\n");
    print("nie moze zepsuc systemu, nawet gdyby bardzo chcial.\n");
    return 0;
}
