#ifndef JS_INT_H
#define JS_INT_H

/* Wnetrze silnika JavaScript - wspolne dla src/js_*.c (patrz js.h).
 *
 * Droga programu:
 *   tekst --js_lex.c--> tokeny --js_parse.c--> drzewo (AST) --js_eval.c--> dzialanie
 *
 * Wartosc JS (jv) to 16 bajtow: rodzaj (liczba, tekst, obiekt...) i sama
 * wartosc. Teksty trzymamy w UTF-16, jak kaze JavaScript ("ą".length == 1).
 * Nazwy wlasciwosci sa "atomami": kazda nazwa istnieje w pamieci tylko raz,
 * wiec porownanie nazw to porownanie wskaznikow.
 *
 * Pamiec: wszystko z areny swiata JS (bez odsmiecania) - zwalniana razem
 * ze strona. Wyjatki (throw) to skok js_longjmp do najblizszego try. */

#include <stdint.h>
#include <stddef.h>
#include "arena.h"
#include "js.h"

typedef struct jstr jstr;
typedef struct jobj jobj;
typedef struct jnode jnode;
typedef struct jenv jenv;
typedef struct js_rt js_rt;

/* ---------- wartosci ---------- */

enum { JT_UNDEF, JT_NULL, JT_BOOL, JT_NUM, JT_STR, JT_OBJ, JT_ACC /* tylko w jprop: getter/setter */ };

typedef struct {
    uint8_t t;
    union {
        double n;
        int b;
        jstr *s;
        jobj *o;
        struct jacc *acc;
    } u;
} jv;

static inline jv jv_num(double d) { jv v; v.t = JT_NUM; v.u.n = d; return v; }
static inline jv jv_bool(int b)   { jv v; v.t = JT_BOOL; v.u.n = 0; v.u.b = b != 0; return v; }
static inline jv jv_str(jstr *s)  { jv v; v.t = JT_STR; v.u.s = s; return v; }
static inline jv jv_obj(jobj *o)  { jv v; v.t = JT_OBJ; v.u.o = o; return v; }
static inline jv jv_undef(void)   { jv v; v.t = JT_UNDEF; v.u.o = 0; return v; }
static inline jv jv_null(void)    { jv v; v.t = JT_NULL; v.u.o = 0; return v; }
#define JV_UNDEF jv_undef()
#define IS_OBJ(v) ((v).t == JT_OBJ)
#define IS_NULLISH(v) ((v).t <= JT_NULL)

/* Tekst UTF-16. Do bufora "buf" mozna dopisywac za koncem (a + b bez
 * kopiowania a, gdy a konczy sie tam, gdzie konczy sie zajeta czesc) */
struct jsbuf {
    uint32_t cap, used;
    uint16_t data[];
};

struct jstr {
    uint16_t *c;
    uint32_t len;
    uint32_t hash;                   /* 0 = nie liczony */
    struct jsbuf *buf;
    jstr *next;                      /* lancuch w tablicy atomow */
    uint8_t atom;
    uint8_t kw;                      /* slowo kluczowe (K_*), 0 = zwykla nazwa */
};

/* ---------- obiekty ---------- */

enum {
    JC_OBJECT, JC_ARRAY, JC_FUNC, JC_NATIVE, JC_BOUND, JC_ERROR, JC_BOOL, JC_NUM, JC_STR,
    JC_DATE, JC_REGEXP, JC_MAP, JC_SET, JC_ARGS, JC_NODE, JC_STYLE, JC_CLASSLIST, JC_DATASET,
    JC_EVENT, JC_PROMISE, JC_CANVAS2D,
};

#define PF_ENUM   1
#define PF_WRITE  2
#define PF_CONF   4
#define PF_ALL    7

struct jprop {
    jstr *key;                       /* NULL = usunieta */
    jv v;                            /* JT_ACC: getter/setter */
    uint32_t flags;
};

struct jacc {
    jobj *get, *set;
};

typedef jv (*jnative)(js_rt *rt, jv self, int argc, jv *argv, jobj *fn);

