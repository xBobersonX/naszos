/* Budowanie drzewa DOM z HTML - patrz dom.h.
 *
 * Czytamy strone znak po znaku. Znacznik otwierajacy (<div>) dodaje dziecko
 * do biezacego wezla i "wchodzi" do niego (stos otwartych znacznikow),
 * zamykajacy (</div>) wraca w gore. HTML wybacza bledy, wiec niektore
 * znaczniki zamykamy sami: nowy <p> albo <div> konczy otwarty <p>, nowy
 * <li> konczy poprzedni <li>, nowa komorka tabeli - poprzednia komorke itd.
 * Znaczniki "puste" (<br>, <img>, <hr>...) nie maja dzieci. */

#include "dom.h"
#include "html.h"
#include "heap.h"
#include "string.h"
#include "utf8.h"
#include "thread.h"

#define DEPTH_MAX 200
#define SHEETS_MAX 8

static const char *const tag_names[T_COUNT] = {
    [T_OTHER] = "", [T_HTML] = "html", [T_HEAD] = "head", [T_BODY] = "body", [T_TITLE] = "title",
    [T_STYLE] = "style", [T_SCRIPT] = "script", [T_LINK] = "link", [T_META] = "meta", [T_A] = "a",
    [T_IMG] = "img", [T_BR] = "br", [T_HR] = "hr", [T_P] = "p", [T_DIV] = "div", [T_SPAN] = "span",
    [T_UL] = "ul", [T_OL] = "ol", [T_LI] = "li", [T_DL] = "dl", [T_DT] = "dt", [T_DD] = "dd",
    [T_TABLE] = "table", [T_THEAD] = "thead", [T_TBODY] = "tbody", [T_TFOOT] = "tfoot",
    [T_TR] = "tr", [T_TD] = "td", [T_TH] = "th", [T_CAPTION] = "caption",
    [T_H1] = "h1", [T_H2] = "h2", [T_H3] = "h3", [T_H4] = "h4", [T_H5] = "h5", [T_H6] = "h6",
    [T_PRE] = "pre", [T_BLOCKQUOTE] = "blockquote", [T_B] = "b", [T_STRONG] = "strong",
    [T_I] = "i", [T_EM] = "em", [T_INPUT] = "input", [T_BUTTON] = "button", [T_SELECT] = "select",
    [T_OPTION] = "option", [T_TEXTAREA] = "textarea", [T_FORM] = "form", [T_NOSCRIPT] = "noscript",
    [T_TEMPLATE] = "template", [T_SVG] = "svg", [T_IFRAME] = "iframe", [T_CENTER] = "center",
    [T_FONT] = "font", [T_SMALL] = "small", [T_SUP] = "sup", [T_SUB] = "sub", [T_LABEL] = "label",
    [T_NAV] = "nav", [T_CANVAS] = "canvas",
};

/* Znaczniki bez dzieci */
static const char *const void_tags[] = {
    "area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param",
    "source", "track", "wbr", 0,
};

/* Znaczniki, ktore koncza otwarty <p> */
static const char *const closes_p[] = {
    "p", "div", "ul", "ol", "dl", "table", "h1", "h2", "h3", "h4", "h5", "h6", "pre",
    "blockquote", "form", "hr", "section", "article", "header", "footer", "nav", "main",
    "aside", "figure", "center", "address", "fieldset", "menu", "details", "figcaption", 0,
};

struct builder {
    struct dom_doc *d;
    struct dom_node *stack[DEPTH_MAX];
    int depth;
    int charset;
    int fragment;                /* innerHTML - skrypty nie trafiaja na liste do wykonania */
    dom_script_fn on_script;     /* wykonanie skryptu w chwili, gdy parser na niego trafi */
    int scripting;               /* JavaScript wlaczony - tresc <noscript> pomijamy */
    void *cb_ctx;
};

static int in_list(const char *name, const char *const *list)
{
    for (; *list; list++)
        if (strcmp(name, *list) == 0)
            return 1;
    return 0;
}

/* media="print" - styl tylko do drukowania */
static int print_only(const char *media)
{
    return strstr_ci(media, "print") && !strstr_ci(media, "screen") && !strstr_ci(media, "all");
}

static int tag_id(const char *name)
{
    for (int i = 1; i < T_COUNT; i++)
        if (strcmp(name, tag_names[i]) == 0)
            return i;
    return T_OTHER;
}

