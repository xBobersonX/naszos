/* Test ochrony pamieci: program celowo probuje zapisac cos w pamieci systemu.
 * Procesor na to nie pozwoli (strona tylko dla kernela), NaszOS zamknie
 * program i bedzie dzialal dalej. */
#include "naszos.h"

int main(void)
{
    set_color(YELLOW, BLACK);
    print("Ten program sprobuje nadpisac pamiec systemu (adres 0x00100000,\n");
    print("czyli kod kernela NaszOS).\n\n");
    set_color(LIGHT_GREY, BLACK);
    print("Nacisnij dowolny klawisz, aby sprobowac...\n");
    get_key();

    volatile unsigned *kernel = (volatile unsigned *)0x00100000;
    *kernel = 0xDEADBEEF;                 /* tu procesor zglosi blad strony */

    print("Tego tekstu nie powinno byc widac - ochrona pamieci nie zadzialala!\n");
    return 0;
}
