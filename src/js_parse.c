/* Parser JavaScriptu: tokeny (js_lex.c) -> drzewo programu (AST).
 *
 * "Zejscie rekurencyjne": kazda regula gramatyki to funkcja, np.
 * wyrazenie przypisania wola wyrazenie warunkowe, ono - dzialania
 * dwuargumentowe itd. az do "atomow": liczb, nazw, nawiasow.
 * Kolejnosc dzialan (* przed +) zalatwia tablica priorytetow.
 *
 * Przy okazji zbieramy deklaracje: "var" i funkcje trafiaja do listy
 * calej funkcji (sa widoczne w calej funkcji), a let/const/class - do
 * listy bloku { }. Interpreter tworzy te zmienne przy wejsciu do funkcji
 * albo bloku.
 *
 * Wyrazenia typu (a, b) => ... albo [x, y] = tablica najpierw czytamy jak
 * zwykle wyrazenia, a gdy okaze sie, ze to parametry strzalki albo lewa
 * strona przypisania - zamieniamy je na "wzorce". */

#include "js_lex.h"
#include "string.h"

#define DEPTH_MAX 1500

struct scope_b {
    jstr **names;
    uint8_t *kinds;
    int n, cap;
    jnode **funcs;
    int nf, fcap;
};

struct jparser {
    struct jlex L;
    js_rt *rt;
    struct jfuninfo *fn;             /* biezaca funkcja (albo caly skrypt) */
    struct jfuninfo *fn_args;        /* najblizsza funkcja nie-strzalka (arguments) */
    struct scope_b *blk;             /* biezacy blok */
    struct scope_b *fn_blk;          /* blok ciala biezacej funkcji */
    int no_in;                       /* w for(...;...) "in" nie jest operatorem */
    int nfuncs_created;              /* licznik funkcji (petle z domknieciami) */
    int depth;
    uint32_t prev_end;               /* koniec poprzedniego tokenu (tekst funkcji) */
    int next_async;                  /* nastepna funkcja (begin_function) bedzie async */
    int async_arrow;                 /* parse_paren: to parametry strzalki async */
    int next_gen;                    /* nastepna funkcja bedzie generatorem (function*) */
};

#define TOK (p->L.tok)

static const char *const kw_names[K_COUNT] = {
    [K_BREAK] = "break", [K_CASE] = "case", [K_CATCH] = "catch", [K_CLASS] = "class",
    [K_CONST] = "const", [K_CONTINUE] = "continue", [K_DEBUGGER] = "debugger",
    [K_DEFAULT] = "default", [K_DELETE] = "delete", [K_DO] = "do", [K_ELSE] = "else",
    [K_EXPORT] = "export", [K_EXTENDS] = "extends", [K_FINALLY] = "finally", [K_FOR] = "for",
    [K_FUNCTION] = "function", [K_IF] = "if", [K_IMPORT] = "import", [K_IN] = "in",
    [K_INSTANCEOF] = "instanceof", [K_NEW] = "new", [K_RETURN] = "return", [K_SUPER] = "super",
    [K_SWITCH] = "switch", [K_THIS] = "this", [K_THROW] = "throw", [K_TRY] = "try",
    [K_TYPEOF] = "typeof", [K_VAR] = "var", [K_VOID] = "void", [K_WHILE] = "while",
    [K_WITH] = "with", [K_NULL] = "null", [K_TRUE] = "true", [K_FALSE] = "false",
    [K_ENUM] = "enum", [K_LET] = "let", [K_STATIC] = "static", [K_YIELD] = "yield",
    [K_AWAIT] = "await", [K_ASYNC] = "async", [K_GET] = "get", [K_SET] = "set", [K_OF] = "of",
    [K_ARGUMENTS] = "arguments",
};

void js_parse_init(js_rt *rt)
{
    for (int i = 1; i < K_COUNT; i++)
        js_atom(rt, kw_names[i])->kw = (uint8_t)i;
}

/* ---------- pomocnicze ---------- */

static void next(struct jparser *p)
{
    p->prev_end = p->L.tok.end;
    lex_next(&p->L);
}

static int is_op(struct jparser *p, int op) { return TOK.type == TK_PUNCT && TOK.op == op; }

static int is_kw(struct jparser *p, int kw)
{
    return TOK.type == TK_IDENT && !TOK.escaped && TOK.str->kw == kw;
}

static const char *tok_text(struct jparser *p, char *buf, unsigned size)
{
    unsigned n = TOK.end - TOK.start;
    if (TOK.type == TK_EOF)
        return "koniec programu";
    if (n > size - 1)
        n = size - 1;
    memcpy(buf, p->L.src + TOK.start, n);
    buf[n] = '\0';
    return buf;
}

static void unexpected(struct jparser *p) __attribute__((noreturn));
static void unexpected(struct jparser *p)
{
    char buf[40];
    lex_error(&p->L, "Unexpected token '%s'", tok_text(p, buf, sizeof(buf)));
}

static void expect(struct jparser *p, int op)
{
    if (!is_op(p, op)) {
        char buf[40];
        if (op < 128)
            lex_error(&p->L, "Oczekiwano '%c', a jest '%s'", op, tok_text(p, buf, sizeof(buf)));
        unexpected(p);
    }
    next(p);
}

/* Srednik albo koniec linii / } / koniec programu (ASI) */
static void semicolon(struct jparser *p)
{
    if (is_op(p, ';')) {
        next(p);
        return;
    }
    if (is_op(p, '}') || TOK.type == TK_EOF || TOK.nl_before)
        return;
    unexpected(p);
}

static jnode *mk(struct jparser *p, int type)
{
    jnode *n = js_alloc(p->rt, sizeof(*n));
    n->type = (uint8_t)type;
    n->line = TOK.line;
    return n;
}

static void enter(struct jparser *p)
{
    if (++p->depth > DEPTH_MAX)
        lex_error(&p->L, "Program zbyt gleboko zagniezdzony");
    js_check_stack(p->rt);
}

static void leave(struct jparser *p) { p->depth--; }

/* Lista w wezle: dopisywanie na koncu */
struct list { jnode *head, *tail; };
static void push(struct list *l, jnode *n)
{
    if (l->tail)
        l->tail->next = n;
    else
        l->head = n;
    l->tail = n;
}

/* Nazwa zmiennej: identyfikator, ktory nie jest zastrzezonym slowem */
static jstr *binding_name(struct jparser *p)
{
    if (TOK.type != TK_IDENT || (!TOK.escaped && TOK.str->kw && TOK.str->kw <= K_ENUM))
        unexpected(p);
    jstr *s = TOK.str;
    next(p);
    return s;
}

/* ---------- deklaracje ---------- */

static void add_var(struct jparser *p, jstr *name)
{
    struct jfuninfo *f = p->fn;
    for (int i = 0; i < f->nvars; i++)
        if (f->vars[i] == name)
            return;
    if (f->nvars == f->vars_cap) {
        int cap = f->vars_cap ? f->vars_cap * 2 : 8;
        jstr **nv = js_alloc(p->rt, sizeof(jstr *) * (unsigned)cap);
        if (f->vars)
            memcpy(nv, f->vars, sizeof(jstr *) * (unsigned)f->nvars);
        f->vars = nv;
        f->vars_cap = cap;
    }
    f->vars[f->nvars++] = name;
}