int dom_tag_id(const char *name) { return tag_id(name); }
const char *dom_tag_name(int tag) { return tag > 0 && tag < T_COUNT ? tag_names[tag] : ""; }

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

const char *dom_attr(const struct dom_node *n, const char *name)
{
    for (const struct dom_attr *a = n->attrs; a; a = a->next)
        if (strcmp(a->name, name) == 0)
            return a->value;
    return 0;
}

int dom_has_class(const struct dom_node *n, const char *cls, unsigned len)
{
    for (const char *s = n->cls; s && *s;) {
        while (*s == ' ' || *s == '\t' || *s == '\n')
            s++;
        const char *w = s;
        while (*s && *s != ' ' && *s != '\t' && *s != '\n')
            s++;
        if ((unsigned)(s - w) == len && memcmp(w, cls, len) == 0)
            return 1;
    }
    return 0;
}

static struct dom_node *cur(struct builder *b) { return b->stack[b->depth - 1]; }

static struct dom_node *new_node(struct builder *b, int type)
{
    struct dom_node *n = arena_alloc(&b->d->mem, sizeof(*n));
    if (n) {
        n->type = (uint8_t)type;
        n->img = -1;
        b->d->nodes++;
    }
    return n;
}

static void append(struct dom_node *parent, struct dom_node *n)
{
    n->parent = parent;
    n->prev = parent->last;
    if (parent->last)
        parent->last->next = n;
    else
        parent->first = n;
    parent->last = n;
}

/* Tekst z encjami (&amp;) i w starym kodowaniu -> UTF-8 w arenie */
static char *decode_text(struct builder *b, const char *s, const char *end, unsigned *out_len)
{
    unsigned cap = (unsigned)(end - s) * (b->charset == HC_UTF8 ? 1 : 3) + 8;
    char *o = arena_alloc(&b->d->mem, cap), *start = o;
    if (!o)
        return 0;
    while (s < end && (unsigned)(o - start) + 4 < cap) {
        if (*s == '&') {
            const char *e = s;
            uint32_t cp = html_entity(&e, end);
            if (cp) {
                o += utf8_put(cp, o);
                s = e;
                continue;
            }
        }
        if (b->charset != HC_UTF8 && (uint8_t)*s >= 0x80) {
            o += utf8_put(html_legacy_char((uint8_t)*s++, b->charset), o);
            continue;
        }
        *o++ = *s++;
    }
    *o = '\0';
    *out_len = (unsigned)(o - start);
    return start;
}

static int only_space(const char *s, const char *end)
{
    for (; s < end; s++)
        if (*s != ' ' && *s != '\n' && *s != '\r' && *s != '\t' && *s != '\f')
            return 0;
    return 1;
}

static void add_text(struct builder *b, const char *s, const char *end)
{
    if (s >= end)
        return;
    struct dom_node *parent = cur(b);
    /* Odstepy miedzy wierszami tabeli czy punktami listy nic nie znacza */
    if (only_space(s, end) && (parent->tag == T_HTML || parent->tag == T_HEAD || parent->tag == T_TABLE ||
                               parent->tag == T_THEAD || parent->tag == T_TBODY || parent->tag == T_TFOOT ||
                               parent->tag == T_TR || parent->tag == T_UL || parent->tag == T_OL ||
                               parent->tag == T_DL || parent->tag == T_SELECT))
        return;
    struct dom_node *n = new_node(b, N_TEXT);
    if (!n)
        return;
    n->text = decode_text(b, s, end, &n->text_len);
    if (n->text)
        append(parent, n);
}

/* Zamyka (zdejmuje ze stosu) znaczniki az do tego o nazwie name. Szukamy
 * najwyzej do "granicy" (np. nie zamykamy <p> spoza biezacej komorki tabeli). */
static int close_to(struct builder *b, const char *name, const char *const *stop)
{
    for (int i = b->depth - 1; i > 0; i--) {
        struct dom_node *n = b->stack[i];
        if (strcmp(n->name, name) == 0) {
            b->depth = i;
            return 1;
        }
        if (stop && in_list(n->name, stop))
            return 0;
    }
    return 0;
}

