/* Wyrazenia regularne JavaScriptu (/wzorzec/flagi) i metody tekstow, ktore
 * z nich korzystaja: replace, match, matchAll, search, split.
 *
 * Wzorzec tlumaczymy na maly program dla "maszyny dopasowujacej":
 * instrukcje typu "znak a", "dowolny znak", "rozgalezienie: sprobuj
 * najpierw tu, a jak sie nie uda - tam". Maszyna idzie po tekscie; przy
 * rozgalezieniu odklada druga droge na stos i wraca do niej, gdy pierwsza
 * zawiedzie (tzw. nawroty). Stos jest w pamieci, nie na stosie C, wiec
 * dlugie teksty go nie przepelnia. */

#include "js_int.h"
#include "string.h"

#define U __attribute__((unused))
#define NATIVE(name) static jv name(js_rt *rt U, jv self U, int argc U, jv *argv U, jobj *fn U)
#define ARG(i) ((i) < argc ? argv[i] : jv_undef())

#define PROG_MAX  60000
#define GROUPS_MAX 64
#define REGS_MAX  256

enum {
    R_CHAR, R_ANY, R_CLASS, R_SPLIT, R_JMP, R_SAVE, R_MATCH, R_BOL, R_EOL, R_WORDB, R_NWORDB,
    R_BACKREF, R_LOOK, R_LOOKEND, R_MARK, R_CHECK,
};

struct rinst {
    uint8_t op;
    uint8_t neg;                     /* R_LOOK: negacja; R_CLASS: klasa zanegowana */
    uint8_t behind;                  /* R_LOOK: (?<= ) */
    int32_t x, y;
};

/* Klasa znakow [a-z\d]: zakresy i skroty (\D \W \S w klasie) */
enum { CI_RANGE, CI_NDIGIT, CI_NWORD, CI_NSPACE };
struct citem {
    uint8_t kind;
    uint32_t lo, hi;
};
struct rclass {
    struct citem *items;
    int n, cap;
};

struct jregex {
    struct rinst *prog;
    int n;
    struct rclass *classes;
    int nclasses;
    int ngroups;                     /* razem z grupa 0 */
    int nregs;
    jstr *names[GROUPS_MAX];
    uint8_t icase, multiline, dotall, global, sticky, unicode;
};

/* ---------- drzewo wzorca ---------- */

enum { RN_CHAR, RN_ANY, RN_CLASS, RN_SEQ, RN_ALT, RN_GROUP, RN_REPEAT, RN_BOL, RN_EOL, RN_WORDB,
       RN_NWORDB, RN_BACKREF, RN_LOOK, RN_EMPTY };

struct rnode {
    uint8_t type, neg, greedy, behind;
    int value;                       /* znak, numer klasy, numer grupy */
    int min, max;                    /* powtorzenie; max -1 = bez konca */
    struct rnode *a, *b;             /* dzieci / lista */
    struct rnode *next;
};

struct rparse {
    js_rt *rt;
    struct jregex *re;
    const uint16_t *s;
    uint32_t i, n;
    char *err;
    unsigned errsize;
    int failed;
    int ngroups;
    int nbackrefs_named;
    jstr *pending_names[GROUPS_MAX];
};

static struct rnode *rn(struct rparse *p, int type)
{
    struct rnode *n = js_alloc(p->rt, sizeof(*n));
    n->type = (uint8_t)type;
    n->greedy = 1;
    return n;
}

static void perr(struct rparse *p, const char *msg)
{
    if (!p->failed)
        ksnprintf(p->err, p->errsize, "Invalid regular expression: %s", msg);
    p->failed = 1;
}

static int peek(struct rparse *p) { return p->i < p->n ? p->s[p->i] : -1; }

static int new_class(struct rparse *p)
{
    struct jregex *re = p->re;
    struct rclass *nc = js_alloc(p->rt, sizeof(struct rclass) * (size_t)(re->nclasses + 1));
    if (re->nclasses)
        memcpy(nc, re->classes, sizeof(struct rclass) * (size_t)re->nclasses);
    re->classes = nc;
    return re->nclasses++;
}

static void class_add(struct rparse *p, int cls, int kind, uint32_t lo, uint32_t hi)
{
    struct rclass *c = &p->re->classes[cls];
    if (c->n == c->cap) {
        int cap = c->cap ? c->cap * 2 : 8;
        struct citem *ni = js_alloc(p->rt, sizeof(struct citem) * (size_t)cap);
        if (c->n)
            memcpy(ni, c->items, sizeof(struct citem) * (size_t)c->n);
        c->items = ni;
        c->cap = cap;
    }
    c->items[c->n].kind = (uint8_t)kind;
    c->items[c->n].lo = lo;
    c->items[c->n++].hi = hi;
}

static const uint32_t space_ranges[][2] = {
    { 9, 13 }, { 32, 32 }, { 0xA0, 0xA0 }, { 0x1680, 0x1680 }, { 0x2000, 0x200A }, { 0x2028, 0x2029 },
    { 0x202F, 0x202F }, { 0x205F, 0x205F }, { 0x3000, 0x3000 }, { 0xFEFF, 0xFEFF },
};

/* \d \w \s do klasy (neg: \D \W \S) */
static void class_shorthand(struct rparse *p, int cls, int ch)
{
    switch (ch) {
    case 'd': class_add(p, cls, CI_RANGE, '0', '9'); break;
    case 'D': class_add(p, cls, CI_NDIGIT, 0, 0); break;
    case 'w':
        class_add(p, cls, CI_RANGE, 'a', 'z');
        class_add(p, cls, CI_RANGE, 'A', 'Z');
        class_add(p, cls, CI_RANGE, '0', '9');
        class_add(p, cls, CI_RANGE, '_', '_');
        break;
    case 'W': class_add(p, cls, CI_NWORD, 0, 0); break;
    case 's':
        for (unsigned k = 0; k < sizeof(space_ranges) / sizeof(space_ranges[0]); k++)
            class_add(p, cls, CI_RANGE, space_ranges[k][0], space_ranges[k][1]);
        break;
    case 'S': class_add(p, cls, CI_NSPACE, 0, 0); break;
    }
}

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') return (c | 0x20) - 'a' + 10;
    return -1;
}