static void add_lex(struct jparser *p, jstr *name, int kind)
{
    struct scope_b *b = p->blk;
    for (int i = 0; i < b->n; i++)
        if (b->names[i] == name) {
            char buf[80];
            js_utf8(name, buf, sizeof(buf));
            lex_error(&p->L, "Identifier '%s' has already been declared", buf);
        }
    if (b->n == b->cap) {
        int cap = b->cap ? b->cap * 2 : 8;
        jstr **nn = js_alloc(p->rt, sizeof(jstr *) * (unsigned)cap);
        uint8_t *nk = js_alloc(p->rt, (unsigned)cap);
        if (b->n) {
            memcpy(nn, b->names, sizeof(jstr *) * (unsigned)b->n);
            memcpy(nk, b->kinds, (unsigned)b->n);
        }
        b->names = nn;
        b->kinds = nk;
        b->cap = cap;
    }
    b->names[b->n] = name;
    b->kinds[b->n++] = (uint8_t)kind;
}

static void add_block_func(struct jparser *p, jnode *fn)
{
    struct scope_b *b = p->blk;
    if (b->nf == b->fcap) {
        int cap = b->fcap ? b->fcap * 2 : 4;
        jnode **nf = js_alloc(p->rt, sizeof(jnode *) * (unsigned)cap);
        if (b->nf)
            memcpy(nf, b->funcs, sizeof(jnode *) * (unsigned)b->nf);
        b->funcs = nf;
        b->fcap = cap;
    }
    b->funcs[b->nf++] = fn;
}

static void add_top_func(struct jparser *p, jnode *fn)
{
    struct jfuninfo *f = p->fn;
    if (f->nfuncs == f->funcs_cap) {
        int cap = f->funcs_cap ? f->funcs_cap * 2 : 4;
        jnode **nf = js_alloc(p->rt, sizeof(jnode *) * (unsigned)cap);
        if (f->nfuncs)
            memcpy(nf, f->funcs, sizeof(jnode *) * (unsigned)f->nfuncs);
        f->funcs = nf;
        f->funcs_cap = cap;
    }
    f->funcs[f->nfuncs++] = fn;
}

static struct jscope *finish_scope(struct jparser *p, struct scope_b *b)
{
    if (!b->n && !b->nf)
        return 0;
    struct jscope *s = js_alloc(p->rt, sizeof(*s));
    s->names = b->names;
    s->kinds = b->kinds;
    s->n = b->n;
    s->funcs = b->funcs;
    s->nfuncs = b->nf;
    return s;
}

/* Wszystkie nazwy zmiennych we wzorcu (let [a, {b}] = ...) */
static void declare_pattern(struct jparser *p, jnode *pat, int kind)
{
    if (!pat)
        return;
    switch (pat->type) {
    case N_IDENT:
        if (kind == VK_VAR)
            add_var(p, pat->u.str);
        else
            add_lex(p, pat->u.str, kind);
        break;
    case N_DEFAULT:
    case N_REST:
        declare_pattern(p, pat->a, kind);
        break;
    case N_ARRPAT:
        for (jnode *e = pat->a; e; e = e->next)
            declare_pattern(p, e, kind);
        break;
    case N_OBJPAT:
        for (jnode *e = pat->a; e; e = e->next)
            declare_pattern(p, e->type == N_PROP ? e->b : e, kind);
        break;
    }
}

/* ---------- wyrazenia ---------- */

static jnode *parse_assign(struct jparser *p);
static jnode *parse_expression(struct jparser *p);
static jnode *parse_statement(struct jparser *p);
static jnode *parse_function(struct jparser *p, int is_decl, int is_method);
static jnode *parse_class(struct jparser *p, int is_decl);
static jnode *parse_unary(struct jparser *p);
static jnode *parse_block_body(struct jparser *p, int is_function);

/* Wyrazenie -> wzorzec (lewa strona przypisania, parametry strzalki).
 * binding: tylko nazwy zmiennych (bez a.b i a[i]) */
static jnode *to_pattern(struct jparser *p, jnode *e, int binding)
{
    switch (e->type) {
    case N_IDENT:
        if (e->u.str->kw && e->u.str->kw <= K_ENUM)
            lex_error(&p->L, "Zla nazwa zmiennej");
        return e;
    case N_MEMBER:
    case N_INDEX:
        if (binding)
            break;
        return e;
    case N_ASSIGN:
        if (e->op != '=')
            break;
        e->type = N_DEFAULT;
        e->a = to_pattern(p, e->a, binding);
        return e;
    case N_ARRAY: {
        e->type = N_ARRPAT;
        for (jnode *el = e->a; el; el = el->next) {
            if (el->type == N_HOLE)
                continue;
            jnode *conv;
            if (el->type == N_SPREAD) {
                el->type = N_REST;
                el->a = to_pattern(p, el->a, binding);
                continue;
            }
            conv = to_pattern(p, el, binding);
            (void)conv;
        }
        return e;
    }
    case N_OBJECT:
        e->type = N_OBJPAT;
        for (jnode *pr = e->a; pr; pr = pr->next) {
            if (pr->op == PK_SPREAD) {
                pr->type = N_REST;
                pr->a = to_pattern(p, pr->b, binding);
                continue;
            }
            if (pr->op != PK_INIT)
                lex_error(&p->L, "Zly wzorzec");
            pr->b = to_pattern(p, pr->b, binding);
        }
        return e;
    case N_DEFAULT:
    case N_ARRPAT:
    case N_OBJPAT:
    case N_REST:
        return e;
    }
    lex_error(&p->L, "Invalid destructuring assignment target");
}

static jnode *parse_args(struct jparser *p)
{
    struct list l = { 0, 0 };
    expect(p, '(');
    int saved = p->no_in;
    p->no_in = 0;
    while (!is_op(p, ')')) {
        jnode *a;
        if (is_op(p, OP_ELLIPSIS)) {
            a = mk(p, N_SPREAD);
            next(p);
            a->a = parse_assign(p);
        } else {
            a = parse_assign(p);
        }
        push(&l, a);
        if (!is_op(p, ')'))
            expect(p, ',');
    }
    p->no_in = saved;
    next(p);
    return l.head;
}

/* `tekst ${wyrazenie} tekst` - biezacy token to pierwszy kawalek */
static jnode *parse_template(struct jparser *p)
{
    jnode *t = mk(p, N_TEMPLATE);
    struct list quasis = { 0, 0 }, exprs = { 0, 0 };
    int saved = p->no_in;
    p->no_in = 0;
    for (;;) {
        jnode *q = mk(p, N_STR);
        q->u.str = TOK.str;
        q->a = mk(p, N_STR);                          /* surowy tekst (String.raw) */
        q->a->u.str = TOK.raw;
        push(&quasis, q);
        if (TOK.tmpl_tail)
            break;
        next(p);
        push(&exprs, parse_expression(p));
        if (!is_op(p, '}'))
            unexpected(p);
        lex_template_cont(&p->L);
    }
    p->no_in = saved;
    next(p);
    t->a = quasis.head;
    t->b = exprs.head;
    return t;
}

/* Klucz wlasciwosci: nazwa, tekst, liczba albo [wyrazenie]. *computed = 1 dla [ ] */
static jnode *parse_prop_key(struct jparser *p, int *computed)
{
    jnode *k;
    *computed = 0;
    if (is_op(p, '[')) {
        next(p);
        int saved = p->no_in;
        p->no_in = 0;
        k = parse_assign(p);
        p->no_in = saved;
        expect(p, ']');
        *computed = 1;
        return k;
    }
    k = mk(p, N_STR);
    if (TOK.type == TK_IDENT || TOK.type == TK_STR || TOK.type == TK_PRIVATE) {
        k->u.str = js_intern(p->rt, TOK.str);
    } else if (TOK.type == TK_NUM) {
        char buf[32];
        js_dtoa(TOK.num, buf);
        k->u.str = js_atom(p->rt, buf);
    } else {
        unexpected(p);
    }
    next(p);
    return k;
}

