/* Interpreter JavaScriptu - "chodzi" po drzewie programu (AST z js_parse.c).
 *
 * js_eval liczy wartosc wyrazenia (np. a + b * 2), exec wykonuje
 * instrukcje (if, for, return...). Instrukcja konczy sie "normalnie" albo
 * skokiem: break, continue, return - wtedy petla/funkcja wyzej wie, co
 * zrobic. Wyjatek (throw) to dalszy skok - js_longjmp do najblizszego try.
 *
 * Zmienne mieszkaja w "srodowiskach": kazde wywolanie funkcji i blok z
 * let/const ma swoje, z wskaznikiem na srodowisko wyzej. Funkcja pamieta
 * srodowisko, w ktorym powstala (domkniecie), wiec widzi zmienne z niego
 * nawet po powrocie z funkcji, ktora je stworzyla. */

#include "js_int.h"
#include "string.h"
#include "heap.h"
#include "timer.h"

enum { CN, CB, CC, CR };             /* normalnie, break, continue, return */
enum { B_ASSIGN, B_INIT, B_DECLARE };

#define DEPTH_MAX 10000
#define JS_STACK_SIZE (8u << 20)

static int exec(js_rt *rt, jnode *s, jenv *env);
static int exec_list(js_rt *rt, jnode *s, jenv *env);
static int exec_block(js_rt *rt, jnode *b, jenv *env);
static void bind_pattern(js_rt *rt, jnode *pat, jv v, jenv *env, int mode);
static jv eval_class(js_rt *rt, jnode *n, jenv *env);

/* ---------- srodowiska ---------- */

static jenv *new_env(js_rt *rt, jenv *parent, int kind, uint32_t cap)
{
    if (cap < 2)
        cap = 2;
    jenv *e = js_alloc(rt, sizeof(jenv) + sizeof(struct jbind) * cap);
    e->b = (struct jbind *)(e + 1);
    e->cap = cap;
    e->parent = parent;
    e->kind = (uint8_t)kind;
    return e;
}

static inline struct jbind *env_find(jenv *e, jstr *name)
{
    struct jbind *b = e->b;
    for (uint32_t i = 0, n = e->n; i < n; i++)
        if (b[i].name == name)
            return &b[i];
    return 0;
}

static struct jbind *env_add(js_rt *rt, jenv *e, jstr *name, jv v, uint32_t flags)
{
    if (e->n == e->cap) {
        uint32_t cap = e->cap * 2;
        struct jbind *nb = js_alloc(rt, sizeof(struct jbind) * cap);
        memcpy(nb, e->b, sizeof(struct jbind) * e->n);
        e->b = nb;
        e->cap = cap;
    }
    struct jbind *b = &e->b[e->n++];
    b->name = name;
    b->v = v;
    b->flags = flags;
    return b;
}

static const char *name8(js_rt *rt, jstr *s)
{
    unsigned n;
    char *t = js_utf8_alloc(rt, s, &n);
    if (n > 60)
        t[60] = '\0';
    return t;
}

static jv lookup(js_rt *rt, jenv *env, jstr *name, int *found)
{
    for (jenv *e = env; e; e = e->parent) {
        struct jbind *b = env_find(e, name);
        if (b) {
            if (b->flags & BF_TDZ)
                js_throw_error(rt, JS_ERR_REFERENCE, "Cannot access '%s' before initialization", name8(rt, name));
            *found = 1;
            return b->v;
        }
        if (e->kind == ENV_GLOBAL) {
            for (jobj *o = rt->global; o; o = o->proto) {
                struct jprop *p = js_own(o, name);
                if (p) {
                    *found = 1;
                    if (p->v.t == JT_ACC)
                        return p->v.u.acc->get ? js_call(rt, jv_obj(p->v.u.acc->get), jv_obj(rt->global), 0, 0) : jv_undef();
                    return p->v;
                }
            }
        }
    }
    *found = 0;
    return jv_undef();
}

static jv get_var(js_rt *rt, jenv *env, jstr *name)
{
    int found;
    jv v = lookup(rt, env, name, &found);
    if (!found)
        js_throw_error(rt, JS_ERR_REFERENCE, "%s is not defined", name8(rt, name));
    return v;
}

static void assign_var(js_rt *rt, jenv *env, jstr *name, jv v)
{
    for (jenv *e = env; e; e = e->parent) {
        struct jbind *b = env_find(e, name);
        if (b) {
            if (b->flags & BF_TDZ)
                js_throw_error(rt, JS_ERR_REFERENCE, "Cannot access '%s' before initialization", name8(rt, name));
            if (b->flags & BF_CONST)
                js_throw_error(rt, JS_ERR_TYPE, "Assignment to constant variable.");
            b->v = v;
            return;
        }
        if (e->kind == ENV_GLOBAL)
            break;
    }
    js_set(rt, jv_obj(rt->global), name, v);
}

/* Pierwsze przypisanie let/const/class (koniec "martwej strefy") */
static void init_var(js_rt *rt, jenv *env, jstr *name, jv v)
{
    for (jenv *e = env; e; e = e->parent) {
        struct jbind *b = env_find(e, name);
        if (b) {
            b->v = v;
            b->flags &= ~(uint32_t)BF_TDZ;
            return;
        }
        if (e->kind == ENV_GLOBAL)
            break;
    }
    js_set(rt, jv_obj(rt->global), name, v);
}

static void declare_var(js_rt *rt, jenv *env, jstr *name, jv v)
{
    struct jbind *b = env_find(env, name);
    if (b) {
        b->v = v;
        b->flags &= ~(uint32_t)BF_TDZ;
    } else {
        env_add(rt, env, name, v, 0);
    }
}

/* Nowe srodowisko z tymi samymi zmiennymi (kazdy obrot petli for(let...) z domknieciami) */
static jenv *copy_env(js_rt *rt, jenv *e)
{
    jenv *c = new_env(rt, e->parent, e->kind, e->n);
    memcpy(c->b, e->b, sizeof(struct jbind) * e->n);
    c->n = e->n;
    return c;
}

/* Deklaracje let/const/class/function bloku */
static void declare_scope(js_rt *rt, struct jscope *sc, jenv *env)
{
    for (int i = 0; i < sc->n; i++)
        env_add(rt, env, sc->names[i], jv_undef(),
                BF_TDZ | BF_LEX | (sc->kinds[i] == VK_CONST ? BF_CONST : 0));
    for (int i = 0; i < sc->nfuncs; i++) {
        jnode *fn = sc->funcs[i];
        jstr *name = fn->u.fn->name;
        jv f = jv_obj(js_new_func(rt, fn, env));
        declare_var(rt, env, name, f);
        /* jak w przegladarkach: funkcja z bloku widoczna tez w calej funkcji */
        for (jenv *e = env->parent; e; e = e->parent) {
            if (e->kind == ENV_FUNC) {
                struct jbind *b = env_find(e, name);
                if (b && !(b->flags & BF_LEX))
                    b->v = f;
                break;
            }
            if (e->kind == ENV_GLOBAL) {
                js_set(rt, jv_obj(rt->global), name, f);
                break;
            }
        }
    }
}

static jenv *this_env(jenv *e)
{
    for (; e; e = e->parent)
        if (e->has_this)
            return e;
    return 0;
}

static jv this_value(js_rt *rt, jenv *env)
{
    jenv *e = this_env(env);
    if (!e)
        return jv_undef();
    if (!e->this_init)
        js_throw_error(rt, JS_ERR_REFERENCE,
                       "Must call super constructor in derived class before accessing 'this' or returning from derived constructor");
    return e->this_val;
}

/* ---------- funkcje ---------- */

jobj *js_new_func(js_rt *rt, jnode *fn, jenv *env)
{
    jobj *f = js_new_object(rt, rt->function_proto);
    f->cls = JC_FUNC;
    f->u.f.node = fn;
    f->u.f.env = env;
    struct jfuninfo *fi = fn->u.fn;
    if (fi->arrow || fi->method || fi->is_async)
        f->flags |= OF_NOCTOR;
    return f;
}

static jobj *make_arguments(js_rt *rt, jobj *f, int argc, jv *argv)
{
    jobj *a = js_new_object(rt, rt->object_proto);
    a->cls = JC_ARGS;
    js_array_reserve(rt, a, (uint32_t)argc);
    for (int i = 0; i < argc; i++)
        a->u.arr.v[i] = argv[i];
    a->u.arr.len = (uint32_t)argc;
    js_define(rt, a, rt->A.callee, jv_obj(f), PF_WRITE | PF_CONF);
    return a;
}