/* Znak po '\' (poza \d \w \s \b i odwolaniami). pos wskazuje za '\' */
static uint32_t escape_char(struct rparse *p)
{
    int c = peek(p);
    p->i++;
    switch (c) {
    case 'n': return '\n';
    case 'r': return '\r';
    case 't': return '\t';
    case 'f': return '\f';
    case 'v': return 0x0B;
    case '0': return 0;
    case 'c': {
        int l = peek(p);
        if ((l | 0x20) >= 'a' && (l | 0x20) <= 'z') {
            p->i++;
            return (uint32_t)(l & 31);
        }
        return '\\';
    }
    case 'x': {
        int h = hexv(peek(p)), l = p->i + 1 < p->n ? hexv(p->s[p->i + 1]) : -1;
        if (h >= 0 && l >= 0) {
            p->i += 2;
            return (uint32_t)(h * 16 + l);
        }
        return 'x';
    }
    case 'u': {
        if (peek(p) == '{') {
            uint32_t v = 0, k = p->i + 1;
            while (k < p->n && hexv(p->s[k]) >= 0)
                v = v * 16 + (uint32_t)hexv(p->s[k++]);
            if (k < p->n && p->s[k] == '}') {
                p->i = k + 1;
                return v;
            }
            return 'u';
        }
        uint32_t v = 0;
        for (int k = 0; k < 4; k++) {
            int h = p->i + k < p->n ? hexv(p->s[p->i + k]) : -1;
            if (h < 0)
                return 'u';
            v = v * 16 + (uint32_t)h;
        }
        p->i += 4;
        return v;
    }
    case -1:
        perr(p, "\\ at end of pattern");
        return 0;
    }
    return (uint32_t)c;
}

static struct rnode *parse_alt(struct rparse *p);

static struct rnode *parse_class(struct rparse *p)
{
    p->i++;                                          /* [ */
    int cls = new_class(p);
    struct rnode *n = rn(p, RN_CLASS);
    n->value = cls;
    if (peek(p) == '^') {
        n->neg = 1;
        p->i++;
    }
    int first = 1;
    while (p->i < p->n && (peek(p) != ']' || (first && 0))) {
        first = 0;
        uint32_t lo;
        int c = peek(p);
        if (c == '\\') {
            p->i++;
            int e = peek(p);
            if (e == 'd' || e == 'D' || e == 'w' || e == 'W' || e == 's' || e == 'S') {
                p->i++;
                class_shorthand(p, cls, e);
                continue;
            }
            if (e == 'b') {
                p->i++;
                lo = 8;
            } else {
                lo = escape_char(p);
            }
        } else {
            lo = (uint32_t)c;
            p->i++;
        }
        uint32_t hi = lo;
        if (peek(p) == '-' && p->i + 1 < p->n && p->s[p->i + 1] != ']') {
            p->i++;
            int c2 = peek(p);
            if (c2 == '\\') {
                p->i++;
                int e = peek(p);
                if (e == 'd' || e == 'D' || e == 'w' || e == 'W' || e == 's' || e == 'S') {
                    /* [a-\d] - myslnik jako znak */
                    class_add(p, cls, CI_RANGE, lo, lo);
                    class_add(p, cls, CI_RANGE, '-', '-');
                    p->i++;
                    class_shorthand(p, cls, e);
                    continue;
                }
                hi = escape_char(p);
            } else {
                hi = (uint32_t)c2;
                p->i++;
            }
            if (hi < lo) {
                perr(p, "Range out of order in character class");
                return n;
            }
        }
        class_add(p, cls, CI_RANGE, lo, hi);
    }
    if (peek(p) != ']')
        perr(p, "Unterminated character class");
    p->i++;
    return n;
}

static int parse_int(struct rparse *p, int *out)
{
    int v = 0, any = 0;
    while (peek(p) >= '0' && peek(p) <= '9') {
        if (v < 100000)
            v = v * 10 + (peek(p) - '0');
        p->i++;
        any = 1;
    }
    *out = v;
    return any;
}

static jstr *group_name(struct rparse *p)
{
    uint32_t st = p->i;
    while (p->i < p->n && p->s[p->i] != '>')
        p->i++;
    if (p->i >= p->n) {
        perr(p, "Invalid capture group name");
        return 0;
    }
    jstr *name = js_intern(p->rt, js_str_utf16(p->rt, p->s + st, p->i - st));
    p->i++;
    return name;
}

static struct rnode *parse_atom(struct rparse *p)
{
    int c = peek(p);
    struct rnode *n;
    switch (c) {
    case '(': {
        p->i++;
        if (peek(p) == '?') {
            p->i++;
            int k = peek(p);
            if (k == ':') {
                p->i++;
                n = parse_alt(p);
            } else if (k == '=' || k == '!') {
                p->i++;
                n = rn(p, RN_LOOK);
                n->neg = k == '!';
                n->a = parse_alt(p);
            } else if (k == '<' && p->i + 1 < p->n && (p->s[p->i + 1] == '=' || p->s[p->i + 1] == '!')) {
                n = rn(p, RN_LOOK);
                n->neg = p->s[p->i + 1] == '!';
                n->behind = 1;
                p->i += 2;
                n->a = parse_alt(p);
            } else if (k == '<') {
                p->i++;
                jstr *name = group_name(p);
                n = rn(p, RN_GROUP);
                n->value = ++p->ngroups;
                if (n->value < GROUPS_MAX)
                    p->re->names[n->value] = name;
                n->a = parse_alt(p);
            } else {
                perr(p, "Invalid group");
                return rn(p, RN_EMPTY);
            }
        } else {
            n = rn(p, RN_GROUP);
            n->value = ++p->ngroups;
            n->a = parse_alt(p);
        }
        if (peek(p) != ')') {
            perr(p, "Unterminated group");
            return n;
        }
        p->i++;
        return n;
    }
    case '[':
        return parse_class(p);
    case '.':
        p->i++;
        return rn(p, RN_ANY);
    case '^':
        p->i++;
        return rn(p, RN_BOL);
    case '$':
        p->i++;
        return rn(p, RN_EOL);
    case '\\': {
        p->i++;
        int e = peek(p);
        if (e == 'd' || e == 'D' || e == 'w' || e == 'W' || e == 's' || e == 'S') {
            p->i++;
            n = rn(p, RN_CLASS);
            n->value = new_class(p);
            class_shorthand(p, n->value, e);
            return n;
        }
        if (e == 'b' || e == 'B') {
            p->i++;
            return rn(p, e == 'b' ? RN_WORDB : RN_NWORDB);
        }
        if (e >= '1' && e <= '9') {
            int v;
            uint32_t save = p->i;
            parse_int(p, &v);
            if (v <= GROUPS_MAX) {
                n = rn(p, RN_BACKREF);
                n->value = v;
                return n;
            }
            p->i = save;
        }
        if (e == 'k' && p->i + 1 < p->n && p->s[p->i + 1] == '<') {
            p->i += 2;
            jstr *name = group_name(p);
            n = rn(p, RN_BACKREF);
            n->value = -1;
            for (int g = 1; g < GROUPS_MAX; g++)
                if (p->re->names[g] == name)
                    n->value = g;
            if (n->value < 0)
                perr(p, "Invalid named capture referenced");
            return n;
        }
        n = rn(p, RN_CHAR);
        n->value = (int)escape_char(p);
        return n;
    }
    case '*': case '+': case '?':
        perr(p, "Nothing to repeat");
        p->i++;
        return rn(p, RN_EMPTY);
    }
    p->i++;
    n = rn(p, RN_CHAR);
    n->value = c;
    /* para zastepcza (znak spoza BMP) traktujemy jako dwa znaki - dziala dla zwyklego tekstu */
    return n;
}