static void add_image_doc(struct dom_doc *d, struct dom_node *n)
{
    const char *src = dom_attr(n, "data-src");
    if (!src)
        src = dom_attr(n, "src");
    if (!src || !src[0])
        return;
    if (d->nimages == d->images_cap) {
        int cap = d->images_cap ? d->images_cap * 2 : 32;
        const char **ni = arena_alloc(&d->mem, sizeof(char *) * (unsigned)cap);
        struct dom_node **nn = arena_alloc(&d->mem, sizeof(struct dom_node *) * (unsigned)cap);
        if (!ni || !nn)
            return;
        if (d->images) {
            memcpy(ni, d->images, sizeof(char *) * (unsigned)d->nimages);
            memcpy(nn, d->image_nodes, sizeof(struct dom_node *) * (unsigned)d->nimages);
        }
        d->images = ni;
        d->image_nodes = nn;
        d->images_cap = cap;
    }
    n->img = d->nimages;
    d->image_nodes[d->nimages] = n;
    d->images[d->nimages++] = src;
}

static int attr_uint(const struct dom_node *n, const char *name, int def)
{
    const char *v = dom_attr(n, name);
    if (!v)
        return def;
    while (*v == ' ')
        v++;
    if (*v < '0' || *v > '9')
        return def;                              /* HTML: zly zapis = wartosc domyslna */
    int x = 0;
    for (; *v >= '0' && *v <= '9'; v++)
        if (x < 100000)
            x = x * 10 + (*v - '0');
    return x;
}

void dom_canvas_dims(const struct dom_node *n, int *w, int *h)
{
    *w = attr_uint(n, "width", 300);
    *h = attr_uint(n, "height", 150);
}

struct dom_canvas *dom_canvas_sync(struct dom_doc *d, struct dom_node *n)
{
    int w, h;
    dom_canvas_dims(n, &w, &h);
    struct dom_canvas *c = n->canvas;
    if (c && c->w == w && c->h == h)
        return c;
    if (!c) {
        c = kzalloc(sizeof(*c));
        if (!c)
            return 0;
        c->next = d->canvases;
        d->canvases = c;
        n->canvas = c;
    }
    /* Nowy rozmiar = nowe, puste plotno (tak jak w prawdziwej przegladarce) */
    uint32_t *px = 0;
    if (w > 0 && h > 0) {
        if ((int64_t)w * h > CANVAS_MAX_PIXELS)
            return 0;
        px = kzalloc((size_t)w * (size_t)h * 4);
        if (!px)
            return 0;
    }
    uint32_t *old = c->px;
    c->px = px;                                  /* najpierw nowe piksele, potem zwalniamy stare */
    c->w = w;
    c->h = h;
    kfree(old);
    d->canvas_gen++;
    return c;
}

static void add_image(struct builder *b, struct dom_node *n)
{
    add_image_doc(b->d, n);
}

void dom_add_image(struct dom_doc *d, struct dom_node *n)
{
    add_image_doc(d, n);
}

static void add_script(struct dom_doc *d, struct dom_node *n)
{
    if (d->nscripts == d->scripts_cap) {
        int cap = d->scripts_cap ? d->scripts_cap * 2 : 16;
        struct dom_node **ns = arena_alloc(&d->mem, sizeof(struct dom_node *) * (unsigned)cap);
        if (!ns)
            return;
        if (d->scripts)
            memcpy(ns, d->scripts, sizeof(struct dom_node *) * (unsigned)d->nscripts);
        d->scripts = ns;
        d->scripts_cap = cap;
    }
    d->scripts[d->nscripts++] = n;
}

static void add_css(struct dom_doc *d, const char *s, unsigned len)
{
    if (d->css_len + len + 2 > d->css_cap) {
        unsigned cap = (d->css_len + len + 2) * 2;
        char *nc = kmalloc(cap);
        if (!nc)
            return;
        if (d->css)
            memcpy(nc, d->css, d->css_len);
        kfree(d->css);
        d->css = nc;
        d->css_cap = cap;
    }
    memcpy(d->css + d->css_len, s, len);
    d->css_len += len;
    d->css[d->css_len++] = '\n';
    d->css[d->css_len] = '\0';
}

/* Tresc surowa az do </name> (skrypt, styl) - zwraca poczatek konca */
static const char *raw_until(const char *s, const char *end, const char *name)
{
    unsigned nl = (unsigned)strlen(name);
    for (; s + nl + 2 <= end; s++)
        if (s[0] == '<' && s[1] == '/') {
            unsigned i = 0;
            while (i < nl && lower(s[2 + i]) == name[i])
                i++;
            if (i == nl)
                return s;
        }
    return end;
}