struct jlistener;
struct jmapent;
struct jregex;
struct dom_node;
struct c2d;

#define OF_NOEXT      1              /* Object.preventExtensions / freeze */
#define OF_FROZEN     2
#define OF_CLASS      4              /* konstruktor klasy - tylko przez new */
#define OF_DERIVED    8              /* klasa z extends */
#define OF_NOCTOR    16              /* strzalka, metoda - nie konstruktor */

struct jobj {
    uint8_t cls;
    uint8_t flags;
    uint32_t nprops, cap;
    jobj *proto;
    struct jprop *props;
    uint32_t *index;                 /* tablica mieszajaca nazw (przy wielu wlasciwosciach) */
    uint32_t index_cap;
    union {
        struct { jv *v; uint32_t len, cap; } arr;                 /* JC_ARRAY, JC_ARGS */
        struct { jnode *node; jenv *env; jobj *home; struct jfields *fields; } f;   /* JC_FUNC */
        struct { jnative fn, ctor; jv data; jstr *name; int16_t len; } nf;    /* JC_NATIVE */
        struct { jobj *target; jv self; jv *args; int nargs; } bf; /* JC_BOUND */
        jv prim;                                                   /* JC_BOOL/NUM/STR/DATE */
        struct { struct jregex *re; jstr *source; uint32_t flags; } re;
        struct { struct jmapent *e; uint32_t n, cap, live; uint32_t *idx; uint32_t icap; } map;  /* JC_MAP, JC_SET */
        struct {
            struct dom_node *node;
            struct jlistener *listeners;
            jobj *style, *classlist, *dataset;
        } dom;                                                     /* JC_NODE i pomocnicze */
        struct { uint8_t stop, stop_now, prevented, bubbles; jobj *target; } ev;
        struct { int state; jv value; struct jreaction *reactions, *last; int handled; } pr;   /* JC_PROMISE */
        struct { struct dom_node *node; struct c2d *c; } cv;          /* JC_CANVAS2D (js_canvas.c) */
    } u;
};

/* Pola klasy (x = 1;) - ustawiane w kazdym nowym obiekcie */
struct jfields {
    int n;
    jstr **keys;
    jobj **inits;                    /* funkcja liczaca wartosc (albo NULL) */
};

struct jlistener {
    jstr *type;
    jv fn;
    uint8_t capture, once, removed;
    struct jlistener *next;
};

struct jmapent {
    jv key, value;
    uint8_t deleted;
};

/* ---------- srodowiska (zasiegi zmiennych) ---------- */

#define BF_CONST 1
#define BF_TDZ   2                   /* let/const przed deklaracja */
#define BF_LEX   4

struct jbind {
    jstr *name;
    jv v;
    uint32_t flags;
};

enum { ENV_BLOCK, ENV_FUNC, ENV_GLOBAL };

struct jenv {
    jenv *parent;
    struct jbind *b;
    uint32_t n, cap;
    uint8_t kind;
    uint8_t has_this;                /* funkcja (nie strzalka) albo zasieg globalny */
    uint8_t this_init;               /* konstruktor klasy pochodnej: this dopiero po super() */
    jv this_val;
    jobj *func;                      /* wywolywana funkcja (super, new.target) */
    jobj *new_target;
};

/* ---------- drzewo programu (AST) ---------- */

enum {
    /* wyrazenia */
    N_NUM, N_STR, N_TEMPLATE, N_TAGGED, N_REGEX, N_IDENT, N_THIS, N_SUPER, N_NULL, N_TRUE,
    N_FALSE, N_ARRAY, N_OBJECT, N_PROP, N_FUNC, N_CLASS, N_UNARY, N_UPDATE, N_BINARY,
    N_LOGICAL, N_ASSIGN, N_COND, N_CALL, N_NEW, N_MEMBER, N_INDEX, N_OPTCHAIN, N_SEQ,
    N_SPREAD, N_HOLE, N_NEWTARGET,
    /* wzorce (destrukturyzacja) */
    N_ARRPAT, N_OBJPAT, N_DEFAULT, N_REST,
    /* instrukcje */
    N_VAR, N_DECL, N_EXPR, N_BLOCK, N_IF, N_FOR, N_FORIN, N_FOROF, N_WHILE, N_DOWHILE,
    N_RETURN, N_BREAK, N_CONTINUE, N_THROW, N_TRY, N_SWITCH, N_CASE, N_LABEL, N_FUNCDECL,
    N_CLASSDECL, N_EMPTY, N_DEBUGGER,
};

