# Generator czcionek dla NaszOS: zamienia fonty DejaVu (TTF) na gotowe
# bitmapy z wygladzaniem (kazdy piksel = przezroczystosc 0-255) i zapisuje
# je jako tablice C w src/font_data.c.
#
# Uzycie:  python tools/make_font.py <katalog z plikami DejaVuSans*.ttf>
# Wymaga: Pillow (pip install pillow). Fonty DejaVu sa np. w pakiecie
# matplotlib (mpl-data/fonts/ttf). Licencja fontow: fonts/LICENSE_DEJAVU.txt

import os
import sys
from PIL import Image, ImageDraw, ImageFont

FACES = [                        # nazwa w C, plik, rozmiar w pikselach
    ('FONT_SMALL',   'DejaVuSans.ttf',          12),
    ('FONT_REGULAR', 'DejaVuSans.ttf',          15),
    ('FONT_BOLD',    'DejaVuSans-Bold.ttf',     15),
    ('FONT_ITALIC',  'DejaVuSans-Oblique.ttf',  15),
    ('FONT_H2',      'DejaVuSans-Bold.ttf',     20),
    ('FONT_H1',      'DejaVuSans.ttf',          28),
]

# ASCII + polskie litery + najczestsze znaki z innych jezykow i typografii
EXTRA = ('ĄąĆćĘęŁłŃńÓóŚśŹźŻż'
         'ÀÁÂÄÇÈÉÊËÍÎÏÑÒÔÖÙÚÛÜßàáâäçèéêëíîïñòôöùúûüýÿčďěňřšťůžČŠŽŘĚ'
         '‘’“”„–—…•·«»©®™€£°×÷±½¼¾²³§¡¿→←↑↓≈≠≤≥')
CHARS = sorted(set(chr(c) for c in range(32, 127)) | set(EXTRA) | {'�'})


def render(path, size):
    font = ImageFont.truetype(path, size)
    ascent, descent = font.getmetrics()
    glyphs = []
    for ch in CHARS:
        adv = int(round(font.getlength(ch)))
        pad = size
        img = Image.new('L', (adv + 2 * pad, ascent + descent + pad), 0)
        ImageDraw.Draw(img).text((pad, 0), ch, font=font, fill=255)   # gora obrazka = linia ascent
        box = img.getbbox()
        if box is None:
            glyphs.append((ord(ch), 0, 0, 0, 0, adv, b''))
            continue
        x0, y0, x1, y1 = box
        data = img.crop(box).tobytes()
        glyphs.append((ord(ch), x0 - pad, y0, x1 - x0, y1 - y0, adv, data))
    return ascent, descent, glyphs


def main():
    font_dir = sys.argv[1]
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = []
    out.append('/* Wygenerowane przez tools/make_font.py - nie edytowac recznie.')
    out.append(' * Czcionki DejaVu: (c) Bitstream (Bitstream Vera), zmiany DejaVu w domenie')
    out.append(' * publicznej, znaki z Arev (c) Tavmjong Bah - licencja w fonts/LICENSE_DEJAVU.txt */')
    out.append('')
    out.append('#include "font.h"')
    out.append('')
    faces_c = []
    for name, file, size in FACES:
        ascent, descent, glyphs = render(os.path.join(font_dir, file), size)
        ident = name.lower()
        alpha = bytearray()
        rows = []
        for cp, xoff, yoff, w, h, adv, data in glyphs:
            rows.append('    { 0x%04X, %d, %d, %d, %d, %d, %d },' % (cp, xoff, yoff, w, h, adv, len(alpha)))
            alpha += data
        out.append('static const uint8_t %s_alpha[%d] = {' % (ident, max(len(alpha), 1)))
        for i in range(0, len(alpha), 24):
            out.append('    ' + ', '.join('%d' % b for b in alpha[i:i + 24]) + ',')
        out.append('};')
        out.append('')
        out.append('static const struct font_glyph %s_glyphs[%d] = {' % (ident, len(glyphs)))
        out.extend(rows)
        out.append('};')
        out.append('')
        faces_c.append('    [%s] = { %d, %d, %d, %s_glyphs, %s_alpha },' % (
            name, ascent + descent, ascent, len(glyphs), ident, ident))
    out.append('const struct font_face font_faces[FONT_COUNT] = {')
    out.extend(faces_c)
    out.append('};')
    path = os.path.join(root, 'src', 'font_data.c')
    with open(path, 'w', encoding='ascii', newline='\n') as f:
        f.write('\n'.join(out) + '\n')
    print('Zapisano %s (%d znakow w %d czcionkach)' % (path, len(CHARS), len(FACES)))


if __name__ == '__main__':
    main()
