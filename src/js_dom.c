/* JavaScript i strona: obiekty document, window, elementy, zdarzenia,
 * timery. Tu tez jest interfejs dla przegladarki (js.h).
 *
 * Kazdy wezel drzewa DOM (dom.c) dostaje - dopiero gdy skrypt o niego
 * zapyta - obiekt JS ("opakowanie"). Wlasciwosci typu el.id czy
 * el.textContent to gettery/settery w prototypie Element.prototype, ktore
 * czytaja i zmieniaja wezel. Kazda zmiana drzewa ustawia znacznik "dirty" -
 * przegladarka liczy wtedy od nowa style (css.c) i uklad strony (layout.c).
 *
 * Zdarzenia (klikniecie, klawisz) ida jak w prawdziwych przegladarkach:
 * najpierw z gory w dol do elementu (capture), potem z elementu w gore
 * (bubbling) - kazdy przodek moze zareagowac. */

#include "js_int.h"
#include "string.h"
#include "heap.h"
#include "timer.h"
#include "rtc.h"
#include "dom.h"
#include "css.h"
#include "http.h"
#include "keyboard.h"

#define U __attribute__((unused))
#define NATIVE(name) static jv name(js_rt *rt U, jv self U, int argc U, jv *argv U, jobj *fn U)
#define ARG(i) ((i) < argc ? argv[i] : jv_undef())

#define JS_MEM_LIMIT   (256u << 20)
#define BUDGET_SCRIPT  20000                 /* duze aplikacje (Vue, React) startuja dlugo */
#define BUDGET_EVENT   3000

void js_parse_init(js_rt *rt);

/* ---------- pomocnicze ---------- */

static jv str8(js_rt *rt, const char *s)
{
    return jv_str(js_str_utf8(rt, s, (uint32_t)strlen(s)));
}

static char *u8(js_rt *rt, jv v, unsigned *len)
{
    return js_utf8_alloc(rt, js_to_str(rt, v), len);
}

static void dirty(js_rt *rt) { rt->dirty = 1; }

static int memchr_ws(const char *s)
{
    for (; *s; s++)
        if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f')
            return 1;
    return 0;
}

static int strncmp_c(const char *a, const char *b, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        if (a[i] != b[i])
            return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i])
            return 0;
    }
    return 0;
}

static struct dom_node *node_of(jv v)
{
    if (v.t == JT_OBJ && v.u.o->cls == JC_NODE)
        return v.u.o->u.dom.node;
    return 0;
}

static struct dom_node *NODE(js_rt *rt, jv self)
{
    struct dom_node *n = node_of(self);
    if (!n)
        js_throw_error(rt, JS_ERR_TYPE, "Illegal invocation");
    return n;
}

static int is_elem(const struct dom_node *n) { return n && n->type == N_ELEMENT && n->name[0] != '#'; }
static int is_frag(const struct dom_node *n) { return n && n->type == N_ELEMENT && strcmp(n->name, "#fragment") == 0; }

static struct dom_node *arg_node(js_rt *rt, jv v, const char *method)
{
    struct dom_node *n = node_of(v);
    if (!n)
        js_throw_error(rt, JS_ERR_TYPE, "Failed to execute '%s' on 'Node': parameter 1 is not of type 'Node'.", method);
    return n;
}

static jobj *proto_for(js_rt *rt, struct dom_node *n)
{
    if (n->type == N_TEXT)
        return rt->text_proto;
    if (rt->doc && n == rt->doc->root)
        return rt->document_proto;
    if (strcmp(n->name, "canvas") == 0 && rt->canvas_proto)
        return rt->canvas_proto;
    return rt->element_proto;
}

jv js_node_wrap(js_rt *rt, struct dom_node *n)
{
    if (!n)
        return jv_null();
    if (n->js)
        return jv_obj((jobj *)n->js);
    jobj *o = js_new_object(rt, proto_for(rt, n));
    o->cls = JC_NODE;
    o->u.dom.node = n;
    n->js = o;
    return jv_obj(o);
}

static struct dom_node *find_tag(struct dom_node *n, int tag)
{
    for (struct dom_node *c = n ? n->first : 0; c; c = c->next) {
        if (c->type == N_ELEMENT && c->tag == tag)
            return c;
        if (c->type == N_ELEMENT && (c->tag == T_HTML || strcmp(c->name, "#fragment") == 0)) {
            struct dom_node *r = find_tag(c, tag);
            if (r)
                return r;
        }
    }
    return 0;
}

static struct dom_node *doc_html(js_rt *rt) { return rt->doc ? find_tag(rt->doc->root, T_HTML) : 0; }
static struct dom_node *doc_body(js_rt *rt) { return find_tag(doc_html(rt), T_BODY); }
static struct dom_node *doc_head(js_rt *rt) { return find_tag(doc_html(rt), T_HEAD); }

/* Nastepny wezel w kolejnosci dokumentu (w obrebie root) */
static struct dom_node *next_in(struct dom_node *n, struct dom_node *root)
{
    if (n->first)
        return n->first;
    while (n && n != root) {
        if (n->next)
            return n->next;
        n = n->parent;
    }
    return 0;
}

static int connected(js_rt *rt, struct dom_node *n)
{
    while (n->parent)
        n = n->parent;
    return rt->doc && n == rt->doc->root;
}

static jobj *new_list(js_rt *rt)
{
    jobj *a = js_new_array(rt, 0);
    if (rt->nodelist_proto)
        a->proto = rt->nodelist_proto;
    return a;
}

/* ---------- tekst i HTML wezlow ---------- */

static void collect_text(struct jsb *b, struct dom_node *n, int inner)
{
    for (struct dom_node *c = n->first; c; c = c->next) {
        if (c->type == N_TEXT) {
            jsb_utf8(b, c->text, c->text_len);
        } else if (c->type == N_ELEMENT) {
            if (inner && (c->tag == T_SCRIPT || c->tag == T_STYLE || (c->style && c->style->display == D_NONE)))
                continue;
            if (inner && c->tag == T_BR) {
                jsb_char(b, '\n');
                continue;
            }
            collect_text(b, c, inner);
            if (inner && (c->tag == T_P || c->tag == T_DIV || c->tag == T_LI || c->tag == T_TR ||
                          (c->tag >= T_H1 && c->tag <= T_H6)) && b->len && b->c[b->len - 1] != '\n')
                jsb_char(b, '\n');
        }
    }
}

static jstr *text_of(js_rt *rt, struct dom_node *n, int inner)
{
    if (n->type == N_TEXT)
        return js_str_utf8(rt, n->text, n->text_len);
    struct jsb b;
    jsb_init(&b, rt);
    collect_text(&b, n, inner);
    if (inner)
        while (b.len && b.c[b.len - 1] == '\n')
            b.len--;
    return jsb_done(&b);
}

static const char *const void_tags[] = {
    "area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr", 0,
};

static int is_void(const char *name)
{
    for (int i = 0; void_tags[i]; i++)
        if (strcmp(name, void_tags[i]) == 0)
            return 1;
    return 0;
}

static void escape_into(struct jsb *b, const char *s, unsigned len, int attr)
{
    for (unsigned i = 0; i < len;) {
        char c = s[i];
        if (c == '&') { jsb_ascii(b, "&amp;"); i++; }
        else if (c == '<' && !attr) { jsb_ascii(b, "&lt;"); i++; }
        else if (c == '>' && !attr) { jsb_ascii(b, "&gt;"); i++; }
        else if (c == '"' && attr) { jsb_ascii(b, "&quot;"); i++; }
        else if ((uint8_t)c == 0xC2 && i + 1 < len && (uint8_t)s[i + 1] == 0xA0) { jsb_ascii(b, "&nbsp;"); i += 2; }
        else {
            unsigned st = i;
            while (i < len && s[i] != '&' && s[i] != '<' && s[i] != '>' && s[i] != '"' && (uint8_t)s[i] != 0xC2)
                i++;
            if (i == st)
                i++;
            jsb_utf8(b, s + st, i - st);
        }
    }
}

static void serialize(js_rt *rt, struct jsb *b, struct dom_node *n, int self, int depth)
{
    if (depth > 400)
        return;
    if (n->type == N_TEXT) {
        const struct dom_node *p = n->parent;
        if (p && (p->tag == T_SCRIPT || p->tag == T_STYLE))
            jsb_utf8(b, n->text, n->text_len);
        else
            escape_into(b, n->text, n->text_len, 0);
        return;
    }
    int real = n->name[0] != '#';
    if (self && real) {
        jsb_char(b, '<');
        jsb_utf8(b, n->name, (unsigned)strlen(n->name));
        for (struct dom_attr *a = n->attrs; a; a = a->next) {
            jsb_char(b, ' ');
            jsb_utf8(b, a->name, (unsigned)strlen(a->name));
            jsb_ascii(b, "=\"");
            escape_into(b, a->value, (unsigned)strlen(a->value), 1);
            jsb_char(b, '"');
        }
        jsb_char(b, '>');
        if (is_void(n->name))
            return;
    }
    for (struct dom_node *c = n->first; c; c = c->next)
        serialize(rt, b, c, 1, depth + 1);
    if (self && real) {
        jsb_ascii(b, "</");
        jsb_utf8(b, n->name, (unsigned)strlen(n->name));
        jsb_char(b, '>');
    }
}

static void remove_children(struct dom_node *n)
{
    while (n->first)
        dom_remove(n->first);
}

/* HTML (z innerHTML, insertAdjacentHTML) -> wezly; wynik: kontener #fragment */
static struct dom_node *parse_html(js_rt *rt, jv html)
{
    unsigned len;
    char *s = u8(rt, html, &len);
    struct dom_node *frag = dom_parse_fragment(rt->doc, s, len);
    if (!frag)
        js_throw_error(rt, JS_ERR_ERROR, "Brak pamieci na strone");
    return frag;
}

static void move_children_before(struct dom_node *from, struct dom_node *parent, struct dom_node *ref)
{
    while (from->first) {
        struct dom_node *c = from->first;
        if (ref)
            dom_insert_before(parent, c, ref);
        else
            dom_append(parent, c);
    }
}

/* ---------- Node.prototype ---------- */

NATIVE(n_node_type)
{
    struct dom_node *n = NODE(rt, self);
    if (n->type == N_TEXT)
        return jv_num(3);
    if (rt->doc && n == rt->doc->root)
        return jv_num(9);
    if (is_frag(n))
        return jv_num(11);
    return jv_num(1);
}

NATIVE(n_node_name)
{
    struct dom_node *n = NODE(rt, self);
    if (n->type == N_TEXT)
        return str8(rt, "#text");
    if (rt->doc && n == rt->doc->root)
        return str8(rt, "#document");
    if (is_frag(n))
        return str8(rt, "#document-fragment");
    jstr *s = js_str_utf8(rt, n->name, (uint32_t)strlen(n->name));
    jstr *u = js_str_new(rt, s->len);
    for (uint32_t i = 0; i < s->len; i++)
        u->c[i] = js_upper(s->c[i]);
    return jv_str(u);
}

NATIVE(n_node_value)
{
    struct dom_node *n = NODE(rt, self);
    return n->type == N_TEXT ? jv_str(js_str_utf8(rt, n->text, n->text_len)) : jv_null();
}

static void set_text_node(js_rt *rt, struct dom_node *n, jv v)
{
    unsigned len;
    char *s = u8(rt, v, &len);
    char *copy = arena_strndup(&rt->doc->mem, s, len);
    if (copy) {
        n->text = copy;
        n->text_len = len;
    }
    dirty(rt);
}

NATIVE(n_set_node_value)
{
    struct dom_node *n = NODE(rt, self);
    if (n->type == N_TEXT && rt->doc)
        set_text_node(rt, n, ARG(0));
    return jv_undef();
}

NATIVE(n_text_content)
{
    struct dom_node *n = NODE(rt, self);
    if (rt->doc && n == rt->doc->root)
        return jv_null();
    return jv_str(text_of(rt, n, 0));
}

NATIVE(n_set_text_content)
{
    struct dom_node *n = NODE(rt, self);
    if (!rt->doc)
        return jv_undef();
    if (n->type == N_TEXT) {
        set_text_node(rt, n, ARG(0));
        return jv_undef();
    }
    remove_children(n);
    jv v = ARG(0);
    if (!IS_NULLISH(v)) {
        unsigned len;
        char *s = u8(rt, v, &len);
        if (len) {
            struct dom_node *t = dom_new_text(rt->doc, s, len);
            if (t)
                dom_append(n, t);
        }
    }
    dirty(rt);
    return jv_undef();
}

NATIVE(n_parent_node)
{
    struct dom_node *n = NODE(rt, self);
    return js_node_wrap(rt, n->parent);
}

NATIVE(n_parent_element)
{
    struct dom_node *n = NODE(rt, self);
    return is_elem(n->parent) ? js_node_wrap(rt, n->parent) : jv_null();
}

NATIVE(n_child_nodes)
{
    struct dom_node *n = NODE(rt, self);
    jobj *a = new_list(rt);
    for (struct dom_node *c = n->first; c; c = c->next)
        js_array_push(rt, a, js_node_wrap(rt, c));
    return jv_obj(a);
}

NATIVE(n_first_child) { return js_node_wrap(rt, NODE(rt, self)->first); }
NATIVE(n_last_child) { return js_node_wrap(rt, NODE(rt, self)->last); }
NATIVE(n_next_sibling) { return js_node_wrap(rt, NODE(rt, self)->next); }
NATIVE(n_prev_sibling) { return js_node_wrap(rt, NODE(rt, self)->prev); }
NATIVE(n_owner_document) { return rt->document ? jv_obj(rt->document) : jv_null(); }
NATIVE(n_is_connected) { return jv_bool(connected(rt, NODE(rt, self))); }
NATIVE(n_has_child_nodes) { return jv_bool(NODE(rt, self)->first != 0); }

static void check_insert(js_rt *rt, struct dom_node *parent, struct dom_node *child)
{
    for (struct dom_node *p = parent; p; p = p->parent)
        if (p == child)
            js_throw_error(rt, JS_ERR_ERROR, "HierarchyRequestError: The new child element contains the parent.");
    if (parent->type == N_TEXT)
        js_throw_error(rt, JS_ERR_ERROR, "HierarchyRequestError: This node type does not support this method.");
}

static void run_inserted_scripts(js_rt *rt, struct dom_node *n);

static void insert_node(js_rt *rt, struct dom_node *parent, struct dom_node *child, struct dom_node *ref)
{
    if (is_frag(child)) {
        move_children_before(child, parent, ref);
    } else {
        check_insert(rt, parent, child);
        if (ref)
            dom_insert_before(parent, child, ref);
        else
            dom_append(parent, child);
        run_inserted_scripts(rt, child);
    }
    dirty(rt);
}

NATIVE(n_append_child)
{
    struct dom_node *p = NODE(rt, self), *c = arg_node(rt, ARG(0), "appendChild");
    insert_node(rt, p, c, 0);
    return argv[0];
}

NATIVE(n_insert_before)
{
    struct dom_node *p = NODE(rt, self), *c = arg_node(rt, ARG(0), "insertBefore");
    struct dom_node *ref = node_of(ARG(1));
    if (ref && ref->parent != p)
        js_throw_error(rt, JS_ERR_ERROR, "NotFoundError: The node before which the new node is to be inserted is not a child of this node.");
    insert_node(rt, p, c, ref);
    return argv[0];
}

NATIVE(n_remove_child)
{
    struct dom_node *p = NODE(rt, self), *c = arg_node(rt, ARG(0), "removeChild");
    if (c->parent != p)
        js_throw_error(rt, JS_ERR_ERROR, "NotFoundError: The node to be removed is not a child of this node.");
    dom_remove(c);
    dirty(rt);
    return argv[0];
}

NATIVE(n_replace_child)
{
    struct dom_node *p = NODE(rt, self), *nw = arg_node(rt, ARG(0), "replaceChild"), *old = arg_node(rt, ARG(1), "replaceChild");
    if (old->parent != p)
        js_throw_error(rt, JS_ERR_ERROR, "NotFoundError: The node to be replaced is not a child of this node.");
    if (nw != old) {
        struct dom_node *ref = old->next;
        dom_remove(old);
        insert_node(rt, p, nw, ref);
    }
    return argv[1];
}

static struct dom_node *clone(js_rt *rt, struct dom_node *n, int deep)
{
    struct dom_doc *d = rt->doc;
    struct dom_node *c = arena_alloc(&d->mem, sizeof(*c));
    if (!c)
        js_throw_error(rt, JS_ERR_ERROR, "Brak pamieci na strone");
    *c = *n;
    c->parent = c->first = c->last = c->next = c->prev = 0;
    c->style = 0;
    c->js = 0;
    c->mgen = 0;
    c->canvas = 0;                                   /* kopia plotna jest pusta (jak w przegladarkach) */
    c->attrs = 0;
    struct dom_attr **tail = &c->attrs;
    for (struct dom_attr *a = n->attrs; a; a = a->next) {
        struct dom_attr *na = arena_alloc(&d->mem, sizeof(*na));
        if (!na)
            break;
        *na = *a;
        na->next = 0;
        *tail = na;
        tail = &na->next;
    }
    d->nodes++;
    if (deep)
        for (struct dom_node *ch = n->first; ch; ch = ch->next)
            dom_append(c, clone(rt, ch, 1));
    return c;
}

NATIVE(n_clone_node)
{
    struct dom_node *n = NODE(rt, self);
    if (!rt->doc || n == rt->doc->root)
        js_throw_error(rt, JS_ERR_ERROR, "NotSupportedError");
    return js_node_wrap(rt, clone(rt, n, js_truthy(ARG(0))));
}

NATIVE(n_contains)
{
    struct dom_node *n = NODE(rt, self), *o = node_of(ARG(0));
    for (; o; o = o->parent)
        if (o == n)
            return jv_bool(1);
    return jv_bool(0);
}

NATIVE(n_remove)
{
    struct dom_node *n = NODE(rt, self);
    if (n->parent) {
        dom_remove(n);
        dirty(rt);
    }
    return jv_undef();
}