static int is_prop_key_start(struct jparser *p)
{
    return TOK.type == TK_IDENT || TOK.type == TK_STR || TOK.type == TK_NUM || TOK.type == TK_PRIVATE ||
           is_op(p, '[');
}

/* Metoda w literale obiektu albo klasie: klucz juz przeczytany, teraz (params) { cialo } */
static jnode *parse_method(struct jparser *p, jnode *key, int kind, int is_ctor)
{
    jnode *f = parse_function(p, 0, is_ctor ? 2 : 1);
    struct jfuninfo *fi = f->u.fn;
    if (!fi->name && key->type == N_STR) {
        if (kind == PK_GET || kind == PK_SET) {
            struct jsb b;
            jsb_init(&b, p->rt);
            jsb_ascii(&b, kind == PK_GET ? "get " : "set ");
            jsb_str(&b, key->u.str);
            fi->name = jsb_done(&b);
        } else {
            fi->name = key->u.str;
        }
    }
    fi->getter = kind == PK_GET;
    return f;
}

static jnode *parse_object(struct jparser *p)
{
    jnode *o = mk(p, N_OBJECT);
    struct list l = { 0, 0 };
    next(p);                                          /* { */
    int saved = p->no_in;
    p->no_in = 0;
    while (!is_op(p, '}')) {
        jnode *pr = mk(p, N_PROP);
        pr->op = PK_INIT;
        if (is_op(p, OP_ELLIPSIS)) {
            next(p);
            pr->op = PK_SPREAD;
            pr->b = parse_assign(p);
        } else {
            int is_async = 0, is_gen = 0;
            if (is_kw(p, K_ASYNC)) {
                struct jlex save = p->L;
                next(p);
                if (is_op(p, '*'))
                    lex_error(&p->L, "Generatory async nie sa obslugiwane");
                if (is_prop_key_start(p) && !TOK.nl_before)
                    is_async = 1;
                else
                    p->L = save;
            } else if (is_op(p, '*')) {
                next(p);
                is_gen = 1;
            }
            int getset = 0;
            if (!is_async && !is_gen && (is_kw(p, K_GET) || is_kw(p, K_SET))) {
                struct jlex save = p->L;
                int k = TOK.str->kw == K_GET ? PK_GET : PK_SET;
                next(p);
                if (is_prop_key_start(p))
                    getset = k;
                else
                    p->L = save;
            }
            int computed;
            jnode *key = parse_prop_key(p, &computed);
            pr->a = key;
            if (computed)
                pr->flags |= NF_COMPUTED;
            if (getset) {
                pr->op = (uint8_t)getset;
                pr->b = parse_method(p, key, getset, 0);
            } else if (is_op(p, ':')) {
                next(p);
                pr->b = parse_assign(p);
            } else if (is_op(p, '(')) {
                pr->op = PK_INIT;
                p->next_async = is_async;
                p->next_gen = is_gen;
                pr->b = parse_method(p, key, PK_METHOD, 0);
            } else if (is_async || is_gen) {
                unexpected(p);
            } else if (!computed && key->type == N_STR) {
                /* { x } albo { x = 1 } (to drugie tylko we wzorcu) */
                jnode *id = mk(p, N_IDENT);
                id->u.str = key->u.str;
                pr->flags |= NF_SHORTHAND;
                if (is_op(p, '=')) {
                    next(p);
                    jnode *as = mk(p, N_ASSIGN);
                    as->op = '=';
                    as->a = id;
                    as->b = parse_assign(p);
                    pr->b = as;
                } else {
                    pr->b = id;
                }
            } else {
                unexpected(p);
            }
        }
        push(&l, pr);
        if (!is_op(p, '}'))
            expect(p, ',');
    }
    p->no_in = saved;
    next(p);
    o->a = l.head;
    return o;
}

static jnode *parse_array(struct jparser *p)
{
    jnode *a = mk(p, N_ARRAY);
    struct list l = { 0, 0 };
    next(p);                                          /* [ */
    int saved = p->no_in;
    p->no_in = 0;
    while (!is_op(p, ']')) {
        jnode *e;
        if (is_op(p, ',')) {
            e = mk(p, N_HOLE);
            push(&l, e);
            next(p);
            continue;
        }
        if (is_op(p, OP_ELLIPSIS)) {
            e = mk(p, N_SPREAD);
            next(p);
            e->a = parse_assign(p);
        } else {
            e = parse_assign(p);
        }
        push(&l, e);
        if (!is_op(p, ']'))
            expect(p, ',');
    }
    p->no_in = saved;
    next(p);
    a->a = l.head;
    return a;
}

/* Parametry funkcji: (a, b = 1, {c}, ...reszta) */
static void finish_params(struct jparser *p, struct jfuninfo *fi, jnode *params)
{
    fi->params = params;
    fi->simple_params = 1;
    int counting = 1;
    for (jnode *e = params; e; e = e->next) {
        if (e->type != N_IDENT)
            fi->simple_params = 0;
        if (e->type == N_DEFAULT || e->type == N_REST)
            counting = 0;
        if (counting)
            fi->nparams++;
    }
    (void)p;
}

static jnode *parse_params(struct jparser *p)
{
    struct list l = { 0, 0 };
    expect(p, '(');
    while (!is_op(p, ')')) {
        jnode *e;
        if (is_op(p, OP_ELLIPSIS)) {
            e = mk(p, N_REST);
            next(p);
            e->a = to_pattern(p, parse_unary(p), 1);
            push(&l, e);
            if (!is_op(p, ')'))
                lex_error(&p->L, "Parametr ...reszta musi byc ostatni");
            break;
        }
        e = to_pattern(p, parse_assign(p), 1);
        push(&l, e);
        if (!is_op(p, ')'))
            expect(p, ',');
    }
    next(p);
    return l.head;
}

/* Wspolne dla funkcji i strzalek: nowy kontekst deklaracji */
struct fn_save {
    struct jfuninfo *fn, *fn_args;
    struct scope_b *blk, *fn_blk;
    int no_in;
};

static struct jfuninfo *begin_function(struct jparser *p, struct fn_save *s, int arrow)
{
    s->fn = p->fn;
    s->fn_args = p->fn_args;
    s->blk = p->blk;
    s->fn_blk = p->fn_blk;
    s->no_in = p->no_in;
    struct jfuninfo *fi = js_alloc(p->rt, sizeof(*fi));
    fi->arrow = (uint8_t)arrow;
    fi->is_async = (uint8_t)p->next_async;
    p->next_async = 0;
    fi->is_gen = (uint8_t)p->next_gen;
    p->next_gen = 0;
    fi->strict = s->fn ? s->fn->strict : 0;
    p->fn = fi;
    if (!arrow)
        p->fn_args = fi;
    p->no_in = 0;
    p->nfuncs_created++;
    return fi;
}

static void end_function(struct jparser *p, struct fn_save *s)
{
    p->fn = s->fn;
    p->fn_args = s->fn_args;
    p->blk = s->blk;
    p->fn_blk = s->fn_blk;
    p->no_in = s->no_in;
}