static void init_fields(js_rt *rt, jobj *f, jobj *obj);

/* Wywolanie funkcji napisanej w JS. derived: konstruktor klasy pochodnej (this po super()) */
static jv call_func(js_rt *rt, jobj *f, jv self, int argc, jv *argv, jobj *nt, int derived, jenv **env_out)
{
    struct jfuninfo *fi = f->u.f.node->u.fn;
    uint32_t cap = (uint32_t)(fi->nvars + fi->nfuncs + 4);
    for (jnode *p = fi->params; p; p = p->next)
        cap++;
    if (!fi->expr_body && fi->body->u.scope)
        cap += (uint32_t)fi->body->u.scope->n;
    jenv *env = new_env(rt, f->u.f.env, ENV_FUNC, cap);
    if (!fi->arrow) {
        env->has_this = 1;
        env->func = f;
        env->new_target = nt;
        if (derived) {
            env->this_init = 0;
        } else {
            env->this_init = 1;
            env->this_val = IS_NULLISH(self) && !fi->strict ? jv_obj(rt->global) : self;
        }
    }
    uint32_t saved_line = rt->line;
    if (fi->simple_params) {
        int i = 0;
        for (jnode *p = fi->params; p; p = p->next, i++)
            declare_var(rt, env, p->u.str, i < argc ? argv[i] : jv_undef());
    } else {
        int i = 0;
        for (jnode *p = fi->params; p; p = p->next, i++) {
            if (p->type == N_REST) {
                jobj *rest = js_new_array(rt, 0);
                for (int j = i; j < argc; j++)
                    js_array_push(rt, rest, argv[j]);
                bind_pattern(rt, p->a, jv_obj(rest), env, B_DECLARE);
                break;
            }
            bind_pattern(rt, p, i < argc ? argv[i] : jv_undef(), env, B_DECLARE);
        }
    }
    if (fi->uses_args && !fi->arrow && !env_find(env, rt->A.arguments))
        env_add(rt, env, rt->A.arguments, jv_obj(make_arguments(rt, f, argc, argv)), 0);
    for (int i = 0; i < fi->nvars; i++)
        if (!env_find(env, fi->vars[i]))
            env_add(rt, env, fi->vars[i], jv_undef(), 0);
    for (int i = 0; i < fi->nfuncs; i++)
        declare_var(rt, env, fi->funcs[i]->u.fn->name, jv_obj(js_new_func(rt, fi->funcs[i], env)));
    if (env_out)
        *env_out = env;
    jv r = jv_undef();
    if (fi->expr_body) {
        r = js_eval(rt, fi->body, env);
    } else {
        if (fi->body->u.scope)
            declare_scope(rt, fi->body->u.scope, env);
        if (exec_list(rt, fi->body->a, env) == CR) {
            r = rt->ret;
            rt->ret = jv_undef();
        }
    }
    rt->line = saved_line;
    return r;
}

jv js_call_body(js_rt *rt, jobj *f, jv self, int argc, jv *argv)
{
    return call_func(rt, f, self, argc, argv, 0, 0, 0);
}

static void enter_call(js_rt *rt)
{
    if (++rt->depth > DEPTH_MAX) {
        rt->depth--;
        js_throw_error(rt, JS_ERR_RANGE, "Maximum call stack size exceeded");
    }
    js_check_stack(rt);
    js_step(rt);
}

/* Argumenty funkcji zwiazanej (bind) + nowe */
static jv *bound_args(js_rt *rt, jobj *f, int argc, jv *argv, int *n)
{
    int total = f->u.bf.nargs + argc;
    jv *all = js_alloc(rt, sizeof(jv) * (size_t)(total ? total : 1));
    for (int i = 0; i < f->u.bf.nargs; i++)
        all[i] = f->u.bf.args[i];
    for (int i = 0; i < argc; i++)
        all[f->u.bf.nargs + i] = argv[i];
    *n = total;
    return all;
}

jv js_call(js_rt *rt, jv fv, jv self, int argc, jv *argv)
{
    if (fv.t != JT_OBJ)
        js_throw_error(rt, JS_ERR_TYPE, "%s is not a function", js_typeof(fv));
    jobj *f = fv.u.o;
    jv r;
    enter_call(rt);
    switch (f->cls) {
    case JC_NATIVE:
        r = f->u.nf.fn(rt, self, argc, argv, f);
        break;
    case JC_FUNC:
        if (f->flags & OF_CLASS)
            js_throw_error(rt, JS_ERR_TYPE, "Class constructor %s cannot be invoked without 'new'",
                           name8(rt, js_func_name(rt, f)));
        if (f->u.f.node->u.fn->is_async)
            r = js_async_call(rt, f, self, argc, argv);
        else
            r = call_func(rt, f, self, argc, argv, 0, 0, 0);
        break;
    case JC_BOUND: {
        int n;
        jv *all = bound_args(rt, f, argc, argv, &n);
        r = js_call(rt, jv_obj(f->u.bf.target), f->u.bf.self, n, all);
        break;
    }
    default:
        js_throw_error(rt, JS_ERR_TYPE, "object is not a function");
    }
    rt->depth--;
    return r;
}

static jobj *proto_from_ctor(js_rt *rt, jobj *nt, jobj *fallback)
{
    jv p = js_get(rt, jv_obj(nt), rt->A.prototype);
    return p.t == JT_OBJ ? p.u.o : fallback;
}

jv js_construct(js_rt *rt, jv fv, int argc, jv *argv, jobj *nt)
{
    if (!js_is_ctor(fv))
        js_throw_error(rt, JS_ERR_TYPE, "%s is not a constructor",
                       js_is_callable(fv) ? name8(rt, js_func_name(rt, fv.u.o)) : js_typeof(fv));
    jobj *f = fv.u.o;
    if (!nt)
        nt = f;
    jv r;
    enter_call(rt);
    switch (f->cls) {
    case JC_NATIVE:
        r = f->u.nf.ctor(rt, jv_undef(), argc, argv, f);
        if (nt != f && r.t == JT_OBJ)
            r.u.o->proto = proto_from_ctor(rt, nt, r.u.o->proto);
        break;
    case JC_BOUND: {
        int n;
        jv *all = bound_args(rt, f, argc, argv, &n);
        r = js_construct(rt, jv_obj(f->u.bf.target), n, all, nt == f ? f->u.bf.target : nt);
        break;
    }
    default:
        if (!f->u.f.node) {                          /* klasa bez konstruktora */
            if (f->flags & OF_DERIVED) {
                if (!f->proto || !js_is_ctor(jv_obj(f->proto)))
                    js_throw_error(rt, JS_ERR_TYPE, "Super constructor is not a constructor");
                r = js_construct(rt, jv_obj(f->proto), argc, argv, nt);
                init_fields(rt, f, r.u.o);
            } else {
                jobj *o = js_new_object(rt, proto_from_ctor(rt, nt, rt->object_proto));
                init_fields(rt, f, o);
                r = jv_obj(o);
            }
        } else if (f->flags & OF_DERIVED) {
            jenv *env;
            jv res = call_func(rt, f, jv_undef(), argc, argv, nt, 1, &env);
            if (res.t == JT_OBJ) {
                r = res;
            } else {
                if (!env->this_init)
                    js_throw_error(rt, JS_ERR_REFERENCE,
                                   "Must call super constructor in derived class before accessing 'this' or returning from derived constructor");
                r = env->this_val;
            }
        } else {
            jobj *o = js_new_object(rt, proto_from_ctor(rt, nt, rt->object_proto));
            init_fields(rt, f, o);
            jv res = call_func(rt, f, jv_obj(o), argc, argv, nt, 0, 0);
            r = res.t == JT_OBJ ? res : jv_obj(o);
        }
    }
    rt->depth--;
    return r;
}

static void init_fields(js_rt *rt, jobj *f, jobj *obj)
{
    struct jfields *fl = f->u.f.fields;
    if (!fl)
        return;
    for (int i = 0; i < fl->n; i++) {
        jv v = jv_undef();
        if (fl->inits[i])
            v = call_func(rt, fl->inits[i], jv_obj(obj), 0, 0, 0, 0, 0);
        js_define(rt, obj, fl->keys[i], v, PF_ALL);
    }
}

/* ---------- pomocnicze do wyrazen ---------- */