/* Argumenty append/prepend/before/after: wezly albo teksty -> fragment */
static struct dom_node *nodes_from_args(js_rt *rt, int argc, jv *argv)
{
    struct dom_node *frag = dom_new_element(rt->doc, "#fragment");
    if (!frag)
        js_throw_error(rt, JS_ERR_ERROR, "Brak pamieci na strone");
    frag->name = "#fragment";
    frag->tag = T_OTHER;
    for (int i = 0; i < argc; i++) {
        struct dom_node *n = node_of(argv[i]);
        if (n) {
            if (is_frag(n))
                move_children_before(n, frag, 0);
            else
                dom_append(frag, n);
        } else {
            unsigned len;
            char *s = u8(rt, argv[i], &len);
            struct dom_node *t = dom_new_text(rt->doc, s, len);
            if (t)
                dom_append(frag, t);
        }
    }
    return frag;
}

NATIVE(n_append)
{
    struct dom_node *n = NODE(rt, self);
    insert_node(rt, n, nodes_from_args(rt, argc, argv), 0);
    return jv_undef();
}

NATIVE(n_prepend)
{
    struct dom_node *n = NODE(rt, self);
    insert_node(rt, n, nodes_from_args(rt, argc, argv), n->first);
    return jv_undef();
}

NATIVE(n_before)
{
    struct dom_node *n = NODE(rt, self);
    if (n->parent)
        insert_node(rt, n->parent, nodes_from_args(rt, argc, argv), n);
    return jv_undef();
}

NATIVE(n_after)
{
    struct dom_node *n = NODE(rt, self);
    if (n->parent)
        insert_node(rt, n->parent, nodes_from_args(rt, argc, argv), n->next);
    return jv_undef();
}

NATIVE(n_replace_with)
{
    struct dom_node *n = NODE(rt, self);
    if (n->parent) {
        struct dom_node *p = n->parent, *ref = n->next;
        struct dom_node *frag = nodes_from_args(rt, argc, argv);
        dom_remove(n);
        insert_node(rt, p, frag, ref);
    }
    return jv_undef();
}

/* ---------- Element.prototype ---------- */

static struct dom_node *ELEM(js_rt *rt, jv self)
{
    struct dom_node *n = NODE(rt, self);
    if (n->type != N_ELEMENT)
        js_throw_error(rt, JS_ERR_TYPE, "Illegal invocation");
    return n;
}

static jv attr_value(js_rt *rt, struct dom_node *n, const char *name)
{
    const char *v = dom_attr(n, name);
    return v ? str8(rt, v) : jv_null();
}

static void set_attr(js_rt *rt, struct dom_node *n, const char *name, jv v)
{
    if (!rt->doc)
        return;
    char *val = u8(rt, v, 0);
    dom_set_attr(rt->doc, n, name, val);
    if (n->tag == T_IMG && strcmp(name, "src") == 0)
        dom_add_image(rt->doc, n);                  /* nowy obrazek - przegladarka go pobierze */
    dirty(rt);
}

static char *attr_name(js_rt *rt, jv v)
{
    char *s = u8(rt, v, 0);
    for (char *p = s; *p; p++)
        if (*p >= 'A' && *p <= 'Z')
            *p = (char)(*p + 32);
    return s;
}

NATIVE(e_get_attribute) { return attr_value(rt, ELEM(rt, self), attr_name(rt, ARG(0))); }
NATIVE(e_has_attribute) { return jv_bool(dom_attr(ELEM(rt, self), attr_name(rt, ARG(0))) != 0); }

NATIVE(e_set_attribute)
{
    struct dom_node *n = ELEM(rt, self);
    set_attr(rt, n, attr_name(rt, ARG(0)), ARG(1));
    return jv_undef();
}

NATIVE(e_remove_attribute)
{
    struct dom_node *n = ELEM(rt, self);
    dom_remove_attr(n, attr_name(rt, ARG(0)));
    dirty(rt);
    return jv_undef();
}

NATIVE(e_toggle_attribute)
{
    struct dom_node *n = ELEM(rt, self);
    char *name = attr_name(rt, ARG(0));
    int has = dom_attr(n, name) != 0;
    int want = argc > 1 ? js_truthy(argv[1]) : !has;
    if (want && !has)
        set_attr(rt, n, name, jv_str(rt->empty));
    else if (!want && has) {
        dom_remove_attr(n, name);
        dirty(rt);
    }
    return jv_bool(want);
}

NATIVE(e_get_attribute_names)
{
    struct dom_node *n = ELEM(rt, self);
    jobj *a = js_new_array(rt, 0);
    for (struct dom_attr *at = n->attrs; at; at = at->next)
        js_array_push(rt, a, str8(rt, at->name));
    return jv_obj(a);
}

NATIVE(e_tag_name) { return n_node_name(rt, self, argc, argv, fn); }
NATIVE(e_local_name) { return str8(rt, ELEM(rt, self)->name); }

/* Atrybuty odbijane we wlasciwosciach: el.id, el.title, el.alt... (nazwa w fn->u.nf.data) */
static jv reflect_get(js_rt *rt, jv self U, int argc U, jv *argv U, jobj *fn)
{
    struct dom_node *n = ELEM(rt, self);
    char name[40];
    js_utf8(fn->u.nf.data.u.s, name, sizeof(name));
    const char *v = dom_attr(n, name);
    return str8(rt, v ? v : "");
}

static jv reflect_set(js_rt *rt, jv self U, int argc, jv *argv, jobj *fn)
{
    struct dom_node *n = ELEM(rt, self);
    char name[40];
    js_utf8(fn->u.nf.data.u.s, name, sizeof(name));
    set_attr(rt, n, name, ARG(0));
    return jv_undef();
}

static jv bool_reflect_get(js_rt *rt, jv self U, int argc U, jv *argv U, jobj *fn)
{
    struct dom_node *n = ELEM(rt, self);
    char name[40];
    js_utf8(fn->u.nf.data.u.s, name, sizeof(name));
    return jv_bool(dom_attr(n, name) != 0);
}

static jv bool_reflect_set(js_rt *rt, jv self U, int argc, jv *argv, jobj *fn)
{
    struct dom_node *n = ELEM(rt, self);
    char name[40];
    js_utf8(fn->u.nf.data.u.s, name, sizeof(name));
    if (js_truthy(ARG(0))) {
        if (!dom_attr(n, name))
            set_attr(rt, n, name, jv_str(rt->empty));
    } else if (dom_attr(n, name)) {
        dom_remove_attr(n, name);
        dirty(rt);
    }
    return jv_undef();
}

static void def_reflect(js_rt *rt, jobj *proto, const char *prop, const char *attr, int boolean)
{
    char buf[60];
    ksnprintf(buf, sizeof(buf), "get %s", prop);
    jobj *g = js_new_native(rt, buf, boolean ? bool_reflect_get : reflect_get, 0);
    ksnprintf(buf, sizeof(buf), "set %s", prop);
    jobj *s = js_new_native(rt, buf, boolean ? bool_reflect_set : reflect_set, 1);
    g->u.nf.data = s->u.nf.data = jv_str(js_atom(rt, attr));
    js_define_acc(rt, proto, js_atom(rt, prop), g, s, PF_CONF);
}

/* href i src: pelny adres wzgledem strony */
/* Pierwszy znak c w tekscie s (NULL = nie ma) */
static const char *find_ch(const char *s, char c)
{
    for (; *s; s++)
        if (*s == c)
            return s;
    return 0;
}

static jv url_attr(js_rt *rt, struct dom_node *n, const char *name)
{
    const char *v = dom_attr(n, name);
    if (!v)
        return jv_str(rt->empty);
    char abs[URL_MAX];
    if (rt->url[0] && !str_starts_ci(v, "javascript:") && !str_starts_ci(v, "data:") && !str_starts_ci(v, "mailto:") &&
        url_resolve(rt->url, v, abs, sizeof(abs))) {
        const char *frag = find_ch(v, '#');           /* url_resolve gubi kotwice (#...) - dopisujemy */
        if (frag && !find_ch(abs, '#') && strlen(abs) + strlen(frag) < sizeof(abs))
            memcpy(abs + strlen(abs), frag, strlen(frag) + 1);
        return str8(rt, abs);
    }
    return str8(rt, v);
}

NATIVE(e_href) { return url_attr(rt, ELEM(rt, self), "href"); }
NATIVE(e_src) { return url_attr(rt, ELEM(rt, self), "src"); }
NATIVE(e_set_href) { set_attr(rt, ELEM(rt, self), "href", ARG(0)); return jv_undef(); }
NATIVE(e_set_src) { set_attr(rt, ELEM(rt, self), "src", ARG(0)); return jv_undef(); }

NATIVE(e_class_name)
{
    struct dom_node *n = ELEM(rt, self);
    return str8(rt, n->cls ? n->cls : "");
}

NATIVE(e_set_class_name)
{
    set_attr(rt, ELEM(rt, self), "class", ARG(0));
    return jv_undef();
}

NATIVE(e_inner_html)
{
    struct dom_node *n = NODE(rt, self);
    struct jsb b;
    jsb_init(&b, rt);
    serialize(rt, &b, n, 0, 0);
    return jv_str(jsb_done(&b));
}

NATIVE(e_set_inner_html)
{
    struct dom_node *n = NODE(rt, self);
    if (!rt->doc)
        return jv_undef();
    remove_children(n);
    jv v = ARG(0);
    if (!IS_NULLISH(v)) {
        struct dom_node *frag = parse_html(rt, v);
        move_children_before(frag, n, 0);
    }
    dirty(rt);
    return jv_undef();
}

NATIVE(e_outer_html)
{
    struct dom_node *n = NODE(rt, self);
    struct jsb b;
    jsb_init(&b, rt);
    serialize(rt, &b, n, 1, 0);
    return jv_str(jsb_done(&b));
}

NATIVE(e_set_outer_html)
{
    struct dom_node *n = ELEM(rt, self);
    if (!n->parent || !rt->doc)
        return jv_undef();
    struct dom_node *frag = parse_html(rt, ARG(0)), *p = n->parent, *ref = n->next;
    dom_remove(n);
    move_children_before(frag, p, ref);
    dirty(rt);
    return jv_undef();
}

NATIVE(e_inner_text) { return jv_str(text_of(rt, NODE(rt, self), 1)); }

NATIVE(e_insert_adjacent_html)
{
    struct dom_node *n = ELEM(rt, self);
    char *where = attr_name(rt, ARG(0));
    struct dom_node *frag = parse_html(rt, ARG(1));
    if (strcmp(where, "beforebegin") == 0 && n->parent)
        move_children_before(frag, n->parent, n);
    else if (strcmp(where, "afterbegin") == 0)
        move_children_before(frag, n, n->first);
    else if (strcmp(where, "beforeend") == 0)
        move_children_before(frag, n, 0);
    else if (strcmp(where, "afterend") == 0 && n->parent)
        move_children_before(frag, n->parent, n->next);
    else
        js_throw_error(rt, JS_ERR_SYNTAX, "The value provided ('%s') is not one of 'beforeBegin', 'afterBegin', 'beforeEnd', or 'afterEnd'.", where);
    dirty(rt);
    return jv_undef();
}

NATIVE(e_insert_adjacent_element)
{
    struct dom_node *n = ELEM(rt, self), *c = arg_node(rt, ARG(1), "insertAdjacentElement");
    char *where = attr_name(rt, ARG(0));
    if (strcmp(where, "beforebegin") == 0 && n->parent)
        insert_node(rt, n->parent, c, n);
    else if (strcmp(where, "afterbegin") == 0)
        insert_node(rt, n, c, n->first);
    else if (strcmp(where, "beforeend") == 0)
        insert_node(rt, n, c, 0);
    else if (strcmp(where, "afterend") == 0 && n->parent)
        insert_node(rt, n->parent, c, n->next);
    return argv[1];
}

NATIVE(e_insert_adjacent_text)
{
    struct dom_node *n = ELEM(rt, self);
    unsigned len;
    char *s = u8(rt, ARG(1), &len);
    struct dom_node *t = dom_new_text(rt->doc, s, len);
    if (!t)
        return jv_undef();
    jv tv = js_node_wrap(rt, t);
    jv args[2] = { ARG(0), tv };
    (void)args;
    char *where = attr_name(rt, ARG(0));
    if (strcmp(where, "beforebegin") == 0 && n->parent)
        insert_node(rt, n->parent, t, n);
    else if (strcmp(where, "afterbegin") == 0)
        insert_node(rt, n, t, n->first);
    else if (strcmp(where, "beforeend") == 0)
        insert_node(rt, n, t, 0);
    else if (strcmp(where, "afterend") == 0 && n->parent)
        insert_node(rt, n->parent, t, n->next);
    return jv_undef();
}

static jv element_list(js_rt *rt, struct dom_node *first, int forward)
{
    jobj *a = new_list(rt);
    for (struct dom_node *c = first; c; c = forward ? c->next : c->prev)
        if (is_elem(c))
            js_array_push(rt, a, js_node_wrap(rt, c));
    return jv_obj(a);
}

NATIVE(e_children) { return element_list(rt, NODE(rt, self)->first, 1); }

NATIVE(e_child_element_count)
{
    int n = 0;
    for (struct dom_node *c = NODE(rt, self)->first; c; c = c->next)
        n += is_elem(c);
    return jv_num(n);
}

NATIVE(e_first_element_child)
{
    for (struct dom_node *c = NODE(rt, self)->first; c; c = c->next)
        if (is_elem(c))
            return js_node_wrap(rt, c);
    return jv_null();
}

NATIVE(e_last_element_child)
{
    for (struct dom_node *c = NODE(rt, self)->last; c; c = c->prev)
        if (is_elem(c))
            return js_node_wrap(rt, c);
    return jv_null();
}

NATIVE(e_next_element_sibling)
{
    for (struct dom_node *c = NODE(rt, self)->next; c; c = c->next)
        if (is_elem(c))
            return js_node_wrap(rt, c);
    return jv_null();
}

NATIVE(e_prev_element_sibling)
{
    for (struct dom_node *c = NODE(rt, self)->prev; c; c = c->prev)
        if (is_elem(c))
            return js_node_wrap(rt, c);
    return jv_null();
}

/* ---- selektory ---- */

static struct css_sel *selector(js_rt *rt, jv text)
{
    /* wlasny kontekst CSS tylko na selektory (querySelector) */
    if (!rt->css) {
        rt->css = css_new(0);
        if (!rt->css)
            return 0;
    }
    jstr *key = js_intern(rt, js_to_str(rt, text));
    for (int i = 0; i < 64; i++)
        if (rt->sel_cache[i].text == key)
            return rt->sel_cache[i].sel;
    char *s = js_utf8_alloc(rt, key, 0);
    struct css_sel *sel = css_sel_parse(rt->css, s);
    rt->sel_cache[rt->sel_next].text = key;
    rt->sel_cache[rt->sel_next].sel = sel;
    rt->sel_next = (rt->sel_next + 1) % 64;
    return sel;
}

static jv query(js_rt *rt, struct dom_node *root, jv text, int all)
{
    struct css_sel *sel = selector(rt, text);
    jobj *out = all ? new_list(rt) : 0;
    if (!sel)
        return all ? jv_obj(out) : jv_null();
    unsigned steps = 0;
    for (struct dom_node *n = root->first ? next_in(root, root) : 0; n; n = next_in(n, root)) {
        if (++steps % 4096 == 0)
            js_step(rt);
        if (n->type != N_ELEMENT || !css_sel_match(sel, n))
            continue;
        if (!all)
            return js_node_wrap(rt, n);
        js_array_push(rt, out, js_node_wrap(rt, n));
    }
    return all ? jv_obj(out) : jv_null();
}

NATIVE(e_query_selector) { return query(rt, NODE(rt, self), ARG(0), 0); }
NATIVE(e_query_selector_all) { return query(rt, NODE(rt, self), ARG(0), 1); }

NATIVE(e_matches)
{
    struct css_sel *sel = selector(rt, ARG(0));
    return jv_bool(sel && css_sel_match(sel, ELEM(rt, self)));
}

NATIVE(e_closest)
{
    struct css_sel *sel = selector(rt, ARG(0));
    for (struct dom_node *n = ELEM(rt, self); sel && is_elem(n); n = n->parent)
        if (css_sel_match(sel, n))
            return js_node_wrap(rt, n);
    return jv_null();
}

static int name_eq_ci(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (x != y)
            return 0;
    }
    return *a == *b;
}

static jv by_tag(js_rt *rt, struct dom_node *root, jv tag)
{
    char *t = u8(rt, tag, 0);
    int any = strcmp(t, "*") == 0;
    jobj *out = new_list(rt);
    for (struct dom_node *n = next_in(root, root); n; n = next_in(n, root))
        if (is_elem(n) && (any || name_eq_ci(n->name, t)))
            js_array_push(rt, out, js_node_wrap(rt, n));
    return jv_obj(out);
}

static jv by_class(js_rt *rt, struct dom_node *root, jv names)
{
    char *s = u8(rt, names, 0);
    jobj *out = new_list(rt);
    for (struct dom_node *n = next_in(root, root); n; n = next_in(n, root)) {
        if (!is_elem(n) || !n->cls)
            continue;
        int ok = 1, any = 0;
        for (const char *p = s; *p && ok;) {
            while (*p == ' ')
                p++;
            const char *w = p;
            while (*p && *p != ' ')
                p++;
            if (p > w) {
                any = 1;
                ok = dom_has_class(n, w, (unsigned)(p - w));
            }
        }
        if (ok && any)
            js_array_push(rt, out, js_node_wrap(rt, n));
    }
    return jv_obj(out);
}

NATIVE(e_get_by_tag) { return by_tag(rt, NODE(rt, self), ARG(0)); }
NATIVE(e_get_by_class) { return by_class(rt, NODE(rt, self), ARG(0)); }

/* ---- classList ---- */

static struct dom_node *CL(js_rt *rt, jv self)
{
    if (self.t != JT_OBJ || self.u.o->cls != JC_CLASSLIST)
        js_throw_error(rt, JS_ERR_TYPE, "Illegal invocation");
    return self.u.o->u.dom.node;
}

