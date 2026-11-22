/* JavaScript - funkcje async i await.
 *
 * Interpreter liczy program rekurencyjnie (funkcja C wola funkcje C), wiec
 * "zatrzymanie funkcji w polowie" przy await nie jest proste: stan funkcji
 * siedzi na stosie C. Rozwiazanie: kazde wywolanie funkcji async dostaje
 * WLASNY stos (korutyna). Przy await zapamietujemy rejestry (js_setjmp),
 * wracamy na stos tego, kto nas uruchomil (js_longjmp), a gdy obietnica
 * sie spelni - skaczemy z powrotem w srodek funkcji. Stos korutyny caly
 * czas lezy w pamieci, wiec po powrocie wszystko jest tak, jak bylo.
 *
 * Wywolanie funkcji async:
 *   1. tworzymy obietnice P i korutyne, uruchamiamy cialo na nowym stosie,
 *   2. cialo dziala do pierwszego await (albo do konca),
 *   3. await: rejestrujemy .then(wznow, wznow-z-bledem) i usypiamy korutyne,
 *      funkcja zwraca P,
 *   4. mikrozadanie z .then budzi korutyne - liczy dalej do kolejnego await,
 *   5. koniec ciala: P spelniona (return) albo odrzucona (throw).
 *
 * Generatory (function*) dzialaja tak samo: yield usypia korutyne i oddaje
 * wartosc do next(), a nastepne next(v) ja budzi. Stos dostaja dopiero przy
 * pierwszym next() i jest mniejszy (rzadko jest w nich gleboka rekurencja). */

#include "js_int.h"
#include "string.h"
#include "heap.h"

#define U __attribute__((unused))
#define ARG(i) ((i) < argc ? argv[i] : jv_undef())

#define CORO_STACK   (1u << 20)          /* stos jednej korutyny (ok. 650 poziomow rekurencji) */
#define CORO_MARGIN  (128u << 10)        /* zapas na funkcje wbudowane (RangeError wczesniej) */
#define CORO_MAX     400                 /* ile funkcji async moze naraz czekac */
#define CORO_POOL    8                   /* tyle zwolnionych stosow trzymamy na pozniej */
#define GEN_STACK    (384u << 10)        /* stos generatora (ok. 200 poziomow rekurencji) */
#define GEN_MARGIN   (96u << 10)
#define GEN_MAX      1000                /* ile generatorow moze naraz czekac w polowie */

enum { CO_NEW, CO_RUNNING, CO_SUSPENDED, CO_DONE, CO_THREW, CO_ABORT };

struct js_coro {
    js_rt *rt;
    int status;
    struct js_jmp ctx;                   /* rejestry uspionej korutyny */
    struct js_jmp caller;                /* dokad wrocic przy await / na koncu */
    uint8_t *stack;
    /* wywolanie */
    jobj *fn;
    jv self;
    int argc;
    jv *argv;
    jobj *promise;
    jobj *holder;                        /* obiekt JS wskazujacy na korutyne (dane funkcji wznawiajacych) */
    /* stan silnika na czas uspienia */
    struct js_catch *catch_top;
    uint32_t line;
    int depth;                           /* glebokosc wywolan wewnatrz korutyny */
    int base;                            /* glebokosc u wznawiajacego */
    /* przekazywane przy wznowieniu */
    int kind;                            /* 1 = wartosc, 2 = blad (throw w miejscu await) */
    jv val;
    jv result;
    int is_gen;                          /* generator (a nie funkcja async) */
    int finished;                        /* generator skonczony - next() daje juz tylko done */
    jv out;                              /* wartosc z yield */
    jv retval;                           /* generator.return(v) */
    struct js_coro *prev_cur;
    struct js_coro *lprev, *lnext;       /* lista zywych korutyn (do sprzatania) */
};

/* ---------- stosy ---------- */

static uint8_t *stack_get(js_rt *rt, int gen)
{
    if (gen)
        return rt->gen_npool ? rt->gen_pool[--rt->gen_npool] : kmalloc(GEN_STACK);
    if (rt->coro_npool)
        return rt->coro_pool[--rt->coro_npool];
    return kmalloc(CORO_STACK);
}

static void stack_put(js_rt *rt, uint8_t *st, int gen)
{
    if (gen && rt->gen_npool < CORO_POOL)
        rt->gen_pool[rt->gen_npool++] = st;
    else if (!gen && rt->coro_npool < CORO_POOL)
        rt->coro_pool[rt->coro_npool++] = st;
    else
        kfree(st);
}

