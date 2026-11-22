/* System plikow w pamieci RAM (ramfs). Kazdy plik i katalog to wezel
 * przydzielony przez kmalloc, a tresc pliku to osobny bufor z kmalloc.
 * Trwalosc zapewnia storage.c, ktory zapisuje cale drzewo na dysk. */

#include "fs.h"
#include "heap.h"
#include "string.h"
#include "kernel.h"

static struct fs_node *root;
static int dirty;                 /* cos sie zmienilo od ostatniego fs_clear_dirty() */

int  fs_is_dirty(void)    { return dirty; }
void fs_clear_dirty(void) { dirty = 0; }

static const char *const errors[] = {
    "OK",
    "nie ma takiego pliku ani katalogu",
    "juz istnieje",
    "to nie jest katalog",
    "to jest katalog",
    "katalog nie jest pusty",
    "brak pamieci",
    "niepoprawna nazwa lub sciezka",
    "katalog jest w uzyciu",
};

const char *fs_strerror(int err)
{
    if (err > 0 || -err >= (int)(sizeof(errors) / sizeof(errors[0])))
        return "nieznany blad";
    return errors[-err];
}

struct fs_node *fs_root(void)
{
    return root;
}

static int contains_node(const struct fs_node *dir, const struct fs_node *n)
{
    for (const struct fs_node *c = dir->children; c; c = c->next)
        if (c == n || (c->is_dir && contains_node(c, n)))
            return 1;
    return 0;
}

int fs_node_valid(const struct fs_node *n)
{
    return n && (n == root || contains_node(root, n));
}

static struct fs_node *find_child(struct fs_node *dir, const char *name, size_t len)
{
    for (struct fs_node *c = dir->children; c; c = c->next)
        if (strlen(c->name) == len && memcmp(c->name, name, len) == 0)
            return c;
    return 0;
}

int fs_lookup(struct fs_node *cwd, const char *path, struct fs_node **out)
{
    struct fs_node *n = (path[0] == '/') ? root : cwd;

    while (*path) {
        while (*path == '/')
            path++;
        const char *end = path;
        while (*end && *end != '/')
            end++;
        size_t len = (size_t)(end - path);
        if (len == 0)
            break;

        if (!n->is_dir)
            return FS_ENOTDIR;
        if (len == 1 && path[0] == '.') {
            /* zostajemy w miejscu */
        } else if (len == 2 && path[0] == '.' && path[1] == '.') {
            if (n->parent)
                n = n->parent;
        } else {
            n = find_child(n, path, len);
            if (!n)
                return FS_ENOENT;
        }
        path = end;
    }
    *out = n;
    return FS_OK;
}

static int valid_name(const char *name)
{
    size_t len = strlen(name);
    if (len == 0 || len > FS_NAME_MAX)
        return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return 0;
    for (size_t i = 0; i < len; i++)
        if (name[i] == '/' || name[i] < ' ')
            return 0;
    return 1;
}

/* Dzieli "a/b/c" na katalog "a/b" i nazwe "c" */
static int split_path(const char *path, char *dir, char *name)
{
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/')      /* ignorujemy koncowe "/" */
        len--;
    if (len == 0 || len >= FS_PATH_MAX)
        return FS_EINVAL;

    size_t slash = len;
    while (slash > 0 && path[slash - 1] != '/')
        slash--;

    size_t name_len = len - slash;
    if (name_len == 0 || name_len > FS_NAME_MAX)
        return FS_EINVAL;
    memcpy(name, path + slash, name_len);
    name[name_len] = '\0';

    if (slash == 0) {
        strcpy(dir, ".");
    } else if (slash == 1) {
        strcpy(dir, "/");
    } else {
        memcpy(dir, path, slash - 1);
        dir[slash - 1] = '\0';
    }
    return FS_OK;
}

