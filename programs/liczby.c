/* Liczy liczby pierwsze - dlugie obliczenia bez przerw. Pokazuje, ze NaszOS
 * odbiera programowi procesor co kilka tykniec zegara: pulpit, zegar i inne
 * okna dzialaja dalej, choc program ani razu sam nie "odpoczywa". */
#include "naszos.h"

static int is_prime(int n)
{
    if (n < 2)
        return 0;
    for (int d = 2; d * d <= n; d++)
        if (n % d == 0)
            return 0;
    return 1;
}

int main(void)
{
    set_color(LIGHT_CYAN, BLACK);
    print("Szukam liczb pierwszych do 15 000 000 (to potrwa chwile)...\n");
    set_color(DARK_GREY, BLACK);
    print("W tym czasie mozesz swobodnie uzywac pulpitu.\n\n");

    int start = time_ms(), found = 0;
    for (int n = 2; n <= 15000000; n++) {
        found += is_prime(n);
        if (n % 1500000 == 0) {
            set_color(LIGHT_GREY, BLACK);
            print("  do ");
            print_int(n);
            print(": ");
            set_color(YELLOW, BLACK);
            print_int(found);
            set_color(LIGHT_GREY, BLACK);
            print(" liczb pierwszych\n");
        }
    }
    set_color(LIGHT_GREEN, BLACK);
    print("\nGotowe w ");
    print_int((time_ms() - start) / 1000);
    print(" s. Ostatni wynik: ");
    print_int(found);
    print("\n");
    return 0;
}
