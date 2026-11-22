/* Lekser JavaScriptu - patrz js_lex.h. */

#include "js_lex.h"
#include "string.h"
#include "utf8.h"
#include <stdarg.h>

static int memchr_c(const char *s, unsigned n, char c)
{
    for (unsigned i = 0; i < n; i++)
        if (s[i] == c)
            return 1;
    return 0;
}

void lex_error(struct jlex *L, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char msg[160];
    kvsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    ksnprintf(L->err, L->errsize, "%s (linia %u)", msg, L->tok.line ? L->tok.line : L->line);
    js_longjmp(L->fail, 1);
}

void lex_init(struct jlex *L, js_rt *rt, const char *src, uint32_t len)
{
    memset(L, 0, sizeof(*L));
    L->rt = rt;
    L->src = src;
    L->len = len;
    L->line = 1;
}

static int peekc(struct jlex *L, uint32_t off)
{
    return L->pos + off < L->len ? (uint8_t)L->src[L->pos + off] : -1;
}

/* Znak Unicode spod pozycji pos (bez przesuwania); *n = liczba bajtow */
static uint32_t cp_at(struct jlex *L, uint32_t pos, unsigned *n)
{
    const char *s = L->src + pos, *start = s;
    uint32_t cp = utf8_next(&s, L->src + L->len);
    *n = (unsigned)(s - start);
    return cp;
}

static int is_id_start(uint32_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '$' || c == '_' ||
           (c >= 0x80 && c != 0xA0 && c != 0xFEFF && c != 0x2028 && c != 0x2029 &&
            !(c >= 0x2000 && c <= 0x200A) && c != 0x3000 && c != 0x202F && c != 0x205F && c != 0x1680);
}

static int is_id_part(uint32_t c)
{
    return is_id_start(c) || (c >= '0' && c <= '9') || c == 0x200C || c == 0x200D;
}

static int is_ws_cp(uint32_t c)
{
    return c == ' ' || c == '\t' || c == 0x0B || c == 0x0C || c == 0xA0 || c == 0xFEFF ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x3000 || c == 0x202F || c == 0x205F || c == 0x1680;
}

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Pomija odstepy i komentarze. Zwraca 1, gdy po drodze byl koniec linii */
static int skip_space(struct jlex *L)
{
    int nl = L->pos == 0;                           /* poczatek programu liczy sie jak nowa linia */
    int line_start = nl;
    while (L->pos < L->len) {
        int c = peekc(L, 0);
        if (c == '\n') {
            L->line++;
            L->pos++;
            nl = line_start = 1;
        } else if (c == ' ' || c == '\t' || c == '\r' || c == 0x0B || c == 0x0C) {
            L->pos++;
        } else if (c == '/' && peekc(L, 1) == '/') {
            while (L->pos < L->len && L->src[L->pos] != '\n')
                L->pos++;
        } else if (c == '/' && peekc(L, 1) == '*') {
            L->pos += 2;
            while (L->pos < L->len && !(L->src[L->pos] == '*' && peekc(L, 1) == '/')) {
                if (L->src[L->pos] == '\n') {
                    L->line++;
                    nl = 1;
                }
                L->pos++;
            }
            if (L->pos >= L->len)
                lex_error(L, "Niezamkniety komentarz /*");
            L->pos += 2;
        } else if (c == '<' && peekc(L, 1) == '!' && peekc(L, 2) == '-' && peekc(L, 3) == '-') {
            /* <!-- ze starych stron (ukrywanie skryptu) - komentarz do konca linii */
            while (L->pos < L->len && L->src[L->pos] != '\n')
                L->pos++;
        } else if (c == '-' && line_start && peekc(L, 1) == '-' && peekc(L, 2) == '>') {
            while (L->pos < L->len && L->src[L->pos] != '\n')
                L->pos++;
        } else if (c >= 0x80) {
            unsigned n;
            uint32_t cp = cp_at(L, L->pos, &n);
            if (cp == 0x2028 || cp == 0x2029) {
                L->line++;
                nl = line_start = 1;
            } else if (!is_ws_cp(cp)) {
                break;
            }
            L->pos += n;
        } else {
            break;
        }
    }
    return nl;
}