static void coro_link(js_rt *rt, struct js_coro *co)
{
    co->lnext = rt->coro_live;
    if (rt->coro_live)
        rt->coro_live->lprev = co;
    rt->coro_live = co;
    if (co->is_gen)
        rt->gen_count++;
    else
        rt->coro_count++;
}

static void coro_unlink(js_rt *rt, struct js_coro *co)
{
    if (co->lprev)
        co->lprev->lnext = co->lnext;
    else
        rt->coro_live = co->lnext;
    if (co->lnext)
        co->lnext->lprev = co->lprev;
    co->lprev = co->lnext = 0;
    if (co->is_gen)
        rt->gen_count--;
    else
        rt->coro_count--;
}

static void coro_release(js_rt *rt, struct js_coro *co)
{
    if (co->stack) {
        stack_put(rt, co->stack, co->is_gen);
        co->stack = 0;
        coro_unlink(rt, co);
    }
}

/* Swiat JS jest zamykany: oddajemy wszystkie stosy (takze uspionych korutyn) */
void js_coro_free_all(js_rt *rt)
{
    for (struct js_coro *co = rt->coro_live, *nx; co; co = nx) {
        nx = co->lnext;
        kfree(co->stack);
        co->stack = 0;
    }
    rt->coro_live = 0;
    rt->coro_count = rt->gen_count = 0;
    while (rt->coro_npool)
        kfree(rt->coro_pool[--rt->coro_npool]);
    while (rt->gen_npool)
        kfree(rt->gen_pool[--rt->gen_npool]);
}

/* ---------- przelaczanie ---------- */

/* Poczatek korutyny - juz na jej wlasnym stosie */
static void coro_entry(void *arg)
{
    struct js_coro *co = arg;
    js_rt *rt = co->rt;
    struct js_catch cat;
    cat.prev = 0;
    cat.depth = rt->depth;
    rt->catch_top = &cat;
    if (js_setjmp(&cat.jb) == 0) {
        co->result = js_call_body(rt, co->fn, co->self, co->argc, co->argv);
        co->status = CO_DONE;
    } else {
        co->result = rt->exc;
        co->status = rt->aborting ? CO_ABORT : CO_THREW;
    }
    js_longjmp(&co->caller, 1);          /* tu nie ma powrotu - wracamy do tego, kto wznowil */
}

/* Uruchamia/wznawia korutyne, az zasnie (await) albo sie skonczy. Zwraca jej stan. */
static int coro_resume(js_rt *rt, struct js_coro *co, int kind, jv val)
{
    struct js_catch *volatile saved_catch = rt->catch_top;
    volatile uintptr_t saved_limit = rt->stack_limit;
    volatile uint32_t saved_line = rt->line;
    volatile int saved_depth = rt->depth;
    jv saved_ret = rt->ret;
    jstr *volatile saved_label = rt->label;
    co->kind = kind;
    co->val = val;
    co->prev_cur = rt->cur_coro;
    rt->cur_coro = co;
    rt->stack_limit = (uintptr_t)co->stack + (co->is_gen ? GEN_MARGIN : CORO_MARGIN);
    co->base = saved_depth;
    rt->depth = saved_depth + co->depth;
    rt->chain_break = 0;
    if (js_setjmp(&co->caller) == 0) {
        if (co->status == CO_NEW) {
            co->status = CO_RUNNING;
            js_on_stack(coro_entry, co, co->stack + (co->is_gen ? GEN_STACK : CORO_STACK));
            /* coro_entry nigdy tu nie wraca (konczy sie skokiem do caller) */
        } else {
            co->status = CO_RUNNING;
            rt->catch_top = co->catch_top;
            rt->line = co->line;
            js_longjmp(&co->ctx, 1);
        }
    }
    /* korutyna zasnela albo sie skonczyla */
    rt->cur_coro = co->prev_cur;
    rt->catch_top = saved_catch;
    rt->stack_limit = saved_limit;
    rt->line = saved_line;
    rt->depth = saved_depth;
    rt->ret = saved_ret;
    rt->label = saved_label;
    rt->chain_break = 0;
    return co->status;
}

/* Wewnatrz korutyny: zasnij i oddaj sterowanie temu, kto nas wznowil */
static void coro_suspend(js_rt *rt, struct js_coro *co)
{
    co->catch_top = rt->catch_top;
    co->line = rt->line;
    co->depth = rt->depth - co->base;
    co->status = CO_SUSPENDED;
    if (js_setjmp(&co->ctx) == 0)
        js_longjmp(&co->caller, 1);
    /* obudzeni - coro_resume ustawil juz stan silnika */
}