/* Rodzaje wlasciwosci w literale obiektu / klasie (jnode.op dla N_PROP) */
enum { PK_INIT, PK_GET, PK_SET, PK_SPREAD, PK_METHOD, PK_FIELD };

/* jnode.flags */
#define NF_COMPUTED  1               /* [klucz] */
#define NF_SHORTHAND 2               /* { x } */
#define NF_STATIC    4
#define NF_OPTIONAL  8               /* a?.b */
#define NF_PREFIX   16               /* ++a */
#define NF_CLOSURE  32               /* w petli tworzone sa funkcje - kazdy obrot dostaje nowe zmienne */
#define NF_PAREN    64               /* wyrazenie w nawiasie */

/* Rodzaje deklaracji (jnode.op dla N_VAR) */
enum { VK_VAR, VK_LET, VK_CONST };

/* Zmienne let/const/class/function zadeklarowane bezposrednio w bloku */
struct jscope {
    jstr **names;
    uint8_t *kinds;                  /* VK_* (function = VK_LET) */
    int n;
    jnode **funcs;                   /* deklaracje funkcji w bloku */
    int nfuncs;
};

struct jfuninfo {
    jstr *name;
    jnode *params;                   /* lista wzorcow */
    jnode *body;                     /* N_BLOCK albo wyrazenie (strzalka) */
    jstr **vars;                     /* var i funkcje z calej funkcji */
    int nvars, vars_cap;
    jnode **funcs;                   /* deklaracje funkcji na gornym poziomie */
    int nfuncs, funcs_cap;
    int nparams;                     /* f.length */
    uint8_t arrow, expr_body, strict, uses_args, simple_params, method, getter, is_class_ctor;
    const char *src;                 /* tekst funkcji (toString) */
    uint32_t src_len;
};

struct jregex_lit {
    jstr *source, *flags;
};

struct jnode {
    uint8_t type;
    uint8_t op;
    uint16_t flags;
    uint32_t line;
    jnode *a, *b, *c, *d;
    jnode *next;
    union {
        double num;
        jstr *str;
        struct jfuninfo *fn;
        struct jregex_lit *re;
        struct jscope *scope;
    } u;
    jstr *label;                     /* petle: etykieta (dla break/continue etykieta) */
};

/* Operatory (tokeny wielu znakow; pojedyncze znaki to ich kody ASCII) */
enum {
    OP_EQ = 256, OP_NE, OP_SEQ, OP_SNE, OP_LE, OP_GE, OP_AND, OP_OR, OP_NULLISH, OP_INC, OP_DEC,
    OP_SHL, OP_SHR, OP_USHR, OP_POW, OP_ARROW, OP_ELLIPSIS, OP_OPTCHAIN,
    OP_ADD_A, OP_SUB_A, OP_MUL_A, OP_DIV_A, OP_MOD_A, OP_SHL_A, OP_SHR_A, OP_USHR_A,
    OP_AND_A, OP_OR_A, OP_XOR_A, OP_POW_A, OP_LAND_A, OP_LOR_A, OP_NULLISH_A,
    OP_TYPEOF, OP_VOID, OP_DELETE, OP_IN, OP_INSTANCEOF, OP_NEG, OP_PLUS,
};

/* jnode.op jest 8-bitowy - operatory zapisujemy jako numer od OP_EQ */
#define OPC(op) ((op) >= 256 ? (op) - 256 + 128 : (op))
#define OPX(c)  ((c) >= 128 ? (c) - 128 + 256 : (c))