/* \uXXXX albo \u{X...} - pos wskazuje na 'u' */
static uint32_t read_unicode_escape(struct jlex *L)
{
    L->pos++;                                      /* 'u' */
    uint32_t v = 0;
    if (peekc(L, 0) == '{') {
        L->pos++;
        int n = 0;
        while (peekc(L, 0) != '}') {
            int h = hexv(peekc(L, 0));
            if (h < 0 || ++n > 6)
                lex_error(L, "Zly zapis \\u{...}");
            v = v * 16 + (uint32_t)h;
            L->pos++;
        }
        L->pos++;
        if (v > 0x10FFFF)
            lex_error(L, "Zly znak \\u{...}");
        return v;
    }
    for (int i = 0; i < 4; i++) {
        int h = hexv(peekc(L, 0));
        if (h < 0)
            lex_error(L, "Zly zapis \\uXXXX");
        v = v * 16 + (uint32_t)h;
        L->pos++;
    }
    return v;
}

static void put_cp(struct jsb *b, uint32_t cp)
{
    if (cp >= 0x10000) {
        cp -= 0x10000;
        jsb_char(b, (uint16_t)(0xD800 + (cp >> 10)));
        jsb_char(b, (uint16_t)(0xDC00 + (cp & 0x3FF)));
    } else {
        jsb_char(b, (uint16_t)cp);
    }
}

static void lex_ident(struct jlex *L)
{
    struct jtok *t = &L->tok;
    struct jsb b;
    jsb_init(&b, L->rt);
    /* szybka droga: sama nazwa ASCII */
    uint32_t p = L->pos;
    while (p < L->len) {
        char c = L->src[p];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '$' || c == '_')
            p++;
        else
            break;
    }
    if (p == L->len || ((uint8_t)L->src[p] < 0x80 && L->src[p] != '\\')) {
        jstr *s = js_str_new(L->rt, p - L->pos);
        for (uint32_t i = 0; i < p - L->pos; i++)
            s->c[i] = (uint8_t)L->src[L->pos + i];
        L->pos = p;
        t->type = TK_IDENT;
        t->str = js_intern(L->rt, s);
        return;
    }
    while (L->pos < L->len) {
        int c = peekc(L, 0);
        uint32_t cp;
        if (c == '\\') {
            if (peekc(L, 1) != 'u')
                lex_error(L, "Zly znak w nazwie");
            L->pos++;
            cp = read_unicode_escape(L);
            t->escaped = 1;
            if (!is_id_part(cp))
                lex_error(L, "Zly znak w nazwie");
        } else {
            unsigned n;
            cp = cp_at(L, L->pos, &n);
            if (!is_id_part(cp))
                break;
            L->pos += n;
        }
        put_cp(&b, cp);
    }
    t->type = TK_IDENT;
    t->str = js_intern(L->rt, jsb_done(&b));
}

