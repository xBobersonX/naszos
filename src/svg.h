/* Grafika SVG w stronie (<svg> ... </svg>) - js_svg.c.
 * Rysujemy ja rasteryzatorem z <canvas> (js_canvas.c, API c2d_* ponizej). */
#ifndef SVG_H
#define SVG_H

struct dom_doc;
struct dom_node;
struct dom_canvas;
struct c2d;

/* Naturalny rozmiar <svg> (z viewBox, inaczej 300 x 150) */
void svg_size(const struct dom_node *n, int *w, int *h);
/* Rysuje <svg> na jego plotnie (n->canvas) w rozmiarze w x h - tylko gdy rozmiar sie zmienil */
void svg_paint(struct dom_doc *d, struct dom_node *n, int w, int h);

/* Rasteryzator z js_canvas.c dla kodu w C (bez silnika JS) */
struct c2d *c2d_new(int w, int h);
void c2d_free(struct c2d *c);
void c2d_begin(struct c2d *c);
void c2d_matrix(struct c2d *c, const double *m);
void c2d_move(struct c2d *c, double x, double y);
void c2d_line(struct c2d *c, double x, double y);
void c2d_close(struct c2d *c);
void c2d_rect(struct c2d *c, double x, double y, double w, double h);
void c2d_ellipse(struct c2d *c, double cx, double cy, double rx, double ry);
int c2d_color(const char *s, double *rgba);
void c2d_fill(struct c2d *c, struct dom_canvas *cv, const double *rgba, double opacity, int evenodd);
void c2d_stroke(struct c2d *c, struct dom_canvas *cv, const double *rgba, double opacity, double width,
                int cap, int join);

#endif