/* Klasy jako lista slow; zmiana = nowy atrybut class */
static int class_words(const char *cls, const char **w, unsigned *l, int max)
{
    int n = 0;
    for (const char *p = cls ? cls : ""; *p && n < max;) {
        while (*p == ' ' || *p == '\t' || *p == '\n')
            p++;
        const char *s = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n')
            p++;
        if (p > s) {
            w[n] = s;
            l[n++] = (unsigned)(p - s);
        }
    }
    return n;
}

static void class_write(js_rt *rt, struct dom_node *n, const char **w, unsigned *l, int cnt)
{
    unsigned total = 1;
    for (int i = 0; i < cnt; i++)
        total += l[i] + 1;
    char *buf = js_alloc(rt, total);
    unsigned k = 0;
    for (int i = 0; i < cnt; i++) {
        if (k)
            buf[k++] = ' ';
        memcpy(buf + k, w[i], l[i]);
        k += l[i];
    }
    buf[k] = 0;
    dom_set_attr(rt->doc, n, "class", buf);
    dirty(rt);
}

static int class_find(const char **w, unsigned *l, int cnt, const char *tok, unsigned tl)
{
    for (int i = 0; i < cnt; i++)
        if (l[i] == tl && memcmp(w[i], tok, tl) == 0)
            return i;
    return -1;
}

#define CLS_MAX 64

static jv classlist_op(js_rt *rt, jv self, int argc, jv *argv, int op)
{
    struct dom_node *n = CL(rt, self);
    const char *w[CLS_MAX + 8];
    unsigned l[CLS_MAX + 8];
    int cnt = class_words(n->cls, w, l, CLS_MAX);
    int changed = 0, result = 0;
    for (int i = 0; i < argc && (op < 2 || i == 0); i++) {
        unsigned tl;
        char *tok = u8(rt, argv[i], &tl);
        if (!tl || memchr_ws(tok))
            js_throw_error(rt, JS_ERR_SYNTAX, "The token provided must not be empty or contain whitespace.");
        int at = class_find(w, l, cnt, tok, tl);
        int want = op == 0 ? 1 : op == 1 ? 0 : op == 2 ? (argc > 1 ? js_truthy(argv[1]) : at < 0) : -1;
        if (op == 3)
            return jv_bool(at >= 0);
        if (want && at < 0 && cnt < CLS_MAX + 8) {
            w[cnt] = tok;
            l[cnt++] = tl;
            changed = 1;
        } else if (!want && at >= 0) {
            for (int k = at; k + 1 < cnt; k++) {
                w[k] = w[k + 1];
                l[k] = l[k + 1];
            }
            cnt--;
            changed = 1;
        }
        result = want;
    }
    if (changed && rt->doc)
        class_write(rt, n, w, l, cnt);
    return op == 2 ? jv_bool(result) : jv_undef();
}

NATIVE(cl_add) { return classlist_op(rt, self, argc, argv, 0); }
NATIVE(cl_remove) { return classlist_op(rt, self, argc, argv, 1); }
NATIVE(cl_toggle) { return classlist_op(rt, self, argc, argv, 2); }
NATIVE(cl_contains) { return argc ? classlist_op(rt, self, 1, argv, 3) : jv_bool(0); }

NATIVE(cl_replace)
{
    struct dom_node *n = CL(rt, self);
    const char *w[CLS_MAX];
    unsigned l[CLS_MAX];
    int cnt = class_words(n->cls, w, l, CLS_MAX);
    unsigned ol, nl;
    char *old = u8(rt, ARG(0), &ol), *nw = u8(rt, ARG(1), &nl);
    int at = class_find(w, l, cnt, old, ol);
    if (at < 0)
        return jv_bool(0);
    w[at] = nw;
    l[at] = nl;
    class_write(rt, n, w, l, cnt);
    return jv_bool(1);
}

NATIVE(cl_length)
{
    const char *w[CLS_MAX];
    unsigned l[CLS_MAX];
    return jv_num(class_words(CL(rt, self)->cls, w, l, CLS_MAX));
}

NATIVE(cl_item)
{
    const char *w[CLS_MAX];
    unsigned l[CLS_MAX];
    int cnt = class_words(CL(rt, self)->cls, w, l, CLS_MAX);
    double i = js_to_integer(rt, ARG(0));
    if (i < 0 || i >= cnt)
        return jv_null();
    return jv_str(js_str_utf8(rt, w[(int)i], l[(int)i]));
}

NATIVE(cl_value)
{
    struct dom_node *n = CL(rt, self);
    return str8(rt, n->cls ? n->cls : "");
}

NATIVE(cl_for_each)
{
    const char *w[CLS_MAX];
    unsigned l[CLS_MAX];
    int cnt = class_words(CL(rt, self)->cls, w, l, CLS_MAX);
    jv f = ARG(0);
    for (int i = 0; i < cnt && js_is_callable(f); i++) {
        jv args[3] = { jv_str(js_str_utf8(rt, w[i], l[i])), jv_num(i), self };
        js_call(rt, f, ARG(1), 3, args);
    }
    return jv_undef();
}

static jobj *sub_object(js_rt *rt, jv self, int cls, jobj *proto)
{
    struct dom_node *n = ELEM(rt, self);
    jobj *w = self.u.o;
    jobj **slot = cls == JC_CLASSLIST ? &w->u.dom.classlist : cls == JC_STYLE ? &w->u.dom.style : &w->u.dom.dataset;
    if (!*slot) {
        jobj *o = js_new_object(rt, proto);
        o->cls = (uint8_t)cls;
        o->u.dom.node = n;
        *slot = o;
    }
    return *slot;
}

NATIVE(e_class_list) { return jv_obj(sub_object(rt, self, JC_CLASSLIST, rt->classlist_proto)); }
NATIVE(e_style) { return jv_obj(sub_object(rt, self, JC_STYLE, rt->style_proto)); }
NATIVE(e_dataset) { return jv_obj(sub_object(rt, self, JC_DATASET, rt->object_proto)); }

/* ---- style="..." ---- */

/* "backgroundColor" -> "background-color"; "cssFloat" -> "float" */
static int css_name(jstr *key, char *out, unsigned size)
{
    unsigned n = 0;
    if (js_str_is(key, "cssFloat")) {
        ksnprintf(out, size, "float");
        return 1;
    }
    for (uint32_t i = 0; i < key->len; i++) {
        uint16_t c = key->c[i];
        if (c >= 128 || n + 3 >= size)
            return 0;
        if (c >= 'A' && c <= 'Z') {
            if (i == 0 || (i > 0 && 0))
                out[n++] = '-';
            else
                out[n++] = '-';
            out[n++] = (char)(c + 32);
        } else if ((c >= 'a' && c <= 'z') || c == '-' || (c >= '0' && c <= '9')) {
            out[n++] = (char)c;
        } else {
            return 0;
        }
    }
    out[n] = 0;
    /* webkitTransform -> -webkit-transform */
    if (str_starts_ci(out, "webkit-") || str_starts_ci(out, "moz-") || str_starts_ci(out, "ms-")) {
        if (n + 2 < size) {
            memmove(out + 1, out, n + 1);
            out[0] = '-';
        }
    }
    return n > 0;
}

/* Deklaracje style="a: b; c: d" - znajdz wlasciwosc */
static int style_find(const char *s, const char *prop, const char **vs, unsigned *vl, const char **ds, const char **de)
{
    unsigned pl = (unsigned)strlen(prop);
    const char *p = s;
    while (p && *p) {
        const char *start = p;
        const char *end = p;
        int depth = 0;
        char q = 0;
        while (*end && (depth || q || *end != ';')) {
            if (q) { if (*end == q) q = 0; }
            else if (*end == '"' || *end == '\'') q = *end;
            else if (*end == '(') depth++;
            else if (*end == ')') depth--;
            end++;
        }
        const char *colon = start;
        while (colon < end && *colon != ':')
            colon++;
        const char *ns = start, *ne = colon;
        while (ns < ne && (*ns == ' ' || *ns == '\t' || *ns == '\n'))
            ns++;
        while (ne > ns && (ne[-1] == ' ' || ne[-1] == '\t' || ne[-1] == '\n'))
            ne--;
        if (colon < end && (unsigned)(ne - ns) == pl) {
            int eq = 1;
            for (unsigned i = 0; i < pl && eq; i++) {
                char c = ns[i] >= 'A' && ns[i] <= 'Z' ? ns[i] + 32 : ns[i];
                eq = c == prop[i];
            }
            if (eq) {
                const char *v = colon + 1, *ve = end;
                while (v < ve && (*v == ' ' || *v == '\t'))
                    v++;
                while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t' || ve[-1] == '\n'))
                    ve--;
                *vs = v;
                *vl = (unsigned)(ve - v);
                *ds = start;
                *de = *end ? end + 1 : end;
                return 1;
            }
        }
        p = *end ? end + 1 : end;
    }
    return 0;
}

static jv style_get(js_rt *rt, struct dom_node *n, const char *prop)
{
    const char *s = dom_attr(n, "style"), *v, *ds, *de;
    unsigned vl;
    if (s && style_find(s, prop, &v, &vl, &ds, &de)) {
        /* bez "!important" */
        const char *imp = v;
        for (unsigned i = 0; i + 1 < vl; i++)
            if (v[i] == '!') {
                imp = v + i;
                break;
            }
        if (imp != v) {
            while (imp > v && imp[-1] == ' ')
                imp--;
            vl = (unsigned)(imp - v);
        }
        return jv_str(js_str_utf8(rt, v, vl));
    }
    return jv_str(rt->empty);
}

static void style_set(js_rt *rt, struct dom_node *n, const char *prop, const char *value)
{
    if (!rt->doc)
        return;
    const char *s = dom_attr(n, "style"), *v, *ds, *de;
    unsigned vl;
    struct jsb b;
    jsb_init(&b, rt);
    const char *rest = s ? s : "";
    if (s && style_find(s, prop, &v, &vl, &ds, &de)) {
        jsb_utf8(&b, s, (unsigned)(ds - s));
        rest = de;
    } else if (s) {
        jsb_utf8(&b, s, (unsigned)strlen(s));
        rest = "";
    }
    /* reszta deklaracji po zmienianej */
    jsb_utf8(&b, rest, (unsigned)strlen(rest));
    while (b.len && (b.c[b.len - 1] == ' ' || b.c[b.len - 1] == ';'))
        b.len--;
    uint32_t lead = 0;
    while (lead < b.len && (b.c[lead] == ' ' || b.c[lead] == ';'))
        lead++;
    if (lead) {
        memmove(b.c, b.c + lead, (b.len - lead) * 2);
        b.len -= lead;
    }
    if (value && value[0]) {
        if (b.len)
            jsb_ascii(&b, "; ");
        jsb_ascii(&b, prop);
        jsb_ascii(&b, ": ");
        jsb_utf8(&b, value, (unsigned)strlen(value));
    }
    if (b.len)
        jsb_char(&b, ';');
    char *out = js_utf8_alloc(rt, jsb_done(&b), 0);
    dom_set_attr(rt->doc, n, "style", out);
    dirty(rt);
}

static char *style_value(js_rt *rt, jv v)
{
    if (IS_NULLISH(v))
        return 0;
    return u8(rt, v, 0);
}

int js_dom_get_hook(js_rt *rt, jobj *o, jstr *key, jv *out)
{
    struct dom_node *n = o->u.dom.node;
    if (o->cls == JC_STYLE) {
        if (js_find(o->proto, key) || key->len == 0 || key->c[0] == '@')
            return 0;
        if (key == rt->A.cssText) {
            const char *s = dom_attr(n, "style");
            *out = str8(rt, s ? s : "");
            return 1;
        }
        if (key == rt->A.length) {
            int cnt = 0;
            for (const char *s = dom_attr(n, "style"); s && *s; s++)
                cnt += *s == ':';
            *out = jv_num(cnt);
            return 1;
        }
        char prop[64];
        if (!css_name(key, prop, sizeof(prop)))
            return 0;
        *out = style_get(rt, n, prop);
        return 1;
    }
    /* dataset: data-nazwa-atrybutu */
    char prop[80];
    if (!css_name(key, prop + 5, sizeof(prop) - 5))
        return 0;
    memcpy(prop, "data-", 5);
    const char *v = dom_attr(n, prop);
    if (!v)
        return 0;
    *out = str8(rt, v);
    return 1;
}

int js_dom_set_hook(js_rt *rt, jobj *o, jstr *key, jv v)
{
    struct dom_node *n = o->u.dom.node;
    if (o->cls == JC_STYLE) {
        if (js_find(o->proto, key) || key->len == 0 || key->c[0] == '@')
            return 0;
        if (key == rt->A.cssText) {
            set_attr(rt, n, "style", IS_NULLISH(v) ? jv_str(rt->empty) : v);
            return 1;
        }
        char prop[64];
        if (!css_name(key, prop, sizeof(prop)))
            return 0;
        style_set(rt, n, prop, style_value(rt, v));
        return 1;
    }
    char prop[80];
    if (!css_name(key, prop + 5, sizeof(prop) - 5))
        return 0;
    memcpy(prop, "data-", 5);
    if (v.t == JT_UNDEF && !dom_attr(n, prop))
        return 1;
    set_attr(rt, n, prop, v);
    return 1;
}

int js_dom_has_hook(js_rt *rt, jobj *o, jstr *key)
{
    jv tmp;
    if (o->cls == JC_STYLE)
        return js_dom_get_hook(rt, o, key, &tmp);
    return js_dom_get_hook(rt, o, key, &tmp);
}

void js_dom_keys_hook(js_rt *rt, jobj *o, jobj *out)
{
    if (o->cls != JC_DATASET)
        return;
    for (struct dom_attr *a = o->u.dom.node->attrs; a; a = a->next) {
        if (strncmp_c(a->name, "data-", 5) != 0)
            continue;
        struct jsb b;
        jsb_init(&b, rt);
        for (const char *p = a->name + 5; *p; p++) {
            if (*p == '-' && p[1]) {
                p++;
                jsb_char(&b, (uint16_t)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p));
            } else {
                jsb_char(&b, (uint8_t)*p);
            }
        }
        js_array_push(rt, out, jv_str(js_intern(rt, jsb_done(&b))));
    }
}

NATIVE(st_get_property_value)
{
    if (self.t != JT_OBJ || self.u.o->cls != JC_STYLE)
        return jv_str(rt->empty);
    char *p = attr_name(rt, ARG(0));
    return style_get(rt, self.u.o->u.dom.node, p);
}

NATIVE(st_set_property)
{
    if (self.t != JT_OBJ || self.u.o->cls != JC_STYLE)
        return jv_undef();
    char *p = attr_name(rt, ARG(0));
    style_set(rt, self.u.o->u.dom.node, p, style_value(rt, ARG(1)));
    return jv_undef();
}

NATIVE(st_remove_property)
{
    if (self.t != JT_OBJ || self.u.o->cls != JC_STYLE)
        return jv_str(rt->empty);
    char *p = attr_name(rt, ARG(0));
    jv old = style_get(rt, self.u.o->u.dom.node, p);
    style_set(rt, self.u.o->u.dom.node, p, 0);
    return old;
}

/* ---- inne wlasciwosci elementow ---- */

NATIVE(e_value)
{
    struct dom_node *n = ELEM(rt, self);
    if (n->tag == T_SELECT) {
        struct dom_node *first = 0;
        for (struct dom_node *o = next_in(n, n); o; o = next_in(o, n)) {
            if (o->type != N_ELEMENT || o->tag != T_OPTION)
                continue;
            if (!first)
                first = o;
            if (dom_attr(o, "selected")) {
                first = o;
                break;
            }
        }
        if (!first)
            return jv_str(rt->empty);
        const char *v = dom_attr(first, "value");
        return v ? str8(rt, v) : jv_str(text_of(rt, first, 0));
    }
    if (n->tag == T_TEXTAREA)
        return jv_str(text_of(rt, n, 0));
    const char *v = dom_attr(n, "value");
    if (!v && n->tag == T_OPTION)
        return jv_str(text_of(rt, n, 0));
    return str8(rt, v ? v : "");
}

NATIVE(e_set_value)
{
    struct dom_node *n = ELEM(rt, self);
    if (n->tag == T_SELECT) {
        char *want = u8(rt, ARG(0), 0);
        for (struct dom_node *o = next_in(n, n); o; o = next_in(o, n)) {
            if (o->type != N_ELEMENT || o->tag != T_OPTION)
                continue;
            const char *v = dom_attr(o, "value");
            if (v && strcmp(v, want) == 0)
                set_attr(rt, o, "selected", jv_str(rt->empty));
            else
                dom_remove_attr(o, "selected");
        }
        dirty(rt);
        return jv_undef();
    }
    if (n->tag == T_TEXTAREA)
        return n_set_text_content(rt, self, argc, argv, fn);
    set_attr(rt, n, "value", ARG(0));
    return jv_undef();
}

static int box_of(js_rt *rt, struct dom_node *n, int *x, int *y, int *w, int *h)
{
    *x = *y = *w = *h = 0;
    if (!rt->host.box)
        return 0;
    return rt->host.box(rt->host.ctx, n, x, y, w, h);
}

enum { BX_W, BX_H, BX_X, BX_Y };

static jv box_value(js_rt *rt, jv self, int what)
{
    struct dom_node *n = ELEM(rt, self);
    int x, y, w, h;
    box_of(rt, n, &x, &y, &w, &h);
    return jv_num(what == BX_W ? w : what == BX_H ? h : what == BX_X ? x : y);
}

NATIVE(e_offset_width) { return box_value(rt, self, BX_W); }
NATIVE(e_offset_height) { return box_value(rt, self, BX_H); }
NATIVE(e_offset_left) { return box_value(rt, self, BX_X); }
NATIVE(e_offset_top) { return box_value(rt, self, BX_Y); }
NATIVE(e_zero) { return jv_num(0); }

