#ifndef GFX_H
#define GFX_H

#include <stdint.h>

/* Grafika pulpitu. Rysujemy do bufora w pamieci (niewidocznego), a gotowy
 * fragment kopiujemy na ekran przez gfx_present() - dzieki temu nic nie
 * miga. Kolory w formacie 0xRRGGBB. Wszystkie funkcje rysujace respektuja
 * prostokat obcinania (gfx_set_clip). */

struct rect { int x, y, w, h; };

int  gfx_begin(void);           /* 1 = mamy tryb graficzny i pamiec na bufory */
void gfx_end(void);             /* oddaje ekran konsoli */
int  gfx_width(void);
int  gfx_height(void);

/* Tlo pulpitu rysujemy raz do osobnego bufora, potem tylko je kopiujemy */
void gfx_target_background(int on);
void gfx_copy_background(void); /* tlo -> bufor roboczy (w obszarze obcinania) */

void gfx_set_clip(const struct rect *r);   /* NULL = caly ekran */
void gfx_use_buffer(uint32_t *px, int w, int h);   /* rysowanie do bufora (testy na PC) */
struct rect gfx_get_clip(void);

void gfx_fill(int x, int y, int w, int h, uint32_t color);
void gfx_frame(int x, int y, int w, int h, uint32_t color);   /* ramka 1 px */
void gfx_gradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void gfx_darken(int x, int y, int w, int h);                  /* cien */
void gfx_lighten(int x, int y, int w, int h);                 /* podswietlenie */
void gfx_text(int x, int y, const char *s, uint32_t color);   /* czcionka 8x16, przezroczyste tlo */
void gfx_text_scaled(int x, int y, const char *s, uint32_t color, int scale);
int  gfx_text_width(const char *s);
void gfx_char(int x, int y, unsigned char ch, uint32_t fg, uint32_t bg);   /* 8x16 z tlem */
/* Kolor przez "maske przezroczystosci" (0 = tlo, 255 = pelny kolor) -
 * tak rysujemy wygladzane litery (src/font.c) */
void gfx_alpha_mask(int x, int y, int w, int h, const uint8_t *alpha, uint32_t color);
uint32_t gfx_mix(uint32_t a, uint32_t b, int num, int den);               /* a..b w proporcji num/den */

/* Kopiuje prostokat pikseli (w x h, wiersz po wierszu) pod (x, y) */
void gfx_blit(int x, int y, int w, int h, const uint32_t *pixels);
/* To samo, ale obrazek sw x sh rozciagniety/zmniejszony do w x h */
void gfx_blit_scaled(int x, int y, int w, int h, const uint32_t *pixels, int sw, int sh);
/* Jak gfx_blit_scaled, ale piksele 0xAARRGGBB z przezroczystoscia (plotno <canvas>) */
void gfx_blit_alpha(int x, int y, int w, int h, const uint32_t *pixels, int sw, int sh);

/* Zaokraglone rogi (CSS border-radius): r[4] = lewy-gorny, prawy-gorny, prawy-dolny, lewy-dolny */
void gfx_fill_round(int x, int y, int w, int h, const int16_t r[4], uint32_t color);
void gfx_ring_round(int x, int y, int w, int h, const int16_t r[4], const int16_t b[4], uint32_t color);
void gfx_blit_round(int x, int y, int w, int h, const uint32_t *pixels, int sw, int sh, const int16_t r[4]);
/* Cien (CSS box-shadow): ksztalt rozmyty na blur px, tylko poza pudelkiem (bx, by, bw, bh, br) */
void gfx_shadow(int x, int y, int w, int h, const int16_t r[4], int blur, uint32_t color, int alpha,
                int bx, int by, int bw, int bh, const int16_t br[4]);

void gfx_present(const struct rect *r);    /* bufor roboczy -> ekran */

/* Kursor myszy: krzyzyk jak w Minecrafcie - odwraca kolory pod soba */
void gfx_cursor_move(int x, int y);

int  rect_contains(const struct rect *r, int x, int y);
struct rect rect_union(struct rect a, struct rect b);   /* w=0 oznacza pusty */

#endif