/* Opis wyrazenia do komunikatu bledu: "document.foo(...)" */
static void describe(js_rt *rt, jnode *n, char *buf, unsigned size)
{
    char tmp[120];
    switch (n->type) {
    case N_IDENT:
        js_utf8(n->u.str, buf, size);
        return;
    case N_THIS:
        ksnprintf(buf, size, "this");
        return;
    case N_MEMBER:
        describe(rt, n->a, tmp, sizeof(tmp));
        {
            char nm[64];
            js_utf8(n->u.str, nm, sizeof(nm));
            ksnprintf(buf, size, "%s.%s", tmp, nm);
        }
        return;
    case N_INDEX:
        describe(rt, n->a, tmp, sizeof(tmp));
        ksnprintf(buf, size, "%s[...]", tmp);
        return;
    case N_CALL:
        describe(rt, n->a, tmp, sizeof(tmp));
        ksnprintf(buf, size, "%s(...)", tmp);
        return;
    default:
        ksnprintf(buf, size, "(wyrazenie)");
    }
}

static void not_a_function(js_rt *rt, jnode *callee, jv v) __attribute__((noreturn));
static void not_a_function(js_rt *rt, jnode *callee, jv v)
{
    char buf[160];
    describe(rt, callee, buf, sizeof(buf));
    (void)v;
    js_throw_error(rt, JS_ERR_TYPE, "%s is not a function", buf);
}

static jv add(js_rt *rt, jv a, jv b)
{
    if (a.t == JT_NUM && b.t == JT_NUM)
        return jv_num(a.u.n + b.u.n);
    if (a.t == JT_STR && b.t == JT_STR)
        return jv_str(js_concat(rt, a.u.s, b.u.s));
    jv pa = js_to_prim(rt, a, 0);
    jv pb = js_to_prim(rt, b, 0);
    if (pa.t == JT_STR || pb.t == JT_STR) {
        jstr *sa = js_to_str(rt, pa);
        jstr *sb = js_to_str(rt, pb);
        return jv_str(js_concat(rt, sa, sb));
    }
    double x = js_to_num(rt, pa);
    double y = js_to_num(rt, pb);
    return jv_num(x + y);
}

/* a < b; *undef = 1 gdy porownanie z NaN */
static int less(js_rt *rt, jv a, jv b, int left_first, int *undef)
{
    jv pa, pb;
    if (left_first) {
        pa = js_to_prim(rt, a, 1);
        pb = js_to_prim(rt, b, 1);
    } else {
        pb = js_to_prim(rt, b, 1);
        pa = js_to_prim(rt, a, 1);
    }
    if (pa.t == JT_STR && pb.t == JT_STR)
        return js_str_cmp(pa.u.s, pb.u.s) < 0;
    double x = js_to_num(rt, pa);
    double y = js_to_num(rt, pb);
    if (js_isnan(x) || js_isnan(y)) {
        *undef = 1;
        return 0;
    }
    return x < y;
}

static int instance_of(js_rt *rt, jv v, jv f)
{
    if (!js_is_callable(f))
        js_throw_error(rt, JS_ERR_TYPE, "Right-hand side of 'instanceof' is not callable");
    jobj *fo = f.u.o;
    while (fo->cls == JC_BOUND)
        fo = fo->u.bf.target;
    if (v.t != JT_OBJ)
        return 0;
    jv proto = js_get(rt, jv_obj(fo), rt->A.prototype);
    if (proto.t != JT_OBJ)
        js_throw_error(rt, JS_ERR_TYPE, "Function has non-object prototype in instanceof check");
    for (jobj *o = v.u.o->proto; o; o = o->proto)
        if (o == proto.u.o)
            return 1;
    return 0;
}

static jv binary_op(js_rt *rt, int op, jv a, jv b)
{
    if (a.t == JT_NUM && b.t == JT_NUM) {
        double x = a.u.n, y = b.u.n;
        switch (op) {
        case '+': return jv_num(x + y);
        case '-': return jv_num(x - y);
        case '*': return jv_num(x * y);
        case '/': return jv_num(x / y);
        case '<': return jv_bool(x < y);
        case '>': return jv_bool(x > y);
        case OP_LE: return jv_bool(x <= y);
        case OP_GE: return jv_bool(x >= y);
        case OP_SEQ: case OP_EQ: return jv_bool(x == y);
        case OP_SNE: case OP_NE: return jv_bool(x != y);
        }
    }
    switch (op) {
    case '+':
        return add(rt, a, b);
    case '-': case '*': case '/': case '%': case OP_POW: {
        double x = js_to_num(rt, a);
        double y = js_to_num(rt, b);
        switch (op) {
        case '-': return jv_num(x - y);
        case '*': return jv_num(x * y);
        case '/': return jv_num(x / y);
        case '%': return jv_num(js_fmod(x, y));
        default: return jv_num(js_pow(x, y));
        }
    }
    case '&': case '|': case '^': case OP_SHL: case OP_SHR: case OP_USHR: {
        int32_t x = js_to_int32(rt, a);
        int32_t y = js_to_int32(rt, b);
        switch (op) {
        case '&': return jv_num(x & y);
        case '|': return jv_num(x | y);
        case '^': return jv_num(x ^ y);
        case OP_SHL: return jv_num((int32_t)((uint32_t)x << (y & 31)));
        case OP_SHR: return jv_num(x >> (y & 31));
        default: return jv_num((uint32_t)x >> (y & 31));
        }
    }
    case '<': {
        int u = 0;
        return jv_bool(less(rt, a, b, 1, &u));
    }
    case '>': {
        int u = 0;
        return jv_bool(less(rt, b, a, 0, &u));
    }
    case OP_LE: {
        int u = 0, r = less(rt, b, a, 0, &u);
        return jv_bool(!u && !r);
    }
    case OP_GE: {
        int u = 0, r = less(rt, a, b, 1, &u);
        return jv_bool(!u && !r);
    }
    case OP_EQ: return jv_bool(js_loose_eq(rt, a, b));
    case OP_NE: return jv_bool(!js_loose_eq(rt, a, b));
    case OP_SEQ: return jv_bool(js_strict_eq(a, b));
    case OP_SNE: return jv_bool(!js_strict_eq(a, b));
    case OP_INSTANCEOF: return jv_bool(instance_of(rt, a, b));
    case OP_IN:
        if (b.t != JT_OBJ)
            js_throw_error(rt, JS_ERR_TYPE, "Cannot use 'in' operator to search for '%s' in %s",
                           name8(rt, js_to_str(rt, a)), js_typeof(b));
        return jv_bool(js_has(rt, b.u.o, js_key_of(rt, a)));
    }
    js_throw_error(rt, JS_ERR_SYNTAX, "Nieznany operator");
}

/* a += b -> dzialanie + */
static int compound_op(int op)
{
    switch (op) {
    case OP_ADD_A: return '+';
    case OP_SUB_A: return '-';
    case OP_MUL_A: return '*';
    case OP_DIV_A: return '/';
    case OP_MOD_A: return '%';
    case OP_SHL_A: return OP_SHL;
    case OP_SHR_A: return OP_SHR;
    case OP_USHR_A: return OP_USHR;
    case OP_AND_A: return '&';
    case OP_OR_A: return '|';
    case OP_XOR_A: return '^';
    case OP_POW_A: return OP_POW;
    }
    return 0;
}

/* Argumenty wywolania (z ...rozwinieciem). buf - miejsce na kilka */
static jv *eval_args(js_rt *rt, jnode *list, jenv *env, int *argc, jv *buf, int bufn)
{
    int n = 0, spread = 0;
    for (jnode *a = list; a; a = a->next) {
        n++;
        if (a->type == N_SPREAD)
            spread = 1;
    }
    if (!spread) {
        jv *out = n <= bufn ? buf : js_alloc(rt, sizeof(jv) * (size_t)n);
        int i = 0;
        for (jnode *a = list; a; a = a->next)
            out[i++] = js_eval(rt, a, env);
        *argc = n;
        return out;
    }
    jobj *arr = js_new_array(rt, 0);
    for (jnode *a = list; a; a = a->next) {
        if (a->type == N_SPREAD) {
            jv v = js_iterate_to_array(rt, js_eval(rt, a->a, env));
            jobj *src = v.u.o;
            for (uint32_t i = 0; i < src->u.arr.len; i++)
                js_array_push(rt, arr, src->u.arr.v[i]);
        } else {
            js_array_push(rt, arr, js_eval(rt, a, env));
        }
    }
    *argc = (int)arr->u.arr.len;
    return arr->u.arr.v;
}