static struct rnode *parse_seq(struct rparse *p)
{
    struct rnode *seq = rn(p, RN_SEQ), *tail = 0;
    while (p->i < p->n && peek(p) != '|' && peek(p) != ')' && !p->failed) {
        struct rnode *a = parse_atom(p);
        int c = peek(p);
        int min = -2, max = -1;
        if (c == '*') { min = 0; max = -1; p->i++; }
        else if (c == '+') { min = 1; max = -1; p->i++; }
        else if (c == '?') { min = 0; max = 1; p->i++; }
        else if (c == '{') {
            uint32_t save = p->i;
            p->i++;
            int lo, hi;
            if (parse_int(p, &lo)) {
                hi = lo;
                if (peek(p) == ',') {
                    p->i++;
                    if (!parse_int(p, &hi))
                        hi = -1;
                }
                if (peek(p) == '}') {
                    p->i++;
                    min = lo;
                    max = hi;
                    if (max >= 0 && max < min)
                        perr(p, "numbers out of order in {} quantifier");
                } else {
                    p->i = save;                      /* zwykly znak { */
                }
            } else {
                p->i = save;
            }
            if (min == -2 && p->i == save) {
                /* "{" bez liczby - literal */
            }
        }
        if (min != -2) {
            if (a->type == RN_BOL || a->type == RN_EOL || (a->type == RN_LOOK && 0))
                perr(p, "Nothing to repeat");
            struct rnode *r = rn(p, RN_REPEAT);
            r->a = a;
            r->min = min;
            r->max = max;
            if (peek(p) == '?') {
                r->greedy = 0;
                p->i++;
            }
            a = r;
        }
        if (tail)
            tail->next = a;
        else
            seq->a = a;
        tail = a;
    }
    return seq;
}

static struct rnode *parse_alt(struct rparse *p)
{
    struct rnode *first = parse_seq(p);
    if (peek(p) != '|')
        return first;
    struct rnode *alt = rn(p, RN_ALT), *tail = first;
    alt->a = first;
    while (peek(p) == '|' && !p->failed) {
        p->i++;
        struct rnode *s = parse_seq(p);
        tail->next = s;
        tail = s;
    }
    return alt;
}

/* ---------- program ---------- */

struct remit {
    js_rt *rt;
    struct jregex *re;
    int cap;
    int failed;
};

static int emit(struct remit *e, int op, int x, int y)
{
    struct jregex *re = e->re;
    if (re->n >= PROG_MAX) {
        e->failed = 1;
        return re->n - 1;
    }
    if (re->n == e->cap) {
        int cap = e->cap ? e->cap * 2 : 64;
        struct rinst *np = js_alloc(e->rt, sizeof(struct rinst) * (size_t)cap);
        if (re->n)
            memcpy(np, re->prog, sizeof(struct rinst) * (size_t)re->n);
        re->prog = np;
        e->cap = cap;
    }
    struct rinst *in = &re->prog[re->n];
    in->op = (uint8_t)op;
    in->neg = 0;
    in->behind = 0;
    in->x = x;
    in->y = y;
    return re->n++;
}

static int can_be_empty(struct rnode *n)
{
    switch (n->type) {
    case RN_CHAR: case RN_ANY: case RN_CLASS: return 0;
    case RN_SEQ:
        for (struct rnode *c = n->a; c; c = c->next)
            if (!can_be_empty(c))
                return 0;
        return 1;
    case RN_ALT:
        for (struct rnode *c = n->a; c; c = c->next)
            if (can_be_empty(c))
                return 1;
        return 0;
    case RN_GROUP: return can_be_empty(n->a);
    case RN_REPEAT: return n->min == 0 || can_be_empty(n->a);
    default: return 1;
    }
}

static void gen(struct remit *e, struct rnode *n);

static void gen_star(struct remit *e, struct rnode *body, int greedy)
{
    int empty = can_be_empty(body);
    int reg = empty && e->re->nregs < REGS_MAX ? e->re->nregs++ : -1;
    int split = emit(e, R_SPLIT, 0, 0);
    int start = e->re->n;
    if (reg >= 0)
        emit(e, R_MARK, reg, 0);
    gen(e, body);
    if (reg >= 0)
        emit(e, R_CHECK, reg, 0);
    emit(e, R_JMP, split, 0);
    int end = e->re->n;
    if (greedy) {
        e->re->prog[split].x = start;
        e->re->prog[split].y = end;
    } else {
        e->re->prog[split].x = end;
        e->re->prog[split].y = start;
    }
}