/* Cialo funkcji { ... } z dyrektywa "use strict" */
static jnode *parse_function_body(struct jparser *p, struct jfuninfo *fi)
{
    struct scope_b *b = js_alloc(p->rt, sizeof(*b));
    p->blk = b;
    p->fn_blk = b;
    jnode *body = mk(p, N_BLOCK);
    expect(p, '{');
    /* dyrektywy na poczatku */
    struct list l = { 0, 0 };
    while (TOK.type == TK_STR) {
        jstr *s = TOK.str;
        jnode *st = parse_statement(p);
        push(&l, st);
        if (st->type != N_EXPR || st->a->type != N_STR)
            break;
        if (js_str_is(s, "use strict"))
            fi->strict = 1;
    }
    while (!is_op(p, '}')) {
        if (TOK.type == TK_EOF)
            lex_error(&p->L, "Brak } na koncu funkcji");
        push(&l, parse_statement(p));
    }
    next(p);
    body->a = l.head;
    body->u.scope = finish_scope(p, b);
    return body;
}

/* function nazwa(params) { cialo }. is_method: 1 metoda, 2 konstruktor klasy */
static jnode *parse_function(struct jparser *p, int is_decl, int is_method)
{
    jnode *f = mk(p, N_FUNC);
    uint32_t start = TOK.start;
    struct fn_save s;
    jstr *name = 0;
    if (!is_method) {
        next(p);                                      /* function */
        if (is_op(p, '*')) {
            if (p->next_async)
                lex_error(&p->L, "Generatory async nie sa obslugiwane");
            next(p);
            p->next_gen = 1;
        }
        if (TOK.type == TK_IDENT && !is_op(p, '('))
            name = binding_name(p);
        else if (is_decl)
            unexpected(p);
    }
    struct jfuninfo *fi = begin_function(p, &s, 0);
    fi->name = name;
    fi->method = is_method == 1;
    fi->is_class_ctor = is_method == 2;
    enter(p);
    finish_params(p, fi, parse_params(p));
    fi->body = parse_function_body(p, fi);
    leave(p);
    fi->src = p->L.src + start;
    fi->src_len = p->prev_end - start;
    end_function(p, &s);
    f->u.fn = fi;
    return f;
}

/* Strzalka: params juz przeczytane (lista wyrazen albo wzorcow), biezacy token "=>" */
static jnode *parse_arrow(struct jparser *p, jnode *params, uint32_t start)
{
    if (TOK.nl_before)
        unexpected(p);
    jnode *f = mk(p, N_FUNC);
    struct fn_save s;
    struct jfuninfo *fi = begin_function(p, &s, 1);
    enter(p);
    struct list l = { 0, 0 };
    for (jnode *e = params, *nx; e; e = nx) {
        nx = e->next;
        e->next = 0;
        push(&l, to_pattern(p, e, 1));
    }
    finish_params(p, fi, l.head);
    next(p);                                          /* => */
    if (is_op(p, '{')) {
        fi->body = parse_function_body(p, fi);
    } else {
        struct scope_b *b = js_alloc(p->rt, sizeof(*b));
        p->blk = b;
        p->fn_blk = b;
        p->no_in = s.no_in;
        fi->body = parse_assign(p);
        fi->expr_body = 1;
    }
    leave(p);
    fi->src = p->L.src + start;
    fi->src_len = p->prev_end - start;
    end_function(p, &s);
    f->u.fn = fi;
    return f;
}

/* ( ... ) - wyrazenie w nawiasie albo parametry strzalki */
static jnode *parse_paren(struct jparser *p)
{
    uint32_t start = TOK.start;
    int is_async = p->async_arrow;
    p->async_arrow = 0;
    next(p);                                          /* ( */
    int saved = p->no_in;
    p->no_in = 0;
    struct list l = { 0, 0 };
    int trailing = 0, rest = 0;
    while (!is_op(p, ')')) {
        if (is_op(p, OP_ELLIPSIS)) {
            jnode *r = mk(p, N_REST);
            next(p);
            r->a = parse_unary(p);
            push(&l, r);
            rest = 1;
            if (!is_op(p, ')'))
                unexpected(p);
            break;
        }
        push(&l, parse_assign(p));
        trailing = 0;
        if (!is_op(p, ')')) {
            expect(p, ',');
            trailing = 1;
        }
    }
    next(p);                                          /* ) */
    p->no_in = saved;
    if (is_op(p, OP_ARROW)) {
        p->next_async = is_async;
        return parse_arrow(p, l.head, start);
    }
    if (!l.head || trailing || rest)
        unexpected(p);
    jnode *e;
    if (l.head->next) {
        e = mk(p, N_SEQ);
        e->a = l.head;
    } else {
        e = l.head;
    }
    e->flags |= NF_PAREN;
    return e;
}

/* Slowo async w wyrazeniu: async function, async x => ..., async (...) => ...
 * Zwraca 0, gdy to zwykla nazwa "async" (np. wywolanie funkcji async(1)) */
static jnode *parse_async(struct jparser *p)
{
    uint32_t start = TOK.start;
    struct jlex save = p->L;
    next(p);                                          /* async */
    if (TOK.nl_before) {
        p->L = save;
        return 0;
    }
    if (is_kw(p, K_FUNCTION)) {
        p->next_async = 1;
        return parse_function(p, 0, 0);
    }
    if (TOK.type == TK_IDENT && !(TOK.str->kw && TOK.str->kw <= K_ENUM)) {
        jnode *n = mk(p, N_IDENT);
        n->u.str = TOK.str;
        next(p);
        if (is_op(p, OP_ARROW) && !TOK.nl_before) {
            p->next_async = 1;
            return parse_arrow(p, n, start);
        }
        p->L = save;
        return 0;
    }
    if (is_op(p, '(')) {
        /* zagladamy za pasujacy nawias: czy dalej jest => */
        struct jlex at = p->L;
        int depth = 0, arrow = 0;
        for (;;) {
            if (TOK.type == TK_EOF)
                break;
            if (TOK.type == TK_PUNCT && (TOK.op == '(' || TOK.op == '[' || TOK.op == '{'))
                depth++;
            else if (TOK.type == TK_PUNCT && (TOK.op == ')' || TOK.op == ']' || TOK.op == '}'))
                depth--;
            lex_next(&p->L);
            if (depth == 0) {
                arrow = is_op(p, OP_ARROW) && !TOK.nl_before;
                break;
            }
        }
        if (arrow) {
            p->L = at;
            p->async_arrow = 1;
            return parse_paren(p);
        }
    }
    p->L = save;
    return 0;
}