/* ---------- wyjatki: wlasne setjmp/longjmp (js_asm.S) ---------- */

struct js_jmp {
    uint64_t r[8];                   /* rbx, rbp, r12-r15, rsp, rip */
};
int  js_setjmp(struct js_jmp *j) __attribute__((returns_twice, sysv_abi));
void js_longjmp(struct js_jmp *j, int v) __attribute__((noreturn, sysv_abi));
/* Wywoluje fn(arg) na innym stosie */
void js_on_stack(void (*fn)(void *), void *arg, void *stack_top) __attribute__((sysv_abi));

struct js_catch {
    struct js_jmp jb;
    struct js_catch *prev;
    int depth;
};

/* ---------- atomy uzywane przez silnik ---------- */

#define JS_ATOMS(X) \
    X(length) X(prototype) X(constructor) X(name) X(message) X(toString) X(valueOf) \
    X(__proto__) X(callee) X(arguments) X(undefined) X(value) X(get) X(set) X(writable) \
    X(enumerable) X(configurable) X(index) X(input) X(lastIndex) X(source) X(flags) \
    X(global) X(ignoreCase) X(multiline) X(toJSON) X(stack) X(then) X(next) X(done) \
    X(join) X(cause) X(type) X(target) X(currentTarget) X(handleEvent) X(key) X(code) \
    X(keyCode) X(which) X(clientX) X(clientY) X(pageX) X(pageY) X(button) X(bubbles) \
    X(timeStamp) X(defaultPrevented) X(document) X(window) X(self) X(style) X(detail) \
    X(let) X(async) X(of) X(await) X(yield) X(eval) X(groups) X(raw) X(anonymous) \
    X(error) X(reason) X(status) X(once) X(capture) X(passive) X(ctrlKey) \
    X(shiftKey) X(altKey) X(metaKey) X(repeat) X(cssText) X(className) X(id) X(src) \
    X(href) X(onclick) X(innerHTML) X(textContent) X(localStorage) X(sessionStorage) \
    X(dotAll) X(sticky) X(unicode) X(hasIndices) X(size)

struct js_atoms {
#define JS_ATOM_FIELD(n) jstr *n;
    JS_ATOMS(JS_ATOM_FIELD)
#undef JS_ATOM_FIELD
};

/* ---------- swiat JS jednej strony ---------- */

#define JS_ATOM_HASH 8192
#define JS_TIMERS_MAX 256

struct js_timer {
    int id;
    int interval;                    /* 0 = jednorazowy */
    int raf;                         /* requestAnimationFrame */
    uint32_t due;                    /* ms od startu systemu */
    jv fn;
    jv *args;
    int nargs;
    const char *code;                /* setTimeout("tekst") */
};

struct jreaction;

/* Zapytanie sieciowe skryptu (js_net.c) czekajace na przegladarke albo na odpowiedz */
#define JS_NET_MAX 64
struct js_netreq {
    int id;
    uint8_t state;                   /* 0 wolne, 1 czeka na wyslanie, 2 w drodze */
    uint8_t xhr;                     /* 1 = XMLHttpRequest, 0 = fetch */
    jobj *target;                    /* Promise (fetch) albo obiekt XMLHttpRequest */
    char method[12];
    char *url, *headers, *body;      /* w arenie swiata JS */
    unsigned body_len;
};

/* Mikrozadanie (Promise.then) - wykonywane zaraz po biezacym skrypcie */
struct jmicro {
    void (*run)(js_rt *rt, struct jmicro *m);
    int kind;
    jv a, b;
    jobj *p;
    struct jmicro *next;
};

