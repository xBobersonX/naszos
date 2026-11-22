/* Kalkulator: liczy wyrazenia z + - * / %, nawiasami i minusem, np. (2+3)*4.
 * Parser "zstepujacy": wyrazenie = skladnik {+|- skladnik},
 * skladnik = czynnik {*|/|% czynnik}, czynnik = liczba | -czynnik | (wyrazenie). */
#include "naszos.h"

static const char *p;
static const char *error;

static void skip_spaces(void)
{
    while (*p == ' ')
        p++;
}

static int expression(void);

static int factor(void)
{
    skip_spaces();
    if (*p == '-') {
        p++;
        return -factor();
    }
    if (*p == '(') {
        p++;
        int v = expression();
        skip_spaces();
        if (*p != ')' && !error)
            error = "brakuje nawiasu )";
        else
            p++;
        return v;
    }
    if (*p < '0' || *p > '9') {
        if (!error)
            error = *p ? "nieoczekiwany znak" : "niedokonczone wyrazenie";
        return 0;
    }
    int v = 0;
    while (*p >= '0' && *p <= '9')
        v = v * 10 + (*p++ - '0');
    return v;
}

static int term(void)
{
    int v = factor();
    for (;;) {
        skip_spaces();
        char op = *p;
        if (op != '*' && op != '/' && op != '%')
            return v;
        p++;
        int r = factor();
        if ((op == '/' || op == '%') && r == 0) {
            if (!error)
                error = "dzielenie przez zero";
            return 0;
        }
        v = op == '*' ? v * r : op == '/' ? v / r : v % r;
    }
}

static int expression(void)
{
    int v = term();
    for (;;) {
        skip_spaces();
        if (*p == '+') {
            p++;
            v += term();
        } else if (*p == '-') {
            p++;
            v -= term();
        } else {
            return v;
        }
    }
}

int main(void)
{
    set_color(LIGHT_CYAN, BLACK);
    print("Kalkulator NaszOS\n");
    set_color(DARK_GREY, BLACK);
    print("Wpisz dzialanie, np. (2+3)*4 albo 100/7. Pusta linia konczy.\n\n");

    char line[80];
    for (;;) {
        set_color(LIGHT_GREEN, BLACK);
        print("> ");
        set_color(WHITE, BLACK);
        if (read_line(line, sizeof(line)) == 0)
            break;
        p = line;
        error = 0;
        int v = expression();
        skip_spaces();
        if (!error && *p)
            error = "nieoczekiwany znak";
        if (error) {
            set_color(LIGHT_RED, BLACK);
            print("  Blad: ");
            print(error);
        } else {
            set_color(YELLOW, BLACK);
            print("  = ");
            print_int(v);
        }
        print("\n");
    }
    set_color(LIGHT_GREY, BLACK);
    print("Do zobaczenia!\n");
    return 0;
}
