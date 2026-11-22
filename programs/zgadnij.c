/* Gra: zgadnij liczbe od 1 do 100. */
#include "naszos.h"

static int parse(const char *s, int *out)
{
    int v = 0, digits = 0;
    while (*s == ' ')
        s++;
    while (*s >= '0' && *s <= '9' && digits < 6) {
        v = v * 10 + (*s++ - '0');
        digits++;
    }
    *out = v;
    return digits > 0 && *s == '\0';
}

int main(void)
{
    char line[20];
    for (;;) {
        int secret = random(100) + 1, tries = 0, guess;
        set_color(LIGHT_CYAN, BLACK);
        print("\nMysle o liczbie od 1 do 100. Zgadnij!\n");
        for (;;) {
            set_color(LIGHT_GREY, BLACK);
            print("Twoj strzal: ");
            set_color(WHITE, BLACK);
            read_line(line, sizeof(line));
            if (!parse(line, &guess) || guess < 1 || guess > 100) {
                set_color(LIGHT_RED, BLACK);
                print("  To nie jest liczba od 1 do 100.\n");
                continue;
            }
            tries++;
            if (guess < secret) {
                set_color(YELLOW, BLACK);
                print("  Za malo!\n");
            } else if (guess > secret) {
                set_color(YELLOW, BLACK);
                print("  Za duzo!\n");
            } else {
                set_color(LIGHT_GREEN, BLACK);
                print("  Brawo! Zgadles w ");
                print_int(tries);
                print(tries == 1 ? " probie!\n" : " probach!\n");
                break;
            }
        }
        set_color(LIGHT_GREY, BLACK);
        print("Jeszcze raz? (t/n) ");
        int k = get_key();
        print_char((char)k);
        print("\n");
        if (k != 't' && k != 'T')
            return tries;
    }
}