static void gen(struct remit *e, struct rnode *n)
{
    if (e->failed)
        return;
    switch (n->type) {
    case RN_CHAR:
        emit(e, R_CHAR, n->value, 0);
        break;
    case RN_ANY:
        emit(e, R_ANY, 0, 0);
        break;
    case RN_CLASS: {
        int i = emit(e, R_CLASS, n->value, 0);
        e->re->prog[i].neg = n->neg;
        break;
    }
    case RN_SEQ:
        for (struct rnode *c = n->a; c; c = c->next)
            gen(e, c);
        break;
    case RN_ALT: {
        int jumps[256], nj = 0;
        for (struct rnode *c = n->a; c; c = c->next) {
            if (c->next) {
                int split = emit(e, R_SPLIT, 0, 0);
                e->re->prog[split].x = e->re->n;
                gen(e, c);
                if (nj < 256)
                    jumps[nj++] = emit(e, R_JMP, 0, 0);
                e->re->prog[split].y = e->re->n;
            } else {
                gen(e, c);
            }
        }
        for (int k = 0; k < nj; k++)
            e->re->prog[jumps[k]].x = e->re->n;
        break;
    }
    case RN_GROUP:
        emit(e, R_SAVE, n->value * 2, 0);
        gen(e, n->a);
        emit(e, R_SAVE, n->value * 2 + 1, 0);
        break;
    case RN_REPEAT: {
        int min = n->min, max = n->max;
        if (min > 1000 || max > 1000) {
            e->failed = 1;
            return;
        }
        for (int k = 0; k < min; k++)
            gen(e, n->a);
        if (max < 0) {
            gen_star(e, n->a, n->greedy);
        } else if (max > min) {
            int splits[1001], ns = 0;
            for (int k = min; k < max; k++) {
                int split = emit(e, R_SPLIT, 0, 0);
                splits[ns++] = split;
                e->re->prog[split].x = e->re->n;
                gen(e, n->a);
            }
            for (int k = 0; k < ns; k++) {
                int s = splits[k];
                if (n->greedy) {
                    e->re->prog[s].y = e->re->n;
                } else {
                    e->re->prog[s].y = e->re->prog[s].x;
                    e->re->prog[s].x = e->re->n;
                }
            }
        }
        break;
    }
    case RN_BOL: emit(e, R_BOL, 0, 0); break;
    case RN_EOL: emit(e, R_EOL, 0, 0); break;
    case RN_WORDB: emit(e, R_WORDB, 0, 0); break;
    case RN_NWORDB: emit(e, R_NWORDB, 0, 0); break;
    case RN_BACKREF: emit(e, R_BACKREF, n->value, 0); break;
    case RN_LOOK: {
        int look = emit(e, R_LOOK, 0, 0);
        e->re->prog[look].neg = n->neg;
        e->re->prog[look].behind = n->behind;
        gen(e, n->a);
        emit(e, R_LOOKEND, 0, 0);
        e->re->prog[look].x = e->re->n;
        break;
    }
    case RN_EMPTY:
        break;
    }
}

struct jregex *js_regex_compile(js_rt *rt, const jstr *src, const jstr *flags, char *err, unsigned errsize)
{
    struct jregex *re = js_alloc(rt, sizeof(*re));
    for (uint32_t i = 0; flags && i < flags->len; i++) {
        uint8_t *f = 0;
        switch (flags->c[i]) {
        case 'g': f = &re->global; break;
        case 'i': f = &re->icase; break;
        case 'm': f = &re->multiline; break;
        case 's': f = &re->dotall; break;
        case 'y': f = &re->sticky; break;
        case 'u': case 'v': f = &re->unicode; break;
        case 'd': continue;
        }
        if (!f || *f) {
            ksnprintf(err, errsize, "Invalid regular expression flags");
            return 0;
        }
        *f = 1;
    }
    struct rparse p;
    memset(&p, 0, sizeof(p));
    p.rt = rt;
    p.re = re;
    p.s = src->c;
    p.n = src->len;
    p.err = err;
    p.errsize = errsize;
    struct rnode *tree = parse_alt(&p);
    if (!p.failed && p.i < p.n)
        perr(&p, "Unmatched ')'");
    if (p.failed)
        return 0;
    re->ngroups = p.ngroups + 1;
    if (re->ngroups > GROUPS_MAX) {
        ksnprintf(err, errsize, "Invalid regular expression: too many groups");
        return 0;
    }
    struct remit e = { rt, re, 0, 0 };
    emit(&e, R_SAVE, 0, 0);
    gen(&e, tree);
    emit(&e, R_SAVE, 1, 0);
    emit(&e, R_MATCH, 0, 0);
    if (e.failed) {
        ksnprintf(err, errsize, "Invalid regular expression: pattern too large");
        return 0;
    }
    return re;
}

int js_regex_ngroups(struct jregex *re) { return re->ngroups; }
jstr *js_regex_group_name(struct jregex *re, int i) { return i < GROUPS_MAX ? re->names[i] : 0; }

/* ---------- maszyna dopasowujaca ---------- */

struct bt {
    int pc, sp;                      /* pc < 0: przywrocenie slotu -pc-1 (sp = stara wartosc) */
};

struct rmatch {
    js_rt *rt;
    struct jregex *re;
    const uint16_t *s;
    int len;
    int *caps;
    int *regs;
    struct bt *stack;
    int top, cap;
    uint32_t steps;
    int look_end;                    /* (?<=...) - dopasowanie musi skonczyc sie tutaj */
};