struct js_rt {
    struct arena mem;                /* cala pamiec swiata JS */
    size_t mem_limit;
    struct js_catch *catch_top;
    jv exc;                          /* rzucana wartosc */
    int aborting;                    /* przerwanie (czas, pamiec) - catch go nie lapie */
    const char *abort_msg;
    uint32_t line;                   /* biezaca linia (komunikaty bledow) */
    const char *script_name;
    int depth;                       /* glebokosc wywolan */
    uintptr_t stack_limit;           /* ponizej tego adresu stos sie konczy */
    uint8_t *stack;                  /* wlasny stos silnika */
    int on_stack;                    /* juz dzialamy na wlasnym stosie */
    int chain_break;                 /* a?.b trafilo na null - reszta lancucha to undefined */
    int dead;                        /* zabraklo pamieci - swiat JS juz nie dziala */
    uint32_t exc_line;               /* linia, w ktorej rzucono wyjatek */
    jv completion;                   /* wartosc ostatniego wyrazenia (terminal) */
    uint32_t last_yield;
    /* zliczanie krokow: co jakis czas oddajemy procesor i sprawdzamy czas */
    uint32_t steps, next_check;
    uint32_t run_start, budget_ms;
    volatile int *cancel;
    /* wynik instrukcji return / etykieta break i continue */
    jv ret;
    jstr *label;
    /* atomy */
    jstr *atoms[JS_ATOM_HASH];
    struct js_atoms A;
    jstr *empty;
    jstr *char_str[128];             /* teksty jednoznakowe */
    jstr *int_str[256];              /* "0".."255" */
    /* obiekty wbudowane */
    jobj *global;
    jenv *genv;
    jobj *object_proto, *function_proto, *array_proto, *string_proto, *number_proto,
         *boolean_proto, *error_proto, *date_proto, *regexp_proto, *map_proto, *set_proto,
         *promise_proto, *iterator_proto;
    jobj *error_ctors[8];            /* Error, TypeError, ... (JS_ERR_*) */
    jobj *error_protos[8];
    jobj *array_ctor, *object_ctor, *promise_ctor;
    jobj *throw_type_error;
    /* DOM i przegladarka */
    struct dom_doc *doc;
    struct css_ctx *css;
    struct js_host host;
    char url[600];
    jobj *document, *node_proto, *element_proto, *text_proto, *document_proto, *event_proto,
         *style_proto, *classlist_proto, *nodelist_proto, *canvas_proto, *ctx2d_proto;
    struct jlistener *win_listeners;
    struct dom_node *active;         /* element z fokusem (document.activeElement) */
    jstr *cookie;                    /* document.cookie (tylko w pamieci strony) */
    char script_name_buf[80];
    int dirty;
    int ready_state;                 /* 0 loading, 1 interactive, 2 complete */
    struct dom_node *cur_script;     /* skrypt, ktory wlasnie dziala (document.write) */
    struct dom_node *write_after;
    int wants_keys;
    jobj *storage;                   /* localStorage (w pamieci strony) */
    /* timery */
    struct js_timer timers[JS_TIMERS_MAX];
    int ntimers, next_timer_id;
    /* Promise: kolejka mikrozadan */
    struct jmicro *micro_head, *micro_tail;
    /* fetch / XMLHttpRequest */
    struct js_netreq net[JS_NET_MAX];
    int net_next_id;
    jobj *xhr_proto, *response_proto, *headers_proto;
    /* selektory CSS (querySelector) - pamiec podreczna */
    struct { jstr *text; void *sel; } sel_cache[64];
    int sel_next;
    /* komunikat ostatniego bledu */
    char last_error[300];
    uint64_t rand_state[2];
    jobj *join_stack[32];            /* [1,[2]].join - ochrona przed petla a = [a] */
    int join_depth;
    int64_t clock_base_ms;           /* Date.now() przy starcie swiata (UTC) */
    uint32_t clock_base_uptime;
    int symbol_count;
    jstr *sym_iterator;              /* Symbol.iterator (u nas tekst "@@iterator") */
    struct { jstr *label; uint32_t start; } console_timers[16];
};

/* ---------- js_eval.c: rdzen ---------- */