/* Atrybuty znacznika: nazwa="wartosc" nazwa='wartosc' nazwa=wartosc nazwa */
static void parse_attrs(struct builder *b, struct dom_node *n, const char *a, const char *end)
{
    struct dom_attr **tail = &n->attrs;
    while (a < end) {
        while (a < end && (*a == ' ' || *a == '\t' || *a == '\n' || *a == '\r' || *a == '/'))
            a++;
        const char *ns = a;
        while (a < end && *a != '=' && *a != ' ' && *a != '\t' && *a != '\n' && *a != '\r' && *a != '/')
            a++;
        unsigned nlen = (unsigned)(a - ns);
        if (!nlen) {
            a++;
            continue;
        }
        while (a < end && (*a == ' ' || *a == '\t' || *a == '\n'))
            a++;
        const char *vs = a, *ve = a;
        if (a < end && *a == '=') {
            a++;
            while (a < end && (*a == ' ' || *a == '\t' || *a == '\n'))
                a++;
            if (a < end && (*a == '"' || *a == '\'')) {
                char q = *a++;
                vs = a;
                while (a < end && *a != q)
                    a++;
                ve = a;
                if (a < end)
                    a++;
            } else {
                vs = a;
                while (a < end && *a != ' ' && *a != '\t' && *a != '\n' && *a != '>')
                    a++;
                ve = a;
            }
        }
        struct dom_attr *at = arena_alloc(&b->d->mem, sizeof(*at));
        char *name = arena_strndup(&b->d->mem, ns, nlen);
        if (!at || !name)
            return;
        for (char *c = name; *c; c++)
            *c = (char)lower(*c);
        unsigned vlen;
        at->name = name;
        at->value = decode_text(b, vs, ve, &vlen);
        if (!at->value)
            return;
        *tail = at;
        tail = &at->next;
        if (strcmp(name, "id") == 0)
            n->id = at->value;
        else if (strcmp(name, "class") == 0)
            n->cls = at->value;
    }
}

static void open_element(struct builder *b, struct dom_node *n, int self_closing)
{
    static const char *const cell_stop[] = { "table", 0 };
    static const char *const li_stop[] = { "ul", "ol", "table", "td", "th", 0 };
    static const char *const dd_stop[] = { "dl", "table", 0 };
    static const char *const p_stop[] = { "table", "td", "th", "button", "li", 0 };
    const char *name = n->name;

    /* Znaczniki zamykane "same" */
    if (in_list(name, closes_p))
        close_to(b, "p", p_stop);
    if (n->tag == T_LI)
        close_to(b, "li", li_stop);
    if (n->tag == T_DT || n->tag == T_DD) {
        if (!close_to(b, "dd", dd_stop))
            close_to(b, "dt", dd_stop);
    }
    if (n->tag == T_TR) {
        close_to(b, "tr", cell_stop);
    }
    if (n->tag == T_TD || n->tag == T_TH) {
        if (!close_to(b, "td", cell_stop))
            close_to(b, "th", cell_stop);
    }
    if (n->tag == T_THEAD || n->tag == T_TBODY || n->tag == T_TFOOT) {
        if (!close_to(b, "tbody", cell_stop) && !close_to(b, "thead", cell_stop))
            close_to(b, "tfoot", cell_stop);
    }
    if (n->tag == T_OPTION && cur(b)->tag == T_OPTION)
        b->depth--;
    if (n->tag == T_A)
        close_to(b, "a", cell_stop);                  /* link w linku niedozwolony */

    append(cur(b), n);
    if (!self_closing && !in_list(name, void_tags) && b->depth < DEPTH_MAX)
        b->stack[b->depth++] = n;
}