NATIVE(e_get_bounding_client_rect)
{
    struct dom_node *n = ELEM(rt, self);
    int x, y, w, h;
    box_of(rt, n, &x, &y, &w, &h);
    int sy = rt->host.scroll_y ? rt->host.scroll_y(rt->host.ctx) : 0;
    jobj *r = js_new_object(rt, rt->object_proto);
    js_def_val(rt, r, "x", jv_num(x));
    js_def_val(rt, r, "y", jv_num(y - sy));
    js_def_val(rt, r, "left", jv_num(x));
    js_def_val(rt, r, "top", jv_num(y - sy));
    js_def_val(rt, r, "width", jv_num(w));
    js_def_val(rt, r, "height", jv_num(h));
    js_def_val(rt, r, "right", jv_num(x + w));
    js_def_val(rt, r, "bottom", jv_num(y - sy + h));
    return jv_obj(r);
}

NATIVE(e_scroll_into_view)
{
    struct dom_node *n = ELEM(rt, self);
    int x, y, w, h;
    if (box_of(rt, n, &x, &y, &w, &h) && rt->host.scroll_to)
        rt->host.scroll_to(rt->host.ctx, y);
    return jv_undef();
}

NATIVE(e_focus)
{
    rt->active = ELEM(rt, self);
    return jv_undef();
}

NATIVE(e_blur)
{
    if (rt->active == ELEM(rt, self))
        rt->active = 0;
    return jv_undef();
}

NATIVE(e_noop) { return jv_undef(); }

/* ---------- zdarzenia ---------- */

static struct jlistener **listeners_of(js_rt *rt, jv target)
{
    if (target.t != JT_OBJ)
        return 0;
    if (target.u.o == rt->global)
        return &rt->win_listeners;
    if (target.u.o->cls == JC_NODE)
        return &target.u.o->u.dom.listeners;
    return 0;
}

static void listener_options(js_rt *rt, jv opt, int *capture, int *once)
{
    *capture = *once = 0;
    if (opt.t == JT_OBJ) {
        *capture = js_truthy(js_get(rt, opt, rt->A.capture));
        *once = js_truthy(js_get(rt, opt, rt->A.once));
    } else {
        *capture = js_truthy(opt);
    }
}

static int is_key_event(jstr *t)
{
    return js_str_is(t, "keydown") || js_str_is(t, "keyup") || js_str_is(t, "keypress");
}

NATIVE(et_add_event_listener)
{
    struct jlistener **list = listeners_of(rt, self);
    jv f = ARG(1);
    if (!list || (!js_is_callable(f) && f.t != JT_OBJ))
        return jv_undef();
    jstr *type = js_intern(rt, js_to_str(rt, ARG(0)));
    int capture, once;
    listener_options(rt, ARG(2), &capture, &once);
    struct jlistener **tail = list;
    for (; *tail; tail = &(*tail)->next)
        if (!(*tail)->removed && (*tail)->type == type && (*tail)->capture == capture && js_strict_eq((*tail)->fn, f))
            return jv_undef();
    struct jlistener *l = js_alloc(rt, sizeof(*l));
    l->type = type;
    l->fn = f;
    l->capture = (uint8_t)capture;
    l->once = (uint8_t)once;
    *tail = l;
    if (is_key_event(type))
        rt->wants_keys = 1;
    return jv_undef();
}

NATIVE(et_remove_event_listener)
{
    struct jlistener **list = listeners_of(rt, self);
    if (!list)
        return jv_undef();
    jstr *type = js_intern(rt, js_to_str(rt, ARG(0)));
    int capture, once;
    listener_options(rt, ARG(2), &capture, &once);
    for (struct jlistener *l = *list; l; l = l->next)
        if (!l->removed && l->type == type && l->capture == capture && js_strict_eq(l->fn, ARG(1))) {
            l->removed = 1;
            break;
        }
    return jv_undef();
}

static jobj *make_event(js_rt *rt, jstr *type, int bubbles, int cancelable)
{
    jobj *e = js_new_object(rt, rt->event_proto);
    e->cls = JC_EVENT;
    e->u.ev.bubbles = (uint8_t)bubbles;
    js_define(rt, e, rt->A.type, jv_str(type), PF_ENUM);
    js_define(rt, e, rt->A.bubbles, jv_bool(bubbles), PF_ENUM);
    js_def_val(rt, e, "cancelable", jv_bool(cancelable));
    js_define(rt, e, rt->A.timeStamp, jv_num(timer_uptime_ms() - rt->clock_base_uptime), PF_ENUM);
    js_def_val(rt, e, "isTrusted", jv_bool(1));
    js_define(rt, e, rt->A.target, jv_null(), PF_ENUM | PF_WRITE);
    js_define(rt, e, rt->A.currentTarget, jv_null(), PF_ENUM | PF_WRITE);
    return e;
}

static void report(js_rt *rt, jv exc)
{
    rt->exc = exc;
    js_report_exception(rt);
}

/* Handler z atrybutu HTML: onclick="kod" */
static jv attr_handler(js_rt *rt, struct dom_node *n, jstr *type)
{
    if (!n || n->type != N_ELEMENT)
        return jv_undef();
    char name[48], t[40];
    js_utf8(type, t, sizeof(t));
    ksnprintf(name, sizeof(name), "on%s", t);
    const char *code = dom_attr(n, name);
    if (!code || !code[0])
        return jv_undef();
    struct jobj *f = js_function_from_text(rt, "event", code);
    return jv_obj(f);
}

/* Wszyscy sluchacze jednego celu (fazy: 1 capture, 2 cel, 3 bubbling) */
static void invoke(js_rt *rt, jv cur, jobj *ev, jstr *type, int phase)
{
    js_set(rt, jv_obj(ev), rt->A.currentTarget, cur);
    struct jlistener **list = listeners_of(rt, cur);
    struct jlistener *snap[64];
    int n = 0;
    for (struct jlistener *l = list ? *list : 0; l && n < 64; l = l->next)
        if (!l->removed && l->type == type && (phase == 2 || (phase == 1) == (l->capture != 0)))
            snap[n++] = l;
    for (int i = 0; i < n; i++) {
        struct jlistener *l = snap[i];
        if (l->removed)
            continue;
        if (l->once)
            l->removed = 1;
        jv f = l->fn, self = cur, r;
        if (!js_is_callable(f)) {
            self = f;
            f = js_get(rt, f, rt->A.handleEvent);
            if (!js_is_callable(f))
                continue;
        }
        jv arg = jv_obj(ev);
        if (js_try_call(rt, f, self, 1, &arg, &r))
            report(rt, r);
        if (ev->u.ev.stop_now)
            return;
    }
    if (phase == 1)
        return;
    /* el.onclick = function... albo <button onclick="..."> */
    char nm[48], t[40];
    js_utf8(type, t, sizeof(t));
    ksnprintf(nm, sizeof(nm), "on%s", t);
    jv h = js_get(rt, cur, js_atom(rt, nm));
    if (!js_is_callable(h)) {
        struct dom_node *node = node_of(cur);
        if (!node && cur.t == JT_OBJ && cur.u.o == rt->global && (js_str_is(type, "load") || js_str_is(type, "unload")))
            node = doc_body(rt);                     /* <body onload="..."> */
        h = attr_handler(rt, node, type);
    }
    if (js_is_callable(h)) {
        jv arg = jv_obj(ev), r;
        if (js_try_call(rt, h, cur, 1, &arg, &r))
            report(rt, r);
        else if (r.t == JT_BOOL && !r.u.b)
            ev->u.ev.prevented = 1;
    }
}

/* Wysyla zdarzenie do celu (wezel albo window). 1 = nikt nie zablokowal domyslnej akcji */
static int fire(js_rt *rt, struct dom_node *target, jobj *ev)
{
    jstr *type = js_get(rt, jv_obj(ev), rt->A.type).u.s;
    jv path[256];
    int n = 0;
    if (target) {
        for (struct dom_node *p = target; p && n < 250; p = p->parent)
            path[n++] = js_node_wrap(rt, p);
        if (connected(rt, target))
            path[n++] = jv_obj(rt->global);
    } else {
        path[n++] = jv_obj(rt->global);
    }
    js_set(rt, jv_obj(ev), rt->A.target, path[0]);
    ev->u.ev.target = path[0].u.o;
    for (int i = n - 1; i > 0 && !ev->u.ev.stop; i--)
        invoke(rt, path[i], ev, type, 1);
    if (!ev->u.ev.stop)
        invoke(rt, path[0], ev, type, 2);
    if (ev->u.ev.bubbles)
        for (int i = 1; i < n && !ev->u.ev.stop; i++)
            invoke(rt, path[i], ev, type, 3);
    js_set(rt, jv_obj(ev), rt->A.currentTarget, jv_null());
    return !ev->u.ev.prevented;
}

NATIVE(et_dispatch_event)
{
    jv e = ARG(0);
    if (e.t != JT_OBJ || e.u.o->cls != JC_EVENT)
        js_throw_error(rt, JS_ERR_TYPE, "Failed to execute 'dispatchEvent' on 'EventTarget': parameter 1 is not of type 'Event'.");
    e.u.o->u.ev.stop = e.u.o->u.ev.stop_now = 0;
    if (self.t == JT_OBJ && self.u.o == rt->global)
        return jv_bool(fire(rt, 0, e.u.o));
    return jv_bool(fire(rt, NODE(rt, self), e.u.o));
}

static jobj *EV(js_rt *rt, jv self)
{
    if (self.t != JT_OBJ || self.u.o->cls != JC_EVENT)
        js_throw_error(rt, JS_ERR_TYPE, "Illegal invocation");
    return self.u.o;
}

NATIVE(ev_prevent_default) { EV(rt, self)->u.ev.prevented = 1; return jv_undef(); }
NATIVE(ev_stop_propagation) { EV(rt, self)->u.ev.stop = 1; return jv_undef(); }
NATIVE(ev_stop_immediate) { jobj *e = EV(rt, self); e->u.ev.stop = e->u.ev.stop_now = 1; return jv_undef(); }
NATIVE(ev_default_prevented) { return jv_bool(EV(rt, self)->u.ev.prevented); }
NATIVE(ev_composed_path) { return jv_obj(js_new_array(rt, 0)); }

NATIVE(ev_init_event)
{
    jobj *e = EV(rt, self);
    js_set(rt, self, rt->A.type, jv_str(js_intern(rt, js_to_str(rt, ARG(0)))));
    e->u.ev.bubbles = (uint8_t)js_truthy(ARG(1));
    js_set(rt, self, rt->A.bubbles, jv_bool(e->u.ev.bubbles));
    return jv_undef();
}

static jv event_ctor(js_rt *rt, jv self U, int argc, jv *argv, jobj *fn U)
{
    if (!argc)
        js_throw_error(rt, JS_ERR_TYPE, "Failed to construct 'Event': 1 argument required, but only 0 present.");
    jv opt = ARG(1);
    int bubbles = opt.t == JT_OBJ && js_truthy(js_get(rt, opt, rt->A.bubbles));
    int cancelable = opt.t == JT_OBJ && js_truthy(js_get(rt, opt, js_atom(rt, "cancelable")));
    jobj *e = make_event(rt, js_intern(rt, js_to_str(rt, argv[0])), bubbles, cancelable);
    js_def_val(rt, e, "isTrusted", jv_bool(0));
    if (opt.t == JT_OBJ) {                          /* detail, key, clientX... - przepisujemy */
        jobj *keys = js_own_keys(rt, opt.u.o, 1);
        for (uint32_t i = 0; i < keys->u.arr.len; i++) {
            jstr *k = keys->u.arr.v[i].u.s;
            if (k != rt->A.bubbles)
                js_define(rt, e, k, js_get(rt, opt, k), PF_ENUM | PF_WRITE);
        }
    }
    if (!js_has_own(rt, e, rt->A.detail))
        js_define(rt, e, rt->A.detail, jv_null(), PF_ENUM | PF_WRITE);
    return jv_obj(e);
}

NATIVE(e_click)
{
    struct dom_node *n = ELEM(rt, self);
    jobj *ev = make_event(rt, js_atom(rt, "click"), 1, 1);
    js_define(rt, ev, rt->A.button, jv_num(0), PF_ENUM);
    int ok = fire(rt, n, ev);
    /* <a href> klikniety ze skryptu - przejscie do strony */
    for (struct dom_node *a = n; ok && a; a = a->parent)
        if (a->type == N_ELEMENT && a->tag == T_A && dom_attr(a, "href")) {
            jv href = url_attr(rt, a, "href");
            char *h = u8(rt, href, 0);
            if (rt->host.navigate && h[0] && !str_starts_ci(h, "javascript:"))
                rt->host.navigate(rt->host.ctx, h);
            break;
        }
    return jv_undef();
}

/* ---------- document ---------- */

NATIVE(d_get_element_by_id)
{
    if (!rt->doc)
        return jv_null();
    char *id = u8(rt, ARG(0), 0);
    struct dom_node *root = rt->doc->root;
    unsigned steps = 0;
    for (struct dom_node *n = next_in(root, root); n; n = next_in(n, root)) {
        if (++steps % 8192 == 0)
            js_step(rt);
        if (n->type == N_ELEMENT && n->id && strcmp(n->id, id) == 0)
            return js_node_wrap(rt, n);
    }
    return jv_null();
}

NATIVE(d_get_elements_by_name)
{
    char *name = u8(rt, ARG(0), 0);
    jobj *out = new_list(rt);
    struct dom_node *root = rt->doc ? rt->doc->root : 0;
    for (struct dom_node *n = root ? next_in(root, root) : 0; n; n = next_in(n, root)) {
        const char *v = n->type == N_ELEMENT ? dom_attr(n, "name") : 0;
        if (v && strcmp(v, name) == 0)
            js_array_push(rt, out, js_node_wrap(rt, n));
    }
    return jv_obj(out);
}

NATIVE(d_create_element)
{
    if (!rt->doc)
        js_throw_error(rt, JS_ERR_ERROR, "Brak dokumentu");
    char *name = u8(rt, ARG(0), 0);
    if (!name[0] || strlen(name) > 30)
        js_throw_error(rt, JS_ERR_ERROR, "InvalidCharacterError: The tag name provided is not a valid name.");
    struct dom_node *n = dom_new_element(rt->doc, name);
    if (!n)
        js_throw_error(rt, JS_ERR_ERROR, "Brak pamieci na strone");
    return js_node_wrap(rt, n);
}

NATIVE(d_create_text_node)
{
    if (!rt->doc)
        js_throw_error(rt, JS_ERR_ERROR, "Brak dokumentu");
    unsigned len;
    char *s = u8(rt, ARG(0), &len);
    struct dom_node *n = dom_new_text(rt->doc, s, len);
    if (!n)
        js_throw_error(rt, JS_ERR_ERROR, "Brak pamieci na strone");
    return js_node_wrap(rt, n);
}

NATIVE(d_create_fragment)
{
    if (!rt->doc)
        js_throw_error(rt, JS_ERR_ERROR, "Brak dokumentu");
    struct dom_node *n = dom_new_element(rt->doc, "fragment");
    if (!n)
        js_throw_error(rt, JS_ERR_ERROR, "Brak pamieci na strone");
    n->name = "#fragment";
    n->tag = T_OTHER;
    return js_node_wrap(rt, n);
}

NATIVE(d_create_event)
{
    jobj *e = make_event(rt, rt->empty, 0, 0);
    return jv_obj(e);
}

NATIVE(d_body) { return js_node_wrap(rt, doc_body(rt)); }
NATIVE(d_head) { return js_node_wrap(rt, doc_head(rt)); }
NATIVE(d_document_element) { return js_node_wrap(rt, doc_html(rt)); }

NATIVE(d_title) { return str8(rt, rt->doc ? rt->doc->title : ""); }

NATIVE(d_set_title)
{
    if (rt->doc) {
        js_utf8(js_to_str(rt, ARG(0)), rt->doc->title, sizeof(rt->doc->title));
        dirty(rt);
    }
    return jv_undef();
}

NATIVE(d_ready_state)
{
    static const char *const st[] = { "loading", "interactive", "complete" };
    return str8(rt, st[rt->ready_state]);
}

NATIVE(d_url) { return str8(rt, rt->url); }
NATIVE(d_cookie) { return jv_str(rt->cookie ? rt->cookie : rt->empty); }

NATIVE(d_set_cookie)
{
    /* "nazwa=wartosc; path=/; expires=..." - zapamietujemy tylko nazwa=wartosc */
    jstr *s = js_to_str(rt, ARG(0));
    uint32_t e = 0;
    while (e < s->len && s->c[e] != ';')
        e++;
    jstr *pair = js_substr(rt, s, 0, e);
    uint32_t eq = 0;
    while (eq < pair->len && pair->c[eq] != '=')
        eq++;
    jstr *name = js_substr(rt, pair, 0, eq);
    /* usun stare ciasteczko o tej nazwie */
    struct jsb b;
    jsb_init(&b, rt);
    jstr *old = rt->cookie ? rt->cookie : rt->empty;
    uint32_t i = 0;
    while (i < old->len) {
        uint32_t j = i;
        while (j < old->len && old->c[j] != ';')
            j++;
        uint32_t k = i;
        while (k < j && old->c[k] == ' ')
            k++;
        uint32_t q = k;
        while (q < j && old->c[q] != '=')
            q++;
        if (!(q - k == name->len && memcmp(old->c + k, name->c, name->len * 2) == 0)) {
            if (b.len)
                jsb_ascii(&b, "; ");
            jsb_str(&b, js_substr(rt, old, k, j));
        }
        i = j + 1;
    }
    int expired = 0;
    for (uint32_t k = e; k + 8 <= s->len; k++)
        if (js_lower(s->c[k]) == 'm' && js_lower(s->c[k + 1]) == 'a' && js_lower(s->c[k + 2]) == 'x' && s->c[k + 3] == '-' &&
            js_lower(s->c[k + 4]) == 'a' && js_lower(s->c[k + 5]) == 'g' && js_lower(s->c[k + 6]) == 'e' && s->c[k + 7] == '=' &&
            k + 8 < s->len && (s->c[k + 8] == '0' || s->c[k + 8] == '-'))
            expired = 1;
    if (!expired && pair->len) {
        if (b.len)
            jsb_ascii(&b, "; ");
        jsb_str(&b, pair);
    }
    rt->cookie = jsb_done(&b);
    return jv_undef();
}