void *js_alloc(js_rt *rt, size_t n);
jv   js_eval(js_rt *rt, jnode *n, jenv *env);
jv   js_call(js_rt *rt, jv fn, jv self, int argc, jv *argv);
jv   js_construct(js_rt *rt, jv fn, int argc, jv *argv, jobj *new_target);
int  js_is_callable(jv v);
int  js_is_ctor(jv v);
void js_throw(js_rt *rt, jv v) __attribute__((noreturn));
enum { JS_ERR_ERROR, JS_ERR_TYPE, JS_ERR_RANGE, JS_ERR_SYNTAX, JS_ERR_REFERENCE, JS_ERR_EVAL, JS_ERR_URI };
void js_throw_error(js_rt *rt, int kind, const char *fmt, ...) __attribute__((noreturn, format(printf, 3, 4)));
jobj *js_make_error(js_rt *rt, int kind, jstr *msg);
void js_abort(js_rt *rt, const char *msg) __attribute__((noreturn));
void js_check_time(js_rt *rt);
static inline void js_step(js_rt *rt) { if (++rt->steps >= rt->next_check) js_check_time(rt); }
void js_check_stack(js_rt *rt);

/* Uruchomienie kawalka programu w chronionym otoczeniu (wlasny stos + catch).
 * 0 = OK, -1 = niezlapany wyjatek (opis w rt->last_error) */
int  js_protect(js_rt *rt, void (*fn)(js_rt *rt, void *arg), void *arg);
void js_report_exception(js_rt *rt);
void js_run_microtasks(js_rt *rt);

/* obiekty */
jobj *js_new_object(js_rt *rt, jobj *proto);
jobj *js_new_array(js_rt *rt, uint32_t len);
jobj *js_new_native(js_rt *rt, const char *name, jnative fn, int len);
jobj *js_new_func(js_rt *rt, jnode *fn, jenv *env);
void js_array_push(js_rt *rt, jobj *a, jv v);
void js_array_reserve(js_rt *rt, jobj *a, uint32_t n);
jv   js_get(js_rt *rt, jv obj, jstr *key);
jv   js_get_index(js_rt *rt, jv obj, jv key);
void js_set(js_rt *rt, jv obj, jstr *key, jv v);
void js_set_index(js_rt *rt, jv obj, jv key, jv v);
struct jprop *js_own(jobj *o, jstr *key);
struct jprop *js_find(jobj *o, jstr *key);           /* z lancuchem prototypow */
int  js_has(js_rt *rt, jobj *o, jstr *key);
int  js_has_own(js_rt *rt, jobj *o, jstr *key);
int  js_delete(js_rt *rt, jobj *o, jstr *key);
void js_define(js_rt *rt, jobj *o, jstr *key, jv v, uint32_t flags);
void js_define_acc(js_rt *rt, jobj *o, jstr *key, jobj *get, jobj *set, uint32_t flags);
void js_def_fn(js_rt *rt, jobj *o, const char *name, jnative fn, int len);
void js_def_val(js_rt *rt, jobj *o, const char *name, jv v);
void js_def_getset(js_rt *rt, jobj *o, const char *name, jnative get, jnative set);
jstr *js_key_of(js_rt *rt, jv key);                  /* ToPropertyKey -> atom */
int  js_array_index(jstr *key, uint32_t *idx);
jobj *js_own_keys(js_rt *rt, jobj *o, int only_enum);  /* tablica nazw */

/* konwersje */
double js_to_num(js_rt *rt, jv v);
jstr  *js_to_str(js_rt *rt, jv v);
int    js_truthy(jv v);
jv     js_to_prim(js_rt *rt, jv v, int hint);        /* 0 default, 1 number, 2 string */
jobj  *js_to_obj(js_rt *rt, jv v);
int32_t  js_to_int32(js_rt *rt, jv v);
uint32_t js_to_uint32(js_rt *rt, jv v);
double js_to_integer(js_rt *rt, jv v);
int    js_strict_eq(jv a, jv b);
int    js_loose_eq(js_rt *rt, jv a, jv b);
int    js_same_value_zero(jv a, jv b);
const char *js_typeof(jv v);
jstr  *js_func_name(js_rt *rt, jobj *f);