static jnode *parse_primary(struct jparser *p)
{
    jnode *n;
    switch (TOK.type) {
    case TK_NUM:
        n = mk(p, N_NUM);
        n->u.num = TOK.num;
        next(p);
        return n;
    case TK_STR:
        n = mk(p, N_STR);
        n->u.str = TOK.str;
        next(p);
        return n;
    case TK_TEMPLATE:
        return parse_template(p);
    case TK_IDENT:
        if (!TOK.escaped) {
            switch (TOK.str->kw) {
            case K_THIS: n = mk(p, N_THIS); next(p); return n;
            case K_NULL: n = mk(p, N_NULL); next(p); return n;
            case K_TRUE: n = mk(p, N_TRUE); next(p); return n;
            case K_FALSE: n = mk(p, N_FALSE); next(p); return n;
            case K_FUNCTION: return parse_function(p, 0, 0);
            case K_CLASS: return parse_class(p, 0);
            case K_SUPER: n = mk(p, N_SUPER); next(p); return n;
            case K_ASYNC: {
                jnode *a = parse_async(p);
                if (a)
                    return a;
                break;
            }
            case K_ARGUMENTS:
                if (p->fn_args)
                    p->fn_args->uses_args = 1;
                break;
            case K_IMPORT:
                lex_error(&p->L, "import nie jest obslugiwany");
            default:
                if (TOK.str->kw && TOK.str->kw <= K_ENUM)
                    unexpected(p);
            }
        }
        {
            uint32_t start = TOK.start;
            n = mk(p, N_IDENT);
            n->u.str = TOK.str;
            next(p);
            if (is_op(p, OP_ARROW) && !TOK.nl_before)
                return parse_arrow(p, n, start);
            return n;
        }
    case TK_REGEX:
        break;
    case TK_PUNCT:
        switch (TOK.op) {
        case '(':
            return parse_paren(p);
        case '[':
            return parse_array(p);
        case '{':
            return parse_object(p);
        case '/':
        case OP_DIV_A:
            lex_regex(&p->L);
            n = mk(p, N_REGEX);
            n->u.re = TOK.re;
            next(p);
            return n;
        }
        break;
    }
    unexpected(p);
}

/* Wyrazenie z .nazwa, [klucz], (argumenty), ?. - "a.b(c)[d]" */
static jnode *parse_call(struct jparser *p, int allow_call)
{
    jnode *e;
    int optional = 0;
    enter(p);
    if (is_kw(p, K_NEW)) {
        jnode *nw = mk(p, N_NEW);
        next(p);
        if (is_op(p, '.')) {
            next(p);
            if (TOK.type != TK_IDENT || !js_str_is(TOK.str, "target"))
                unexpected(p);
            next(p);
            e = mk(p, N_NEWTARGET);
        } else {
            nw->a = parse_call(p, 0);
            if (is_op(p, '('))
                nw->b = parse_args(p);
            e = nw;
        }
    } else {
        e = parse_primary(p);
    }
    for (;;) {
        if (is_op(p, '.')) {
            next(p);
            if (TOK.type != TK_IDENT && TOK.type != TK_PRIVATE)
                unexpected(p);
            jnode *m = mk(p, N_MEMBER);
            m->a = e;
            m->u.str = TOK.str;
            next(p);
            e = m;
        } else if (is_op(p, OP_OPTCHAIN)) {
            if (!allow_call)
                unexpected(p);
            next(p);
            optional = 1;
            if (is_op(p, '(')) {
                jnode *c = mk(p, N_CALL);
                c->a = e;
                c->b = parse_args(p);
                c->flags |= NF_OPTIONAL;
                e = c;
            } else if (is_op(p, '[')) {
                jnode *ix = mk(p, N_INDEX);
                next(p);
                int saved = p->no_in;
                p->no_in = 0;
                ix->a = e;
                ix->b = parse_expression(p);
                p->no_in = saved;
                expect(p, ']');
                ix->flags |= NF_OPTIONAL;
                e = ix;
            } else {
                if (TOK.type != TK_IDENT && TOK.type != TK_PRIVATE)
                    unexpected(p);
                jnode *m = mk(p, N_MEMBER);
                m->a = e;
                m->u.str = TOK.str;
                m->flags |= NF_OPTIONAL;
                next(p);
                e = m;
            }
        } else if (is_op(p, '[')) {
            jnode *ix = mk(p, N_INDEX);
            next(p);
            int saved = p->no_in;
            p->no_in = 0;
            ix->a = e;
            ix->b = parse_expression(p);
            p->no_in = saved;
            expect(p, ']');
            e = ix;
        } else if (is_op(p, '(') && allow_call) {
            jnode *c = mk(p, N_CALL);
            c->a = e;
            c->b = parse_args(p);
            e = c;
        } else if (TOK.type == TK_TEMPLATE) {
            if (optional)
                unexpected(p);
            jnode *t = mk(p, N_TAGGED);
            t->a = e;
            t->b = parse_template(p);
            e = t;
        } else {
            break;
        }
    }
    leave(p);
    if (optional) {
        jnode *c = mk(p, N_OPTCHAIN);
        c->a = e;
        return c;
    }
    return e;
}

static int valid_target(jnode *e)
{
    return e->type == N_IDENT || e->type == N_MEMBER || e->type == N_INDEX;
}

static jnode *parse_unary(struct jparser *p)
{
    enter(p);
    jnode *n;
    int op = 0;
    if (TOK.type == TK_PUNCT && (TOK.op == '!' || TOK.op == '~' || TOK.op == '+' || TOK.op == '-'))
        op = TOK.op == '+' ? OP_PLUS : TOK.op == '-' ? OP_NEG : TOK.op;
    else if (is_kw(p, K_TYPEOF))
        op = OP_TYPEOF;
    else if (is_kw(p, K_VOID))
        op = OP_VOID;
    else if (is_kw(p, K_DELETE))
        op = OP_DELETE;
    if (is_kw(p, K_AWAIT)) {
        if (p->fn->is_async) {
            n = mk(p, N_AWAIT);
            next(p);
            n->a = parse_unary(p);
            leave(p);
            return n;
        }
        struct jlex save = p->L;
        next(p);
        if (!TOK.nl_before && (TOK.type == TK_IDENT || TOK.type == TK_STR || TOK.type == TK_NUM))
            lex_error(&p->L, "await is only valid in async functions");
        p->L = save;
    }
    if (op) {
        n = mk(p, N_UNARY);
        n->op = (uint8_t)OPC(op);
        next(p);
        n->a = parse_unary(p);
        leave(p);
        return n;
    }
    if (is_op(p, OP_INC) || is_op(p, OP_DEC)) {
        n = mk(p, N_UPDATE);
        n->op = (uint8_t)OPC(TOK.op);
        n->flags |= NF_PREFIX;
        next(p);
        n->a = parse_unary(p);
        if (!valid_target(n->a))
            lex_error(&p->L, "Invalid left-hand side expression in prefix operation");
        leave(p);
        return n;
    }
    jnode *e = parse_call(p, 1);
    if ((is_op(p, OP_INC) || is_op(p, OP_DEC)) && !TOK.nl_before) {
        if (!valid_target(e))
            lex_error(&p->L, "Invalid left-hand side expression in postfix operation");
        n = mk(p, N_UPDATE);
        n->op = (uint8_t)OPC(TOK.op);
        n->a = e;
        next(p);
        e = n;
    }
    leave(p);
    return e;
}

/* Priorytet dzialania dwuargumentowego (0 = to nie takie dzialanie) */
static int binary_prec(struct jparser *p, int *op)
{
    if (TOK.type == TK_IDENT && !TOK.escaped) {
        if (TOK.str->kw == K_INSTANCEOF) { *op = OP_INSTANCEOF; return 8; }
        if (TOK.str->kw == K_IN && !p->no_in) { *op = OP_IN; return 8; }
        return 0;
    }
    if (TOK.type != TK_PUNCT)
        return 0;
    *op = TOK.op;
    switch (TOK.op) {
    case OP_NULLISH: return 1;
    case OP_OR: return 2;
    case OP_AND: return 3;
    case '|': return 4;
    case '^': return 5;
    case '&': return 6;
    case OP_EQ: case OP_NE: case OP_SEQ: case OP_SNE: return 7;
    case '<': case '>': case OP_LE: case OP_GE: return 8;
    case OP_SHL: case OP_SHR: case OP_USHR: return 9;
    case '+': case '-': return 10;
    case '*': case '/': case '%': return 11;
    case OP_POW: return 12;
    }
    return 0;
}