static int is_word(uint16_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static int is_lt(uint16_t c) { return c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029; }

static int class_match(struct rclass *cl, uint16_t c, int icase)
{
    for (int k = 0; k < cl->n; k++) {
        struct citem *it = &cl->items[k];
        switch (it->kind) {
        case CI_RANGE:
            if (c >= it->lo && c <= it->hi)
                return 1;
            if (icase) {
                uint16_t l = js_lower(c), u = js_upper(c);
                if ((l >= it->lo && l <= it->hi) || (u >= it->lo && u <= it->hi))
                    return 1;
            }
            break;
        case CI_NDIGIT:
            if (!(c >= '0' && c <= '9'))
                return 1;
            break;
        case CI_NWORD:
            if (!is_word(c))
                return 1;
            break;
        case CI_NSPACE:
            if (!js_is_space(c))
                return 1;
            break;
        }
    }
    return 0;
}

static void push(struct rmatch *m, int pc, int sp)
{
    if (m->top == m->cap) {
        int cap = m->cap ? m->cap * 2 : 256;
        if (cap > (1 << 22))
            js_throw_error(m->rt, JS_ERR_RANGE, "Maximum call stack size exceeded (regular expression)");
        struct bt *ns = js_alloc(m->rt, sizeof(struct bt) * (size_t)cap);
        if (m->top)
            memcpy(ns, m->stack, sizeof(struct bt) * (size_t)m->top);
        m->stack = ns;
        m->cap = cap;
    }
    m->stack[m->top].pc = pc;
    m->stack[m->top++].sp = sp;
}

/* Sloty: 0 .. 2*ngroups-1 to granice grup, dalej rejestry petli */
static void set_slot(struct rmatch *m, int slot, int v)
{
    int *p = slot < 2 * m->re->ngroups ? &m->caps[slot] : &m->regs[slot - 2 * m->re->ngroups];
    push(m, -slot - 1, *p);
    *p = v;
}

static int get_slot(struct rmatch *m, int slot)
{
    return slot < 2 * m->re->ngroups ? m->caps[slot] : m->regs[slot - 2 * m->re->ngroups];
}

static int run(struct rmatch *m, int pc, int sp);

static int look(struct rmatch *m, struct rinst *in, int pc, int sp)
{
    int n = 2 * m->re->ngroups;
    int *saved = js_alloc(m->rt, sizeof(int) * (size_t)n);
    memcpy(saved, m->caps, sizeof(int) * (size_t)n);
    int base = m->top, saved_end = m->look_end, found = 0;
    if (!in->behind) {
        m->look_end = -1;
        found = run(m, pc + 1, sp);
    } else {
        m->look_end = sp;
        for (int k = sp; k >= 0 && !found; k--)
            found = run(m, pc + 1, k);
    }
    m->top = base;
    m->look_end = saved_end;
    if (!found || in->neg)
        memcpy(m->caps, saved, sizeof(int) * (size_t)n);
    return found != in->neg;
}

static int run(struct rmatch *m, int pc, int sp)
{
    struct jregex *re = m->re;
    const uint16_t *s = m->s;
    int len = m->len, base = m->top;
    for (;;) {
        if ((++m->steps & 0xFFF) == 0)
            js_step(m->rt);
        struct rinst *in = &re->prog[pc];
        switch (in->op) {
        case R_CHAR:
            if (sp < len && (s[sp] == (uint16_t)in->x || (re->icase && js_lower(s[sp]) == js_lower((uint16_t)in->x)))) {
                sp++;
                pc++;
                continue;
            }
            goto fail;
        case R_ANY:
            if (sp < len && (re->dotall || !is_lt(s[sp]))) {
                sp++;
                pc++;
                continue;
            }
            goto fail;
        case R_CLASS:
            if (sp < len && class_match(&re->classes[in->x], s[sp], re->icase) != in->neg) {
                sp++;
                pc++;
                continue;
            }
            goto fail;
        case R_SPLIT:
            push(m, in->y, sp);
            pc = in->x;
            continue;
        case R_JMP:
            pc = in->x;
            continue;
        case R_SAVE:
            set_slot(m, in->x, sp);
            pc++;
            continue;
        case R_MARK:
            set_slot(m, 2 * re->ngroups + in->x, sp);
            pc++;
            continue;
        case R_CHECK:
            if (get_slot(m, 2 * re->ngroups + in->x) == sp)
                goto fail;                          /* obrot petli niczego nie zjadl */
            pc++;
            continue;
        case R_BOL:
            if (sp == 0 || (re->multiline && is_lt(s[sp - 1]))) {
                pc++;
                continue;
            }
            goto fail;
        case R_EOL:
            if (sp == len || (re->multiline && is_lt(s[sp]))) {
                pc++;
                continue;
            }
            goto fail;
        case R_WORDB:
        case R_NWORDB: {
            int a = sp > 0 && is_word(s[sp - 1]), b = sp < len && is_word(s[sp]);
            if ((a != b) == (in->op == R_WORDB)) {
                pc++;
                continue;
            }
            goto fail;
        }
        case R_BACKREF: {
            int st = in->x < re->ngroups ? m->caps[in->x * 2] : -1, en = in->x < re->ngroups ? m->caps[in->x * 2 + 1] : -1;
            if (st < 0 || en < 0) {
                pc++;
                continue;
            }
            int l = en - st;
            if (sp + l > len)
                goto fail;
            int ok = 1;
            for (int k = 0; k < l && ok; k++)
                ok = s[st + k] == s[sp + k] || (re->icase && js_lower(s[st + k]) == js_lower(s[sp + k]));
            if (!ok)
                goto fail;
            sp += l;
            pc++;
            continue;
        }
        case R_LOOK:
            if (look(m, in, pc, sp)) {
                pc = in->x;
                continue;
            }
            goto fail;
        case R_LOOKEND:
            if (m->look_end >= 0 && sp != m->look_end)
                goto fail;
            m->top = base;
            return 1;
        case R_MATCH:
            m->top = base;
            return 1;
        }
    fail:
        for (;;) {
            if (m->top == base)
                return 0;
            struct bt b = m->stack[--m->top];
            if (b.pc < 0) {
                int slot = -b.pc - 1;
                if (slot < 2 * re->ngroups)
                    m->caps[slot] = b.sp;
                else
                    m->regs[slot - 2 * re->ngroups] = b.sp;
                continue;
            }
            pc = b.pc;
            sp = b.sp;
            break;
        }
    }
}

int js_regex_exec(js_rt *rt, struct jregex *re, const jstr *s, uint32_t start, int *caps)
{
    struct rmatch m;
    memset(&m, 0, sizeof(m));
    m.rt = rt;
    m.re = re;
    m.s = s->c;
    m.len = (int)s->len;
    m.caps = caps;
    int regs_local[REGS_MAX];
    m.regs = regs_local;
    m.look_end = -1;
    /* szybki test: wzorzec zaczynajacy sie od konkretnego znaku */
    int first = -1;
    if (re->n > 1 && re->prog[1].op == R_CHAR && !re->icase)
        first = re->prog[1].x;
    for (uint32_t pos = start; pos <= s->len; pos++) {
        if (first >= 0 && !re->sticky) {
            while (pos < s->len && s->c[pos] != (uint16_t)first)
                pos++;
            if (pos >= s->len)
                break;
        }
        for (int i = 0; i < 2 * re->ngroups; i++)
            caps[i] = -1;
        for (int i = 0; i < re->nregs; i++)
            regs_local[i] = -1;
        m.top = 0;
        if (run(&m, 0, (int)pos))
            return 1;
        if (re->sticky)
            break;
    }
    return 0;
}

/* ---------- obiekt RegExp ---------- */

static jobj *this_regexp(js_rt *rt, jv self)
{
    if (self.t != JT_OBJ || self.u.o->cls != JC_REGEXP)
        js_throw_error(rt, JS_ERR_TYPE, "RegExp method called on incompatible receiver");
    return self.u.o;
}

static jobj *regexp_create(js_rt *rt, jstr *source, jstr *flags)
{
    char err[160];
    struct jregex *re = js_regex_compile(rt, source, flags, err, sizeof(err));
    if (!re)
        js_throw_error(rt, JS_ERR_SYNTAX, "%s", err);
    jobj *o = js_new_object(rt, rt->regexp_proto);
    o->cls = JC_REGEXP;
    o->u.re.re = re;
    o->u.re.source = source->len ? source : js_atom(rt, "(?:)");
    /* flagi w porzadku alfabetycznym, jak w przegladarkach */
    struct jsb b;
    jsb_init(&b, rt);
    static const char order[] = "dgimsuvy";
    for (const char *f = order; *f; f++)
        for (uint32_t i = 0; flags && i < flags->len; i++)
            if (flags->c[i] == (uint8_t)*f)
                jsb_char(&b, (uint16_t)*f);
    o->u.re.flags = 0;
    js_define(rt, o, rt->A.flags, jv_str(jsb_done(&b)), 0);
    js_define(rt, o, rt->A.lastIndex, jv_num(0), PF_WRITE);
    return o;
}

jv js_regexp_new(js_rt *rt, jstr *source, jstr *flags)
{
    return jv_obj(regexp_create(rt, source, flags));
}

static jv regexp_ctor(js_rt *rt, jv self U, int argc, jv *argv, jobj *fn U)
{
    jv pat = ARG(0), fl = ARG(1);
    jstr *source, *flags = fl.t == JT_UNDEF ? 0 : js_to_str(rt, fl);
    if (pat.t == JT_OBJ && pat.u.o->cls == JC_REGEXP) {
        source = pat.u.o->u.re.source;
        if (!flags) {
            jv f = js_get(rt, pat, rt->A.flags);
            flags = f.t == JT_STR ? f.u.s : 0;
        }
    } else {
        source = pat.t == JT_UNDEF ? rt->empty : js_to_str(rt, pat);
    }
    return jv_obj(regexp_create(rt, source, flags ? flags : rt->empty));
}

static double get_last_index(js_rt *rt, jobj *re)
{
    jv li = js_get(rt, jv_obj(re), rt->A.lastIndex);
    double d = js_to_integer(rt, li);
    return d < 0 ? 0 : d;
}

static void set_last_index(js_rt *rt, jobj *re, double v)
{
    js_set(rt, jv_obj(re), rt->A.lastIndex, jv_num(v));
}

/* Tablica wyniku exec: [dopasowanie, grupy...], index, input, groups */
static jv match_result(js_rt *rt, struct jregex *re, jstr *s, int *caps)
{
    jobj *a = js_new_array(rt, 0);
    for (int g = 0; g < re->ngroups; g++) {
        if (caps[2 * g] >= 0 && caps[2 * g + 1] >= 0)
            js_array_push(rt, a, jv_str(js_substr(rt, s, (uint32_t)caps[2 * g], (uint32_t)caps[2 * g + 1])));
        else
            js_array_push(rt, a, jv_undef());
    }
    js_define(rt, a, rt->A.index, jv_num(caps[0]), PF_ALL);
    js_define(rt, a, rt->A.input, jv_str(s), PF_ALL);
    jv groups = jv_undef();
    for (int g = 1; g < re->ngroups; g++)
        if (re->names[g]) {
            if (groups.t == JT_UNDEF)
                groups = jv_obj(js_new_object(rt, 0));
            js_define(rt, groups.u.o, re->names[g], a->u.arr.v[g], PF_ALL);
        }
    js_define(rt, a, rt->A.groups, groups, PF_ALL);
    return jv_obj(a);
}

/* exec: 1 = znaleziono (caps wypelnione) */
static int regexp_exec_raw(js_rt *rt, jobj *ro, jstr *s, int *caps)
{
    struct jregex *re = ro->u.re.re;
    double li = re->global || re->sticky ? get_last_index(rt, ro) : 0;
    if (li > s->len) {
        set_last_index(rt, ro, 0);
        return 0;
    }
    if (!js_regex_exec(rt, re, s, (uint32_t)li, caps)) {
        if (re->global || re->sticky)
            set_last_index(rt, ro, 0);
        return 0;
    }
    if (re->global || re->sticky)
        set_last_index(rt, ro, caps[1]);
    return 1;
}

NATIVE(rep_exec)
{
    jobj *ro = this_regexp(rt, self);
    jstr *s = js_to_str(rt, ARG(0));
    int caps[2 * GROUPS_MAX];
    if (!regexp_exec_raw(rt, ro, s, caps))
        return jv_null();
    return match_result(rt, ro->u.re.re, s, caps);
}

NATIVE(rep_test)
{
    jobj *ro = this_regexp(rt, self);
    jstr *s = js_to_str(rt, ARG(0));
    int caps[2 * GROUPS_MAX];
    return jv_bool(regexp_exec_raw(rt, ro, s, caps));
}

NATIVE(rep_to_string)
{
    jobj *ro = this_regexp(rt, self);
    struct jsb b;
    jsb_init(&b, rt);
    jsb_char(&b, '/');
    jsb_str(&b, ro->u.re.source);
    jsb_char(&b, '/');
    jv f = js_get(rt, self, rt->A.flags);
    if (f.t == JT_STR)
        jsb_str(&b, f.u.s);
    return jv_str(jsb_done(&b));
}

NATIVE(rep_source) { return jv_str(this_regexp(rt, self)->u.re.source); }
NATIVE(rep_global) { return jv_bool(this_regexp(rt, self)->u.re.re->global); }
NATIVE(rep_icase) { return jv_bool(this_regexp(rt, self)->u.re.re->icase); }
NATIVE(rep_multiline) { return jv_bool(this_regexp(rt, self)->u.re.re->multiline); }
NATIVE(rep_sticky) { return jv_bool(this_regexp(rt, self)->u.re.re->sticky); }
NATIVE(rep_unicode) { return jv_bool(this_regexp(rt, self)->u.re.re->unicode); }
NATIVE(rep_dotall) { return jv_bool(this_regexp(rt, self)->u.re.re->dotall); }

/* ---------- metody tekstow z wyrazeniami regularnymi ---------- */

static jstr *this_str(js_rt *rt, jv self)
{
    if (self.t == JT_STR)
        return self.u.s;
    if (IS_NULLISH(self))
        js_throw_error(rt, JS_ERR_TYPE, "String.prototype method called on null or undefined");
    return js_to_str(rt, self);
}

static int is_regexp(jv v) { return v.t == JT_OBJ && v.u.o->cls == JC_REGEXP; }

static int64_t find_str(const jstr *s, const jstr *w, uint32_t from)
{
    if (!w->len)
        return from <= s->len ? from : -1;
    for (uint32_t i = from; i + w->len <= s->len; i++)
        if (memcmp(s->c + i, w->c, (size_t)w->len * 2) == 0)
            return i;
    return -1;
}

/* Tekst zastepujacy z $1, $&, $<nazwa>... */
static void expand_replacement(js_rt *rt, struct jsb *b, jstr *rep, jstr *s, int *caps, int ngroups,
                               struct jregex *re)
{
    for (uint32_t i = 0; i < rep->len; i++) {
        uint16_t c = rep->c[i];
        if (c != '$' || i + 1 >= rep->len) {
            jsb_char(b, c);
            continue;
        }
        uint16_t d = rep->c[i + 1];
        if (d == '$') {
            jsb_char(b, '$');
            i++;
        } else if (d == '&') {
            jsb_str(b, js_substr(rt, s, (uint32_t)caps[0], (uint32_t)caps[1]));
            i++;
        } else if (d == '`') {
            jsb_str(b, js_substr(rt, s, 0, (uint32_t)caps[0]));
            i++;
        } else if (d == '\'') {
            jsb_str(b, js_substr(rt, s, (uint32_t)caps[1], s->len));
            i++;
        } else if (d >= '0' && d <= '9') {
            int g = d - '0', used = 1;
            if (i + 2 < rep->len && rep->c[i + 2] >= '0' && rep->c[i + 2] <= '9' && g * 10 + (rep->c[i + 2] - '0') < ngroups) {
                g = g * 10 + (rep->c[i + 2] - '0');
                used = 2;
            }
            if (g >= 1 && g < ngroups) {
                if (caps[2 * g] >= 0)
                    jsb_str(b, js_substr(rt, s, (uint32_t)caps[2 * g], (uint32_t)caps[2 * g + 1]));
                i += (uint32_t)used;
            } else {
                jsb_char(b, '$');
            }
        } else if (d == '<' && re) {
            uint32_t k = i + 2;
            while (k < rep->len && rep->c[k] != '>')
                k++;
            if (k >= rep->len) {
                jsb_char(b, '$');
                continue;
            }
            jstr *name = js_intern(rt, js_str_utf16(rt, rep->c + i + 2, k - i - 2));
            for (int g = 1; g < ngroups; g++)
                if (re->names[g] == name && caps[2 * g] >= 0)
                    jsb_str(b, js_substr(rt, s, (uint32_t)caps[2 * g], (uint32_t)caps[2 * g + 1]));
            i = k;
        } else {
            jsb_char(b, '$');
        }
    }
}

/* Jedno zastapienie: funkcja albo tekst */
static void replace_one(js_rt *rt, struct jsb *b, jv replacement, jstr *s, int *caps, int ngroups, struct jregex *re)
{
    if (js_is_callable(replacement)) {
        jv *args = js_alloc(rt, sizeof(jv) * (size_t)(ngroups + 3));
        int n = 0;
        for (int g = 0; g < ngroups; g++)
            args[n++] = caps[2 * g] >= 0 ? jv_str(js_substr(rt, s, (uint32_t)caps[2 * g], (uint32_t)caps[2 * g + 1])) : jv_undef();
        args[n++] = jv_num(caps[0]);
        args[n++] = jv_str(s);
        if (re) {
            jobj *groups = 0;
            for (int g = 1; g < ngroups; g++)
                if (re->names[g]) {
                    if (!groups)
                        groups = js_new_object(rt, 0);
                    js_define(rt, groups, re->names[g], args[g], PF_ALL);
                }
            if (groups)
                args[n++] = jv_obj(groups);
        }
        jv r = js_call(rt, replacement, jv_undef(), n, args);
        jsb_str(b, js_to_str(rt, r));
    } else {
        expand_replacement(rt, b, js_to_str(rt, replacement), s, caps, ngroups, re);
    }
}

static jv do_replace(js_rt *rt, jv self, jv pattern, jv replacement, int all)
{
    jstr *s = this_str(rt, self);
    struct jsb b;
    jsb_init(&b, rt);
    if (!js_is_callable(replacement))
        replacement = jv_str(js_to_str(rt, replacement));
    if (is_regexp(pattern)) {
        jobj *ro = pattern.u.o;
        struct jregex *re = ro->u.re.re;
        if (all && !re->global)
            js_throw_error(rt, JS_ERR_TYPE, "replaceAll must be called with a global RegExp");
        int caps[2 * GROUPS_MAX];
        uint32_t last = 0;
        if (!re->global) {
            if (!regexp_exec_raw(rt, ro, s, caps))
                return jv_str(s);
            jsb_str(&b, js_substr(rt, s, 0, (uint32_t)caps[0]));
            replace_one(rt, &b, replacement, s, caps, re->ngroups, re);
            jsb_str(&b, js_substr(rt, s, (uint32_t)caps[1], s->len));
            return jv_str(jsb_done(&b));
        }
        uint32_t pos = 0;
        while (pos <= s->len && js_regex_exec(rt, re, s, pos, caps)) {
            jsb_str(&b, js_substr(rt, s, last, (uint32_t)caps[0]));
            replace_one(rt, &b, replacement, s, caps, re->ngroups, re);
            last = (uint32_t)caps[1];
            pos = caps[1] > caps[0] ? (uint32_t)caps[1] : (uint32_t)caps[1] + 1;
            js_step(rt);
        }
        set_last_index(rt, ro, 0);
        jsb_str(&b, js_substr(rt, s, last, s->len));
        return jv_str(jsb_done(&b));
    }
    jstr *w = js_to_str(rt, pattern);
    uint32_t last = 0;
    int64_t at = find_str(s, w, 0);
    if (at < 0)
        return jv_str(s);
    while (at >= 0) {
        int caps[2] = { (int)at, (int)(at + w->len) };
        jsb_str(&b, js_substr(rt, s, last, (uint32_t)at));
        replace_one(rt, &b, replacement, s, caps, 1, 0);
        last = (uint32_t)(at + w->len);
        if (!all)
            break;
        at = find_str(s, w, w->len ? last : last + 1);
        if (!w->len && at > (int64_t)s->len)
            break;
    }
    jsb_str(&b, js_substr(rt, s, last, s->len));
    return jv_str(jsb_done(&b));
}

NATIVE(strp_replace) { return do_replace(rt, self, ARG(0), ARG(1), 0); }
NATIVE(strp_replace_all) { return do_replace(rt, self, ARG(0), ARG(1), 1); }

static jobj *to_regexp(js_rt *rt, jv v, const char *flags)
{
    if (is_regexp(v))
        return v.u.o;
    return regexp_create(rt, v.t == JT_UNDEF ? rt->empty : js_to_str(rt, v), js_str_c(rt, flags));
}

NATIVE(strp_match)
{
    jstr *s = this_str(rt, self);
    jobj *ro = to_regexp(rt, ARG(0), "");
    struct jregex *re = ro->u.re.re;
    int caps[2 * GROUPS_MAX];
    if (!re->global) {
        if (!regexp_exec_raw(rt, ro, s, caps))
            return jv_null();
        return match_result(rt, re, s, caps);
    }
    jobj *out = js_new_array(rt, 0);
    uint32_t pos = 0;
    while (pos <= s->len && js_regex_exec(rt, re, s, pos, caps)) {
        js_array_push(rt, out, jv_str(js_substr(rt, s, (uint32_t)caps[0], (uint32_t)caps[1])));
        pos = caps[1] > caps[0] ? (uint32_t)caps[1] : (uint32_t)caps[1] + 1;
        js_step(rt);
    }
    set_last_index(rt, ro, 0);
    return out->u.arr.len ? jv_obj(out) : jv_null();
}

NATIVE(strp_match_all)
{
    jstr *s = this_str(rt, self);
    jobj *ro = to_regexp(rt, ARG(0), "g");
    struct jregex *re = ro->u.re.re;
    if (!re->global)
        js_throw_error(rt, JS_ERR_TYPE, "String.prototype.matchAll called with a non-global RegExp argument");
    int caps[2 * GROUPS_MAX];
    jobj *out = js_new_array(rt, 0);
    uint32_t pos = 0;
    while (pos <= s->len && js_regex_exec(rt, re, s, pos, caps)) {
        js_array_push(rt, out, match_result(rt, re, s, caps));
        pos = caps[1] > caps[0] ? (uint32_t)caps[1] : (uint32_t)caps[1] + 1;
        js_step(rt);
    }
    return jv_obj(out);
}

NATIVE(strp_search)
{
    jstr *s = this_str(rt, self);
    jobj *ro = to_regexp(rt, ARG(0), "");
    int caps[2 * GROUPS_MAX];
    if (!js_regex_exec(rt, ro->u.re.re, s, 0, caps))
        return jv_num(-1);
    return jv_num(caps[0]);
}

NATIVE(strp_split)
{
    jstr *s = this_str(rt, self);
    jv sep = ARG(0);
    uint32_t limit = ARG(1).t == JT_UNDEF ? 0xFFFFFFFFu : js_to_uint32(rt, argv[1]);
    jobj *out = js_new_array(rt, 0);
    if (!limit)
        return jv_obj(out);
    if (sep.t == JT_UNDEF) {
        js_array_push(rt, out, jv_str(s));
        return jv_obj(out);
    }
    if (is_regexp(sep)) {
        struct jregex *re = sep.u.o->u.re.re;
        int caps[2 * GROUPS_MAX];
        if (!s->len) {
            if (!js_regex_exec(rt, re, s, 0, caps))
                js_array_push(rt, out, jv_str(s));
            return jv_obj(out);
        }
        uint32_t last = 0, pos = 0;
        while (pos < s->len) {
            if (!js_regex_exec(rt, re, s, pos, caps))
                break;
            uint32_t ms = (uint32_t)caps[0], me = (uint32_t)caps[1];
            if (ms >= s->len)
                break;
            if (me == last) {                        /* puste dopasowanie tuz za poprzednim - znak dalej */
                pos = ms + 1;
                continue;
            }
            js_array_push(rt, out, jv_str(js_substr(rt, s, last, ms)));
            if (out->u.arr.len >= limit)
                return jv_obj(out);
            for (int g = 1; g < re->ngroups; g++) {
                js_array_push(rt, out, caps[2 * g] >= 0 ? jv_str(js_substr(rt, s, (uint32_t)caps[2 * g], (uint32_t)caps[2 * g + 1])) : jv_undef());
                if (out->u.arr.len >= limit)
                    return jv_obj(out);
            }
            last = me;
            pos = me;
            js_step(rt);
        }
        if (last <= s->len)
            js_array_push(rt, out, jv_str(js_substr(rt, s, last, s->len)));
        return jv_obj(out);
    }
    jstr *w = js_to_str(rt, sep);
    if (!w->len) {
        for (uint32_t i = 0; i < s->len && out->u.arr.len < limit; i++)
            js_array_push(rt, out, jv_str(js_substr(rt, s, i, i + 1)));
        return jv_obj(out);
    }
    uint32_t last = 0;
    int64_t at;
    while ((at = find_str(s, w, last)) >= 0) {
        js_array_push(rt, out, jv_str(js_substr(rt, s, last, (uint32_t)at)));
        if (out->u.arr.len >= limit)
            return jv_obj(out);
        last = (uint32_t)at + w->len;
    }
    js_array_push(rt, out, jv_str(js_substr(rt, s, last, s->len)));
    return jv_obj(out);
}

void js_regex_string_init(js_rt *rt)
{
    jobj *rp = rt->regexp_proto;
    jobj *R = js_new_native(rt, "RegExp", regexp_ctor, 2);
    R->u.nf.ctor = regexp_ctor;
    js_define(rt, R, rt->A.prototype, jv_obj(rp), 0);
    js_define(rt, rp, rt->A.constructor, jv_obj(R), PF_WRITE | PF_CONF);
    js_def_val(rt, rt->global, "RegExp", jv_obj(R));
    js_def_fn(rt, rp, "exec", rep_exec, 1);
    js_def_fn(rt, rp, "test", rep_test, 1);
    js_def_fn(rt, rp, "toString", rep_to_string, 0);
    js_def_getset(rt, rp, "source", rep_source, 0);
    js_def_getset(rt, rp, "global", rep_global, 0);
    js_def_getset(rt, rp, "ignoreCase", rep_icase, 0);
    js_def_getset(rt, rp, "multiline", rep_multiline, 0);
    js_def_getset(rt, rp, "sticky", rep_sticky, 0);
    js_def_getset(rt, rp, "unicode", rep_unicode, 0);
    js_def_getset(rt, rp, "dotAll", rep_dotall, 0);

    jobj *sp = rt->string_proto;
    js_def_fn(rt, sp, "replace", strp_replace, 2);
    js_def_fn(rt, sp, "replaceAll", strp_replace_all, 2);
    js_def_fn(rt, sp, "match", strp_match, 1);
    js_def_fn(rt, sp, "matchAll", strp_match_all, 1);
    js_def_fn(rt, sp, "search", strp_search, 1);
    js_def_fn(rt, sp, "split", strp_split, 2);
}