static jobj *super_base(js_rt *rt, jenv *env)
{
    jenv *fe = this_env(env);
    jobj *home = fe && fe->func && fe->func->cls == JC_FUNC ? fe->func->u.f.home : 0;
    if (!home)
        js_throw_error(rt, JS_ERR_SYNTAX, "'super' keyword unexpected here");
    return home->proto;
}

static jv super_call(js_rt *rt, jnode *n, jenv *env)
{
    jenv *fe = this_env(env);
    if (!fe || !fe->func || !(fe->func->flags & OF_DERIVED))
        js_throw_error(rt, JS_ERR_SYNTAX, "'super' keyword unexpected here");
    jobj *parent = fe->func->proto;
    jv buf[8];
    int argc;
    jv *argv = eval_args(rt, n->b, env, &argc, buf, 8);
    if (!parent || !js_is_ctor(jv_obj(parent)))
        js_throw_error(rt, JS_ERR_TYPE, "Super constructor is not a constructor");
    if (fe->this_init)
        js_throw_error(rt, JS_ERR_REFERENCE, "Super constructor may only be called once");
    jv r = js_construct(rt, jv_obj(parent), argc, argv, fe->new_target ? fe->new_target : fe->func);
    fe->this_val = r;
    fe->this_init = 1;
    init_fields(rt, fe->func, r.u.o);
    return jv_undef();
}

static __attribute__((noinline)) jv eval_template(js_rt *rt, jnode *n, jenv *env)
{
    jnode *q = n->a, *e = n->b;
    jstr *s = q->u.str;
    for (q = q->next; q; q = q->next, e = e->next) {
        jv v = js_eval(rt, e, env);
        s = js_concat(rt, s, v.t == JT_STR ? v.u.s : js_to_str(rt, v.t == JT_OBJ ? js_to_prim(rt, v, 2) : v));
        s = js_concat(rt, s, q->u.str);
    }
    return jv_str(s);
}

static __attribute__((noinline)) jv eval_object(js_rt *rt, jnode *n, jenv *env)
{
    jobj *o = js_new_object(rt, rt->object_proto);
    for (jnode *p = n->a; p; p = p->next) {
        if (p->op == PK_SPREAD) {
            jv src = js_eval(rt, p->b, env);
            if (IS_NULLISH(src))
                continue;
            jobj *so = js_to_obj(rt, src);
            jobj *keys = js_own_keys(rt, so, 1);
            for (uint32_t i = 0; i < keys->u.arr.len; i++) {
                jstr *k = keys->u.arr.v[i].u.s;
                js_define(rt, o, k, js_get(rt, jv_obj(so), k), PF_ALL);
            }
            continue;
        }
        jstr *key = (p->flags & NF_COMPUTED) ? js_key_of(rt, js_eval(rt, p->a, env)) : p->a->u.str;
        if (p->op == PK_GET || p->op == PK_SET) {
            jobj *f = js_new_func(rt, p->b, env);
            f->u.f.home = o;
            js_define_acc(rt, o, key, p->op == PK_GET ? f : 0, p->op == PK_SET ? f : 0, PF_ENUM | PF_CONF);
            continue;
        }
        if (key == rt->A.__proto__ && !(p->flags & (NF_COMPUTED | NF_SHORTHAND)) && p->b->type != N_FUNC) {
            jv pv = js_eval(rt, p->b, env);
            if (pv.t == JT_OBJ)
                o->proto = pv.u.o;
            else if (pv.t == JT_NULL)
                o->proto = 0;
            continue;
        }
        jv v = js_eval(rt, p->b, env);
        if (p->b->type == N_FUNC && v.t == JT_OBJ) {
            if (p->b->u.fn->method)
                v.u.o->u.f.home = o;
            if (!p->b->u.fn->name && (p->flags & NF_COMPUTED))
                js_define(rt, v.u.o, rt->A.name, jv_str(key), PF_CONF);
        }
        js_define(rt, o, key, v, PF_ALL);
    }
    return jv_obj(o);
}

static __attribute__((noinline)) jv eval_array(js_rt *rt, jnode *n, jenv *env)
{
    jobj *a = js_new_array(rt, 0);
    for (jnode *e = n->a; e; e = e->next) {
        if (e->type == N_HOLE) {
            js_array_push(rt, a, jv_undef());
        } else if (e->type == N_SPREAD) {
            jv v = js_iterate_to_array(rt, js_eval(rt, e->a, env));
            jobj *src = v.u.o;
            js_array_reserve(rt, a, a->u.arr.len + src->u.arr.len);
            for (uint32_t i = 0; i < src->u.arr.len; i++)
                js_array_push(rt, a, src->u.arr.v[i]);
        } else {
            js_array_push(rt, a, js_eval(rt, e, env));
        }
    }
    return jv_obj(a);
}

static __attribute__((noinline)) jv eval_assign(js_rt *rt, jnode *n, jenv *env)
{
    jnode *t = n->a;
    int op = OPX(n->op);
    if (op == '=') {
        if (t->type == N_IDENT) {
            jv v = js_eval(rt, n->b, env);
            assign_var(rt, env, t->u.str, v);
            return v;
        }
        if (t->type == N_MEMBER) {
            jv obj = js_eval(rt, t->a, env);
            jv v = js_eval(rt, n->b, env);
            js_set(rt, obj, t->u.str, v);
            return v;
        }
        if (t->type == N_INDEX) {
            jv obj = js_eval(rt, t->a, env);
            jv key = js_eval(rt, t->b, env);
            jv v = js_eval(rt, n->b, env);
            js_set_index(rt, obj, key, v);
            return v;
        }
        jv v = js_eval(rt, n->b, env);
        bind_pattern(rt, t, v, env, B_ASSIGN);
        return v;
    }
    /* a += b, a ||= b ... */
    jv obj = jv_undef(), key = jv_undef(), old;
    if (t->type == N_IDENT) {
        old = get_var(rt, env, t->u.str);
    } else if (t->type == N_MEMBER) {
        obj = js_eval(rt, t->a, env);
        old = js_get(rt, obj, t->u.str);
    } else {
        obj = js_eval(rt, t->a, env);
        key = js_eval(rt, t->b, env);
        old = js_get_index(rt, obj, key);
    }
    jv v;
    if (op == OP_LAND_A || op == OP_LOR_A || op == OP_NULLISH_A) {
        int keep = op == OP_LAND_A ? !js_truthy(old) : op == OP_LOR_A ? js_truthy(old) : !IS_NULLISH(old);
        if (keep)
            return old;
        v = js_eval(rt, n->b, env);
    } else {
        jv rhs = js_eval(rt, n->b, env);
        v = binary_op(rt, compound_op(op), old, rhs);
    }
    if (t->type == N_IDENT)
        assign_var(rt, env, t->u.str, v);
    else if (t->type == N_MEMBER)
        js_set(rt, obj, t->u.str, v);
    else
        js_set_index(rt, obj, key, v);
    return v;
}

static __attribute__((noinline)) jv eval_update(js_rt *rt, jnode *n, jenv *env)
{
    jnode *t = n->a;
    double d = OPX(n->op) == OP_INC ? 1 : -1;
    jv obj = jv_undef(), key = jv_undef(), old;
    if (t->type == N_IDENT) {
        old = get_var(rt, env, t->u.str);
    } else if (t->type == N_MEMBER) {
        obj = js_eval(rt, t->a, env);
        old = js_get(rt, obj, t->u.str);
    } else {
        obj = js_eval(rt, t->a, env);
        key = js_eval(rt, t->b, env);
        old = js_get_index(rt, obj, key);
    }
    double x = old.t == JT_NUM ? old.u.n : js_to_num(rt, old);
    jv nv = jv_num(x + d);
    if (t->type == N_IDENT)
        assign_var(rt, env, t->u.str, nv);
    else if (t->type == N_MEMBER)
        js_set(rt, obj, t->u.str, nv);
    else
        js_set_index(rt, obj, key, nv);
    return (n->flags & NF_PREFIX) ? nv : jv_num(x);
}