NATIVE(d_active_element)
{
    if (rt->active && connected(rt, rt->active))
        return js_node_wrap(rt, rt->active);
    return js_node_wrap(rt, doc_body(rt));
}

NATIVE(d_default_view) { return jv_obj(rt->global); }
NATIVE(d_has_focus) { return jv_bool(1); }
NATIVE(d_visibility) { return str8(rt, "visible"); }
NATIVE(d_false) { return jv_bool(0); }
NATIVE(d_empty_string) { return jv_str(rt->empty); }

NATIVE(d_domain)
{
    struct url u;
    if (url_parse(rt->url, &u))
        return str8(rt, u.host);
    return jv_str(rt->empty);
}

static void doc_write(js_rt *rt, int argc, jv *argv, int newline)
{
    if (!rt->doc)
        return;
    struct jsb b;
    jsb_init(&b, rt);
    for (int i = 0; i < argc; i++)
        jsb_str(&b, js_to_str(rt, argv[i]));
    if (newline)
        jsb_char(&b, '\n');
    struct dom_node *frag = parse_html(rt, jv_str(jsb_done(&b)));
    struct dom_node *anchor = rt->write_after && rt->write_after->parent ? rt->write_after : 0;
    struct dom_node *parent = anchor ? anchor->parent : doc_body(rt);
    if (!parent)
        return;
    while (frag->first) {
        struct dom_node *c = frag->first;
        if (anchor) {
            dom_insert_before(parent, c, anchor->next);
            anchor = c;
        } else {
            dom_append(parent, c);
        }
    }
    if (anchor)
        rt->write_after = anchor;
    dirty(rt);
}

NATIVE(d_write) { doc_write(rt, argc, argv, 0); return jv_undef(); }
NATIVE(d_writeln) { doc_write(rt, argc, argv, 1); return jv_undef(); }

static jv collection(js_rt *rt, int tag, const char *attr)
{
    jobj *out = new_list(rt);
    struct dom_node *root = rt->doc ? rt->doc->root : 0;
    for (struct dom_node *n = root ? next_in(root, root) : 0; n; n = next_in(n, root))
        if (n->type == N_ELEMENT && n->tag == tag && (!attr || dom_attr(n, attr)))
            js_array_push(rt, out, js_node_wrap(rt, n));
    return jv_obj(out);
}

NATIVE(d_forms) { return collection(rt, T_FORM, 0); }
NATIVE(d_images) { return collection(rt, T_IMG, 0); }
NATIVE(d_links) { return collection(rt, T_A, "href"); }
NATIVE(d_scripts) { return collection(rt, T_SCRIPT, 0); }
NATIVE(d_current_script) { return js_node_wrap(rt, rt->cur_script); }

/* ---------- window ---------- */

NATIVE(w_alert)
{
    char buf[512];
    js_utf8(argc ? js_to_str(rt, argv[0]) : rt->empty, buf, sizeof(buf));
    if (rt->host.alert)
        rt->host.alert(rt->host.ctx, buf);
    return jv_undef();
}

NATIVE(w_confirm)
{
    w_alert(rt, self, argc, argv, fn);
    return jv_bool(1);
}

NATIVE(w_prompt)
{
    w_alert(rt, self, argc, argv, fn);
    if (argc > 1)
        return jv_str(js_to_str(rt, argv[1]));
    return jv_null();
}

NATIVE(w_inner_width) { return jv_num(rt->host.view_w); }
NATIVE(w_inner_height) { return jv_num(rt->host.view_h); }
NATIVE(w_scroll_y) { return jv_num(rt->host.scroll_y ? rt->host.scroll_y(rt->host.ctx) : 0); }
NATIVE(w_zero) { return jv_num(0); }
NATIVE(w_one) { return jv_num(1); }

NATIVE(w_scroll_to)
{
    double y = 0;
    if (ARG(0).t == JT_OBJ)
        y = js_to_num(rt, js_get(rt, argv[0], js_atom(rt, "top")));
    else
        y = js_to_num(rt, ARG(1));
    if (js_isfinite(y) && rt->host.scroll_to)
        rt->host.scroll_to(rt->host.ctx, (int)y);
    return jv_undef();
}

NATIVE(w_scroll_by)
{
    double dy = ARG(0).t == JT_OBJ ? js_to_num(rt, js_get(rt, argv[0], js_atom(rt, "top"))) : js_to_num(rt, ARG(1));
    int cur = rt->host.scroll_y ? rt->host.scroll_y(rt->host.ctx) : 0;
    if (js_isfinite(dy) && rt->host.scroll_to)
        rt->host.scroll_to(rt->host.ctx, cur + (int)dy);
    return jv_undef();
}

static const char *const display_names[] = {
    "none", "inline", "block", "list-item", "inline-block", "table", "table-row-group", "table-row",
    "table-cell", "table-caption", "flex",
};

static jv rgb(js_rt *rt, uint32_t c)
{
    char buf[40];
    ksnprintf(buf, sizeof(buf), "rgb(%u, %u, %u)", c >> 16 & 255, c >> 8 & 255, c & 255);
    return str8(rt, buf);
}

NATIVE(cs_get_property_value)
{
    jstr *name = js_to_str(rt, ARG(0));
    struct jsb b;
    jsb_init(&b, rt);
    for (uint32_t i = 0; i < name->len; i++) {
        if (name->c[i] == '-' && i + 1 < name->len) {
            jsb_char(&b, js_upper(name->c[++i]));
        } else {
            jsb_char(&b, name->c[i]);
        }
    }
    jv v = js_get(rt, self, js_intern(rt, jsb_done(&b)));
    return v.t == JT_UNDEF ? jv_str(rt->empty) : v;
}

NATIVE(w_get_computed_style)
{
    struct dom_node *n = node_of(ARG(0));
    jobj *o = js_new_object(rt, rt->object_proto);
    js_def_fn(rt, o, "getPropertyValue", cs_get_property_value, 1);
    if (!n || n->type != N_ELEMENT || !n->style)
        return jv_obj(o);
    const struct style *st = n->style;
    char buf[32];
    js_def_val(rt, o, "display", str8(rt, st->display < sizeof(display_names) / sizeof(display_names[0]) ? display_names[st->display] : "block"));
    js_def_val(rt, o, "color", rgb(rt, st->color));
    js_def_val(rt, o, "backgroundColor", st->has_bg ? rgb(rt, st->bg) : str8(rt, "rgba(0, 0, 0, 0)"));
    ksnprintf(buf, sizeof(buf), "%dpx", st->font_px);
    js_def_val(rt, o, "fontSize", str8(rt, buf));
    js_def_val(rt, o, "fontWeight", str8(rt, st->bold ? "700" : "400"));
    js_def_val(rt, o, "fontStyle", str8(rt, st->italic ? "italic" : "normal"));
    js_def_val(rt, o, "visibility", str8(rt, st->hidden ? "hidden" : "visible"));
    js_def_val(rt, o, "position", str8(rt, st->out_of_flow ? "absolute" : "static"));
    js_def_val(rt, o, "textAlign", str8(rt, st->align == AL_CENTER ? "center" : st->align == AL_RIGHT ? "right" : "left"));
    int x, y, w, h;
    box_of(rt, n, &x, &y, &w, &h);
    ksnprintf(buf, sizeof(buf), "%dpx", w);
    js_def_val(rt, o, "width", str8(rt, buf));
    ksnprintf(buf, sizeof(buf), "%dpx", h);
    js_def_val(rt, o, "height", str8(rt, buf));
    return jv_obj(o);
}

static int media_matches(js_rt *rt, const char *q)
{
    /* (min-width: 600px), (max-width: 900px), (prefers-color-scheme: dark) */
    int ok = 1;
    for (const char *p = q; *p; p++) {
        if (*p != '(')
            continue;
        const char *e = p;
        while (*e && *e != ')')
            e++;
        int val = 0;
        for (const char *d = p; d < e; d++)
            if (*d >= '0' && *d <= '9') {
                val = 0;
                while (d < e && *d >= '0' && *d <= '9')
                    val = val * 10 + (*d++ - '0');
                break;
            }
        if (str_starts_ci(p + 1, "min-width"))
            ok &= rt->host.view_w >= val;
        else if (str_starts_ci(p + 1, "max-width"))
            ok &= rt->host.view_w <= val;
        else if (str_starts_ci(p + 1, "min-height"))
            ok &= rt->host.view_h >= val;
        else if (str_starts_ci(p + 1, "max-height"))
            ok &= rt->host.view_h <= val;
        else if (str_starts_ci(p + 1, "prefers-color-scheme"))
            ok &= strstr_ci(p, "light") != 0;
        else if (str_starts_ci(p + 1, "prefers-reduced-motion"))
            ok &= strstr_ci(p, "no-preference") != 0;
        else if (str_starts_ci(p + 1, "hover") || str_starts_ci(p + 1, "pointer"))
            ok &= strstr_ci(p, "none") == 0 && strstr_ci(p, "coarse") == 0;
        p = *e ? e : e - 1;
    }
    return ok;
}

NATIVE(w_match_media)
{
    jstr *q = js_to_str(rt, ARG(0));
    char *s = js_utf8_alloc(rt, q, 0);
    jobj *o = js_new_object(rt, rt->object_proto);
    js_def_val(rt, o, "matches", jv_bool(media_matches(rt, s)));
    js_def_val(rt, o, "media", jv_str(q));
    js_def_val(rt, o, "onchange", jv_null());
    js_def_fn(rt, o, "addListener", e_noop, 1);
    js_def_fn(rt, o, "removeListener", e_noop, 1);
    js_def_fn(rt, o, "addEventListener", e_noop, 2);
    js_def_fn(rt, o, "removeEventListener", e_noop, 2);
    return jv_obj(o);
}

NATIVE(w_open)
{
    jv u = ARG(0);
    if (u.t != JT_UNDEF && rt->host.navigate) {
        char *s = u8(rt, u, 0), abs[URL_MAX];
        if (s[0] && url_resolve(rt->url, s, abs, sizeof(abs)))
            rt->host.navigate(rt->host.ctx, abs);
    }
    return jv_null();
}

/* ---- location ---- */

/* Czesc adresu: 0 href, 1 protocol, 2 host, 3 hostname, 4 port, 5 pathname, 6 search, 7 hash, 8 origin */
static jv url_part(js_rt *rt, const char *url, int part)
{
    struct url u;
    if (!url_parse(url, &u))
        return part == 0 ? str8(rt, url) : jv_str(rt->empty);
    char buf[URL_MAX];
    const char *path = u.path, *q = path, *h, *qe;
    while (*q && *q != '?' && *q != '#')
        q++;
    qe = q;                                         /* koniec ?zapytania */
    while (*qe && *qe != '#')
        qe++;
    h = qe;
    if (!*h) {                                      /* kotwica mogla zostac tylko w tekscie adresu */
        const char *hh = find_ch(url, '#');
        if (hh)
            h = hh;
    }
    switch (part) {
    case 0: return str8(rt, url);
    case 1: return str8(rt, u.https ? "https:" : "http:");
    case 2:
        if ((u.https && u.port != 443) || (!u.https && u.port != 80))
            ksnprintf(buf, sizeof(buf), "%s:%u", u.host, u.port);
        else
            ksnprintf(buf, sizeof(buf), "%s", u.host);
        return str8(rt, buf);
    case 3: return str8(rt, u.host);
    case 4:
        if ((u.https && u.port != 443) || (!u.https && u.port != 80)) {
            ksnprintf(buf, sizeof(buf), "%u", u.port);
            return str8(rt, buf);
        }
        return jv_str(rt->empty);
    case 5: return jv_str(js_str_utf8(rt, path, (uint32_t)(q - path)));
    case 6: return *q == '?' && qe - q > 1 ? jv_str(js_str_utf8(rt, q, (uint32_t)(qe - q))) : jv_str(rt->empty);
    case 7: return *h == '#' && h[1] ? str8(rt, h) : jv_str(rt->empty);
    default:
        ksnprintf(buf, sizeof(buf), "%s://%s", u.https ? "https" : "http", u.host);
        if ((u.https && u.port != 443) || (!u.https && u.port != 80))
            ksnprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), ":%u", u.port);
        return str8(rt, buf);
    }
}

static jv loc_part(js_rt *rt, int part) { return url_part(rt, rt->url, part); }

/* <a href> / <area href>: a.pathname, a.host... (biblioteki parsuja tak adresy) */
static jv a_part(js_rt *rt, jv self, int part)
{
    struct dom_node *n = ELEM(rt, self);
    if (!n || (strcmp(n->name, "a") != 0 && strcmp(n->name, "area") != 0))
        return jv_undef();
    jv h = url_attr(rt, n, "href");
    if (h.t != JT_STR || !h.u.s->len)
        return jv_str(rt->empty);
    return url_part(rt, u8(rt, h, 0), part);
}

NATIVE(a_protocol) { return a_part(rt, self, 1); }
NATIVE(a_host) { return a_part(rt, self, 2); }
NATIVE(a_hostname) { return a_part(rt, self, 3); }
NATIVE(a_port) { return a_part(rt, self, 4); }
NATIVE(a_pathname) { return a_part(rt, self, 5); }
NATIVE(a_search) { return a_part(rt, self, 6); }
NATIVE(a_hash) { return a_part(rt, self, 7); }
NATIVE(a_origin) { return a_part(rt, self, 8); }

/* ---- customElements: rejestr wlasnych znacznikow (bez "ozywiania" elementow) ---- */

NATIVE(ce_define)
{
    jobj *reg = fn->u.nf.data.u.o;
    jstr *name = js_to_str(rt, ARG(0));
    if (js_has_own(rt, reg, js_intern(rt, name)))
        js_throw_error(rt, JS_ERR_ERROR, "the name has already been used with this registry");
    js_set(rt, jv_obj(reg), js_intern(rt, name), ARG(1));
    return jv_undef();
}

NATIVE(ce_get)
{
    jobj *reg = fn->u.nf.data.u.o;
    jstr *name = js_intern(rt, js_to_str(rt, ARG(0)));
    return js_has_own(rt, reg, name) ? js_get(rt, jv_obj(reg), name) : jv_undef();
}

NATIVE(ce_when_defined)
{
    jobj *reg = fn->u.nf.data.u.o, *p = js_new_promise(rt);
    jstr *name = js_intern(rt, js_to_str(rt, ARG(0)));
    if (js_has_own(rt, reg, name))
        js_promise_resolve(rt, p, js_get(rt, jv_obj(reg), name));
    return jv_obj(p);                                /* niezdefiniowany: czeka (jak w przegladarce) */
}

NATIVE(ce_noop) { return jv_undef(); }

NATIVE(loc_href) { return loc_part(rt, 0); }
NATIVE(loc_protocol) { return loc_part(rt, 1); }
NATIVE(loc_host) { return loc_part(rt, 2); }
NATIVE(loc_hostname) { return loc_part(rt, 3); }
NATIVE(loc_port) { return loc_part(rt, 4); }
NATIVE(loc_pathname) { return loc_part(rt, 5); }
NATIVE(loc_search) { return loc_part(rt, 6); }
NATIVE(loc_hash) { return loc_part(rt, 7); }
NATIVE(loc_origin) { return loc_part(rt, 8); }

static void go_to(js_rt *rt, jv where)
{
    char *s = u8(rt, where, 0), abs[URL_MAX];
    if (!rt->host.navigate || !s[0])
        return;
    if (s[0] == '#') {                              /* tylko kotwica - zostajemy na stronie */
        char *h = rt->url;
        while (*h && *h != '#')
            h++;
        ksnprintf(h, sizeof(rt->url) - (size_t)(h - rt->url), "%s", s);
        return;
    }
    if (url_resolve(rt->url, s, abs, sizeof(abs)))
        rt->host.navigate(rt->host.ctx, abs);
}

NATIVE(loc_set_href) { go_to(rt, ARG(0)); return jv_undef(); }
NATIVE(loc_assign) { go_to(rt, ARG(0)); return jv_undef(); }

NATIVE(loc_set_hash)
{
    jstr *h = js_to_str(rt, ARG(0));
    jv v = jv_str(h->len && h->c[0] == '#' ? h : js_concat(rt, js_str_c(rt, "#"), h));
    go_to(rt, v);
    return jv_undef();
}

NATIVE(loc_reload)
{
    if (rt->host.navigate)
        rt->host.navigate(rt->host.ctx, rt->url);
    return jv_undef();
}

NATIVE(w_location) { return js_get(rt, jv_obj(rt->global), js_atom(rt, "__location")); }
NATIVE(w_set_location) { go_to(rt, ARG(0)); return jv_undef(); }

NATIVE(h_back)
{
    if (rt->host.back)
        rt->host.back(rt->host.ctx);
    return jv_undef();
}

/* ---- localStorage (w pamieci strony) ---- */

static jobj *storage_data(js_rt *rt, jv self)
{
    if (self.t != JT_OBJ)
        js_throw_error(rt, JS_ERR_TYPE, "Illegal invocation");
    return self.u.o;
}

NATIVE(ls_get_item)
{
    jobj *o = storage_data(rt, self);
    struct jprop *p = js_own(o, js_key_of(rt, ARG(0)));
    return p && p->v.t == JT_STR ? p->v : jv_null();
}

NATIVE(ls_set_item)
{
    jobj *o = storage_data(rt, self);
    js_define(rt, o, js_key_of(rt, ARG(0)), jv_str(js_to_str(rt, ARG(1))), PF_ALL);
    return jv_undef();
}

NATIVE(ls_remove_item)
{
    js_delete(rt, storage_data(rt, self), js_key_of(rt, ARG(0)));
    return jv_undef();
}

NATIVE(ls_clear)
{
    jobj *o = storage_data(rt, self);
    for (uint32_t i = 0; i < o->nprops; i++)
        o->props[i].key = 0;
    o->index = 0;
    o->nprops = 0;
    return jv_undef();
}

