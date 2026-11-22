/* Grafika: odbijajace sie pilki. Klikniecie mysza dodaje pilke w tym miejscu,
 * spacja dodaje losowa, C czysci, Q konczy. Liczby calkowite w "stalym
 * przecinku" (x 256), bo programy nie uzywaja koprocesora. */
#include "naszos.h"

#define W 640
#define H 420
#define MAX_BALLS 40
#define FIX 256

struct ball { int x, y, vx, vy, r; unsigned color; };

static struct ball balls[MAX_BALLS];
static int count;

static const unsigned colors[] = {
    0xEF4444, 0xF97316, 0xFACC15, 0x22C55E, 0x06B6D4, 0x3B82F6, 0x8B5CF6, 0xEC4899,
};

static int isqrt(int v)
{
    int r = 0;
    while ((r + 1) * (r + 1) <= v)
        r++;
    return r;
}

static void add_ball(int x, int y)
{
    if (count == MAX_BALLS)
        return;
    struct ball *b = &balls[count++];
    b->r = 10 + random(16);
    b->x = x * FIX;
    b->y = y * FIX;
    b->vx = (random(9) - 4) * FIX / 2 + (random(2) ? 1 : -1) * FIX;
    b->vy = -(random(6) + 2) * FIX;
    b->color = colors[random(8)];
}

static void draw_ball(const struct ball *b)
{
    int cx = b->x / FIX, cy = b->y / FIX, r = b->r;
    for (int dy = -r; dy <= r; dy++) {
        int w = isqrt(r * r - dy * dy);
        canvas_fill(cx - w, cy + dy, 2 * w + 1, 1, b->color);
    }
    canvas_fill(cx - r / 3, cy - r / 2, r / 3, r / 4, 0xFFFFFF);   /* odblask */
}

static void step(struct ball *b)
{
    b->vy += FIX / 5;                                              /* grawitacja */
    b->x += b->vx;
    b->y += b->vy;
    if (b->x < b->r * FIX)           { b->x = b->r * FIX;       b->vx = -b->vx; }
    if (b->x > (W - b->r) * FIX)     { b->x = (W - b->r) * FIX; b->vx = -b->vx; }
    if (b->y > (H - b->r) * FIX) {                                 /* podloga: odbicie z utrata */
        b->y = (H - b->r) * FIX;
        b->vy = -b->vy * 9 / 10;
    }
}

int main(void)
{
    if (canvas_open(W, H) != 0) {
        print("Ten program rysuje grafike - uruchom go na pulpicie (tryb graficzny).\n");
        return 1;
    }
    for (int i = 0; i < 5; i++)
        add_ball(100 + random(W - 200), 60 + random(120));

    int was_pressed = 0;
    for (;;) {
        int k = poll_key();
        if (k == 'q' || k == 'Q' || k == 27)
            break;
        if (k == ' ')
            add_ball(100 + random(W - 200), 60);
        if (k == 'c' || k == 'C')
            count = 0;

        struct mouse m;
        get_mouse(&m);
        int pressed = m.buttons & 1;
        if (pressed && !was_pressed && m.x >= 0 && m.y >= 0 && m.x < W && m.y < H)
            add_ball(m.x, m.y);
        was_pressed = pressed;

        for (int i = 0; i < count; i++)
            step(&balls[i]);

        canvas_fill(0, 0, W, H, 0x0F172A);                         /* tlo */
        canvas_fill(0, H - 4, W, 4, 0x334155);                     /* podloga */
        for (int i = 0; i < count; i++)
            draw_ball(&balls[i]);
        canvas_text(10, 8, "Klik - nowa pilka   Spacja - losowa   C - czysc   Q - koniec", 0x94A3B8);
        canvas_show();
        sleep_ms(25);
    }
    return count;
}