static __attribute__((noinline)) jv eval_unary(js_rt *rt, jnode *n, jenv *env)
{
    int op = OPX(n->op);
    if (op == OP_TYPEOF) {
        jv v;
        if (n->a->type == N_IDENT) {
            int found;
            v = lookup(rt, env, n->a->u.str, &found);
        } else {
            v = js_eval(rt, n->a, env);
        }
        return jv_str(js_atom(rt, js_typeof(v)));
    }
    if (op == OP_DELETE) {
        jnode *t = n->a;
        if (t->type == N_MEMBER || t->type == N_INDEX) {
            jv obj = js_eval(rt, t->a, env);
            jstr *key = t->type == N_MEMBER ? t->u.str : js_key_of(rt, js_eval(rt, t->b, env));
            if (IS_NULLISH(obj))
                js_throw_error(rt, JS_ERR_TYPE, "Cannot convert undefined or null to object");
            if (obj.t != JT_OBJ)
                return jv_bool(1);
            return jv_bool(js_delete(rt, obj.u.o, key));
        }
        if (t->type == N_IDENT)
            return jv_bool(0);
        js_eval(rt, t, env);
        return jv_bool(1);
    }
    jv v = js_eval(rt, n->a, env);
    switch (op) {
    case '!': return jv_bool(!js_truthy(v));
    case '~': return jv_num(~js_to_int32(rt, v));
    case OP_NEG: return jv_num(-(v.t == JT_NUM ? v.u.n : js_to_num(rt, v)));
    case OP_PLUS: return jv_num(v.t == JT_NUM ? v.u.n : js_to_num(rt, v));
    case OP_VOID: return jv_undef();
    }
    return jv_undef();
}

static __attribute__((noinline)) jv eval_call(js_rt *rt, jnode *n, jenv *env)
{
    jnode *c = n->a;
    jv fn, self = jv_undef();
    if (c->type == N_SUPER)
        return super_call(rt, n, env);
    if (c->type == N_MEMBER || c->type == N_INDEX) {
        if (c->a->type == N_SUPER) {
            self = this_value(rt, env);
            jobj *base = super_base(rt, env);
            jstr *key = c->type == N_MEMBER ? c->u.str : js_key_of(rt, js_eval(rt, c->b, env));
            fn = base ? js_get(rt, jv_obj(base), key) : jv_undef();
        } else {
            self = js_eval(rt, c->a, env);
            if (rt->chain_break)
                return jv_undef();
            if ((c->flags & NF_OPTIONAL) && IS_NULLISH(self)) {
                rt->chain_break = 1;
                return jv_undef();
            }
            if (c->type == N_MEMBER) {
                fn = js_get(rt, self, c->u.str);
            } else {
                jv k = js_eval(rt, c->b, env);
                fn = js_get_index(rt, self, k);
            }
        }
    } else {
        fn = js_eval(rt, c, env);
        if (rt->chain_break)
            return jv_undef();
    }
    if ((n->flags & NF_OPTIONAL) && IS_NULLISH(fn)) {
        rt->chain_break = 1;
        return jv_undef();
    }
    if (!js_is_callable(fn))
        not_a_function(rt, c, fn);
    jv buf[8];
    int argc;
    jv *argv = eval_args(rt, n->b, env, &argc, buf, 8);
    rt->line = n->line;
    return js_call(rt, fn, self, argc, argv);
}

static __attribute__((noinline)) jv eval_tagged(js_rt *rt, jnode *n, jenv *env)
{
    jnode *t = n->b;
    jobj *strings = js_new_array(rt, 0), *raw = js_new_array(rt, 0);
    int count = 0;
    for (jnode *q = t->a; q; q = q->next) {
        js_array_push(rt, strings, jv_str(q->u.str));
        js_array_push(rt, raw, jv_str(q->a->u.str));
        count++;
    }
    js_define(rt, strings, rt->A.raw, jv_obj(raw), 0);
    jv *argv = js_alloc(rt, sizeof(jv) * (size_t)(count + 1));
    argv[0] = jv_obj(strings);
    int argc = 1;
    for (jnode *e = t->b; e; e = e->next)
        argv[argc++] = js_eval(rt, e, env);
    jv self = jv_undef(), fn;
    if (n->a->type == N_MEMBER) {
        self = js_eval(rt, n->a->a, env);
        fn = js_get(rt, self, n->a->u.str);
    } else {
        fn = js_eval(rt, n->a, env);
    }
    if (!js_is_callable(fn))
        not_a_function(rt, n->a, fn);
    return js_call(rt, fn, self, argc, argv);
}

static __attribute__((noinline)) jv eval_new(js_rt *rt, jnode *n, jenv *env)
{
    jv fn = js_eval(rt, n->a, env);
    jv buf[8];
    int argc;
    jv *argv = eval_args(rt, n->b, env, &argc, buf, 8);
    if (!js_is_ctor(fn)) {
        char d[160];
        describe(rt, n->a, d, sizeof(d));
        js_throw_error(rt, JS_ERR_TYPE, "%s is not a constructor", d);
    }
    rt->line = n->line;
    return js_construct(rt, fn, argc, argv, 0);
}

jv js_eval(js_rt *rt, jnode *n, jenv *env)
{
    switch (n->type) {
    case N_NUM:
        return jv_num(n->u.num);
    case N_STR:
        return jv_str(n->u.str);
    case N_IDENT:
        return get_var(rt, env, n->u.str);
    case N_TRUE:
        return jv_bool(1);
    case N_FALSE:
        return jv_bool(0);
    case N_NULL:
        return jv_null();
    case N_THIS:
        return this_value(rt, env);
    case N_TEMPLATE:
        return eval_template(rt, n, env);
    case N_TAGGED:
        return eval_tagged(rt, n, env);
    case N_REGEX:
        return js_regexp_new(rt, n->u.re->source, n->u.re->flags);
    case N_ARRAY:
        return eval_array(rt, n, env);
    case N_OBJECT:
        return eval_object(rt, n, env);
    case N_AWAIT:
        return js_await(rt, js_eval(rt, n->a, env));
    case N_FUNC: {
        struct jfuninfo *fi = n->u.fn;
        if (fi->name && !fi->arrow && !fi->method && !fi->is_class_ctor) {
            /* nazwana funkcja widzi sama siebie: var f = function g() { g(); } */
            jenv *fe = new_env(rt, env, ENV_BLOCK, 1);
            jobj *f = js_new_func(rt, n, fe);
            env_add(rt, fe, fi->name, jv_obj(f), BF_CONST);
            return jv_obj(f);
        }
        return jv_obj(js_new_func(rt, n, env));
    }
    case N_CLASS:
        return eval_class(rt, n, env);
    case N_UNARY:
        return eval_unary(rt, n, env);
    case N_UPDATE:
        return eval_update(rt, n, env);
    case N_BINARY: {
        jv a = js_eval(rt, n->a, env);
        jv b = js_eval(rt, n->b, env);
        return binary_op(rt, OPX(n->op), a, b);
    }
    case N_LOGICAL: {
        jv a = js_eval(rt, n->a, env);
        int op = OPX(n->op);
        if (op == OP_AND)
            return js_truthy(a) ? js_eval(rt, n->b, env) : a;
        if (op == OP_OR)
            return js_truthy(a) ? a : js_eval(rt, n->b, env);
        return IS_NULLISH(a) ? js_eval(rt, n->b, env) : a;
    }
    case N_ASSIGN:
        return eval_assign(rt, n, env);
    case N_COND:
        return js_truthy(js_eval(rt, n->a, env)) ? js_eval(rt, n->b, env) : js_eval(rt, n->c, env);
    case N_CALL:
        return eval_call(rt, n, env);
    case N_NEW:
        return eval_new(rt, n, env);
    case N_MEMBER: {
        if (n->a->type == N_SUPER) {
            jobj *base = super_base(rt, env);
            jv self = this_value(rt, env);
            if (!base)
                return jv_undef();
            struct jprop *p = js_find(base, n->u.str);
            if (p && p->v.t == JT_ACC)
                return p->v.u.acc->get ? js_call(rt, jv_obj(p->v.u.acc->get), self, 0, 0) : jv_undef();
            return p ? p->v : jv_undef();
        }
        jv obj = js_eval(rt, n->a, env);
        if (rt->chain_break)
            return jv_undef();
        if ((n->flags & NF_OPTIONAL) && IS_NULLISH(obj)) {
            rt->chain_break = 1;
            return jv_undef();
        }
        return js_get(rt, obj, n->u.str);
    }
    case N_INDEX: {
        if (n->a->type == N_SUPER) {
            jobj *base = super_base(rt, env);
            jv self = this_value(rt, env);
            jstr *key = js_key_of(rt, js_eval(rt, n->b, env));
            (void)self;
            return base ? js_get(rt, jv_obj(base), key) : jv_undef();
        }
        jv obj = js_eval(rt, n->a, env);
        if (rt->chain_break)
            return jv_undef();
        if ((n->flags & NF_OPTIONAL) && IS_NULLISH(obj)) {
            rt->chain_break = 1;
            return jv_undef();
        }
        jv key = js_eval(rt, n->b, env);
        return js_get_index(rt, obj, key);
    }
    case N_OPTCHAIN: {
        int saved = rt->chain_break;
        rt->chain_break = 0;
        jv v = js_eval(rt, n->a, env);
        if (rt->chain_break)
            v = jv_undef();
        rt->chain_break = saved;
        return v;
    }
    case N_SEQ: {
        jv v = jv_undef();
        for (jnode *e = n->a; e; e = e->next)
            v = js_eval(rt, e, env);
        return v;
    }
    case N_NEWTARGET: {
        jenv *e = this_env(env);
        return e && e->new_target ? jv_obj(e->new_target) : jv_undef();
    }
    case N_SUPER:
        js_throw_error(rt, JS_ERR_SYNTAX, "'super' keyword unexpected here");
    case N_SPREAD:
        js_throw_error(rt, JS_ERR_SYNTAX, "Unexpected '...'");
    case N_ARRPAT:
    case N_OBJPAT:
        js_throw_error(rt, JS_ERR_SYNTAX, "Invalid destructuring");
    case N_DEFAULT:
        /* { x = 1 } poza wzorcem - traktujemy jak przypisanie */
        {
            jv v = js_eval(rt, n->b, env);
            bind_pattern(rt, n->a, v, env, B_ASSIGN);
            return v;
        }
    }
    js_throw_error(rt, JS_ERR_SYNTAX, "Nieznane wyrazenie (%d)", n->type);
}