NATIVE(ls_length)
{
    jobj *o = storage_data(rt, self);
    return jv_num(js_own_keys(rt, o, 1)->u.arr.len);
}

NATIVE(ls_key)
{
    jobj *keys = js_own_keys(rt, storage_data(rt, self), 1);
    double i = js_to_integer(rt, ARG(0));
    if (i < 0 || i >= keys->u.arr.len)
        return jv_null();
    return keys->u.arr.v[(uint32_t)i];
}

/* ---------- timery ---------- */

static int add_timer(js_rt *rt, jv fn, int argc, jv *argv, int interval, int raf)
{
    if (rt->ntimers >= JS_TIMERS_MAX)
        return 0;
    struct js_timer *t = &rt->timers[rt->ntimers++];
    memset(t, 0, sizeof(*t));
    t->id = ++rt->next_timer_id;
    double ms = raf ? 16 : js_to_num(rt, ARG(1));
    if (!js_isfinite(ms) || ms < 0)
        ms = 0;
    if (ms > 1e9)
        ms = 1e9;
    if (interval && ms < 4)
        ms = 4;
    t->interval = interval ? (int)ms : 0;
    t->raf = raf;
    t->due = timer_uptime_ms() + (uint32_t)ms;
    if (js_is_callable(ARG(0))) {
        t->fn = argv[0];
    } else {
        unsigned len;
        char *code = u8(rt, ARG(0), &len);
        t->code = code;
    }
    if (!raf && argc > 2) {
        t->nargs = argc - 2;
        t->args = js_alloc(rt, sizeof(jv) * (size_t)t->nargs);
        memcpy(t->args, argv + 2, sizeof(jv) * (size_t)t->nargs);
    }
    (void)fn;
    return t->id;
}

static void clear_timer(js_rt *rt, int id)
{
    for (int i = 0; i < rt->ntimers; i++)
        if (rt->timers[i].id == id) {
            rt->timers[i] = rt->timers[--rt->ntimers];
            return;
        }
}

NATIVE(w_set_timeout) { return jv_num(add_timer(rt, ARG(0), argc, argv, 0, 0)); }
NATIVE(w_set_interval) { return jv_num(add_timer(rt, ARG(0), argc, argv, 1, 0)); }
NATIVE(w_raf) { return jv_num(add_timer(rt, ARG(0), argc, argv, 0, 1)); }

NATIVE(w_clear_timer)
{
    double id = js_to_num(rt, ARG(0));
    if (js_isfinite(id))
        clear_timer(rt, (int)id);
    return jv_undef();
}

NATIVE(arr_item_native)
{
    jv v = js_get_index(rt, self, ARG(0));
    return v.t == JT_UNDEF ? jv_null() : v;
}

NATIVE(d_create_element_ns) { return d_create_element(rt, self, argc > 1 ? 1 : 0, argv + 1, fn); }

/* ---------- skrypty dodawane ze skryptu ---------- */

/* Program pod ochrona - blad jest zglaszany, ale nie przerywa wywolujacego */
static void run_nested(js_rt *rt, const char *code, unsigned len)
{
    struct js_catch cat;
    cat.prev = rt->catch_top;
    cat.depth = rt->depth;
    rt->catch_top = &cat;
    if (js_setjmp(&cat.jb) == 0) {
        js_run_program(rt, code, len);
        rt->catch_top = cat.prev;
        return;
    }
    rt->catch_top = cat.prev;
    rt->depth = cat.depth;
    rt->chain_break = 0;
    if (rt->aborting)
        js_longjmp(&rt->catch_top->jb, 1);
    js_report_exception(rt);
}

static void run_script_node(js_rt *rt, struct dom_node *n)
{
    n->script_done = 1;
    const char *type = dom_attr(n, "type");
    if (type && type[0] && !strstr_ci(type, "javascript") && !strstr_ci(type, "ecmascript"))
        return;
    const char *src = dom_attr(n, "src");
    struct dom_node *saved = rt->cur_script;
    rt->cur_script = n;
    if (src && src[0]) {
        char abs[URL_MAX], *body = 0;
        unsigned len = 0;
        /* pobieramy tylko w trakcie ladowania strony (osobne zadanie) - pozniej
         * czekanie na siec zatrzymaloby pulpit */
        if (rt->ready_state < 2 && rt->host.fetch && url_resolve(rt->url, src, abs, sizeof(abs)) &&
            rt->host.fetch(rt->host.ctx, abs, &body, &len) == 0) {
            char *copy = js_alloc(rt, len + 1);
            memcpy(copy, body, len);
            kfree(body);
            run_nested(rt, copy, len);
            jobj *ev = make_event(rt, js_atom(rt, "load"), 0, 0);
            fire(rt, n, ev);
        } else {
            jobj *ev = make_event(rt, rt->A.error, 0, 0);
            fire(rt, n, ev);
        }
    } else {
        struct jsb b;
        jsb_init(&b, rt);
        collect_text(&b, n, 0);
        unsigned len;
        char *code = js_utf8_alloc(rt, jsb_done(&b), &len);
        run_nested(rt, code, len);
    }
    rt->cur_script = saved;
}

static void run_inserted_scripts(js_rt *rt, struct dom_node *n)
{
    if (!connected(rt, n))
        return;
    for (struct dom_node *c = n; c; c = next_in(c, n))
        if (c->type == N_ELEMENT && c->tag == T_SCRIPT && !c->script_done)
            run_script_node(rt, c);
}

/* ---------- start swiata JS ---------- */

static jobj *make_ctor_proto(js_rt *rt, const char *name, jnative call, jnative ctor, jobj *proto)
{
    jobj *f = js_new_native(rt, name, call, 0);
    f->u.nf.ctor = ctor;
    js_define(rt, f, rt->A.prototype, jv_obj(proto), 0);
    if (!js_own(proto, rt->A.constructor))
        js_define(rt, proto, rt->A.constructor, jv_obj(f), PF_WRITE | PF_CONF);
    js_def_val(rt, rt->global, name, jv_obj(f));
    return f;
}

static jv illegal_ctor(js_rt *rt, jv self U, int argc U, jv *argv U, jobj *fn U)
{
    js_throw_error(rt, JS_ERR_TYPE, "Illegal constructor");
}

static jv image_ctor(js_rt *rt, jv self U, int argc, jv *argv, jobj *fn U)
{
    if (!rt->doc)
        js_throw_error(rt, JS_ERR_ERROR, "Brak dokumentu");
    struct dom_node *n = dom_new_element(rt->doc, "img");
    if (!n)
        js_throw_error(rt, JS_ERR_ERROR, "Brak pamieci na strone");
    jv w = js_node_wrap(rt, n);
    if (argc > 0)
        set_attr(rt, n, "width", argv[0]);
    if (argc > 1)
        set_attr(rt, n, "height", argv[1]);
    return w;
}