/* Obsluga "<...>" zaczynajacego sie w s. Zwraca, gdzie czytac dalej. */
static const char *tag(struct builder *b, const char *s, const char *end)
{
    const char *q = s + 1;
    if (q + 3 <= end && memcmp(q, "!--", 3) == 0) {             /* komentarz */
        for (q += 3; q + 3 <= end && memcmp(q, "-->", 3) != 0; q++)
            ;
        return q + 3 <= end ? q + 3 : end;
    }
    if (q < end && (*q == '!' || *q == '?')) {                 /* <!DOCTYPE ...>, <?xml ...?> */
        while (q < end && *q != '>')
            q++;
        return q < end ? q + 1 : end;
    }
    int closing = q < end && *q == '/';
    if (closing)
        q++;
    const char *ns = q;
    while (q < end && ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') ||
                       *q == '-' || *q == ':'))
        q++;
    unsigned nlen = (unsigned)(q - ns);
    if (!nlen || nlen > 31) {
        add_text(b, s, s + 1);                                 /* samotny "<" w tekscie */
        return s + 1;
    }
    char name[32];
    for (unsigned i = 0; i < nlen; i++)
        name[i] = (char)lower(ns[i]);
    name[nlen] = '\0';
    /* Koniec znacznika: ">" poza cudzyslowem */
    const char *attrs = q;
    char quote = 0;
    while (q < end && (quote || *q != '>')) {
        if (quote && *q == quote)
            quote = 0;
        else if (!quote && (*q == '"' || *q == '\''))
            quote = *q;
        q++;
    }
    const char *attrs_end = q;
    int self_closing = attrs_end > attrs && attrs_end[-1] == '/';
    const char *next = q < end ? q + 1 : end;

    if (closing) {
        if (strcmp(name, "br") == 0) {                          /* </br> jak <br> */
            struct dom_node *n = new_node(b, N_ELEMENT);
            if (n) {
                n->name = "br";
                n->tag = T_BR;
                append(cur(b), n);
            }
        } else if (strcmp(name, "p") == 0 && !close_to(b, "p", 0)) {
            /* </p> bez <p> - nic */
        } else {
            close_to(b, name, 0);
        }
        return next;
    }

    int t = tag_id(name);
    struct dom_doc *d = b->d;

    /* <script>: wezel w drzewie (z trescia jako tekst) i na liste skryptow strony */
    if (t == T_SCRIPT) {
        struct dom_node *n = new_node(b, N_ELEMENT);
        if (!n)
            return next;
        n->name = tag_names[T_SCRIPT];
        n->tag = T_SCRIPT;
        parse_attrs(b, n, attrs, attrs_end);
        const char *stop = self_closing ? next : raw_until(next, end, name);
        if (stop > next) {
            struct dom_node *txt = new_node(b, N_TEXT);
            if (txt) {
                txt->text = arena_strndup(&d->mem, next, (size_t)(stop - next));
                txt->text_len = txt->text ? (unsigned)(stop - next) : 0;
                if (txt->text)
                    append(n, txt);
            }
        }
        append(cur(b), n);
        if (!b->fragment) {
            add_script(d, n);
            if (b->on_script)                     /* skrypt widzi strone tylko do tego miejsca */
                b->on_script(b->cb_ctx, d, n);
        }
        if (self_closing)
            return next;
        while (stop < end && *stop != '>')
            stop++;
        return stop < end ? stop + 1 : end;
    }

    /* Tresc, ktorej nie pokazujemy - ale <style> zbieramy dla CSS */
    if (t == T_TEMPLATE || t == T_STYLE || t == T_TEXTAREA || (t == T_NOSCRIPT && b->scripting) ||
        strcmp(name, "math") == 0 || strcmp(name, "noembed") == 0) {
        if (self_closing)
            return next;
        const char *stop = raw_until(next, end, name);
        if (t == T_STYLE) {
            char media[64];
            if (!html_attr(attrs, attrs_end, "media", media, sizeof(media)) || !print_only(media))
                add_css(d, next, (unsigned)(stop - next));
        }
        while (stop < end && *stop != '>')
            stop++;
        return stop < end ? stop + 1 : end;
    }
    if (t == T_TITLE) {
        const char *stop = raw_until(next, end, "title");
        unsigned tl;
        char *text = decode_text(b, next, stop, &tl);
        if (text && !d->title[0]) {
            unsigned n = 0;
            for (unsigned i = 0; i < tl && n + 1 < sizeof(d->title); i++) {
                char c = text[i] == '\n' || text[i] == '\t' || text[i] == '\r' ? ' ' : text[i];
                if (c == ' ' && (n == 0 || d->title[n - 1] == ' '))
                    continue;
                d->title[n++] = c;
            }
            while (n && d->title[n - 1] == ' ')
                n--;
            d->title[n] = '\0';
        }
        while (stop < end && *stop != '>')
            stop++;
        return stop < end ? stop + 1 : end;
    }

    struct dom_node *n = new_node(b, N_ELEMENT);
    if (!n)
        return next;
    n->name = t ? tag_names[t] : arena_strndup(&d->mem, name, nlen);
    n->tag = (uint8_t)t;
    parse_attrs(b, n, attrs, attrs_end);
    if (t == T_LINK) {
        const char *rel = dom_attr(n, "rel"), *href = dom_attr(n, "href"), *media = dom_attr(n, "media");
        if (rel && href && strstr_ci(rel, "stylesheet") && !strstr_ci(rel, "alternate") &&
            (!media || !print_only(media)) && d->nsheets < SHEETS_MAX) {
            if (!d->sheets)
                d->sheets = arena_alloc(&d->mem, sizeof(char *) * SHEETS_MAX);
            if (d->sheets)
                d->sheets[d->nsheets++] = href;
        }
    }
    if (t == T_IMG)
        add_image(b, n);
    open_element(b, n, self_closing);
    return next;
}

