/* Programy uzytkownika - patrz process.h.
 *
 * Uruchomienie: tworzymy nowy katalog stron, kopiujemy segmenty pliku ELF pod
 * ich adresy (USER_BASE...), mapujemy stos pod USER_END i "wracamy" do ring 3
 * pod adres startowy programu. Program konczy sie wywolaniem SYS_EXIT, bledem
 * (wyjatek procesora) albo zamknieciem okna - we wszystkich przypadkach
 * wracamy do process_run przez kjmp_go (jak longjmp) i sprzatamy. */

#include "process.h"
#include "paging.h"
#include "thread.h"
#include "idt.h"
#include "vga.h"
#include "fs.h"
#include "heap.h"
#include "timer.h"
#include "keyboard.h"
#include "string.h"
#include "multiboot.h"

#define STACK_PAGES  16
#define STACK_BOTTOM (USER_END - STACK_PAGES * PAGE_SIZE)
#define PROGRAMS_DIR "/programy"

#define MAX_CANVAS_W 1600
#define MAX_CANVAS_H 1000

/* Numery uslug systemowych - te same sa w programs/lib/naszos.h */
enum {
    SYS_EXIT, SYS_WRITE, SYS_GETKEY, SYS_POLLKEY, SYS_SLEEP, SYS_TIME,
    SYS_CANVAS_OPEN, SYS_CANVAS_FILL, SYS_CANVAS_TEXT, SYS_CANVAS_SHOW,
    SYS_RANDOM, SYS_MOUSE, SYS_SETCOLOR, SYS_CLEAR,
};

struct process {
    uint64_t dir;
    uint64_t jmp[8];            /* miejsce powrotu do process_run (kjmp_set) */
    int exit_code;
    const char *error;
    uint64_t fault_addr;
};

extern void enter_user(uint64_t entry, uint64_t stack);
extern int kjmp_set(uint64_t *buf);
extern void kjmp_go(uint64_t *buf, int value) __attribute__((noreturn));

/* ---------- ELF ---------- */

/* Naglowki ELF64 - pola adresow i rozmiarow maja po 64 bity */
struct elf_header {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed));

struct elf_phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} __attribute__((packed));

#define PT_LOAD 1

static int load_elf(struct process *p, const uint8_t *data, uint64_t size, uint64_t *entry)
{
    const struct elf_header *h = (const void *)data;
    if (size < sizeof(*h) || memcmp(h->ident, "\x7F" "ELF", 4) != 0 ||
        h->ident[4] != 2 || h->ident[5] != 1 ||          /* 64 bity, little endian */
        h->type != 2 || h->machine != 62 ||              /* plik wykonywalny, x86-64 */
        h->phentsize != sizeof(struct elf_phdr) ||
        h->phoff > size || h->phnum > (size - h->phoff) / sizeof(struct elf_phdr))
        return PROCESS_EFORMAT;

    const struct elf_phdr *ph = (const void *)(data + h->phoff);
    for (int i = 0; i < h->phnum; i++) {
        if (ph[i].type != PT_LOAD || ph[i].memsz == 0)
            continue;
        uint64_t va = ph[i].vaddr, mem = ph[i].memsz, file = ph[i].filesz;
        if (file > mem || ph[i].offset > size || file > size - ph[i].offset ||
            va < USER_BASE || va >= STACK_BOTTOM || mem > STACK_BOTTOM - va)
            return PROCESS_EFORMAT;
        for (uint64_t page = va & ~(uint64_t)(PAGE_SIZE - 1); page < va + mem; page += PAGE_SIZE)
            if (!paging_map_user(p->dir, page))
                return PROCESS_ENOMEM;
        /* Katalog stron programu jest juz zaladowany - kopiujemy wprost pod jego adresy.
         * Reszta (mem - file, zmienne "bss") zostaje wyzerowana przez paging_map_user. */
        memcpy((void *)va, data + ph[i].offset, file);
    }
    if (h->entry < USER_BASE || h->entry >= STACK_BOTTOM)
        return PROCESS_EFORMAT;
    *entry = h->entry;
    return 0;
}

/* ---------- uruchamianie ---------- */

static void free_canvas(struct console *c)
{
    if (!c->canvas)
        return;
    kfree(c->canvas);
    c->canvas = 0;
    c->canvas_w = c->canvas_h = 0;
    c->canvas_resize = 1;                /* okno wraca do rozmiaru terminala */
    c->dirty = 1;
}