static jnode *parse_binary(struct jparser *p, int min_prec)
{
    enter(p);
    jnode *lhs = parse_unary(p);
    for (;;) {
        int op, prec = binary_prec(p, &op);
        if (!prec || prec < min_prec)
            break;
        next(p);
        /* ** laczy sie od prawej: 2 ** 3 ** 2 = 2 ** 9 */
        jnode *rhs = parse_binary(p, op == OP_POW ? prec : prec + 1);
        jnode *n = mk(p, op == OP_AND || op == OP_OR || op == OP_NULLISH ? N_LOGICAL : N_BINARY);
        n->op = (uint8_t)OPC(op);
        n->a = lhs;
        n->b = rhs;
        lhs = n;
    }
    leave(p);
    return lhs;
}

static jnode *parse_conditional(struct jparser *p)
{
    jnode *test = parse_binary(p, 1);
    if (!is_op(p, '?'))
        return test;
    jnode *n = mk(p, N_COND);
    next(p);
    int saved = p->no_in;
    p->no_in = 0;
    n->a = test;
    n->b = parse_assign(p);
    p->no_in = saved;
    expect(p, ':');
    n->c = parse_assign(p);
    return n;
}

static int is_assign_op(int op)
{
    return op == '=' || (op >= OP_ADD_A && op <= OP_NULLISH_A);
}

static jnode *parse_assign(struct jparser *p)
{
    enter(p);
    if (is_kw(p, K_YIELD) && p->fn->is_gen) {
        jnode *y = mk(p, N_YIELD);
        next(p);
        if (is_op(p, '*') && !TOK.nl_before) {        /* yield* inny_generator */
            next(p);
            y->op = 1;
            y->a = parse_assign(p);
        } else if (!TOK.nl_before && TOK.type != TK_EOF &&
                   !(TOK.type == TK_PUNCT && (TOK.op == ')' || TOK.op == ']' || TOK.op == '}' ||
                                              TOK.op == ',' || TOK.op == ';' || TOK.op == ':'))) {
            y->a = parse_assign(p);
        }
        leave(p);
        return y;
    }
    jnode *lhs = parse_conditional(p);
    if (TOK.type == TK_PUNCT && is_assign_op(TOK.op)) {
        int op = TOK.op;
        jnode *n = mk(p, N_ASSIGN);
        if (op == '=' && (lhs->type == N_ARRAY || lhs->type == N_OBJECT) && !(lhs->flags & NF_PAREN))
            lhs = to_pattern(p, lhs, 0);
        else if (!valid_target(lhs))
            lex_error(&p->L, "Invalid left-hand side in assignment");
        next(p);
        n->op = (uint8_t)OPC(op);
        n->a = lhs;
        n->b = parse_assign(p);
        leave(p);
        return n;
    }
    leave(p);
    return lhs;
}

static jnode *parse_expression(struct jparser *p)
{
    jnode *e = parse_assign(p);
    if (!is_op(p, ','))
        return e;
    jnode *seq = mk(p, N_SEQ);
    struct list l = { 0, 0 };
    push(&l, e);
    while (is_op(p, ',')) {
        next(p);
        push(&l, parse_assign(p));
    }
    seq->a = l.head;
    return seq;
}

/* ---------- klasy ---------- */

static jnode *parse_class(struct jparser *p, int is_decl)
{
    jnode *c = mk(p, N_CLASS);
    next(p);                                          /* class */
    if (TOK.type == TK_IDENT && !is_kw(p, K_EXTENDS))
        c->u.str = binding_name(p);
    else if (is_decl)
        unexpected(p);
    if (is_kw(p, K_EXTENDS)) {
        next(p);
        c->a = parse_call(p, 1);
    }
    expect(p, '{');
    int saved_strict = p->fn->strict;
    p->fn->strict = 1;                                /* klasy sa zawsze "strict" */
    p->nfuncs_created++;
    struct list l = { 0, 0 };
    while (!is_op(p, '}')) {
        if (is_op(p, ';')) {
            next(p);
            continue;
        }
        jnode *m = mk(p, N_PROP);
        m->op = PK_METHOD;
        if (is_kw(p, K_STATIC)) {
            struct jlex save = p->L;
            next(p);
            if (is_op(p, '(') || is_op(p, '=') || is_op(p, ';') || is_op(p, '}'))
                p->L = save;                          /* metoda o nazwie "static" */
            else if (is_op(p, '{'))
                lex_error(&p->L, "Bloki static { } w klasie nie sa obslugiwane");
            else
                m->flags |= NF_STATIC;
        }
        int is_async = 0, is_gen = 0;
        if (is_kw(p, K_ASYNC)) {
            struct jlex save = p->L;
            next(p);
            if (is_op(p, '*'))
                lex_error(&p->L, "Generatory async nie sa obslugiwane");
            if (is_prop_key_start(p) && !TOK.nl_before)
                is_async = 1;
            else
                p->L = save;
        } else if (is_op(p, '*')) {
            next(p);
            is_gen = 1;
        }
        int getset = 0;
        if (!is_async && !is_gen && (is_kw(p, K_GET) || is_kw(p, K_SET))) {
            struct jlex save = p->L;
            int k = TOK.str->kw == K_GET ? PK_GET : PK_SET;
            next(p);
            if (is_prop_key_start(p))
                getset = k;
            else
                p->L = save;
        }
        int computed;
        jnode *key = parse_prop_key(p, &computed);
        m->a = key;
        if (computed)
            m->flags |= NF_COMPUTED;
        if (getset) {
            m->op = (uint8_t)getset;
            m->b = parse_method(p, key, getset, 0);
        } else if (is_op(p, '(')) {
            int is_ctor = !computed && !(m->flags & NF_STATIC) && key->type == N_STR &&
                          key->u.str == p->rt->A.constructor;
            if (is_ctor && (is_async || is_gen))
                lex_error(&p->L, "Class constructor may not be an async method");
            p->next_async = is_async;
            p->next_gen = is_gen;
            jnode *f = parse_method(p, key, PK_METHOD, is_ctor);
            if (is_ctor) {
                if (c->c)
                    lex_error(&p->L, "A class may only have one constructor");
                f->u.fn->name = c->u.str;
                c->c = f;
                continue;
            }
            m->b = f;
        } else {
            /* pole: nazwa = wartosc; - wartosc liczona jak w metodzie (this = obiekt) */
            m->op = PK_FIELD;
            if (is_op(p, '=')) {
                next(p);
                struct fn_save s;
                struct jfuninfo *fi = begin_function(p, &s, 0);
                fi->method = 1;
                fi->expr_body = 1;
                struct scope_b *b = js_alloc(p->rt, sizeof(*b));
                p->blk = b;
                p->fn_blk = b;
                fi->body = parse_assign(p);
                end_function(p, &s);
                jnode *f = mk(p, N_FUNC);
                f->u.fn = fi;
                m->b = f;
            }
            semicolon(p);
        }
        push(&l, m);
    }
    next(p);
    p->fn->strict = (uint8_t)saved_strict;
    c->b = l.head;
    return c;
}

/* ---------- instrukcje ---------- */