/* ---------- wzorce (destrukturyzacja) ---------- */

static void bind_name(js_rt *rt, jstr *name, jv v, jenv *env, int mode)
{
    if (mode == B_ASSIGN)
        assign_var(rt, env, name, v);
    else if (mode == B_INIT)
        init_var(rt, env, name, v);
    else
        declare_var(rt, env, name, v);
}

static __attribute__((noinline)) void bind_object_pattern(js_rt *rt, jnode *pat, jv v, jenv *env, int mode)
{
    if (IS_NULLISH(v))
        js_throw_error(rt, JS_ERR_TYPE, "Cannot destructure '%s' as it is %s.",
                       v.t == JT_NULL ? "null" : "undefined", v.t == JT_NULL ? "null" : "undefined");
    jstr *used[64];
    int nused = 0;
    for (jnode *p = pat->a; p; p = p->next) {
        if (p->type == N_REST) {
            jobj *rest = js_new_object(rt, rt->object_proto);
            jobj *src = js_to_obj(rt, v);
            jobj *keys = js_own_keys(rt, src, 1);
            for (uint32_t i = 0; i < keys->u.arr.len; i++) {
                jstr *k = keys->u.arr.v[i].u.s;
                int skip = 0;
                for (int j = 0; j < nused; j++)
                    if (used[j] == k)
                        skip = 1;
                if (!skip)
                    js_define(rt, rest, k, js_get(rt, v, k), PF_ALL);
            }
            bind_pattern(rt, p->a, jv_obj(rest), env, mode);
            break;
        }
        jstr *key = (p->flags & NF_COMPUTED) ? js_key_of(rt, js_eval(rt, p->a, env)) : p->a->u.str;
        if (nused < 64)
            used[nused++] = key;
        bind_pattern(rt, p->b, js_get(rt, v, key), env, mode);
    }
}

static void bind_pattern(js_rt *rt, jnode *pat, jv v, jenv *env, int mode)
{
    switch (pat->type) {
    case N_IDENT:
        bind_name(rt, pat->u.str, v, env, mode);
        return;
    case N_MEMBER: {
        jv obj = js_eval(rt, pat->a, env);
        js_set(rt, obj, pat->u.str, v);
        return;
    }
    case N_INDEX: {
        jv obj = js_eval(rt, pat->a, env);
        jv key = js_eval(rt, pat->b, env);
        js_set_index(rt, obj, key, v);
        return;
    }
    case N_DEFAULT:
    case N_ASSIGN:
        if (v.t == JT_UNDEF) {
            v = js_eval(rt, pat->b, env);
            if (pat->b->type == N_FUNC && v.t == JT_OBJ && !pat->b->u.fn->name && pat->a->type == N_IDENT)
                js_define(rt, v.u.o, rt->A.name, jv_str(pat->a->u.str), PF_CONF);
        }
        bind_pattern(rt, pat->a, v, env, mode);
        return;
    case N_ARRPAT: {
        if (IS_NULLISH(v))
            js_throw_error(rt, JS_ERR_TYPE, "%s is not iterable", v.t == JT_NULL ? "null" : "undefined");
        jobj *arr = js_iterate_to_array(rt, v).u.o;
        uint32_t i = 0;
        for (jnode *e = pat->a; e; e = e->next) {
            if (e->type == N_HOLE) {
                i++;
                continue;
            }
            if (e->type == N_REST) {
                jobj *rest = js_new_array(rt, 0);
                for (; i < arr->u.arr.len; i++)
                    js_array_push(rt, rest, arr->u.arr.v[i]);
                bind_pattern(rt, e->a, jv_obj(rest), env, mode);
                break;
            }
            jv item = i < arr->u.arr.len ? arr->u.arr.v[i] : jv_undef();
            i++;
            bind_pattern(rt, e, item, env, mode);
        }
        return;
    }
    case N_OBJPAT:
        bind_object_pattern(rt, pat, v, env, mode);
        return;
    }
    js_throw_error(rt, JS_ERR_SYNTAX, "Invalid destructuring assignment target");
}

/* ---------- klasy ---------- */

static __attribute__((noinline)) jv eval_class(js_rt *rt, jnode *n, jenv *env)
{
    jobj *parent = 0, *proto_parent = rt->object_proto;
    int derived = 0;
    if (n->a) {
        jv pv = js_eval(rt, n->a, env);
        if (pv.t == JT_NULL) {
            proto_parent = 0;
        } else {
            if (!js_is_ctor(pv))
                js_throw_error(rt, JS_ERR_TYPE, "Class extends value is not a constructor or null");
            parent = pv.u.o;
            jv pp = js_get(rt, pv, rt->A.prototype);
            if (pp.t != JT_OBJ && pp.t != JT_NULL)
                js_throw_error(rt, JS_ERR_TYPE, "Class extends value does not have valid prototype property");
            proto_parent = pp.t == JT_OBJ ? pp.u.o : 0;
        }
        derived = 1;
    }
    jenv *cenv = env;
    if (n->u.str) {
        cenv = new_env(rt, env, ENV_BLOCK, 1);
        env_add(rt, cenv, n->u.str, jv_undef(), BF_TDZ | BF_CONST);
    }
    jobj *proto = js_new_object(rt, proto_parent);
    jobj *F;
    if (n->c) {
        F = js_new_func(rt, n->c, cenv);
        F->flags &= ~OF_NOCTOR;
    } else {
        F = js_new_object(rt, rt->function_proto);
        F->cls = JC_FUNC;
        F->u.f.env = cenv;
    }
    F->flags |= OF_CLASS | (derived ? OF_DERIVED : 0);
    if (parent)
        F->proto = parent;
    F->u.f.home = proto;
    js_define(rt, F, rt->A.prototype, jv_obj(proto), 0);
    js_define(rt, proto, rt->A.constructor, jv_obj(F), PF_WRITE | PF_CONF);
    js_define(rt, F, rt->A.name, jv_str(n->u.str ? n->u.str : rt->empty), PF_CONF);
    struct jfields *fl = 0;
    for (jnode *m = n->b; m; m = m->next) {
        jstr *key = (m->flags & NF_COMPUTED) ? js_key_of(rt, js_eval(rt, m->a, cenv)) : m->a->u.str;
        jobj *target = (m->flags & NF_STATIC) ? F : proto;
        if (m->op == PK_METHOD) {
            jobj *f = js_new_func(rt, m->b, cenv);
            f->u.f.home = target;
            js_define(rt, target, key, jv_obj(f), PF_WRITE | PF_CONF);
        } else if (m->op == PK_GET || m->op == PK_SET) {
            jobj *f = js_new_func(rt, m->b, cenv);
            f->u.f.home = target;
            js_define_acc(rt, target, key, m->op == PK_GET ? f : 0, m->op == PK_SET ? f : 0, PF_CONF);
        } else if (m->op == PK_FIELD) {
            jobj *init = 0;
            if (m->b) {
                init = js_new_func(rt, m->b, cenv);
                init->u.f.home = target;
            }
            if (m->flags & NF_STATIC) {
                jv v = init ? call_func(rt, init, jv_obj(F), 0, 0, 0, 0, 0) : jv_undef();
                js_define(rt, F, key, v, PF_ALL);
            } else {
                if (!fl) {
                    fl = js_alloc(rt, sizeof(*fl));
                    int cnt = 0;
                    for (jnode *k = n->b; k; k = k->next)
                        cnt++;
                    fl->keys = js_alloc(rt, sizeof(jstr *) * (size_t)cnt);
                    fl->inits = js_alloc(rt, sizeof(jobj *) * (size_t)cnt);
                }
                fl->keys[fl->n] = key;
                fl->inits[fl->n++] = init;
            }
        }
    }
    F->u.f.fields = fl;
    if (n->u.str)
        init_var(rt, cenv, n->u.str, jv_obj(F));
    return jv_obj(F);
}