/* ---------- teksty i atomy (js_eval.c) ---------- */

jstr *js_str_new(js_rt *rt, uint32_t len);            /* c[] do wypelnienia */
jstr *js_str_utf16(js_rt *rt, const uint16_t *c, uint32_t len);
jstr *js_str_utf8(js_rt *rt, const char *s, uint32_t len);
jstr *js_str_c(js_rt *rt, const char *s);
jstr *js_concat(js_rt *rt, jstr *a, jstr *b);
jstr *js_substr(js_rt *rt, jstr *s, uint32_t from, uint32_t to);
jstr *js_intern(js_rt *rt, jstr *s);
jstr *js_atom(js_rt *rt, const char *s);
int   js_str_eq(const jstr *a, const jstr *b);
int   js_str_cmp(const jstr *a, const jstr *b);
int   js_str_is(const jstr *s, const char *ascii);
/* UTF-8 do bufora (zawsze konczy '\0'). Zwraca dlugosc */
unsigned js_utf8(const jstr *s, char *out, unsigned size);
char *js_utf8_alloc(js_rt *rt, const jstr *s, unsigned *len);   /* w arenie */
uint16_t js_lower(uint16_t c);
uint16_t js_upper(uint16_t c);
int js_is_space(uint16_t c);

/* Budowanie tekstu kawalkami */
struct jsb {
    js_rt *rt;
    uint16_t *c;
    uint32_t len, cap;
};
void jsb_init(struct jsb *b, js_rt *rt);
void jsb_char(struct jsb *b, uint16_t c);
void jsb_ascii(struct jsb *b, const char *s);
void jsb_utf8(struct jsb *b, const char *s, unsigned len);
void jsb_str(struct jsb *b, const jstr *s);
void jsb_num(struct jsb *b, double d);
jstr *jsb_done(struct jsb *b);

/* ---------- js_num.c: liczby ---------- */

/* Liczba -> tekst jak w JS (najkrotszy zapis, ktory da te sama liczbe) */
int    js_dtoa(double d, char *out);                  /* out >= 32 bajty */
double js_strtod(const char *s, const char **end);    /* tekst ASCII -> liczba */
double js_str_to_num(const jstr *s);                  /* Number("..."), NaN gdy zle */
int    js_fixed(double d, int digits, char *out, unsigned size);   /* toFixed */
int    js_precision(double d, int p, char *out, unsigned size);    /* toPrecision */
int    js_exponential(double d, int digits, char *out, unsigned size);
int    js_radix(double d, int radix, char *out, unsigned size);
extern const double js_nan, js_inf;
static inline int js_isnan(double d) { return d != d; }
static inline int js_isinf(double d) { return d == js_inf || d == -js_inf; }
static inline int js_isfinite(double d) { return !js_isnan(d) && !js_isinf(d); }
double js_floor(double x);
double js_ceil(double x);
double js_trunc(double x);
double js_round(double x);
double js_fabs(double x);
double js_sqrt(double x);
double js_fmod(double x, double y);
double js_sin(double x);
double js_cos(double x);
double js_tan(double x);
double js_atan(double x);
double js_atan2(double y, double x);
double js_asin(double x);
double js_acos(double x);
double js_exp(double x);
double js_log(double x);
double js_pow(double x, double y);
double js_cbrt(double x);
double js_ldexp(double x, int e);
double js_expm1(double x);
double js_log1p(double x);

/* ---------- js_lex.c / js_parse.c ---------- */