static jnode *parse_var(struct jparser *p, int kind, int in_for)
{
    jnode *v = mk(p, N_VAR);
    v->op = (uint8_t)kind;
    next(p);                                          /* var / let / const */
    struct list l = { 0, 0 };
    for (;;) {
        jnode *d = mk(p, N_DECL);
        if (is_op(p, '[') || is_op(p, '{'))
            d->a = to_pattern(p, parse_primary(p), 1);
        else {
            d->a = mk(p, N_IDENT);
            d->a->u.str = binding_name(p);
        }
        declare_pattern(p, d->a, kind);
        if (is_op(p, '=')) {
            next(p);
            d->b = parse_assign(p);
        } else if (!in_for && (kind == VK_CONST || d->a->type != N_IDENT)) {
            lex_error(&p->L, "Missing initializer in declaration");
        }
        push(&l, d);
        if (!is_op(p, ','))
            break;
        next(p);
    }
    v->a = l.head;
    return v;
}

/* Czy "let" zaczyna deklaracje (let x, let [a], let {a}) */
static int let_decl(struct jparser *p)
{
    if (!is_kw(p, K_LET))
        return 0;
    struct jlex save = p->L;
    next(p);
    int r = (TOK.type == TK_IDENT && !(TOK.str->kw == K_IN || TOK.str->kw == K_INSTANCEOF)) ||
            is_op(p, '[') || is_op(p, '{');
    p->L = save;
    return r;
}

static jnode *parse_block_body(struct jparser *p, int is_function)
{
    (void)is_function;
    jnode *b = mk(p, N_BLOCK);
    struct scope_b *saved = p->blk;
    struct scope_b *sb = js_alloc(p->rt, sizeof(*sb));
    p->blk = sb;
    expect(p, '{');
    struct list l = { 0, 0 };
    while (!is_op(p, '}')) {
        if (TOK.type == TK_EOF)
            lex_error(&p->L, "Brak } na koncu bloku");
        push(&l, parse_statement(p));
    }
    next(p);
    b->a = l.head;
    b->u.scope = finish_scope(p, sb);
    p->blk = saved;
    return b;
}

/* Instrukcja jako cialo if/petli: deklaracja funkcji dostaje wlasny blok */
static jnode *parse_substatement(struct jparser *p)
{
    if (is_kw(p, K_FUNCTION) || is_kw(p, K_CLASS) || is_kw(p, K_CONST) || let_decl(p)) {
        jnode *b = mk(p, N_BLOCK);
        struct scope_b *saved = p->blk;
        struct scope_b *sb = js_alloc(p->rt, sizeof(*sb));
        p->blk = sb;
        b->a = parse_statement(p);
        b->u.scope = finish_scope(p, sb);
        p->blk = saved;
        return b;
    }
    return parse_statement(p);
}

static jnode *parse_for(struct jparser *p)
{
    jnode *f = mk(p, N_FOR);
    int created = p->nfuncs_created;
    next(p);                                          /* for */
    int for_await = 0;
    if (is_kw(p, K_AWAIT)) {
        if (!p->fn->is_async)
            lex_error(&p->L, "for await is only valid in async functions");
        for_await = 1;
        next(p);
    }
    expect(p, '(');
    struct scope_b *saved = p->blk;
    struct scope_b *sb = js_alloc(p->rt, sizeof(*sb));
    p->blk = sb;
    jnode *init = 0;
    int saved_in = p->no_in;
    p->no_in = 1;
    if (is_op(p, ';')) {
        /* bez init */
    } else if (is_kw(p, K_VAR) || is_kw(p, K_CONST) || let_decl(p)) {
        int kind = is_kw(p, K_VAR) ? VK_VAR : is_kw(p, K_CONST) ? VK_CONST : VK_LET;
        init = parse_var(p, kind, 1);
    } else {
        init = parse_expression(p);
    }
    p->no_in = saved_in;
    int is_in = is_kw(p, K_IN), is_of = is_kw(p, K_OF);
    if (init && (is_in || is_of)) {
        f->type = is_in ? N_FORIN : N_FOROF;
        if (for_await && is_in)
            unexpected(p);
        if (for_await)
            f->flags |= NF_AWAIT;
        if (init->type == N_VAR) {
            if (init->a->next)
                lex_error(&p->L, "Tylko jedna zmienna w for-in/for-of");
        } else {
            init = to_pattern(p, init, 0);
        }
        next(p);
        f->a = init;
        f->b = is_of ? parse_assign(p) : parse_expression(p);
        expect(p, ')');
    } else {
        if (for_await)
            unexpected(p);
        if (init && init->type == N_VAR && init->op != VK_VAR) {
            for (jnode *d = init->a; d; d = d->next)
                if (!d->b && (init->op == VK_CONST || d->a->type != N_IDENT))
                    lex_error(&p->L, "Missing initializer in declaration");
        } else if (init && init->type == N_VAR) {
            for (jnode *d = init->a; d; d = d->next)
                if (!d->b && d->a->type != N_IDENT)
                    lex_error(&p->L, "Missing initializer in declaration");
        }
        if (init && init->type != N_VAR) {
            jnode *e = mk(p, N_EXPR);
            e->a = init;
            init = e;
        }
        f->a = init;
        expect(p, ';');
        if (!is_op(p, ';'))
            f->b = parse_expression(p);
        expect(p, ';');
        if (!is_op(p, ')'))
            f->c = parse_expression(p);
        expect(p, ')');
    }
    f->d = parse_substatement(p);
    f->u.scope = finish_scope(p, sb);
    p->blk = saved;
    if (p->nfuncs_created != created)
        f->flags |= NF_CLOSURE;
    return f;
}