static void lex_number(struct jlex *L)
{
    struct jtok *t = &L->tok;
    int c = peekc(L, 0), c1 = peekc(L, 1) | 0x20;
    t->type = TK_NUM;
    if (c == '0' && (c1 == 'x' || c1 == 'o' || c1 == 'b')) {
        int radix = c1 == 'x' ? 16 : c1 == 'o' ? 8 : 2;
        L->pos += 2;
        double v = 0;
        int any = 0;
        for (;;) {
            int d = peekc(L, 0);
            if (d == '_') {
                L->pos++;
                continue;
            }
            int h = hexv(d);
            if (h < 0 || h >= radix)
                break;
            v = v * radix + h;
            any = 1;
            L->pos++;
        }
        if (!any)
            lex_error(L, "Zly zapis liczby");
        if (peekc(L, 0) == 'n')
            lex_error(L, "BigInt nie jest obslugiwany");
        t->num = v;
        return;
    }
    /* Stary zapis osemkowy: 0777 */
    if (c == '0' && c1 >= '0' && c1 <= '7') {
        uint32_t p = L->pos + 1;
        double v = 0;
        int octal = 1;
        while (p < L->len && L->src[p] >= '0' && L->src[p] <= '9') {
            if (L->src[p] > '7')
                octal = 0;
            v = v * 8 + (L->src[p] - '0');
            p++;
        }
        if (octal) {
            L->pos = p;
            t->num = v;
            return;
        }
    }
    char buf[400];
    unsigned n = 0;
    while (L->pos < L->len && n < sizeof(buf) - 8) {
        int d = peekc(L, 0);
        if ((d >= '0' && d <= '9') || d == '.') {
            if (d == '.' && (memchr_c(buf, n, '.') || memchr_c(buf, n, 'e')))
                break;
            buf[n++] = (char)d;
            L->pos++;
        } else if (d == '_') {
            L->pos++;
        } else if ((d == 'e' || d == 'E') && !memchr_c(buf, n, 'e')) {
            int s = peekc(L, 1);
            if (!((s >= '0' && s <= '9') || ((s == '+' || s == '-') && peekc(L, 2) >= '0' && peekc(L, 2) <= '9')))
                break;
            buf[n++] = 'e';
            L->pos++;
            if (s == '+' || s == '-') {
                buf[n++] = (char)s;
                L->pos++;
            }
        } else {
            break;
        }
    }
    buf[n] = '\0';
    if (peekc(L, 0) == 'n')
        lex_error(L, "BigInt nie jest obslugiwany");
    if (peekc(L, 0) >= 0 && is_id_start((uint32_t)peekc(L, 0)) && peekc(L, 0) < 0x80)
        lex_error(L, "Nazwa zaraz po liczbie");
    t->num = js_strtod(buf, 0);
}

/* Znak po '\' w tekscie albo szablonie. Zwraca -1 dla kontynuacji linii */
static int32_t read_escape(struct jlex *L, int in_template)
{
    int c = peekc(L, 0);
    L->pos++;
    switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'b': return '\b';
    case 'f': return '\f';
    case 'v': return 0x0B;
    case '0':
        if (peekc(L, 0) >= '0' && peekc(L, 0) <= '9' && !in_template) {
            /* \012 - stary zapis osemkowy */
            int v = 0;
            L->pos--;
            for (int i = 0; i < 3 && peekc(L, 0) >= '0' && peekc(L, 0) <= '7'; i++)
                v = v * 8 + (L->src[L->pos++] - '0');
            return v;
        }
        return 0;
    case 'x': {
        int h = hexv(peekc(L, 0)), l = hexv(peekc(L, 1));
        if (h < 0 || l < 0)
            lex_error(L, "Zly zapis \\xHH");
        L->pos += 2;
        return h * 16 + l;
    }
    case 'u':
        L->pos--;
        return (int32_t)read_unicode_escape(L);
    case '\r':
        if (peekc(L, 0) == '\n')
            L->pos++;
        L->line++;
        return -1;
    case '\n':
        L->line++;
        return -1;
    default:
        if (c >= '1' && c <= '7' && !in_template) {
            int v = c - '0';
            for (int i = 0; i < 2 && peekc(L, 0) >= '0' && peekc(L, 0) <= '7'; i++)
                v = v * 8 + (L->src[L->pos++] - '0');
            return v;
        }
        if (c >= 0x80) {                           /* znak UTF-8 po '\' - sam znak */
            unsigned n;
            L->pos--;
            uint32_t cp = cp_at(L, L->pos, &n);
            L->pos += n;
            if (cp == 0x2028 || cp == 0x2029)
                return -1;
            return (int32_t)cp;
        }
        if (c < 0)
            lex_error(L, "Niezamkniety tekst");
        return c;
    }
}