int fs_create(struct fs_node *cwd, const char *path, int is_dir, struct fs_node **out)
{
    char dir_path[FS_PATH_MAX], name[FS_NAME_MAX + 1];
    struct fs_node *dir;
    int err;

    if ((err = split_path(path, dir_path, name)) != FS_OK)
        return err;
    if (!valid_name(name))
        return FS_EINVAL;
    if ((err = fs_lookup(cwd, dir_path, &dir)) != FS_OK)
        return err;
    if (!dir->is_dir)
        return FS_ENOTDIR;
    if (find_child(dir, name, strlen(name)))
        return FS_EEXIST;

    /* Pelna sciezka (np. /a/b/plik) musi sie zmiescic w FS_PATH_MAX - inaczej
     * nie dalaby sie zapisac na dysk i wczytac z powrotem */
    size_t full = strlen(name) + 1;
    for (const struct fs_node *p = dir; p != root; p = p->parent)
        full += strlen(p->name) + 1;
    if (full >= FS_PATH_MAX)
        return FS_EINVAL;

    struct fs_node *n = kzalloc(sizeof(*n));
    if (!n)
        return FS_ENOMEM;
    strcpy(n->name, name);
    n->is_dir = is_dir;
    n->parent = dir;

    /* wstawiamy tak, zeby lista byla posortowana alfabetycznie */
    struct fs_node **link = &dir->children;
    while (*link && strcmp((*link)->name, name) < 0)
        link = &(*link)->next;
    n->next = *link;
    *link = n;
    dirty = 1;

    if (out)
        *out = n;
    return FS_OK;
}

int fs_write(struct fs_node *file, const char *data, uint32_t len, int append)
{
    if (file->is_dir)
        return FS_EISDIR;

    uint32_t keep = append ? file->size : 0;
    char *buf = 0;
    if (keep + len > 0) {
        buf = kmalloc(keep + len);
        if (!buf)
            return FS_ENOMEM;
        memcpy(buf, file->data, keep);
        memcpy(buf + keep, data, len);
    }
    kfree(file->data);
    file->data = buf;
    file->size = keep + len;
    dirty = 1;
    return FS_OK;
}

int fs_remove(struct fs_node *cwd, const char *path)
{
    struct fs_node *n;
    int err = fs_lookup(cwd, path, &n);
    if (err != FS_OK)
        return err;
    if (n == root)
        return FS_EINVAL;
    if (n->is_dir && n->children)
        return FS_ENOTEMPTY;
    for (struct fs_node *p = cwd; p; p = p->parent)   /* nie usuwamy katalogu, w ktorym stoimy */
        if (p == n)
            return FS_EBUSY;

    struct fs_node **link = &n->parent->children;
    while (*link != n)
        link = &(*link)->next;
    *link = n->next;

    kfree(n->data);
    kfree(n);
    dirty = 1;
    return FS_OK;
}

void fs_path(const struct fs_node *node, char *buf, size_t size)
{
    const struct fs_node *chain[FS_PATH_MAX / 2];
    int depth = 0;
    for (; node && node != root && depth < (int)(sizeof(chain) / sizeof(chain[0])); node = node->parent)
        chain[depth++] = node;

    size_t pos = 0;
    if (depth == 0 && size > 1)
        buf[pos++] = '/';
    while (depth-- > 0) {
        const char *name = chain[depth]->name;
        if (pos + 1 < size)
            buf[pos++] = '/';
        while (*name && pos + 1 < size)
            buf[pos++] = *name++;
    }
    buf[pos] = '\0';
}

static void add_file(const char *path, const char *text)
{
    struct fs_node *f;
    if (fs_create(root, path, 0, &f) == FS_OK)
        fs_write(f, text, strlen(text), 0);
}

void fs_init(void)
{
    root = kzalloc(sizeof(*root));
    if (!root)
        panic("fs_init: brak pamieci na katalog glowny");
    root->is_dir = 1;
}

void fs_seed(void)
{
    add_file("/witaj.txt",
             "Witaj w systemie plikow NaszOS!\n"
             "Pliki sa zapisywane na dysku i zostaja po wylaczeniu komputera.\n"
             "Wpisz 'help', aby zobaczyc komendy do obslugi plikow.\n");
    fs_create(root, "/dokumenty", 1, 0);
    add_file("/dokumenty/lista.txt", "1. System operacyjny\n2. System plikow\n3. ...\n");
}