void js_dom_init(js_rt *rt)
{
    jobj *G = rt->global, *op = rt->object_proto;

    /* Node, Element, Text, Document */
    rt->node_proto = js_new_object(rt, op);
    rt->element_proto = js_new_object(rt, rt->node_proto);
    rt->text_proto = js_new_object(rt, rt->node_proto);
    rt->document_proto = js_new_object(rt, rt->node_proto);
    rt->event_proto = js_new_object(rt, op);
    rt->style_proto = js_new_object(rt, op);
    rt->classlist_proto = js_new_object(rt, op);
    rt->nodelist_proto = js_new_object(rt, rt->array_proto);
    rt->canvas_proto = 0;

    jobj *np = rt->node_proto, *ep = rt->element_proto, *tp = rt->text_proto, *dp = rt->document_proto;
    js_def_fn(rt, np, "addEventListener", et_add_event_listener, 2);
    js_def_fn(rt, np, "removeEventListener", et_remove_event_listener, 2);
    js_def_fn(rt, np, "dispatchEvent", et_dispatch_event, 1);
    js_def_getset(rt, np, "nodeType", n_node_type, 0);
    js_def_getset(rt, np, "nodeName", n_node_name, 0);
    js_def_getset(rt, np, "nodeValue", n_node_value, n_set_node_value);
    js_def_getset(rt, np, "textContent", n_text_content, n_set_text_content);
    js_def_getset(rt, np, "parentNode", n_parent_node, 0);
    js_def_getset(rt, np, "parentElement", n_parent_element, 0);
    js_def_getset(rt, np, "childNodes", n_child_nodes, 0);
    js_def_getset(rt, np, "firstChild", n_first_child, 0);
    js_def_getset(rt, np, "lastChild", n_last_child, 0);
    js_def_getset(rt, np, "nextSibling", n_next_sibling, 0);
    js_def_getset(rt, np, "previousSibling", n_prev_sibling, 0);
    js_def_getset(rt, np, "ownerDocument", n_owner_document, 0);
    js_def_getset(rt, np, "isConnected", n_is_connected, 0);
    js_def_fn(rt, np, "hasChildNodes", n_has_child_nodes, 0);
    js_def_fn(rt, np, "appendChild", n_append_child, 1);
    js_def_fn(rt, np, "insertBefore", n_insert_before, 2);
    js_def_fn(rt, np, "removeChild", n_remove_child, 1);
    js_def_fn(rt, np, "replaceChild", n_replace_child, 2);
    js_def_fn(rt, np, "cloneNode", n_clone_node, 1);
    js_def_fn(rt, np, "contains", n_contains, 1);
    js_def_fn(rt, np, "remove", n_remove, 0);
    js_def_fn(rt, np, "before", n_before, 0);
    js_def_fn(rt, np, "after", n_after, 0);
    js_def_fn(rt, np, "replaceWith", n_replace_with, 0);
    js_def_fn(rt, np, "append", n_append, 0);
    js_def_fn(rt, np, "prepend", n_prepend, 0);
    js_def_fn(rt, np, "querySelector", e_query_selector, 1);
    js_def_fn(rt, np, "querySelectorAll", e_query_selector_all, 1);
    js_def_fn(rt, np, "getElementsByTagName", e_get_by_tag, 1);
    js_def_fn(rt, np, "getElementsByClassName", e_get_by_class, 1);
    js_def_getset(rt, np, "children", e_children, 0);
    js_def_getset(rt, np, "childElementCount", e_child_element_count, 0);
    js_def_getset(rt, np, "firstElementChild", e_first_element_child, 0);
    js_def_getset(rt, np, "lastElementChild", e_last_element_child, 0);
    js_define(rt, np, js_atom(rt, "ELEMENT_NODE"), jv_num(1), 0);
    js_define(rt, np, js_atom(rt, "TEXT_NODE"), jv_num(3), 0);
    js_define(rt, np, js_atom(rt, "DOCUMENT_NODE"), jv_num(9), 0);

    js_def_getset(rt, tp, "data", n_node_value, n_set_node_value);
    js_def_getset(rt, tp, "wholeText", n_node_value, 0);

    js_def_getset(rt, ep, "tagName", e_tag_name, 0);
    js_def_getset(rt, ep, "localName", e_local_name, 0);
    js_def_getset(rt, ep, "className", e_class_name, e_set_class_name);
    js_def_getset(rt, ep, "classList", e_class_list, 0);
    js_def_getset(rt, ep, "style", e_style, 0);
    js_def_getset(rt, ep, "dataset", e_dataset, 0);
    js_def_getset(rt, ep, "innerHTML", e_inner_html, e_set_inner_html);
    js_def_getset(rt, ep, "outerHTML", e_outer_html, e_set_outer_html);
    js_def_getset(rt, ep, "innerText", e_inner_text, n_set_text_content);
    js_def_getset(rt, ep, "outerText", e_inner_text, 0);
    js_def_getset(rt, ep, "nextElementSibling", e_next_element_sibling, 0);
    js_def_getset(rt, ep, "previousElementSibling", e_prev_element_sibling, 0);
    js_def_getset(rt, ep, "value", e_value, e_set_value);
    js_def_getset(rt, ep, "href", e_href, e_set_href);
    js_def_getset(rt, ep, "protocol", a_protocol, 0);
    js_def_getset(rt, ep, "host", a_host, 0);
    js_def_getset(rt, ep, "hostname", a_hostname, 0);
    js_def_getset(rt, ep, "port", a_port, 0);
    js_def_getset(rt, ep, "pathname", a_pathname, 0);
    js_def_getset(rt, ep, "search", a_search, 0);
    js_def_getset(rt, ep, "hash", a_hash, 0);
    js_def_getset(rt, ep, "origin", a_origin, 0);
    js_def_getset(rt, ep, "src", e_src, e_set_src);
    js_def_getset(rt, ep, "offsetWidth", e_offset_width, 0);
    js_def_getset(rt, ep, "offsetHeight", e_offset_height, 0);
    js_def_getset(rt, ep, "offsetLeft", e_offset_left, 0);
    js_def_getset(rt, ep, "offsetTop", e_offset_top, 0);
    js_def_getset(rt, ep, "clientWidth", e_offset_width, 0);
    js_def_getset(rt, ep, "clientHeight", e_offset_height, 0);
    js_def_getset(rt, ep, "scrollWidth", e_offset_width, 0);
    js_def_getset(rt, ep, "scrollHeight", e_offset_height, 0);
    js_def_getset(rt, ep, "scrollTop", e_zero, e_noop);
    js_def_getset(rt, ep, "scrollLeft", e_zero, e_noop);
    js_def_getset(rt, ep, "offsetParent", n_parent_element, 0);
    js_def_fn(rt, ep, "getAttribute", e_get_attribute, 1);
    js_def_fn(rt, ep, "setAttribute", e_set_attribute, 2);
    js_def_fn(rt, ep, "removeAttribute", e_remove_attribute, 1);
    js_def_fn(rt, ep, "hasAttribute", e_has_attribute, 1);
    js_def_fn(rt, ep, "toggleAttribute", e_toggle_attribute, 1);
    js_def_fn(rt, ep, "getAttributeNames", e_get_attribute_names, 0);
    js_def_fn(rt, ep, "matches", e_matches, 1);
    js_def_fn(rt, ep, "closest", e_closest, 1);
    js_def_fn(rt, ep, "insertAdjacentHTML", e_insert_adjacent_html, 2);
    js_def_fn(rt, ep, "insertAdjacentElement", e_insert_adjacent_element, 2);
    js_def_fn(rt, ep, "insertAdjacentText", e_insert_adjacent_text, 2);
    js_def_fn(rt, ep, "click", e_click, 0);
    js_def_fn(rt, ep, "focus", e_focus, 0);
    js_def_fn(rt, ep, "blur", e_blur, 0);
    js_def_fn(rt, ep, "select", e_noop, 0);
    js_def_fn(rt, ep, "scrollIntoView", e_scroll_into_view, 0);
    js_def_fn(rt, ep, "getBoundingClientRect", e_get_bounding_client_rect, 0);
    js_def_fn(rt, ep, "getClientRects", e_noop, 0);
    js_def_fn(rt, ep, "animate", e_noop, 0);
    static const char *const reflected[][2] = {
        { "id", "id" }, { "title", "title" }, { "alt", "alt" }, { "name", "name" }, { "type", "type" },
        { "placeholder", "placeholder" }, { "lang", "lang" }, { "dir", "dir" }, { "htmlFor", "for" },
        { "target", "target" }, { "rel", "rel" }, { "action", "action" }, { "method", "method" },
        { "role", "role" }, { "tabIndex", "tabindex" }, { "defaultValue", "value" },
    };
    for (unsigned i = 0; i < sizeof(reflected) / sizeof(reflected[0]); i++)
        def_reflect(rt, ep, reflected[i][0], reflected[i][1], 0);
    static const char *const booleans[] = { "hidden", "checked", "disabled", "selected", "readOnly", "required", "multiple", "autofocus" };
    for (unsigned i = 0; i < sizeof(booleans) / sizeof(booleans[0]); i++) {
        char low[20];
        unsigned k = 0;
        for (; booleans[i][k]; k++)
            low[k] = (char)(booleans[i][k] >= 'A' && booleans[i][k] <= 'Z' ? booleans[i][k] + 32 : booleans[i][k]);
        low[k] = 0;
        def_reflect(rt, ep, booleans[i], low, 1);
    }
    def_reflect(rt, ep, "width", "width", 0);
    def_reflect(rt, ep, "height", "height", 0);
    js_canvas_init(rt);                              /* <canvas> i rysowanie (js_canvas.c) */

    jobj *cp = rt->classlist_proto;
    js_def_fn(rt, cp, "add", cl_add, 1);
    js_def_fn(rt, cp, "remove", cl_remove, 1);
    js_def_fn(rt, cp, "toggle", cl_toggle, 1);
    js_def_fn(rt, cp, "contains", cl_contains, 1);
    js_def_fn(rt, cp, "replace", cl_replace, 2);
    js_def_fn(rt, cp, "item", cl_item, 1);
    js_def_fn(rt, cp, "forEach", cl_for_each, 1);
    js_def_fn(rt, cp, "toString", cl_value, 0);
    js_def_getset(rt, cp, "length", cl_length, 0);
    js_def_getset(rt, cp, "value", cl_value, 0);

    jobj *sp = rt->style_proto;
    js_def_fn(rt, sp, "getPropertyValue", st_get_property_value, 1);
    js_def_fn(rt, sp, "setProperty", st_set_property, 2);
    js_def_fn(rt, sp, "removeProperty", st_remove_property, 1);

    js_def_fn(rt, rt->nodelist_proto, "item", arr_item_native, 1);

    /* document */
    js_def_fn(rt, dp, "getElementById", d_get_element_by_id, 1);
    js_def_fn(rt, dp, "getElementsByName", d_get_elements_by_name, 1);
    js_def_fn(rt, dp, "createElement", d_create_element, 1);
    js_def_fn(rt, dp, "createElementNS", d_create_element_ns, 2);
    js_def_fn(rt, dp, "createTextNode", d_create_text_node, 1);
    js_def_fn(rt, dp, "createComment", d_create_text_node, 1);
    js_def_fn(rt, dp, "createDocumentFragment", d_create_fragment, 0);
    js_def_fn(rt, dp, "createEvent", d_create_event, 1);
    js_def_fn(rt, dp, "write", d_write, 1);
    js_def_fn(rt, dp, "writeln", d_writeln, 1);
    js_def_fn(rt, dp, "hasFocus", d_has_focus, 0);
    js_def_fn(rt, dp, "open", e_noop, 0);
    js_def_fn(rt, dp, "close", e_noop, 0);
    js_def_getset(rt, dp, "body", d_body, 0);
    js_def_getset(rt, dp, "head", d_head, 0);
    js_def_getset(rt, dp, "documentElement", d_document_element, 0);
    js_def_getset(rt, dp, "title", d_title, d_set_title);
    js_def_getset(rt, dp, "readyState", d_ready_state, 0);
    js_def_getset(rt, dp, "URL", d_url, 0);
    js_def_getset(rt, dp, "documentURI", d_url, 0);
    js_def_getset(rt, dp, "cookie", d_cookie, d_set_cookie);
    js_def_getset(rt, dp, "activeElement", d_active_element, 0);
    js_def_getset(rt, dp, "defaultView", d_default_view, 0);
    js_def_getset(rt, dp, "visibilityState", d_visibility, 0);
    js_def_getset(rt, dp, "hidden", d_false, 0);
    js_def_getset(rt, dp, "referrer", d_empty_string, 0);
    js_def_getset(rt, dp, "domain", d_domain, 0);
    js_def_getset(rt, dp, "forms", d_forms, 0);
    js_def_getset(rt, dp, "images", d_images, 0);
    js_def_getset(rt, dp, "links", d_links, 0);
    js_def_getset(rt, dp, "scripts", d_scripts, 0);
    js_def_getset(rt, dp, "currentScript", d_current_script, 0);
    js_def_getset(rt, dp, "location", w_location, w_set_location);
    js_def_val(rt, dp, "characterSet", str8(rt, "UTF-8"));
    js_def_val(rt, dp, "compatMode", str8(rt, "CSS1Compat"));
    js_def_val(rt, dp, "contentType", str8(rt, "text/html"));

    /* zdarzenia */
    jobj *evp = rt->event_proto;
    js_def_fn(rt, evp, "preventDefault", ev_prevent_default, 0);
    js_def_fn(rt, evp, "stopPropagation", ev_stop_propagation, 0);
    js_def_fn(rt, evp, "stopImmediatePropagation", ev_stop_immediate, 0);
    js_def_fn(rt, evp, "initEvent", ev_init_event, 3);
    js_def_fn(rt, evp, "composedPath", ev_composed_path, 0);
    js_def_getset(rt, evp, "defaultPrevented", ev_default_prevented, 0);
    static const char *const events[] = { "Event", "CustomEvent", "KeyboardEvent", "MouseEvent", "PointerEvent",
                                          "FocusEvent", "InputEvent", "UIEvent", "TouchEvent", "WheelEvent" };
    for (unsigned i = 0; i < sizeof(events) / sizeof(events[0]); i++)
        make_ctor_proto(rt, events[i], event_ctor, event_ctor, evp);

    /* konstruktory do instanceof */
    make_ctor_proto(rt, "EventTarget", illegal_ctor, 0, np);
    make_ctor_proto(rt, "Node", illegal_ctor, 0, np);
    make_ctor_proto(rt, "Element", illegal_ctor, 0, ep);
    make_ctor_proto(rt, "HTMLElement", illegal_ctor, 0, ep);
    static const char *const html_elems[] = {
        "HTMLDivElement", "HTMLSpanElement", "HTMLInputElement", "HTMLButtonElement", "HTMLAnchorElement",
        "HTMLImageElement", "HTMLFormElement", "HTMLSelectElement", "HTMLTextAreaElement", "HTMLParagraphElement",
        "HTMLScriptElement", "HTMLLIElement", "HTMLUListElement", "HTMLTableElement", "HTMLBodyElement",
        "HTMLCanvasElement", "HTMLMediaElement", "HTMLVideoElement", "HTMLAudioElement", "SVGElement",
    };
    for (unsigned i = 0; i < sizeof(html_elems) / sizeof(html_elems[0]); i++)
        make_ctor_proto(rt, html_elems[i], illegal_ctor, 0,
                        strcmp(html_elems[i], "HTMLCanvasElement") == 0 && rt->canvas_proto ? rt->canvas_proto : ep);
    if (rt->ctx2d_proto)
        make_ctor_proto(rt, "CanvasRenderingContext2D", illegal_ctor, 0, rt->ctx2d_proto);
    make_ctor_proto(rt, "Text", illegal_ctor, 0, tp);
    make_ctor_proto(rt, "CharacterData", illegal_ctor, 0, tp);
    make_ctor_proto(rt, "Document", illegal_ctor, 0, dp);
    make_ctor_proto(rt, "HTMLDocument", illegal_ctor, 0, dp);
    make_ctor_proto(rt, "NodeList", illegal_ctor, 0, rt->nodelist_proto);
    make_ctor_proto(rt, "HTMLCollection", illegal_ctor, 0, rt->nodelist_proto);
    make_ctor_proto(rt, "Image", image_ctor, image_ctor, ep);
    js_net_init(rt);                                 /* fetch, Response, XMLHttpRequest (js_net.c) */

    /* window */
    js_def_val(rt, G, "window", jv_obj(G));
    js_def_val(rt, G, "self", jv_obj(G));
    js_def_val(rt, G, "top", jv_obj(G));
    js_def_val(rt, G, "parent", jv_obj(G));
    js_def_val(rt, G, "frames", jv_obj(G));
    js_def_fn(rt, G, "addEventListener", et_add_event_listener, 2);
    js_def_fn(rt, G, "removeEventListener", et_remove_event_listener, 2);
    js_def_fn(rt, G, "dispatchEvent", et_dispatch_event, 1);
    js_def_fn(rt, G, "alert", w_alert, 1);
    js_def_fn(rt, G, "confirm", w_confirm, 1);
    js_def_fn(rt, G, "prompt", w_prompt, 2);
    js_def_fn(rt, G, "setTimeout", w_set_timeout, 2);
    js_def_fn(rt, G, "setInterval", w_set_interval, 2);
    js_def_fn(rt, G, "clearTimeout", w_clear_timer, 1);
    js_def_fn(rt, G, "clearInterval", w_clear_timer, 1);
    js_def_fn(rt, G, "requestAnimationFrame", w_raf, 1);
    js_def_fn(rt, G, "cancelAnimationFrame", w_clear_timer, 1);
    js_def_fn(rt, G, "requestIdleCallback", w_set_timeout, 1);
    js_def_fn(rt, G, "getComputedStyle", w_get_computed_style, 1);
    js_def_fn(rt, G, "matchMedia", w_match_media, 1);
    js_def_fn(rt, G, "scrollTo", w_scroll_to, 2);
    js_def_fn(rt, G, "scroll", w_scroll_to, 2);
    js_def_fn(rt, G, "scrollBy", w_scroll_by, 2);
    js_def_fn(rt, G, "open", w_open, 1);
    js_def_fn(rt, G, "close", e_noop, 0);
    js_def_fn(rt, G, "print", e_noop, 0);
    js_def_fn(rt, G, "focus", e_noop, 0);
    js_def_fn(rt, G, "blur", e_noop, 0);
    js_def_fn(rt, G, "postMessage", e_noop, 1);
    js_def_getset(rt, G, "innerWidth", w_inner_width, 0);
    js_def_getset(rt, G, "innerHeight", w_inner_height, 0);
    js_def_getset(rt, G, "outerWidth", w_inner_width, 0);
    js_def_getset(rt, G, "outerHeight", w_inner_height, 0);
    js_def_getset(rt, G, "scrollX", w_zero, 0);
    js_def_getset(rt, G, "scrollY", w_scroll_y, 0);
    js_def_getset(rt, G, "pageXOffset", w_zero, 0);
    js_def_getset(rt, G, "pageYOffset", w_scroll_y, 0);
    js_def_getset(rt, G, "devicePixelRatio", w_one, 0);
    js_def_val(rt, G, "onload", jv_null());
    js_def_val(rt, G, "name", jv_str(rt->empty));
    js_def_val(rt, G, "isSecureContext", jv_bool(str_starts_ci(rt->url, "https:")));

    /* location */
    jobj *loc = js_new_object(rt, op);
    js_def_getset(rt, loc, "href", loc_href, loc_set_href);
    js_def_getset(rt, loc, "protocol", loc_protocol, 0);
    js_def_getset(rt, loc, "host", loc_host, 0);
    js_def_getset(rt, loc, "hostname", loc_hostname, 0);
    js_def_getset(rt, loc, "port", loc_port, 0);
    js_def_getset(rt, loc, "pathname", loc_pathname, 0);
    js_def_getset(rt, loc, "search", loc_search, 0);
    js_def_getset(rt, loc, "hash", loc_hash, loc_set_hash);
    js_def_getset(rt, loc, "origin", loc_origin, 0);
    js_def_fn(rt, loc, "assign", loc_assign, 1);
    js_def_fn(rt, loc, "replace", loc_assign, 1);
    js_def_fn(rt, loc, "reload", loc_reload, 0);
    js_def_fn(rt, loc, "toString", loc_href, 0);
    js_define(rt, G, js_atom(rt, "__location"), jv_obj(loc), 0);
    js_def_getset(rt, G, "location", w_location, w_set_location);

    /* navigator, history, screen */
    jobj *nav = js_new_object(rt, op);
    js_def_val(rt, G, "navigator", jv_obj(nav));
    {
        jobj *ce = js_new_object(rt, rt->object_proto), *reg = js_new_object(rt, 0);
        static const struct { const char *n; jnative f; int len; } cef[] = {
            { "define", ce_define, 2 }, { "get", ce_get, 1 }, { "whenDefined", ce_when_defined, 1 },
            { "upgrade", ce_noop, 1 },
        };
        for (unsigned i = 0; i < sizeof(cef) / sizeof(cef[0]); i++) {
            jobj *f = js_new_native(rt, cef[i].n, cef[i].f, cef[i].len);
            f->u.nf.data = jv_obj(reg);
            js_define(rt, ce, js_atom(rt, cef[i].n), jv_obj(f), PF_WRITE | PF_CONF);
        }
        js_def_val(rt, G, "customElements", jv_obj(ce));
    }
    js_def_val(rt, nav, "userAgent", str8(rt, "Mozilla/5.0 (NaszOS; x86_64) NaszaPrzegladarka/1.0"));
    js_def_val(rt, nav, "appName", str8(rt, "Netscape"));
    js_def_val(rt, nav, "appVersion", str8(rt, "5.0 (NaszOS)"));
    js_def_val(rt, nav, "platform", str8(rt, "NaszOS"));
    js_def_val(rt, nav, "vendor", str8(rt, "NaszOS"));
    js_def_val(rt, nav, "language", str8(rt, "pl-PL"));
    jobj *langs = js_new_array(rt, 0);
    js_array_push(rt, langs, str8(rt, "pl-PL"));
    js_array_push(rt, langs, str8(rt, "pl"));
    js_array_push(rt, langs, str8(rt, "en"));
    js_def_val(rt, nav, "languages", jv_obj(langs));
    js_def_val(rt, nav, "onLine", jv_bool(1));
    js_def_val(rt, nav, "cookieEnabled", jv_bool(1));
    js_def_val(rt, nav, "hardwareConcurrency", jv_num(1));
    js_def_val(rt, nav, "maxTouchPoints", jv_num(0));
    js_def_val(rt, nav, "webdriver", jv_bool(0));
    jobj *hist = js_new_object(rt, op);
    js_def_val(rt, G, "history", jv_obj(hist));
    js_def_fn(rt, hist, "back", h_back, 0);
    js_def_fn(rt, hist, "forward", e_noop, 0);
    js_def_fn(rt, hist, "go", e_noop, 1);
    js_def_fn(rt, hist, "pushState", e_noop, 3);
    js_def_fn(rt, hist, "replaceState", e_noop, 3);
    js_def_val(rt, hist, "length", jv_num(1));
    js_def_val(rt, hist, "state", jv_null());
    jobj *scr = js_new_object(rt, op);
    js_def_val(rt, G, "screen", jv_obj(scr));
    js_def_getset(rt, scr, "width", w_inner_width, 0);
    js_def_getset(rt, scr, "height", w_inner_height, 0);
    js_def_getset(rt, scr, "availWidth", w_inner_width, 0);
    js_def_getset(rt, scr, "availHeight", w_inner_height, 0);
    js_def_val(rt, scr, "colorDepth", jv_num(24));

    /* localStorage / sessionStorage */
    jobj *stp = js_new_object(rt, op);
    js_def_fn(rt, stp, "getItem", ls_get_item, 1);
    js_def_fn(rt, stp, "setItem", ls_set_item, 2);
    js_def_fn(rt, stp, "removeItem", ls_remove_item, 1);
    js_def_fn(rt, stp, "clear", ls_clear, 0);
    js_def_fn(rt, stp, "key", ls_key, 1);
    js_def_getset(rt, stp, "length", ls_length, 0);
    rt->storage = js_new_object(rt, stp);
    js_def_val(rt, G, "localStorage", jv_obj(rt->storage));
    js_def_val(rt, G, "sessionStorage", jv_obj(js_new_object(rt, stp)));

    /* document */
    if (rt->doc) {
        rt->document = js_node_wrap(rt, rt->doc->root).u.o;
        js_def_val(rt, G, "document", jv_obj(rt->document));
    }
}

/* ---------- interfejs dla przegladarki (js.h) ---------- */

static void init_cb(js_rt *rt, void *arg U)
{
    js_parse_init(rt);
    js_lib_init(rt);
    js_dom_init(rt);
}

void js_set_mem_limit(struct js_rt *rt, unsigned long long bytes)
{
    if (rt && bytes > rt->mem_limit)
        rt->mem_limit = bytes;
}

struct js_rt *js_new(struct dom_doc *doc, struct css_ctx *css, const char *url, const struct js_host *host)
{
    js_rt *rt = kzalloc(sizeof(*rt));
    if (!rt)
        return 0;
    (void)css;                                       /* selektory maja wlasny kontekst CSS */
    rt->mem_limit = JS_MEM_LIMIT;
    rt->doc = doc;
    if (host)
        rt->host = *host;
    ksnprintf(rt->url, sizeof(rt->url), "%s", url ? url : "");
    rt->empty = js_intern(rt, js_str_new(rt, 0));
#define INIT_ATOM(n) rt->A.n = js_atom(rt, #n);
    JS_ATOMS(INIT_ATOM)
#undef INIT_ATOM
    for (int i = 0; i < 128; i++) {
        jstr *c = js_str_new(rt, 1);
        c->c[0] = (uint16_t)i;
        rt->char_str[i] = js_intern(rt, c);
    }
    for (int i = 0; i < 256; i++) {
        char buf[8];
        ksnprintf(buf, sizeof(buf), "%d", i);
        rt->int_str[i] = js_atom(rt, buf);
    }
    rt->sym_iterator = js_atom(rt, "@@iterator");
    jobj *op = rt->object_proto = js_new_object(rt, 0);
    rt->function_proto = js_new_native(rt, "", e_noop, 0);
    rt->function_proto->proto = op;
    rt->array_proto = js_new_array(rt, 0);
    rt->array_proto->proto = op;
    rt->string_proto = js_new_object(rt, op);
    rt->number_proto = js_new_object(rt, op);
    rt->boolean_proto = js_new_object(rt, op);
    rt->error_proto = js_new_object(rt, op);
    rt->date_proto = js_new_object(rt, op);
    rt->regexp_proto = js_new_object(rt, op);
    rt->map_proto = js_new_object(rt, op);
    rt->set_proto = js_new_object(rt, op);
    rt->promise_proto = js_new_object(rt, op);
    rt->global = js_new_object(rt, op);
    jenv *g = js_alloc(rt, sizeof(jenv) + sizeof(struct jbind) * 64);
    g->b = (struct jbind *)(g + 1);
    g->cap = 64;
    g->kind = ENV_GLOBAL;
    g->has_this = 1;
    g->this_init = 1;
    g->this_val = jv_obj(rt->global);
    rt->genv = g;
    /* losowosc: licznik czasu i zegar */
    struct rtc_time t;
    rtc_read(&t);
    int64_t local = rtc_to_unix(&t);
    rt->rand_state[0] = (uint64_t)local * 6364136223846793005ULL + timer_ticks() + 1442695040888963407ULL;
    rt->rand_state[1] = ((uint64_t)timer_uptime_ms() << 32 ^ (uint64_t)(uintptr_t)rt) | 1;
    rt->clock_base_ms = (local - rtc_poland_offset(local - 3600)) * 1000;
    rt->clock_base_uptime = timer_uptime_ms();
    if (js_protect(rt, init_cb, 0) != 0) {
        js_free(rt);
        return 0;
    }
    return rt;
}

void js_free(struct js_rt *rt)
{
    if (!rt)
        return;
    if (rt->doc)                                     /* wezly nie moga wskazywac na zwolniona pamiec */
        for (struct dom_node *n = rt->doc->root; n; n = next_in(n, rt->doc->root))
            n->js = 0;
    js_coro_free_all(rt);
    arena_free(&rt->mem);
    css_free(rt->css);
    kfree(rt->stack);
    kfree(rt);
}

void js_set_host(struct js_rt *rt, const struct js_host *host)
{
    if (rt && host)
        rt->host = *host;
}

void js_set_cancel(struct js_rt *rt, volatile int *flag)
{
    if (rt)
        rt->cancel = flag;
}

const char *js_last_error(struct js_rt *rt)
{
    return rt ? rt->last_error : "";
}

unsigned js_memory_kb(struct js_rt *rt)
{
    return rt ? (unsigned)(rt->mem.used / 1024) : 0;
}

int js_dirty(struct js_rt *rt)
{
    if (!rt)
        return 0;
    int d = rt->dirty;
    rt->dirty = 0;
    return d;
}