static void lex_string(struct jlex *L, int quote)
{
    struct jsb b;
    jsb_init(&b, L->rt);
    L->pos++;
    for (;;) {
        if (L->pos >= L->len)
            lex_error(L, "Niezamkniety tekst");
        int c = peekc(L, 0);
        if (c == quote) {
            L->pos++;
            break;
        }
        if (c == '\n' || c == '\r')
            lex_error(L, "Koniec linii w tekscie");
        if (c == '\\') {
            L->pos++;
            int32_t e = read_escape(L, 0);
            if (e >= 0)
                put_cp(&b, (uint32_t)e);
            continue;
        }
        if (c < 0x80) {
            jsb_char(&b, (uint16_t)c);
            L->pos++;
        } else {
            unsigned n;
            put_cp(&b, cp_at(L, L->pos, &n));
            L->pos += n;
        }
    }
    L->tok.type = TK_STR;
    L->tok.str = jsb_done(&b);
}

/* Kawalek szablonu `...${ albo }...` - pos za ` albo } */
static void lex_template_part(struct jlex *L)
{
    struct jsb b, raw;
    jsb_init(&b, L->rt);
    jsb_init(&raw, L->rt);
    for (;;) {
        if (L->pos >= L->len)
            lex_error(L, "Niezamkniety szablon `");
        int c = peekc(L, 0);
        if (c == '`') {
            L->pos++;
            L->tok.tmpl_tail = 1;
            break;
        }
        if (c == '$' && peekc(L, 1) == '{') {
            L->pos += 2;
            L->tok.tmpl_tail = 0;
            break;
        }
        if (c == '\\') {
            uint32_t from = L->pos;
            L->pos++;
            int32_t e = read_escape(L, 1);
            if (e >= 0)
                put_cp(&b, (uint32_t)e);
            jsb_utf8(&raw, L->src + from, L->pos - from);
            continue;
        }
        if (c == '\r') {                            /* \r\n w szablonie to \n */
            L->pos++;
            if (peekc(L, 0) == '\n')
                L->pos++;
            jsb_char(&b, '\n');
            jsb_char(&raw, '\n');
            L->line++;
            continue;
        }
        if (c == '\n')
            L->line++;
        if (c < 0x80) {
            jsb_char(&b, (uint16_t)c);
            jsb_char(&raw, (uint16_t)c);
            L->pos++;
        } else {
            unsigned n;
            uint32_t cp = cp_at(L, L->pos, &n);
            put_cp(&b, cp);
            put_cp(&raw, cp);
            L->pos += n;
        }
    }
    L->tok.type = TK_TEMPLATE;
    L->tok.str = jsb_done(&b);
    L->tok.raw = jsb_done(&raw);
}

/* Znaki dzialan - najdluzsze pasujace */
static const struct { const char *s; int op; } puncts[] = {
    { ">>>=", OP_USHR_A }, { "...", OP_ELLIPSIS }, { "===", OP_SEQ }, { "!==", OP_SNE },
    { "**=", OP_POW_A }, { "<<=", OP_SHL_A }, { ">>=", OP_SHR_A }, { ">>>", OP_USHR },
    { "&&=", OP_LAND_A }, { "||=", OP_LOR_A }, { "?\?=", OP_NULLISH_A },
    { "=>", OP_ARROW }, { "==", OP_EQ }, { "!=", OP_NE }, { "<=", OP_LE }, { ">=", OP_GE },
    { "&&", OP_AND }, { "||", OP_OR }, { "??", OP_NULLISH }, { "?.", OP_OPTCHAIN },
    { "++", OP_INC }, { "--", OP_DEC }, { "<<", OP_SHL }, { ">>", OP_SHR }, { "**", OP_POW },
    { "+=", OP_ADD_A }, { "-=", OP_SUB_A }, { "*=", OP_MUL_A }, { "/=", OP_DIV_A },
    { "%=", OP_MOD_A }, { "&=", OP_AND_A }, { "|=", OP_OR_A }, { "^=", OP_XOR_A },
    { 0, 0 },
};