/* Przenosi dzieci "from" (od wezla first) do nowego rodzica "to" */
static void adopt(struct dom_node *from, struct dom_node *first, struct dom_node *to)
{
    struct dom_node *n = first;
    if (first->prev)
        first->prev->next = 0;
    else
        from->first = 0;
    from->last = first->prev;
    while (n) {
        struct dom_node *next = n->next;
        n->next = n->prev = 0;
        append(to, n);
        n = next;
    }
}

static struct dom_node *child_tag(struct dom_node *p, int tag)
{
    for (struct dom_node *c = p->first; c; c = c->next)
        if (c->type == N_ELEMENT && c->tag == tag)
            return c;
    return 0;
}

/* Strona bez <html> albo <body> - przegladarki dopisuja je same (style dla body!) */
static void ensure_body(struct builder *b)
{
    struct dom_node *root = b->d->root, *html = child_tag(root, T_HTML);
    if (!html) {
        html = new_node(b, N_ELEMENT);
        if (!html)
            return;
        html->name = "html";
        html->tag = T_HTML;
        if (root->first)
            adopt(root, root->first, html);
        append(root, html);
    }
    if (child_tag(html, T_BODY))
        return;
    struct dom_node *first = html->first;                 /* pomijamy <head> */
    while (first && first->type == N_ELEMENT && first->tag == T_HEAD)
        first = first->next;
    struct dom_node *body = new_node(b, N_ELEMENT);
    if (!body)
        return;
    body->name = "body";
    body->tag = T_BODY;
    if (first)
        adopt(html, first, body);
    append(html, body);
}

static void parse_html(struct builder *b, const char *src, const char *end)
{
    const char *s = src, *text = src, *breathe = src;
    while (s < end) {
        if (s - breathe > 16384) {                      /* duza strona - pulpit tez chce dzialac */
            breathe = s;
            thread_yield();
        }
        if (*s == '<' && s + 1 < end &&
            ((s[1] >= 'a' && s[1] <= 'z') || (s[1] >= 'A' && s[1] <= 'Z') || s[1] == '/' || s[1] == '!' || s[1] == '?')) {
            add_text(b, text, s);
            s = tag(b, s, end);
            text = s;
            continue;
        }
        s++;
    }
    add_text(b, text, end);
}

struct dom_doc *dom_parse(const char *src, unsigned len, int is_html, int charset)
{
    return dom_parse_scripts(src, len, is_html, charset, 0, 0);
}

struct dom_doc *dom_parse_scripts(const char *src, unsigned len, int is_html, int charset,
                                  dom_script_fn on_script, void *ctx)
{
    struct dom_doc *d = kzalloc(sizeof(*d));
    if (!d)
        return 0;
    struct builder *b = kzalloc(sizeof(*b));
    if (!b) {
        kfree(d);
        return 0;
    }
    b->d = d;
    b->charset = charset;
    b->on_script = on_script;
    b->cb_ctx = ctx;
    b->scripting = on_script != 0;
    d->root = new_node(b, N_ELEMENT);
    if (!d->root) {
        kfree(b);
        dom_free(d);
        return 0;
    }
    d->root->name = "#document";
    b->stack[0] = d->root;
    b->depth = 1;

    const char *end = src + len;
    if (!is_html) {                                     /* zwykly tekst: jeden <pre> */
        struct dom_node *pre = new_node(b, N_ELEMENT);
        if (pre) {
            pre->name = "pre";
            pre->tag = T_PRE;
            open_element(b, pre, 0);
            add_text(b, src, end);
        }
        ensure_body(b);
        kfree(b);
        return d;
    }
    parse_html(b, src, end);
    ensure_body(b);
    kfree(b);
    return d;
}