int process_run(const char *path, const char **error)
{
    static const char *const messages[] = {
        "nie ma takiego programu", "to nie jest program NaszOS (zly format pliku)",
        "brak pamieci na uruchomienie programu",
    };
    *error = 0;

    struct fs_node *cwd = fs_node_valid(con_cur->cwd) ? con_cur->cwd : fs_root();
    struct fs_node *f;
    if (fs_lookup(cwd, path, &f) != FS_OK || f->is_dir) {
        *error = messages[0];
        return PROCESS_ENOFILE;
    }

    struct process *p = kzalloc(sizeof(*p));
    if (!p || !(p->dir = paging_new_dir())) {
        kfree(p);
        *error = messages[2];
        return PROCESS_ENOMEM;
    }
    thread_set_address_space(p->dir, p);

    uint64_t entry = 0;
    int err = load_elf(p, (const uint8_t *)f->data, f->size, &entry);
    for (uint64_t page = STACK_BOTTOM; !err && page < USER_END; page += PAGE_SIZE)
        if (!paging_map_user(p->dir, page))
            err = PROCESS_ENOMEM;

    if (!err && kjmp_set(p->jmp) == 0) {
        /* Przerwania z programu dostana stos ponizej tej funkcji (nie nadpisza jej ramki) */
        uint64_t rsp;
        __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
        thread_set_user_kernel_stack((rsp - 64) & ~15ull);  /* wyrownany do 16 */
        enter_user(entry, USER_END - 8);    /* rsp = 8 mod 16, jak po "call" */   /* tu program startuje; wracamy przez kjmp_go */
    }

    /* Koniec programu (albo blad ladowania) - sprzatamy */
    __asm__ volatile ("sti");
    p = thread_process();
    thread_set_user_kernel_stack(0);
    thread_set_address_space(0, 0);
    paging_free_dir(p->dir);
    free_canvas(con_cur);
    int code = err ? err : p->exit_code;
    if (err)
        *error = messages[err == PROCESS_EFORMAT ? 1 : 2];
    else
        *error = p->error;
    kfree(p);
    return code;
}

/* Konczy biezacy program - wraca do process_run (nie wraca tutaj) */
static void __attribute__((noreturn)) finish(int code, const char *error)
{
    struct process *p = thread_process();
    p->exit_code = code;
    p->error = error;
    kjmp_go(p->jmp, 1);
}

void process_fault(struct regs *r, const char *what)
{
    struct process *p = thread_process();
    if (!p)
        return;                          /* nie powinno sie zdarzyc - kernel_panic zajmie sie reszta */
    if (r->int_no == 14)
        __asm__ volatile ("mov %%cr2, %0" : "=r"(p->fault_addr));
    free_canvas(con_cur);
    vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
    kprintf("\n[Program wykonal niedozwolona operacje: %s", what);
    if (r->int_no == 14)
        kprintf(" pod adresem 0x%lx", p->fault_addr);
    kprintf(" (RIP=0x%lx)]\n", r->rip);
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    finish(PROCESS_CRASHED, what);
}

/* Z przerwania zegara, gdy przerwany zostal program */
void process_tick(void)
{
    if (con_cur->closed)                 /* okno programu zamkniete - konczymy go */
        finish(PROCESS_KILLED, 0);
    thread_preempt();
}

/* ---------- uslugi systemowe ---------- */

static int user_ok(uint64_t ptr, uint64_t len)
{
    struct process *p = thread_process();
    return p && paging_user_range_ok(p->dir, ptr, len);
}

/* Klawisze dla programu: pomijamy wewnetrzne (klik terminala, odswiezenie) */
static int program_poll_key(void)
{
    for (;;) {
        if (con_cur->closed)
            finish(PROCESS_KILLED, 0);
        int k = keyboard_poll();
        if (k != KEY_MOUSE_LEFT && k != KEY_REFRESH)
            return k;
    }
}

static void canvas_fill(struct console *c, int x, int y, int w, int h, uint32_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > c->canvas_w) w = c->canvas_w - x;
    if (y + h > c->canvas_h) h = c->canvas_h - y;
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
            c->canvas[row * c->canvas_w + col] = color;
}

static void canvas_text(struct console *c, int x, int y, uint64_t str, uint32_t color)
{
    const uint8_t *font = vga_font();
    for (int i = 0; i < 256 && user_ok(str + i, 1); i++, x += 8) {
        uint8_t ch = *(const uint8_t *)(str + i);
        if (!ch)
            break;
        for (int row = 0; row < 16; row++)
            for (int col = 0; col < 8; col++)
                if (font[ch * 16 + row] & (0x80 >> col)) {
                    int px = x + col, py = y + row;
                    if (px >= 0 && py >= 0 && px < c->canvas_w && py < c->canvas_h)
                        c->canvas[py * c->canvas_w + px] = color;
                }
    }
}

static uint32_t random_state;