void lex_next(struct jlex *L)
{
    struct jtok *t = &L->tok;
    int nl = skip_space(L);
    memset(t, 0, sizeof(*t));
    t->nl_before = nl;
    t->line = L->line;
    t->start = L->pos;
    if (L->pos >= L->len) {
        t->type = TK_EOF;
        t->end = L->pos;
        return;
    }
    int c = peekc(L, 0);
    if (c >= '0' && c <= '9') {
        lex_number(L);
    } else if (c == '.' && peekc(L, 1) >= '0' && peekc(L, 1) <= '9') {
        lex_number(L);
    } else if (c == '"' || c == '\'') {
        lex_string(L, c);
    } else if (c == '`') {
        L->pos++;
        lex_template_part(L);
    } else if (c == '#' && L->pos + 1 < L->len && is_id_start((uint8_t)L->src[L->pos + 1])) {
        /* pole prywatne klasy (#x) - traktujemy jak zwykla nazwe "#x" */
        L->pos++;
        lex_ident(L);
        jstr *name = t->str;
        jstr *s = js_str_new(L->rt, name->len + 1);
        s->c[0] = '#';
        memcpy(s->c + 1, name->c, name->len * 2);
        t->type = TK_PRIVATE;
        t->str = js_intern(L->rt, s);
    } else if (c == '\\' || (c < 0x80 && is_id_start((uint32_t)c))) {
        lex_ident(L);
    } else if (c >= 0x80) {
        unsigned n;
        uint32_t cp = cp_at(L, L->pos, &n);
        if (!is_id_start(cp))
            lex_error(L, "Nieznany znak U+%x", cp);
        lex_ident(L);
    } else {
        t->type = TK_PUNCT;
        t->op = 0;
        for (int i = 0; puncts[i].s; i++) {
            unsigned n = (unsigned)strlen(puncts[i].s);
            if (L->pos + n <= L->len && memcmp(L->src + L->pos, puncts[i].s, n) == 0) {
                /* "?." przed cyfra to "?" i liczba (a?.5:1) */
                if (puncts[i].op == OP_OPTCHAIN && peekc(L, 2) >= '0' && peekc(L, 2) <= '9')
                    continue;
                t->op = puncts[i].op;
                L->pos += n;
                break;
            }
        }
        if (!t->op) {
            if (!memchr_c("{}()[];,<>+-*/%&|^!~?:=.@", 25, (char)c))
                lex_error(L, "Nieoczekiwany znak '%c'", c);
            t->op = c;
            L->pos++;
        }
    }
    t->end = L->pos;
}

void lex_regex(struct jlex *L)
{
    struct jtok *t = &L->tok;
    L->pos = t->start + 1;
    int in_class = 0;
    uint32_t body = L->pos;
    for (;;) {
        int c = peekc(L, 0);
        if (c < 0 || c == '\n' || c == '\r')
            lex_error(L, "Niezamkniete wyrazenie regularne");
        if (c == '\\') {
            L->pos += 2;
            continue;
        }
        if (c == '[')
            in_class = 1;
        else if (c == ']')
            in_class = 0;
        else if (c == '/' && !in_class)
            break;
        L->pos++;
    }
    uint32_t body_end = L->pos;
    L->pos++;
    uint32_t fl = L->pos;
    while (L->pos < L->len && ((L->src[L->pos] >= 'a' && L->src[L->pos] <= 'z') || (L->src[L->pos] >= 'A' && L->src[L->pos] <= 'Z')))
        L->pos++;
    struct jregex_lit *re = js_alloc(L->rt, sizeof(*re));
    re->source = js_str_utf8(L->rt, L->src + body, body_end - body);
    re->flags = js_str_utf8(L->rt, L->src + fl, L->pos - fl);
    t->type = TK_REGEX;
    t->re = re;
    t->end = L->pos;
}

void lex_template_cont(struct jlex *L)
{
    struct jtok *t = &L->tok;
    L->pos = t->start + 1;
    uint32_t line = t->line;
    int nl = t->nl_before;
    memset(t, 0, sizeof(*t));
    t->line = line;
    t->nl_before = nl;
    t->start = L->pos;
    lex_template_part(L);
    t->end = L->pos;
}