struct run_args {
    const char *code;
    unsigned len;
    jv result;
};

static void run_cb(js_rt *rt, void *arg)
{
    struct run_args *a = arg;
    a->result = js_run_program(rt, a->code, a->len);
}

static int protected_run(js_rt *rt, void (*fn)(js_rt *, void *), void *arg, uint32_t budget)
{
    rt->budget_ms = budget;
    rt->abort_msg = 0;
    rt->last_error[0] = '\0';
    return js_protect(rt, fn, arg);
}

int js_run(struct js_rt *rt, const char *code, unsigned len, const char *name, struct dom_node *script)
{
    if (!rt)
        return -1;
    ksnprintf(rt->script_name_buf, sizeof(rt->script_name_buf), "%s", name ? name : "skrypt");
    rt->script_name = rt->script_name_buf;
    rt->cur_script = script;
    rt->write_after = script;
    if (script)
        script->script_done = 1;
    struct run_args a = { code, len, jv_undef() };
    int r = protected_run(rt, run_cb, &a, BUDGET_SCRIPT);
    rt->cur_script = 0;
    return r;
}

struct text_args {
    struct run_args run;
    char *out;
    unsigned size;
};

static void eval_text_cb(js_rt *rt, void *arg)
{
    struct text_args *a = arg;
    a->run.result = js_run_program(rt, a->run.code, a->run.len);
    jv v = a->run.result;
    jstr *s = v.t == JT_STR ? js_concat(rt, js_concat(rt, js_str_c(rt, "'"), v.u.s), js_str_c(rt, "'")) : js_inspect(rt, v, 0);
    js_utf8(s, a->out, a->size);
}

int js_eval_text(struct js_rt *rt, const char *code, unsigned len, char *out, unsigned size)
{
    if (!rt)
        return -1;
    out[0] = '\0';
    rt->script_name = "terminal";
    struct text_args a = { { code, len, jv_undef() }, out, size };
    return protected_run(rt, eval_text_cb, &a, BUDGET_SCRIPT);
}

static void script_cb(js_rt *rt, void *arg)
{
    run_script_node(rt, arg);
}

int js_run_script(struct js_rt *rt, struct dom_node *script, const char *name)
{
    if (!rt || !script || script->script_done)
        return 0;
    const char *src = dom_attr(script, "src");
    ksnprintf(rt->script_name_buf, sizeof(rt->script_name_buf), "%s", src && src[0] ? src : name ? name : "skrypt");
    rt->script_name = rt->script_name_buf;
    rt->write_after = script;
    return protected_run(rt, script_cb, script, BUDGET_SCRIPT);
}

static void loaded_cb(js_rt *rt, void *arg U)
{
    rt->ready_state = 1;
    if (rt->document) {
        jobj *ev = make_event(rt, js_atom(rt, "DOMContentLoaded"), 1, 0);
        fire(rt, rt->doc->root, ev);
    }
    rt->ready_state = 2;
    jobj *ev = make_event(rt, js_atom(rt, "load"), 0, 0);
    fire(rt, 0, ev);
}

void js_fire_loaded(struct js_rt *rt)
{
    if (!rt)
        return;
    rt->script_name = "load";
    protected_run(rt, loaded_cb, 0, BUDGET_SCRIPT);
    rt->ready_state = 2;
}

struct image_args {
    struct dom_node *n;
    int ok;
};

static void image_cb(js_rt *rt, void *arg)
{
    struct image_args *a = arg;
    fire(rt, a->n, make_event(rt, a->ok ? js_atom(rt, "load") : rt->A.error, 0, 0));
}

void js_image_done(struct js_rt *rt, struct dom_node *img, int ok)
{
    if (!rt || !img)
        return;
    struct image_args a = { img, ok };
    rt->script_name = ok ? "img.onload" : "img.onerror";
    protected_run(rt, image_cb, &a, BUDGET_EVENT);
}

struct click_args {
    struct dom_node *target;
    int x, y;
    int prevented;
};

/* x, y - miejsce na stronie; offsetX/Y liczymy wzgledem klikanego elementu
 * (np. <canvas> - gra chce wiedziec, w ktory piksel plotna kliknieto) */
static jobj *mouse_event(js_rt *rt, const char *type, int x, int y, int ox, int oy)
{
    jobj *ev = make_event(rt, js_atom(rt, type), 1, 1);
    int sy = rt->host.scroll_y ? rt->host.scroll_y(rt->host.ctx) : 0;
    js_define(rt, ev, rt->A.clientX, jv_num(x), PF_ENUM);
    js_define(rt, ev, rt->A.clientY, jv_num(y - sy), PF_ENUM);
    js_define(rt, ev, rt->A.pageX, jv_num(x), PF_ENUM);
    js_define(rt, ev, rt->A.pageY, jv_num(y), PF_ENUM);
    js_def_val(rt, ev, "offsetX", jv_num(ox));
    js_def_val(rt, ev, "offsetY", jv_num(oy));
    js_def_val(rt, ev, "screenX", jv_num(x));
    js_def_val(rt, ev, "screenY", jv_num(y - sy));
    js_define(rt, ev, rt->A.button, jv_num(0), PF_ENUM);
    js_def_val(rt, ev, "buttons", jv_num(1));
    js_define(rt, ev, rt->A.which, jv_num(1), PF_ENUM);
    js_def_val(rt, ev, "detail", jv_num(1));
    js_define(rt, ev, rt->A.ctrlKey, jv_bool(0), PF_ENUM);
    js_define(rt, ev, rt->A.shiftKey, jv_bool(0), PF_ENUM);
    js_define(rt, ev, rt->A.altKey, jv_bool(0), PF_ENUM);
    js_define(rt, ev, rt->A.metaKey, jv_bool(0), PF_ENUM);
    js_def_val(rt, ev, "pointerType", str8(rt, "mouse"));
    return ev;
}

static void click_cb(js_rt *rt, void *arg)
{
    struct click_args *a = arg;
    struct dom_node *t = a->target;
    if (t && t->type == N_TEXT)
        t = t->parent;
    if (!t)
        t = doc_body(rt);
    if (!t)
        return;
    int bx, by, bw, bh, ox = a->x, oy = a->y;
    if (rt->host.box && rt->host.box(rt->host.ctx, t, &bx, &by, &bw, &bh)) {
        ox = a->x - bx;
        oy = a->y - by;
    }
    static const char *const seq[] = { "pointerdown", "mousedown", "pointerup", "mouseup", "click" };
    for (int i = 0; i < 5; i++) {
        jobj *ev = mouse_event(rt, seq[i], a->x, a->y, ox, oy);
        int ok = fire(rt, t, ev);
        if (i == 4 && !ok)
            a->prevented = 1;
    }
    /* fokus: pola formularza i linki */
    struct dom_node *f = t;
    while (f && !(f->type == N_ELEMENT && (f->tag == T_INPUT || f->tag == T_TEXTAREA || f->tag == T_SELECT ||
                                          f->tag == T_BUTTON || f->tag == T_A)))
        f = f->parent;
    rt->active = f;
}

int js_click(struct js_rt *rt, struct dom_node *target, int x, int y)
{
    if (!rt || !rt->doc)
        return 0;
    struct click_args a = { target, x, y, 0 };
    rt->script_name = "click";
    protected_run(rt, click_cb, &a, BUDGET_EVENT);
    return a.prevented;
}

struct key_args {
    int key;
    int prevented;
};

static void key_event_fields(js_rt *rt, jobj *ev, int key)
{
    const char *name = 0, *code = 0;
    int kc = 0, ctrl = 0, shift = 0;
    char buf[8], cbuf[16];
    switch (key) {
    case KEY_UP: name = code = "ArrowUp"; kc = 38; break;
    case KEY_DOWN: name = code = "ArrowDown"; kc = 40; break;
    case KEY_LEFT: name = code = "ArrowLeft"; kc = 37; break;
    case KEY_RIGHT: name = code = "ArrowRight"; kc = 39; break;
    case KEY_HOME: name = code = "Home"; kc = 36; break;
    case KEY_END: name = code = "End"; kc = 35; break;
    case KEY_DELETE: name = code = "Delete"; kc = 46; break;
    case KEY_PGUP: name = code = "PageUp"; kc = 33; break;
    case KEY_PGDN: name = code = "PageDown"; kc = 34; break;
    case '\n': name = code = "Enter"; kc = 13; break;
    case '\b': name = code = "Backspace"; kc = 8; break;
    case 27: name = code = "Escape"; kc = 27; break;
    case '\t': name = code = "Tab"; kc = 9; break;
    case ' ': name = " "; code = "Space"; kc = 32; break;
    default:
        if (key >= 1 && key <= 26) {
            buf[0] = (char)('a' + key - 1);
            buf[1] = 0;
            name = buf;
            ksnprintf(cbuf, sizeof(cbuf), "Key%c", 'A' + key - 1);
            code = cbuf;
            kc = 'A' + key - 1;
            ctrl = 1;
        } else if (key > ' ' && key < 127) {
            buf[0] = (char)key;
            buf[1] = 0;
            name = buf;
            if (key >= 'a' && key <= 'z') {
                ksnprintf(cbuf, sizeof(cbuf), "Key%c", key - 32);
                kc = key - 32;
            } else if (key >= 'A' && key <= 'Z') {
                ksnprintf(cbuf, sizeof(cbuf), "Key%c", key);
                kc = key;
                shift = 1;
            } else if (key >= '0' && key <= '9') {
                ksnprintf(cbuf, sizeof(cbuf), "Digit%c", key);
                kc = key;
            } else {
                ksnprintf(cbuf, sizeof(cbuf), "%s", "Unidentified");
                kc = key;
            }
            code = cbuf;
        } else {
            name = code = "Unidentified";
        }
    }
    js_define(rt, ev, rt->A.key, str8(rt, name), PF_ENUM);
    js_define(rt, ev, rt->A.code, str8(rt, code), PF_ENUM);
    js_define(rt, ev, rt->A.keyCode, jv_num(kc), PF_ENUM);
    js_define(rt, ev, rt->A.which, jv_num(kc), PF_ENUM);
    js_def_val(rt, ev, "charCode", jv_num(key > ' ' && key < 127 ? key : 0));
    js_define(rt, ev, rt->A.ctrlKey, jv_bool(ctrl), PF_ENUM);
    js_define(rt, ev, rt->A.shiftKey, jv_bool(shift), PF_ENUM);
    js_define(rt, ev, rt->A.altKey, jv_bool(0), PF_ENUM);
    js_define(rt, ev, rt->A.metaKey, jv_bool(0), PF_ENUM);
    js_define(rt, ev, rt->A.repeat, jv_bool(0), PF_ENUM);
}

/* Pole tekstowe <input>, do ktorego mozna pisac */
static int is_text_input(struct dom_node *n)
{
    if (!n || n->type != N_ELEMENT || n->tag != T_INPUT || dom_attr(n, "disabled") || dom_attr(n, "readonly"))
        return 0;
    const char *t = dom_attr(n, "type");
    return !t || !t[0] || strstr_ci(t, "text") || strstr_ci(t, "search") || strstr_ci(t, "email") ||
           strstr_ci(t, "number") || strstr_ci(t, "password") || strstr_ci(t, "url") || strstr_ci(t, "tel");
}

int js_input_focused(struct js_rt *rt)
{
    return rt && rt->active && connected(rt, rt->active) && is_text_input(rt->active);
}

/* Klawisz w polu tekstowym: dopisanie / skasowanie znaku, zdarzenia input i change */
static int edit_input(js_rt *rt, struct dom_node *n, int key)
{
    const char *v = dom_attr(n, "value");
    unsigned len = v ? (unsigned)strlen(v) : 0;
    char *buf = js_alloc(rt, len + 8);
    if (v)
        memcpy(buf, v, len);
    buf[len] = 0;
    if (key >= ' ' && key < 127) {
        buf[len++] = (char)key;
        buf[len] = 0;
    } else if (key == '\b') {
        if (!len)
            return 1;
        len--;
        while (len && ((uint8_t)buf[len] & 0xC0) == 0x80)
            len--;
        buf[len] = 0;
    } else if (key == '\n') {
        jobj *ev = make_event(rt, js_atom(rt, "change"), 1, 0);
        fire(rt, n, ev);
        for (struct dom_node *f = n->parent; f; f = f->parent)
            if (f->type == N_ELEMENT && f->tag == T_FORM) {
                ev = make_event(rt, js_atom(rt, "submit"), 1, 1);
                fire(rt, f, ev);
                break;
            }
        return 1;
    } else {
        return 0;
    }
    set_attr(rt, n, "value", str8(rt, buf));
    jobj *ev = make_event(rt, js_atom(rt, "input"), 1, 0);
    fire(rt, n, ev);
    return 1;
}

static void key_cb(js_rt *rt, void *arg)
{
    struct key_args *a = arg;
    struct dom_node *t = rt->active && connected(rt, rt->active) ? rt->active : doc_body(rt);
    if (!t && rt->doc)
        t = rt->doc->root;
    jobj *ev = make_event(rt, js_atom(rt, "keydown"), 1, 1);
    key_event_fields(rt, ev, a->key);
    if (!fire(rt, t, ev))
        a->prevented = 1;
    if (!a->prevented && is_text_input(t) && edit_input(rt, t, a->key))
        a->prevented = 1;                             /* klawisz zjadlo pole tekstowe */
    else if (!a->prevented && ((a->key >= ' ' && a->key < 127) || a->key == '\n')) {
        ev = make_event(rt, js_atom(rt, "keypress"), 1, 1);
        key_event_fields(rt, ev, a->key);
        if (!fire(rt, t, ev))
            a->prevented = 1;
    }
    ev = make_event(rt, js_atom(rt, "keyup"), 1, 1);
    key_event_fields(rt, ev, a->key);
    fire(rt, t, ev);
}

int js_key(struct js_rt *rt, int key)
{
    if (!rt || !rt->doc)
        return 0;
    struct key_args a = { key, 0 };
    rt->script_name = "klawisz";
    protected_run(rt, key_cb, &a, BUDGET_EVENT);
    return a.prevented;
}

static int has_handler(js_rt *rt, jobj *o, const char *name)
{
    struct jprop *p = o ? js_own(o, js_atom(rt, name)) : 0;
    return p && js_is_callable(p->v);
}

int js_wants_keys(struct js_rt *rt)
{
    if (!rt || rt->dead)
        return 0;
    if (rt->wants_keys)
        return 1;
    static const char *const names[] = { "onkeydown", "onkeyup", "onkeypress" };
    struct dom_node *body = doc_body(rt);
    for (int i = 0; i < 3; i++) {
        if (has_handler(rt, rt->global, names[i]) || has_handler(rt, rt->document, names[i]))
            return 1;
        if (body && (dom_attr(body, names[i]) || (body->js && has_handler(rt, body->js, names[i]))))
            return 1;
    }
    return 0;
}

struct timer_args {
    struct js_timer t;
};

static void timer_cb(js_rt *rt, void *arg)
{
    struct js_timer *t = &((struct timer_args *)arg)->t;
    if (t->code) {
        js_run_program(rt, t->code, (unsigned)strlen(t->code));
        return;
    }
    if (t->raf) {
        jv now = jv_num(timer_uptime_ms() - rt->clock_base_uptime);
        js_call(rt, t->fn, jv_undef(), 1, &now);
    } else {
        js_call(rt, t->fn, jv_undef(), t->nargs, t->args);
    }
}

void js_timers(struct js_rt *rt)
{
    if (!rt || rt->dead || !rt->ntimers)
        return;
    uint32_t now = timer_uptime_ms();
    int ids[64], n = 0;
    uint32_t dues[64];
    for (int i = 0; i < rt->ntimers && n < 64; i++)
        if ((int32_t)(now - rt->timers[i].due) >= 0) {
            /* po kolei wedlug czasu (a przy rownym - wedlug kolejnosci utworzenia) */
            int k = n++;
            while (k > 0 && ((int32_t)(dues[k - 1] - rt->timers[i].due) > 0 ||
                             (dues[k - 1] == rt->timers[i].due && ids[k - 1] > rt->timers[i].id))) {
                ids[k] = ids[k - 1];
                dues[k] = dues[k - 1];
                k--;
            }
            ids[k] = rt->timers[i].id;
            dues[k] = rt->timers[i].due;
        }
    for (int k = 0; k < n; k++) {
        struct js_timer *t = 0;
        for (int i = 0; i < rt->ntimers; i++)
            if (rt->timers[i].id == ids[k])
                t = &rt->timers[i];
        if (!t)
            continue;                                /* wylaczony przez wczesniejszy timer */
        struct timer_args a;
        a.t = *t;
        if (t->interval)
            t->due = timer_uptime_ms() + (uint32_t)t->interval;
        else
            clear_timer(rt, t->id);
        rt->script_name = "timer";
        if (protected_run(rt, timer_cb, &a, BUDGET_EVENT) != 0 && rt->abort_msg && a.t.interval)
            clear_timer(rt, a.t.id);                 /* zawieszajacy sie setInterval - wylaczamy */
        if (rt->dead)
            return;
    }
}

int js_has_timers(struct js_rt *rt)
{
    return rt && !rt->dead && (rt->ntimers > 0 || js_net_pending(rt) > 0);   /* tez czekajace fetch */
}

void js_run_url(struct js_rt *rt, const char *code)
{
    if (!rt)
        return;
    char dec[URL_MAX];
    url_decode(code, dec, sizeof(dec));
    js_run(rt, dec, (unsigned)strlen(dec), "javascript:", 0);
}

static void resize_cb(js_rt *rt, void *arg U)
{
    jobj *ev = make_event(rt, js_atom(rt, "resize"), 0, 0);
    fire(rt, 0, ev);
}

void js_resize(struct js_rt *rt, int view_w, int view_h)
{
    if (!rt)
        return;
    if (rt->host.view_w == view_w && rt->host.view_h == view_h)
        return;
    rt->host.view_w = view_w;
    rt->host.view_h = view_h;
    rt->script_name = "resize";
    protected_run(rt, resize_cb, 0, BUDGET_EVENT);
}