static jnode *parse_statement(struct jparser *p)
{
    enter(p);
    jnode *n;
    if (TOK.type == TK_PUNCT) {
        if (TOK.op == '{') {
            n = parse_block_body(p, 0);
            leave(p);
            return n;
        }
        if (TOK.op == ';') {
            n = mk(p, N_EMPTY);
            next(p);
            leave(p);
            return n;
        }
    }
    if (TOK.type == TK_IDENT && !TOK.escaped) {
        switch (TOK.str->kw) {
        case K_VAR:
        case K_CONST:
            n = parse_var(p, TOK.str->kw == K_VAR ? VK_VAR : VK_CONST, 0);
            semicolon(p);
            leave(p);
            return n;
        case K_LET:
            if (let_decl(p)) {
                n = parse_var(p, VK_LET, 0);
                semicolon(p);
                leave(p);
                return n;
            }
            break;
        case K_FUNCTION:
        func_decl: {
            jnode *f = parse_function(p, 1, 0);
            n = mk(p, N_FUNCDECL);
            n->a = f;
            jstr *name = f->u.fn->name;
            if (p->blk == p->fn_blk) {
                add_var(p, name);
                add_top_func(p, f);
            } else {
                add_lex(p, name, VK_LET);
                add_block_func(p, f);
                add_var(p, name);                     /* jak w starych przegladarkach: widoczna w calej funkcji */
            }
            leave(p);
            return n;
        }
        case K_ASYNC: {
            struct jlex save = p->L;
            next(p);
            if (is_kw(p, K_FUNCTION) && !TOK.nl_before) {
                p->next_async = 1;
                goto func_decl;
            }
            p->L = save;
            break;
        }
        case K_CLASS: {
            n = mk(p, N_CLASSDECL);
            n->a = parse_class(p, 1);
            add_lex(p, n->a->u.str, VK_LET);
            leave(p);
            return n;
        }
        case K_IF:
            n = mk(p, N_IF);
            next(p);
            expect(p, '(');
            n->a = parse_expression(p);
            expect(p, ')');
            n->b = parse_substatement(p);
            if (is_kw(p, K_ELSE)) {
                next(p);
                n->c = parse_substatement(p);
            }
            leave(p);
            return n;
        case K_FOR:
            n = parse_for(p);
            leave(p);
            return n;
        case K_WHILE: {
            int created = p->nfuncs_created;
            n = mk(p, N_WHILE);
            next(p);
            expect(p, '(');
            n->a = parse_expression(p);
            expect(p, ')');
            n->d = parse_substatement(p);
            if (p->nfuncs_created != created)
                n->flags |= NF_CLOSURE;
            leave(p);
            return n;
        }
        case K_DO:
            n = mk(p, N_DOWHILE);
            next(p);
            n->d = parse_substatement(p);
            if (!is_kw(p, K_WHILE))
                unexpected(p);
            next(p);
            expect(p, '(');
            n->a = parse_expression(p);
            expect(p, ')');
            if (is_op(p, ';'))
                next(p);
            leave(p);
            return n;
        case K_RETURN:
            n = mk(p, N_RETURN);
            next(p);
            if (!is_op(p, ';') && !is_op(p, '}') && TOK.type != TK_EOF && !TOK.nl_before)
                n->a = parse_expression(p);
            semicolon(p);
            leave(p);
            return n;
        case K_BREAK:
        case K_CONTINUE:
            n = mk(p, TOK.str->kw == K_BREAK ? N_BREAK : N_CONTINUE);
            next(p);
            if (TOK.type == TK_IDENT && !TOK.nl_before && !(TOK.str->kw && TOK.str->kw <= K_ENUM)) {
                n->u.str = TOK.str;
                next(p);
            }
            semicolon(p);
            leave(p);
            return n;
        case K_THROW:
            n = mk(p, N_THROW);
            next(p);
            if (TOK.nl_before)
                lex_error(&p->L, "Illegal newline after throw");
            n->a = parse_expression(p);
            semicolon(p);
            leave(p);
            return n;
        case K_TRY:
            n = mk(p, N_TRY);
            next(p);
            n->a = parse_block_body(p, 0);
            if (is_kw(p, K_CATCH)) {
                next(p);
                if (is_op(p, '(')) {
                    next(p);
                    if (is_op(p, '[') || is_op(p, '{'))
                        n->b = to_pattern(p, parse_primary(p), 1);
                    else {
                        n->b = mk(p, N_IDENT);
                        n->b->u.str = binding_name(p);
                    }
                    expect(p, ')');
                }
                n->c = parse_block_body(p, 0);
                if (!n->c->a)
                    n->c->a = mk(p, N_EMPTY);          /* znak: jest (pusty) catch */
            }
            if (is_kw(p, K_FINALLY)) {
                next(p);
                n->d = parse_block_body(p, 0);
            }
            if (!n->c && !n->d)
                lex_error(&p->L, "Missing catch or finally after try");
            leave(p);
            return n;
        case K_SWITCH: {
            n = mk(p, N_SWITCH);
            next(p);
            expect(p, '(');
            n->a = parse_expression(p);
            expect(p, ')');
            expect(p, '{');
            struct scope_b *saved = p->blk;
            struct scope_b *sb = js_alloc(p->rt, sizeof(*sb));
            p->blk = sb;
            struct list cases = { 0, 0 };
            int has_default = 0;
            while (!is_op(p, '}')) {
                jnode *c = mk(p, N_CASE);
                if (is_kw(p, K_CASE)) {
                    next(p);
                    c->a = parse_expression(p);
                } else if (is_kw(p, K_DEFAULT)) {
                    if (has_default)
                        lex_error(&p->L, "More than one default clause in switch statement");
                    has_default = 1;
                    next(p);
                    c->flags |= NF_STATIC;              /* znak: default */
                } else {
                    unexpected(p);
                }
                expect(p, ':');
                struct list body = { 0, 0 };
                while (!is_kw(p, K_CASE) && !is_kw(p, K_DEFAULT) && !is_op(p, '}')) {
                    if (TOK.type == TK_EOF)
                        unexpected(p);
                    push(&body, parse_statement(p));
                }
                c->b = body.head;
                push(&cases, c);
            }
            next(p);
            n->b = cases.head;
            n->u.scope = finish_scope(p, sb);
            p->blk = saved;
            leave(p);
            return n;
        }
        case K_DEBUGGER:
            n = mk(p, N_DEBUGGER);
            next(p);
            semicolon(p);
            leave(p);
            return n;
        case K_WITH:
            lex_error(&p->L, "with nie jest obslugiwane");
        case K_IMPORT:
        case K_EXPORT:
            lex_error(&p->L, "Moduly (import/export) nie sa obslugiwane");
        }
    }
    /* wyrazenie albo etykieta */
    jnode *e = parse_expression(p);
    if (e->type == N_IDENT && !(e->flags & NF_PAREN) && is_op(p, ':')) {
        next(p);
        n = mk(p, N_LABEL);
        n->u.str = e->u.str;
        if (is_kw(p, K_FUNCTION))
            n->a = parse_substatement(p);
        else
            n->a = parse_statement(p);
        jnode *body = n->a;
        if (body->type == N_FOR || body->type == N_FORIN || body->type == N_FOROF ||
            body->type == N_WHILE || body->type == N_DOWHILE)
            body->label = n->u.str;
        leave(p);
        return n;
    }
    n = mk(p, N_EXPR);
    n->a = e;
    n->line = e->line;
    semicolon(p);
    leave(p);
    return n;
}

/* ---------- wejscie ---------- */

struct parse_ctx {
    struct jparser *p;
    jnode *result;
};

jnode *js_parse(js_rt *rt, const char *src, unsigned len, char *err, unsigned errsize,
                struct jfuninfo **top)
{
    struct jparser *p = js_alloc(rt, sizeof(*p));
    struct js_jmp fail;
    lex_init(&p->L, rt, src, len);
    p->L.err = err;
    p->L.errsize = errsize;
    p->L.fail = &fail;
    p->rt = rt;
    if (js_setjmp(&fail))
        return 0;
    struct jfuninfo *fi = js_alloc(rt, sizeof(*fi));
    p->fn = fi;
    p->fn_args = 0;
    struct scope_b *sb = js_alloc(rt, sizeof(*sb));
    p->blk = sb;
    p->fn_blk = sb;
    jnode *prog = mk(p, N_BLOCK);
    next(p);
    struct list l = { 0, 0 };
    int directives = 1;
    while (TOK.type != TK_EOF) {
        int is_str = TOK.type == TK_STR;
        jstr *s = TOK.str;
        jnode *st = parse_statement(p);
        if (directives && is_str && st->type == N_EXPR && st->a->type == N_STR) {
            if (js_str_is(s, "use strict"))
                fi->strict = 1;
        } else {
            directives = 0;
        }
        push(&l, st);
    }
    prog->a = l.head;
    prog->u.scope = finish_scope(p, sb);
    fi->body = prog;
    *top = fi;
    return prog;
}

jnode *js_parse_function(js_rt *rt, const char *params, const char *body, char *err, unsigned errsize)
{
    unsigned pl = (unsigned)strlen(params), bl = (unsigned)strlen(body);
    char *src = js_alloc(rt, pl + bl + 64);
    ksnprintf(src, pl + bl + 64, "(function anonymous(%s\n) {\n%s\n})", params, body);
    struct jfuninfo *top;
    jnode *prog = js_parse(rt, src, (unsigned)strlen(src), err, errsize, &top);
    if (!prog)
        return 0;
    if (!prog->a || prog->a->type != N_EXPR || prog->a->next || prog->a->a->type != N_FUNC) {
        ksnprintf(err, errsize, "Zla tresc funkcji");
        return 0;
    }
    return prog->a->a;
}