static void syscall_handler(struct regs *r)
{
    __asm__ volatile ("sti");            /* uslugi moga czekac - przerwania musza dzialac */
    struct console *c = con_cur;
    if (!thread_process()) {
        r->rax = (uint64_t)-1;
        return;
    }
    if (c->closed)
        finish(PROCESS_KILLED, 0);

    /* Argumenty: wskazniki maja 64 bity, liczby uzywamy jako 32-bitowe */
    uint64_t a = r->rdi, b = r->rsi, d = r->rdx, e = r->r10, f = r->r8;
    int64_t result = 0;

    switch (r->rax) {
    case SYS_EXIT:
        finish((int32_t)a, 0);
    case SYS_WRITE:
        if (b > 65536 || !user_ok(a, b)) {
            result = -1;
            break;
        }
        for (uint64_t i = 0; i < b; i++)
            vga_putc(((const char *)a)[i]);
        result = (int64_t)b;
        break;
    case SYS_GETKEY: {
        int k;
        while ((k = program_poll_key()) < 0)
            cpu_idle();
        result = k;
        break;
    }
    case SYS_POLLKEY:
        result = program_poll_key();
        break;
    case SYS_SLEEP: {
        uint32_t end = timer_ticks() + (uint32_t)(((a > 60000 ? 60000 : a) * TIMER_HZ + 999) / 1000);
        while ((int32_t)(end - timer_ticks()) > 0) {
            if (c->closed)
                finish(PROCESS_KILLED, 0);
            cpu_idle();
        }
        break;
    }
    case SYS_TIME:
        result = (int32_t)timer_uptime_ms();
        break;
    case SYS_CANVAS_OPEN:
        if (!c->cells || !vga_is_graphics() || a < 16 || b < 16 || a > MAX_CANVAS_W || b > MAX_CANVAS_H) {
            result = -1;                 /* plotno jest tylko w oknie na pulpicie */
            break;
        }
        free_canvas(c);
        c->canvas = kzalloc(a * b * 4);
        if (!c->canvas) {
            result = -1;
            break;
        }
        c->canvas_w = (int)a;
        c->canvas_h = (int)b;
        c->canvas_resize = 1;
        c->dirty = 1;
        break;
    case SYS_CANVAS_FILL:
        if (c->canvas)
            canvas_fill(c, (int32_t)a, (int32_t)b, (int32_t)d, (int32_t)e, (uint32_t)f);
        break;
    case SYS_CANVAS_TEXT:
        if (c->canvas)
            canvas_text(c, (int32_t)a, (int32_t)b, d, (uint32_t)e);
        break;
    case SYS_CANVAS_SHOW:
        c->dirty = 1;
        cpu_idle();                      /* pulpit ma szanse narysowac klatke */
        break;
    case SYS_RANDOM:
        if (!random_state)
            random_state = timer_ticks() * 2654435761u + 1;
        random_state ^= random_state << 13;
        random_state ^= random_state >> 17;
        random_state ^= random_state << 5;
        result = (int32_t)(random_state & 0x7FFFFFFF);
        break;
    case SYS_MOUSE:
        if (!user_ok(a, 12)) {
            result = -1;
            break;
        }
        ((int32_t *)a)[0] = c->mouse_x;
        ((int32_t *)a)[1] = c->mouse_y;
        ((int32_t *)a)[2] = c->mouse_buttons;
        break;
    case SYS_SETCOLOR:
        vga_set_color((enum vga_color)(a & 15), (enum vga_color)(b & 15));
        break;
    case SYS_CLEAR:
        vga_clear();
        break;
    default:
        result = -1;
    }
    r->rax = (uint64_t)result;
}

void process_init(void)
{
    interrupt_register(0x80, syscall_handler);
}

/* ---------- uruchamianie z powloki ---------- */

struct run_request {
    const char *path;
    const char *error;
    int code;
    volatile int done;
};

static void run_thread(void *arg)
{
    struct run_request *req = arg;
    req->code = process_run(req->path, &req->error);
    req->done = 1;
}

int process_run_and_wait(const char *path)
{
    struct run_request req = { .path = path };
    if (thread_create(run_thread, &req, con_cur) < 0) {
        vga_write("Nie mozna uruchomic programu: za duzo dzialajacych zadan.\n");
        return PROCESS_ENOMEM;
    }
    while (!req.done)                    /* program ma ta sama konsole - czekamy */
        cpu_idle();
    if (req.error && req.code != PROCESS_CRASHED) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        kprintf("%s: %s\n", path, req.error);
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }
    return req.code;
}

/* ---------- programy od bootloadera ---------- */

int process_import_modules(struct multiboot_info *mbi)
{
    if (!(mbi->flags & MULTIBOOT_INFO_MODS) || mbi->mods_count == 0)
        return 0;
    struct fs_node *dir;
    if (fs_lookup(fs_root(), PROGRAMS_DIR, &dir) != FS_OK &&
        fs_create(fs_root(), PROGRAMS_DIR, 1, &dir) != FS_OK)
        return 0;

    const struct multiboot_module *mods = (const void *)(uintptr_t)mbi->mods_addr;
    int imported = 0;
    for (uint32_t i = 0; i < mbi->mods_count; i++) {
        /* Nazwa = ostatni czlon sciezki podanej bootloaderowi (build\programs\x.exe) */
        const char *name = mods[i].string ? (const char *)(uintptr_t)mods[i].string : "program.exe";
        for (const char *s = name; *s; s++)
            if (*s == '/' || *s == '\\')
                name = s + 1;
        char path[FS_PATH_MAX];
        ksnprintf(path, sizeof(path), "%s/%s", PROGRAMS_DIR, name);

        struct fs_node *f;
        int err = fs_lookup(fs_root(), path, &f);
        if (err == FS_ENOENT)
            err = fs_create(fs_root(), path, 0, &f);
        if (err == FS_OK && !f->is_dir &&
            fs_write(f, (const char *)(uintptr_t)mods[i].mod_start, mods[i].mod_end - mods[i].mod_start, 0) == FS_OK)
            imported++;
    }
    return imported;
}