/* ---------- instrukcje ---------- */

/* Blok try (albo catch z finally) pod ochrona: *threw = 1 gdy rzucono wyjatek */
static int run_protected(js_rt *rt, jnode *blk, jenv *env, int *threw, jv *exc)
{
    struct js_catch cat;
    cat.prev = rt->catch_top;
    cat.depth = rt->depth;
    rt->catch_top = &cat;
    if (js_setjmp(&cat.jb) == 0) {
        int c = exec_block(rt, blk, env);
        rt->catch_top = cat.prev;
        *threw = 0;
        return c;
    }
    rt->catch_top = cat.prev;
    rt->depth = cat.depth;
    rt->chain_break = 0;
    if (rt->aborting)                               /* przerwania (czas, pamiec) nie da sie zlapac */
        js_longjmp(&rt->catch_top->jb, 1);
    *threw = 1;
    *exc = rt->exc;
    return CN;
}

static __attribute__((noinline)) int exec_try(js_rt *rt, jnode *s, jenv *env)
{
    int threw;
    jv exc = jv_undef();
    int c = run_protected(rt, s->a, env, &threw, &exc);
    if (threw && s->c) {
        jenv *ce = env;
        if (s->b) {
            ce = new_env(rt, env, ENV_BLOCK, 2);
            bind_pattern(rt, s->b, exc, ce, B_DECLARE);
        }
        if (s->d) {
            c = run_protected(rt, s->c, ce, &threw, &exc);
        } else {
            threw = 0;
            c = exec_block(rt, s->c, ce);
        }
    }
    if (s->d) {
        jv saved_ret = rt->ret;
        jstr *saved_label = rt->label;
        int fc = exec_block(rt, s->d, env);
        if (fc != CN)
            return fc;
        rt->ret = saved_ret;
        rt->label = saved_label;
    }
    if (threw)
        js_throw(rt, exc);
    return c;
}

/* Wynik ciala petli: 1 = wyjdz z petli (break), 0 = dalej; *ret = skok dalej w gore */
static int loop_result(js_rt *rt, jnode *loop, int c, int *out)
{
    *out = CN;
    if (c == CN)
        return 0;
    if (c == CB) {
        if (!rt->label || rt->label == loop->label) {
            rt->label = 0;
            return 1;
        }
        *out = CB;
        return 1;
    }
    if (c == CC) {
        if (!rt->label || rt->label == loop->label) {
            rt->label = 0;
            return 0;
        }
        *out = CC;
        return 1;
    }
    *out = c;
    return 1;
}

static jobj *forin_keys(js_rt *rt, jv v)
{
    jobj *out = js_new_array(rt, 0);
    if (IS_NULLISH(v))
        return out;
    jobj *o = js_to_obj(rt, v);
    int first = 1;
    for (jobj *q = o; q; q = q->proto) {
        if (q == rt->object_proto || q == rt->array_proto || q == rt->function_proto)
            break;
        jobj *keys = js_own_keys(rt, q, 1);
        for (uint32_t i = 0; i < keys->u.arr.len; i++) {
            jstr *k = keys->u.arr.v[i].u.s;
            int dup = 0;
            if (!first)
                for (uint32_t j = 0; j < out->u.arr.len && !dup; j++)
                    dup = out->u.arr.v[j].u.s == k;
            if (!dup)
                js_array_push(rt, out, jv_str(k));
        }
        first = 0;
    }
    return out;
}

static __attribute__((noinline)) int exec_forx(js_rt *rt, jnode *s, jenv *env)
{
    jv src = js_eval(rt, s->b, env);
    jobj *items;
    int live = 0;
    if (s->type == N_FORIN) {
        items = forin_keys(rt, src);
    } else if (src.t == JT_OBJ && (src.u.o->cls == JC_ARRAY || src.u.o->cls == JC_ARGS)) {
        items = src.u.o;
        live = 1;
    } else {
        items = js_iterate_to_array(rt, src).u.o;
    }
    jnode *t = s->a;
    int lexical = t->type == N_VAR && t->op != VK_VAR;
    jenv *ie = lexical ? new_env(rt, env, ENV_BLOCK, 2) : env;
    for (uint32_t i = 0; i < items->u.arr.len; i++) {
        js_step(rt);
        jv item = items->u.arr.v[i];
        if (s->flags & NF_AWAIT)
            item = js_await(rt, item);
        if (s->type == N_FORIN && !live && src.t == JT_OBJ && !js_has(rt, src.u.o, item.u.s))
            continue;                               /* usuniety w trakcie petli */
        if (lexical && (s->flags & NF_CLOSURE) && i)
            ie = new_env(rt, env, ENV_BLOCK, 2);
        if (t->type == N_VAR)
            bind_pattern(rt, t->a->a, item, ie, lexical ? B_DECLARE : B_ASSIGN);
        else
            bind_pattern(rt, t, item, env, B_ASSIGN);
        int c = exec(rt, s->d, ie), out;
        if (loop_result(rt, s, c, &out))
            return out;
    }
    return CN;
}

static __attribute__((noinline)) int exec_for(js_rt *rt, jnode *s, jenv *env)
{
    jenv *le = env;
    if (s->u.scope) {
        le = new_env(rt, env, ENV_BLOCK, (uint32_t)s->u.scope->n);
        declare_scope(rt, s->u.scope, le);
    }
    if (s->a)
        exec(rt, s->a, le);
    int per_iter = (s->flags & NF_CLOSURE) && s->u.scope;
    if (per_iter)
        le = copy_env(rt, le);
    for (;;) {
        js_step(rt);
        if (s->b) {
            jv t = js_eval(rt, s->b, le);
            if (!js_truthy(t))
                break;
        }
        int c = exec(rt, s->d, le), out;
        if (loop_result(rt, s, c, &out))
            return out;
        if (per_iter)
            le = copy_env(rt, le);
        if (s->c)
            js_eval(rt, s->c, le);
    }
    return CN;
}

static __attribute__((noinline)) int exec_switch(js_rt *rt, jnode *s, jenv *env)
{
    jv d = js_eval(rt, s->a, env);
    jenv *se = env;
    if (s->u.scope) {
        se = new_env(rt, env, ENV_BLOCK, (uint32_t)s->u.scope->n);
        declare_scope(rt, s->u.scope, se);
    }
    jnode *start = 0, *def = 0;
    for (jnode *c = s->b; c; c = c->next) {
        if (c->flags & NF_STATIC) {
            def = c;
            continue;
        }
        if (js_strict_eq(d, js_eval(rt, c->a, se))) {
            start = c;
            break;
        }
    }
    if (!start)
        start = def;
    for (jnode *c = start; c; c = c->next) {
        int r = exec_list(rt, c->b, se);
        if (r == CB && !rt->label)
            return CN;
        if (r != CN)
            return r;
    }
    return CN;
}

static int exec_block(js_rt *rt, jnode *b, jenv *env)
{
    if (b->type != N_BLOCK)
        return exec(rt, b, env);
    if (b->u.scope) {
        jenv *e = new_env(rt, env, ENV_BLOCK, (uint32_t)(b->u.scope->n + 1));
        declare_scope(rt, b->u.scope, e);
        env = e;
    }
    return exec_list(rt, b->a, env);
}

static int exec_list(js_rt *rt, jnode *s, jenv *env)
{
    for (; s; s = s->next) {
        int c = exec(rt, s, env);
        if (c != CN)
            return c;
    }
    return CN;
}

