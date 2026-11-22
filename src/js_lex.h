#ifndef JS_LEX_H
#define JS_LEX_H

/* Lekser JavaScriptu (src/js_lex.c) - wspolny z parserem (js_parse.c).
 * Tnie tekst programu na tokeny: slowa (nazwy), liczby, teksty, znaki
 * dzialan. Zapamietuje, czy przed tokenem byl koniec linii - JS pozwala
 * pominac srednik na koncu linii (ASI). */

#include "js_int.h"

enum { TK_EOF, TK_NUM, TK_STR, TK_TEMPLATE, TK_IDENT, TK_PUNCT, TK_REGEX, TK_PRIVATE };

struct jtok {
    int type;
    int op;                          /* TK_PUNCT: znak ASCII albo OP_* */
    int nl_before;                   /* przed tokenem byl koniec linii */
    int escaped;                     /* nazwa zawierala \u - to nie slowo kluczowe */
    int tmpl_tail;                   /* szablon: ostatni kawalek (konczy sie znakiem `) */
    uint32_t line;
    uint32_t start, end;             /* polozenie w tekscie */
    double num;
    jstr *str;                       /* tekst / nazwa (atom) / kawalek szablonu */
    jstr *raw;                       /* szablon: tekst bez zamiany \n itp. */
    struct jregex_lit *re;
};

struct jlex {
    js_rt *rt;
    const char *src;
    uint32_t len, pos, line;
    struct jtok tok;
    char *err;
    unsigned errsize;
    struct js_jmp *fail;             /* skok przy bledzie skladni */
};

void lex_init(struct jlex *L, js_rt *rt, const char *src, uint32_t len);
void lex_next(struct jlex *L);
void lex_regex(struct jlex *L);          /* biezacy token "/" albo "/=" to poczatek wyrazenia regularnego */
void lex_template_cont(struct jlex *L);  /* biezacy token "}" konczy ${...} w szablonie */
void lex_error(struct jlex *L, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));

#endif