/* Slowa kluczowe (jstr.kw). Do K_ENUM - zastrzezone (nie moga byc nazwami zmiennych) */
enum {
    K_NONE, K_BREAK, K_CASE, K_CATCH, K_CLASS, K_CONST, K_CONTINUE, K_DEBUGGER, K_DEFAULT,
    K_DELETE, K_DO, K_ELSE, K_EXPORT, K_EXTENDS, K_FINALLY, K_FOR, K_FUNCTION, K_IF, K_IMPORT,
    K_IN, K_INSTANCEOF, K_NEW, K_RETURN, K_SUPER, K_SWITCH, K_THIS, K_THROW, K_TRY, K_TYPEOF,
    K_VAR, K_VOID, K_WHILE, K_WITH, K_NULL, K_TRUE, K_FALSE, K_ENUM,
    K_LET, K_STATIC, K_YIELD, K_AWAIT, K_ASYNC, K_GET, K_SET, K_OF, K_ARGUMENTS, K_COUNT,
};
void js_parse_init(js_rt *rt);                        /* oznacza atomy slow kluczowych */

/* Rozbior programu. NULL = blad skladni (komunikat w err) */
jnode *js_parse(js_rt *rt, const char *src, unsigned len, char *err, unsigned errsize,
                struct jfuninfo **top);
/* new Function("a", "b", "return a+b") */
jnode *js_parse_function(js_rt *rt, const char *params, const char *body, char *err, unsigned errsize);

/* ---------- js_lib.c: obiekty wbudowane ---------- */

void js_lib_init(js_rt *rt);
jv   js_array_join(js_rt *rt, jobj *a, jstr *sep);
jstr *js_json_stringify(js_rt *rt, jv v, jv indent);
jv   js_date_now_ms(void);
jobj *js_new_promise(js_rt *rt);
void js_promise_resolve(js_rt *rt, jobj *p, jv v);
void js_promise_reject(js_rt *rt, jobj *p, jv v);
jv   js_iterate_to_array(js_rt *rt, jv v);           /* for-of, spread: tablica elementow */
jstr *js_error_text(js_rt *rt, jv exc);               /* "TypeError: ..." */
jstr *js_inspect(js_rt *rt, jv v, int depth);        /* console.log */

/* Wywolanie z przechwyceniem wyjatku: 0 = OK (*out = wynik), 1 = wyjatek (*out = wartosc) */
int  js_try_call(js_rt *rt, jv fn, jv self, int argc, jv *argv, jv *out);

/* js_eval.c: program na poziomie globalnym (wartosc ostatniego wyrazenia) */
jv   js_run_program(js_rt *rt, const char *code, unsigned len);
jobj *js_function_from_text(js_rt *rt, const char *params, const char *body);

/* ---------- js_regex.c ---------- */

jv   js_regexp_new(js_rt *rt, jstr *source, jstr *flags);

struct jregex *js_regex_compile(js_rt *rt, const jstr *src, const jstr *flags, char *err, unsigned errsize);
/* Szuka od pozycji start. 1 = znaleziono; caps[2*i], caps[2*i+1] = poczatek i koniec grupy i (-1 = brak) */
int js_regex_exec(js_rt *rt, struct jregex *re, const jstr *s, uint32_t start, int *caps);
int js_regex_ngroups(struct jregex *re);
jstr *js_regex_group_name(struct jregex *re, int i);
void js_regex_lib_init(js_rt *rt);

/* ---------- js_dom.c ---------- */

void js_dom_init(js_rt *rt);
/* Obiekty specjalne (element.style, element.dataset): 1 = obsluzone */
int  js_dom_get_hook(js_rt *rt, jobj *o, jstr *key, jv *out);
int  js_dom_set_hook(js_rt *rt, jobj *o, jstr *key, jv v);
int  js_dom_has_hook(js_rt *rt, jobj *o, jstr *key);
void js_dom_keys_hook(js_rt *rt, jobj *o, jobj *out);
jv   js_node_wrap(js_rt *rt, struct dom_node *n);

/* ---------- js_canvas.c ---------- */

/* <canvas>: HTMLCanvasElement.prototype i CanvasRenderingContext2D (po js_dom_init) */
void js_canvas_init(js_rt *rt);

/* ---------- js_net.c ---------- */

/* fetch, Response, Headers, XMLHttpRequest */
void js_net_init(js_rt *rt);
int  js_net_pending(js_rt *rt);                     /* zapytania jeszcze bez odpowiedzi */

#endif