static int exec(js_rt *rt, jnode *s, jenv *env)
{
    rt->line = s->line;
    switch (s->type) {
    case N_EXPR:
        rt->completion = js_eval(rt, s->a, env);
        return CN;
    case N_VAR:
        for (jnode *d = s->a; d; d = d->next) {
            if (s->op == VK_VAR) {
                if (d->b) {
                    jv v = js_eval(rt, d->b, env);
                    if (d->b->type == N_FUNC && v.t == JT_OBJ && !d->b->u.fn->name && d->a->type == N_IDENT)
                        js_define(rt, v.u.o, rt->A.name, jv_str(d->a->u.str), PF_CONF);
                    bind_pattern(rt, d->a, v, env, B_ASSIGN);
                }
            } else {
                jv v = d->b ? js_eval(rt, d->b, env) : jv_undef();
                if (d->b && d->b->type == N_FUNC && v.t == JT_OBJ && !d->b->u.fn->name && d->a->type == N_IDENT)
                    js_define(rt, v.u.o, rt->A.name, jv_str(d->a->u.str), PF_CONF);
                bind_pattern(rt, d->a, v, env, B_INIT);
            }
        }
        return CN;
    case N_FUNCDECL:
    case N_EMPTY:
    case N_DEBUGGER:
        return CN;
    case N_CLASSDECL: {
        jv c = eval_class(rt, s->a, env);
        init_var(rt, env, s->a->u.str, c);
        return CN;
    }
    case N_BLOCK:
        return exec_block(rt, s, env);
    case N_IF:
        if (js_truthy(js_eval(rt, s->a, env)))
            return exec(rt, s->b, env);
        if (s->c)
            return exec(rt, s->c, env);
        return CN;
    case N_FOR:
        return exec_for(rt, s, env);
    case N_FORIN:
    case N_FOROF: {
        jenv *le = env;
        if (s->u.scope) {
            le = new_env(rt, env, ENV_BLOCK, (uint32_t)s->u.scope->n);
            declare_scope(rt, s->u.scope, le);
        }
        return exec_forx(rt, s, le);
    }
    case N_WHILE:
        for (;;) {
            js_step(rt);
            if (!js_truthy(js_eval(rt, s->a, env)))
                break;
            int c = exec(rt, s->d, env), out;
            if (loop_result(rt, s, c, &out))
                return out;
        }
        return CN;
    case N_DOWHILE:
        for (;;) {
            js_step(rt);
            int c = exec(rt, s->d, env), out;
            if (loop_result(rt, s, c, &out))
                return out;
            if (!js_truthy(js_eval(rt, s->a, env)))
                break;
        }
        return CN;
    case N_RETURN:
        rt->ret = s->a ? js_eval(rt, s->a, env) : jv_undef();
        return CR;
    case N_BREAK:
        rt->label = s->u.str;
        return CB;
    case N_CONTINUE:
        rt->label = s->u.str;
        return CC;
    case N_THROW: {
        jv v = js_eval(rt, s->a, env);
        rt->line = s->line;
        js_throw(rt, v);
    }
    case N_TRY:
        return exec_try(rt, s, env);
    case N_SWITCH:
        return exec_switch(rt, s, env);
    case N_LABEL: {
        int c = exec(rt, s->a, env);
        if (c == CB && rt->label == s->u.str) {
            rt->label = 0;
            return CN;
        }
        return c;
    }
    }
    js_throw_error(rt, JS_ERR_SYNTAX, "Nieznana instrukcja (%d)", s->type);
}

/* ---------- uruchamianie ---------- */

int js_try_call(js_rt *rt, jv fn, jv self, int argc, jv *argv, jv *out)
{
    struct js_catch cat;
    cat.prev = rt->catch_top;
    cat.depth = rt->depth;
    rt->catch_top = &cat;
    if (js_setjmp(&cat.jb) == 0) {
        jv r = js_call(rt, fn, self, argc, argv);
        rt->catch_top = cat.prev;
        *out = r;
        return 0;
    }
    rt->catch_top = cat.prev;
    rt->depth = cat.depth;
    rt->chain_break = 0;
    if (rt->aborting)
        js_longjmp(&rt->catch_top->jb, 1);
    *out = rt->exc;
    return 1;
}

/* Program na poziomie globalnym: var i funkcje staja sie wlasciwosciami window */
jv js_run_program(js_rt *rt, const char *code, unsigned len)
{
    char err[200];
    struct jfuninfo *top;
    uint32_t saved_line = rt->line;
    jnode *prog = js_parse(rt, code, len, err, sizeof(err), &top);
    if (!prog)
        js_throw_error(rt, JS_ERR_SYNTAX, "%s", err);
    rt->line = saved_line;
    jenv *g = rt->genv;
    for (int i = 0; i < top->nvars; i++)
        if (!js_has_own(rt, rt->global, top->vars[i]) && !env_find(g, top->vars[i]))
            js_define(rt, rt->global, top->vars[i], jv_undef(), PF_ENUM | PF_WRITE);
    for (int i = 0; i < top->nfuncs; i++)
        js_set(rt, jv_obj(rt->global), top->funcs[i]->u.fn->name, jv_obj(js_new_func(rt, top->funcs[i], g)));
    struct jscope *sc = prog->u.scope;
    if (sc) {
        for (int i = 0; i < sc->n; i++) {
            struct jbind *b = env_find(g, sc->names[i]);
            if (b && !(b->flags & BF_TDZ))
                js_throw_error(rt, JS_ERR_SYNTAX, "Identifier '%s' has already been declared", name8(rt, sc->names[i]));
            if (!b)
                env_add(rt, g, sc->names[i], jv_undef(), BF_TDZ | BF_LEX | (sc->kinds[i] == VK_CONST ? BF_CONST : 0));
        }
        for (int i = 0; i < sc->nfuncs; i++)
            js_set(rt, jv_obj(rt->global), sc->funcs[i]->u.fn->name, jv_obj(js_new_func(rt, sc->funcs[i], g)));
    }
    rt->completion = jv_undef();
    exec_list(rt, prog->a, g);
    return rt->completion;
}

/* Funkcja z tekstu (new Function) - zawsze w zasiegu globalnym */
jobj *js_function_from_text(js_rt *rt, const char *params, const char *body)
{
    char err[200];
    jnode *fn = js_parse_function(rt, params, body, err, sizeof(err));
    if (!fn)
        js_throw_error(rt, JS_ERR_SYNTAX, "%s", err);
    return js_new_func(rt, fn, rt->genv);
}

struct protect_ctx {
    js_rt *rt;
    void (*fn)(js_rt *rt, void *arg);
    void *arg;
    int result;
    int outer;
};

static void protect_inner(void *p)
{
    struct protect_ctx *c = p;
    js_rt *rt = c->rt;
    struct js_catch cat;
    cat.prev = rt->catch_top;
    cat.depth = rt->depth;
    rt->catch_top = &cat;
    if (js_setjmp(&cat.jb) == 0) {
        c->fn(rt, c->arg);
        if (c->outer) {
            js_run_microtasks(rt);
            if (rt->nunhandled)
                js_report_unhandled(rt);
        }
        c->result = 0;
    } else {
        rt->depth = cat.depth;
        rt->chain_break = 0;
        if (rt->aborting && cat.prev) {             /* przerwanie idzie az na sam wierzch */
            rt->catch_top = cat.prev;
            js_longjmp(&cat.prev->jb, 1);
        }
        js_report_exception(rt);
        c->result = -1;
    }
    rt->catch_top = cat.prev;
}

int js_protect(js_rt *rt, void (*fn)(js_rt *rt, void *arg), void *arg)
{
    if (rt->dead)
        return -1;
    struct protect_ctx c = { rt, fn, arg, 0, 0 };
    if (rt->on_stack) {
        protect_inner(&c);
        return c.result;
    }
    if (!rt->stack) {
        rt->stack = kmalloc(JS_STACK_SIZE);
        if (!rt->stack)
            return -1;
        rt->stack_limit = (uintptr_t)rt->stack + 256 * 1024;
    }
    c.outer = 1;
    rt->on_stack = 1;
    rt->aborting = 0;
    rt->depth = 0;
    rt->run_start = rt->last_yield = timer_uptime_ms();
    rt->steps = 0;
    rt->next_check = 8192;
    js_on_stack(protect_inner, &c, rt->stack + JS_STACK_SIZE);
    rt->on_stack = 0;
    rt->aborting = 0;
    rt->catch_top = 0;
    return c.result;
}