/* Po powrocie z korutyny: rozliczenie obietnicy */
static void coro_after(js_rt *rt, struct js_coro *co, int st)
{
    switch (st) {
    case CO_SUSPENDED:
        return;
    case CO_DONE:
        coro_release(rt, co);
        js_promise_resolve(rt, co->promise, co->result);
        return;
    case CO_THREW:
        coro_release(rt, co);
        js_promise_reject(rt, co->promise, co->result);
        return;
    default:                             /* przerwanie (czas, pamiec) idzie dalej w gore */
        coro_release(rt, co);
        js_longjmp(&rt->catch_top->jb, 1);
    }
}

/* ---------- API dla interpretera ---------- */

jv js_async_call(js_rt *rt, jobj *f, jv self, int argc, jv *argv)
{
    jobj *p = js_new_promise(rt);
    if (rt->coro_count >= CORO_MAX) {
        js_promise_reject(rt, p, jv_obj(js_make_error(rt, JS_ERR_RANGE,
                          js_str_c(rt, "Za duzo funkcji async czeka naraz"))));
        return jv_obj(p);
    }
    struct js_coro *co = js_alloc(rt, sizeof(*co));
    memset(co, 0, sizeof(*co));
    co->stack = stack_get(rt, 0);
    if (!co->stack) {
        js_promise_reject(rt, p, jv_obj(js_make_error(rt, JS_ERR_RANGE,
                          js_str_c(rt, "Brak pamieci na funkcje async"))));
        return jv_obj(p);
    }
    co->rt = rt;
    co->status = CO_NEW;
    co->fn = f;
    co->self = self;
    co->argc = argc;
    co->argv = js_alloc(rt, sizeof(jv) * (size_t)(argc ? argc : 1));
    for (int i = 0; i < argc; i++)
        co->argv[i] = argv[i];
    co->promise = p;
    coro_link(rt, co);
    coro_after(rt, co, coro_resume(rt, co, 0, jv_undef()));
    return jv_obj(p);
}

/* Funkcja wznawiajaca (podpieta pod .then obietnicy z await). len: 1 = wartosc, 2 = blad */
static jv co_react(js_rt *rt, jv self U, int argc, jv *argv, jobj *fn)
{
    struct js_coro *co = fn->u.nf.data.u.o->u.co.co;
    if (co->status != CO_SUSPENDED)
        return jv_undef();
    coro_after(rt, co, coro_resume(rt, co, fn->u.nf.len, ARG(0)));
    return jv_undef();
}

jv js_await(js_rt *rt, jv v)
{
    struct js_coro *co = rt->cur_coro;
    if (!co)
        js_throw_error(rt, JS_ERR_SYNTAX, "await is only valid in async functions");
    jobj *p;
    if (v.t == JT_OBJ && v.u.o->cls == JC_PROMISE) {
        p = v.u.o;
    } else {
        p = js_new_promise(rt);
        js_promise_resolve(rt, p, v);
    }
    if (!co->holder) {
        co->holder = js_new_object(rt, 0);
        co->holder->u.co.co = co;
    }
    jobj *ok = js_new_native(rt, "", co_react, 1), *bad = js_new_native(rt, "", co_react, 1);
    ok->u.nf.data = bad->u.nf.data = jv_obj(co->holder);
    ok->u.nf.len = 1;
    bad->u.nf.len = 2;
    js_promise_then(rt, p, jv_obj(ok), jv_obj(bad));
    coro_suspend(rt, co);
    if (co->kind == 2)
        js_throw(rt, co->val);
    return co->val;
}

/* ---------- generatory (function*) ---------- */

jobj *js_gen_create(js_rt *rt, jobj *f, jv self, int argc, jv *argv)
{
    struct js_coro *co = js_alloc(rt, sizeof(*co));
    memset(co, 0, sizeof(*co));
    co->rt = rt;
    co->is_gen = 1;
    co->status = CO_NEW;
    co->fn = f;
    co->self = self;
    co->argc = argc;
    co->argv = js_alloc(rt, sizeof(jv) * (size_t)(argc ? argc : 1));
    for (int i = 0; i < argc; i++)
        co->argv[i] = argv[i];
    jobj *g = js_new_object(rt, rt->gen_proto);
    g->cls = JC_GEN;
    g->u.co.co = co;
    return g;
}