struct dom_node *dom_parse_fragment(struct dom_doc *d, const char *html, unsigned len)
{
    struct builder *b = kzalloc(sizeof(*b));
    if (!b)
        return 0;
    b->d = d;
    b->charset = HC_UTF8;
    b->fragment = 1;
    b->scripting = 1;
    struct dom_node *root = new_node(b, N_ELEMENT);
    if (!root) {
        kfree(b);
        return 0;
    }
    root->name = "#fragment";
    b->stack[0] = root;
    b->depth = 1;
    parse_html(b, html, html + len);
    kfree(b);
    return root;
}

/* Nazwa znacznika malymi literami (do 31 znakow) */
static unsigned lower_name(const char *name, char *out, unsigned size)
{
    unsigned l = 0;
    for (; name[l] && l < size - 1; l++)
        out[l] = (char)lower(name[l]);
    out[l] = 0;
    return l;
}

struct dom_node *dom_new_element(struct dom_doc *d, const char *name)
{
    struct dom_node *n = arena_alloc(&d->mem, sizeof(*n));
    if (!n)
        return 0;
    char low[32];
    unsigned l = lower_name(name, low, sizeof(low));
    n->type = N_ELEMENT;
    n->img = -1;
    n->tag = (uint8_t)tag_id(low);
    n->name = n->tag ? tag_names[n->tag] : arena_strndup(&d->mem, low, l);
    d->nodes++;
    return n;
}

struct dom_node *dom_new_text(struct dom_doc *d, const char *text, unsigned len)
{
    struct dom_node *n = arena_alloc(&d->mem, sizeof(*n));
    if (!n)
        return 0;
    n->type = N_TEXT;
    n->img = -1;
    n->text = arena_strndup(&d->mem, text, len);
    n->text_len = n->text ? len : 0;
    d->nodes++;
    return n;
}

void dom_remove(struct dom_node *n)
{
    struct dom_node *p = n->parent;
    if (!p)
        return;
    if (n->prev)
        n->prev->next = n->next;
    else
        p->first = n->next;
    if (n->next)
        n->next->prev = n->prev;
    else
        p->last = n->prev;
    n->parent = n->prev = n->next = 0;
}

void dom_append(struct dom_node *parent, struct dom_node *n)
{
    dom_remove(n);
    n->next = 0;
    append(parent, n);
}

void dom_insert_before(struct dom_node *parent, struct dom_node *n, struct dom_node *ref)
{
    if (!ref || ref->parent != parent) {
        dom_append(parent, n);
        return;
    }
    if (n == ref)
        return;
    dom_remove(n);
    n->parent = parent;
    n->next = ref;
    n->prev = ref->prev;
    if (ref->prev)
        ref->prev->next = n;
    else
        parent->first = n;
    ref->prev = n;
}

void dom_set_attr(struct dom_doc *d, struct dom_node *n, const char *name, const char *value)
{
    char low[64];
    unsigned l = lower_name(name, low, sizeof(low));
    const char *v = arena_strndup(&d->mem, value, strlen(value));
    if (!v)
        return;
    struct dom_attr *a = n->attrs, *last = 0;
    for (; a; last = a, a = a->next)
        if (strcmp(a->name, low) == 0)
            break;
    if (!a) {
        a = arena_alloc(&d->mem, sizeof(*a));
        if (!a)
            return;
        a->name = arena_strndup(&d->mem, low, l);
        if (!a->name)
            return;
        if (last)
            last->next = a;
        else
            n->attrs = a;
    }
    a->value = v;
    if (strcmp(low, "id") == 0)
        n->id = v;
    else if (strcmp(low, "class") == 0)
        n->cls = v;
}

void dom_remove_attr(struct dom_node *n, const char *name)
{
    struct dom_attr **pa = &n->attrs;
    while (*pa) {
        if (strcmp((*pa)->name, name) == 0) {
            *pa = (*pa)->next;
            break;
        }
        pa = &(*pa)->next;
    }
    if (strcmp(name, "id") == 0)
        n->id = 0;
    else if (strcmp(name, "class") == 0)
        n->cls = 0;
}

void dom_free(struct dom_doc *d)
{
    if (!d)
        return;
    for (struct dom_canvas *c = d->canvases, *next; c; c = next) {
        next = c->next;
        kfree(c->px);
        kfree(c);
    }
    arena_free(&d->mem);
    kfree(d->css);
    kfree(d);
}
