/* Komendy plikowe powloki: ls, cd, pwd, mkdir, touch, write, append, cat, rm */

#include "shell_fs.h"
#include "fs.h"
#include "editor.h"
#include "vga.h"
#include "string.h"

/* Katalog biezacy jest zapisany w konsoli - kazde okno terminala ma swoj */
static struct fs_node *current_dir(void)
{
    if (!fs_node_valid(con_cur->cwd))    /* brak albo usuniety w innym oknie */
        con_cur->cwd = fs_root();
    return con_cur->cwd;
}

void shell_cwd_path(char *buf, size_t size)
{
    fs_path(current_dir(), buf, size);
}

/* Odcina pierwsze slowo z *args i przesuwa *args za nie */
static char *next_arg(char **args)
{
    char *s = *args;
    while (*s == ' ')
        s++;
    if (!*s)
        return 0;
    char *start = s;
    while (*s && *s != ' ')
        s++;
    if (*s)
        *s++ = '\0';
    while (*s == ' ')
        s++;
    *args = s;
    return start;
}

static void error(const char *cmd, const char *path, int err)
{
    vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
    kprintf("%s: %s: %s\n", cmd, path, fs_strerror(err));
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

static void usage(const char *text)
{
    kprintf("Uzycie: %s\n", text);
}

static void print_entry(const struct fs_node *n)
{
    if (n->is_dir) {
        vga_set_color(VGA_LIGHT_BLUE, VGA_BLACK);
        kprintf("  %s/\n", n->name);
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    } else {
        kprintf("  %s", n->name);
        for (size_t i = strlen(n->name); i < FS_NAME_MAX + 2; i++)
            vga_putc(' ');
        kprintf("%6u B\n", n->size);
    }
}

void cmd_ls(char *args)
{
    char *path = next_arg(&args);
    struct fs_node *n;
    int err = fs_lookup(current_dir(), path ? path : ".", &n);
    if (err) {
        error("ls", path, err);
        return;
    }
    if (!n->is_dir) {
        print_entry(n);
        return;
    }
    if (!n->children)
        vga_write("  (pusty katalog)\n");

    uint32_t files = 0, dirs = 0, bytes = 0;
    for (struct fs_node *c = n->children; c; c = c->next) {
        print_entry(c);
        if (c->is_dir) {
            dirs++;
        } else {
            files++;
            bytes += c->size;
        }
    }
    kprintf("Katalogow: %u, plikow: %u, razem %u B\n", dirs, files, bytes);
}

void cmd_cd(char *args)
{
    char *path = next_arg(&args);
    struct fs_node *n;
    int err = fs_lookup(current_dir(), path ? path : "/", &n);
    if (!err && !n->is_dir)
        err = FS_ENOTDIR;
    if (err) {
        error("cd", path, err);
        return;
    }
    con_cur->cwd = n;
}

void cmd_pwd(char *args)
{
    (void)args;
    char buf[FS_PATH_MAX];
    shell_cwd_path(buf, sizeof(buf));
    kprintf("%s\n", buf);
}

void cmd_mkdir(char *args)
{
    char *path = next_arg(&args);
    if (!path) {
        usage("mkdir <katalog>");
        return;
    }
    int err = fs_create(current_dir(), path, 1, 0);
    if (err)
        error("mkdir", path, err);
}

void cmd_touch(char *args)
{
    char *path = next_arg(&args);
    if (!path) {
        usage("touch <plik>");
        return;
    }
    struct fs_node *n;
    if (fs_lookup(current_dir(), path, &n) == FS_OK)
        return;                         /* plik juz istnieje - nic do zrobienia */
    int err = fs_create(current_dir(), path, 0, 0);
    if (err)
        error("touch", path, err);
}

/* write/append: tworzy plik, jesli go nie ma, i zapisuje tekst jako jedna linie */
static void write_common(const char *cmd, char *args, int append)
{
    char *path = next_arg(&args);
    if (!path) {
        kprintf("Uzycie: %s <plik> <tekst>\n", cmd);
        return;
    }
    struct fs_node *f;
    int err = fs_lookup(current_dir(), path, &f);
    if (err == FS_ENOENT)
        err = fs_create(current_dir(), path, 0, &f);
    if (err) {
        error(cmd, path, err);
        return;
    }

    size_t len = strlen(args);
    if (len > 0)
        args[len++] = '\n';             /* zamiast '\0' - kazdy zapis to osobna linia */
    err = fs_write(f, args, (uint32_t)len, append);
    if (err)
        error(cmd, path, err);
}

void cmd_write(char *args)
{
    write_common("write", args, 0);
}

void cmd_append(char *args)
{
    write_common("append", args, 1);
}

void cmd_cat(char *args)
{
    char *path = next_arg(&args);
    if (!path) {
        usage("cat <plik>");
        return;
    }
    struct fs_node *f;
    int err = fs_lookup(current_dir(), path, &f);
    if (!err && f->is_dir)
        err = FS_EISDIR;
    if (err) {
        error("cat", path, err);
        return;
    }
    for (uint32_t i = 0; i < f->size; i++)
        vga_putc(f->data[i]);
    if (f->size > 0 && f->data[f->size - 1] != '\n')
        vga_putc('\n');
}

void cmd_edit(char *args)
{
    char *path = next_arg(&args);
    if (!path) {
        usage("edit <plik>");
        return;
    }
    struct fs_node *f;
    int err = fs_lookup(current_dir(), path, &f);
    if (err == FS_OK && f->is_dir)
        err = FS_EISDIR;
    if (err != FS_OK && err != FS_ENOENT) {
        error("edit", path, err);
        return;
    }
    editor_run(current_dir(), path);
}

void cmd_rm(char *args)
{
    char *path = next_arg(&args);
    if (!path) {
        usage("rm <plik lub pusty katalog>");
        return;
    }
    int err = fs_remove(current_dir(), path);
    if (err)
        error("rm", path, err);
}