jv js_gen_resume(js_rt *rt, jobj *g, int kind, jv v, int *done)
{
    struct js_coro *co = g->u.co.co;
    *done = 1;
    if (co->status == CO_NEW && kind != 1)
        co->finished = 1;                        /* return/throw przed startem: koniec bez uruchamiania */
    if (co->finished) {
        if (kind == 2)
            js_throw(rt, v);
        return kind == 3 ? v : jv_undef();
    }
    if (co->status == CO_RUNNING)
        js_throw_error(rt, JS_ERR_TYPE, "Generator is already running");
    if (co->status == CO_NEW) {
        if (rt->gen_count >= GEN_MAX)
            js_throw_error(rt, JS_ERR_RANGE, "Za duzo niedokonczonych generatorow");
        co->stack = stack_get(rt, 1);
        if (!co->stack)
            js_throw_error(rt, JS_ERR_RANGE, "Brak pamieci na generator");
        coro_link(rt, co);
    }
    int st = coro_resume(rt, co, kind, v);
    switch (st) {
    case CO_SUSPENDED:
        *done = 0;
        return co->out;
    case CO_DONE:
        coro_release(rt, co);
        co->finished = 1;
        return co->result;
    case CO_THREW:
        coro_release(rt, co);
        co->finished = 1;
        if (js_is_gen_return(rt, co->result))    /* return() przeszedl przez wszystkie finally */
            return co->retval;
        js_throw(rt, co->result);
    default:
        coro_release(rt, co);
        co->finished = 1;
        js_longjmp(&rt->catch_top->jb, 1);
    }
}

/* yield v (delegate: yield* iterowalne) - wewnatrz generatora */
jv js_yield(js_rt *rt, jv v, int delegate)
{
    struct js_coro *co = rt->cur_coro;
    if (!co || !co->is_gen)
        js_throw_error(rt, JS_ERR_SYNTAX, "yield poza generatorem");
    if (delegate) {
        if (v.t == JT_OBJ && v.u.o->cls == JC_GEN) {   /* przekazujemy next/throw/return dalej */
            int kind = 1, done;
            jv sent = jv_undef();
            for (;;) {
                jv r = js_gen_resume(rt, v.u.o, kind, sent, &done);
                if (done) {
                    if (kind == 3) {                     /* return() szedl przez wewnetrzny - konczymy i my */
                        co->retval = r;
                        js_throw(rt, jv_obj(rt->gen_token));
                    }
                    return r;                            /* wartosc wyrazenia yield* */
                }
                co->out = r;
                coro_suspend(rt, co);
                kind = co->kind;
                sent = co->val;
            }
        }
        jobj *items = js_iterate_to_array(rt, v).u.o;  /* zwykla tablica, napis, Set... */
        for (uint32_t i = 0; i < items->u.arr.len; i++)
            js_yield(rt, items->u.arr.v[i], 0);
        return jv_undef();
    }
    co->out = v;
    coro_suspend(rt, co);
    if (co->kind == 2)
        js_throw(rt, co->val);
    if (co->kind == 3) {
        co->retval = co->val;
        js_throw(rt, jv_obj(rt->gen_token));
    }
    return co->val;
}

static jobj *this_gen(js_rt *rt, jv self)
{
    if (self.t != JT_OBJ || self.u.o->cls != JC_GEN)
        js_throw_error(rt, JS_ERR_TYPE, "next method called on incompatible receiver");
    return self.u.o;
}

static jv gen_step(js_rt *rt, jv self, int kind, jv v)
{
    int done;
    jv r = js_gen_resume(rt, this_gen(rt, self), kind, v, &done);
    jobj *o = js_new_object(rt, rt->object_proto);
    js_set(rt, jv_obj(o), rt->A.value, r);
    js_set(rt, jv_obj(o), rt->A.done, jv_bool(done));
    return jv_obj(o);
}

static jv gen_next(js_rt *rt, jv self, int argc, jv *argv, jobj *fn U) { return gen_step(rt, self, 1, ARG(0)); }
static jv gen_throw(js_rt *rt, jv self, int argc, jv *argv, jobj *fn U) { return gen_step(rt, self, 2, ARG(0)); }
static jv gen_return(js_rt *rt, jv self, int argc, jv *argv, jobj *fn U) { return gen_step(rt, self, 3, ARG(0)); }

void js_gen_init(js_rt *rt)
{
    rt->gen_token = js_new_object(rt, 0);
    rt->gen_proto = js_new_object(rt, rt->iterator_proto);
    js_def_fn(rt, rt->gen_proto, "next", gen_next, 1);
    js_def_fn(rt, rt->gen_proto, "return", gen_return, 1);
    js_def_fn(rt, rt->gen_proto, "throw", gen_throw, 1);
}
