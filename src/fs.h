#ifndef FS_H
#define FS_H

#include <stddef.h>
#include <stdint.h>

#define FS_NAME_MAX 31
#define FS_PATH_MAX 128

/* Kody bledow (ujemne); fs_strerror() zamienia je na tekst */
enum {
    FS_OK        =  0,
    FS_ENOENT    = -1,   /* nie ma takiego pliku ani katalogu */
    FS_EEXIST    = -2,
    FS_ENOTDIR   = -3,
    FS_EISDIR    = -4,
    FS_ENOTEMPTY = -5,
    FS_ENOMEM    = -6,
    FS_EINVAL    = -7,   /* niepoprawna nazwa lub sciezka */
    FS_EBUSY     = -8,   /* katalog jest w uzyciu (np. biezacy) */
};

/* Wezel drzewa: plik albo katalog. Dzieci katalogu tworza liste
 * posortowana alfabetycznie (children -> next -> next ...). */
struct fs_node {
    char name[FS_NAME_MAX + 1];
    int is_dir;
    uint32_t size;               /* pliki: dlugosc danych */
    char *data;                  /* pliki: tresc z kmalloc (bez '\0' na koncu) */
    struct fs_node *parent;
    struct fs_node *children;    /* katalogi: pierwszy element */
    struct fs_node *next;        /* nastepny w tym samym katalogu */
};

void fs_init(void);                  /* pusty katalog glowny */
void fs_seed(void);                  /* pliki przykladowe (gdy nie ma nic na dysku) */
struct fs_node *fs_root(void);

/* Czy wezel nadal jest w drzewie (np. okno pamieta plik, ktory ktos usunal) */
int fs_node_valid(const struct fs_node *n);

/* Czy od ostatniego zapisu na dysk cos sie zmienilo */
int  fs_is_dirty(void);
void fs_clear_dirty(void);

/* Sciezki moga byc bezwzgledne (/a/b) lub wzgledne wobec cwd; obsluguja . i .. */
int fs_lookup(struct fs_node *cwd, const char *path, struct fs_node **out);
int fs_create(struct fs_node *cwd, const char *path, int is_dir, struct fs_node **out);
int fs_remove(struct fs_node *cwd, const char *path);
int fs_write(struct fs_node *file, const char *data, uint32_t len, int append);

void fs_path(const struct fs_node *node, char *buf, size_t size);
const char *fs_strerror(int err);

#endif
