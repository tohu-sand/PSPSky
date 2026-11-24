#!/usr/bin/env python3
"""Generate the UI alpha-mask atlas; requires Pillow.

Masks are supersampled and box-filtered. Repeated edge texels prevent
bilinear filtering from sampling adjacent icons.

Every shape is drawn here from primitives; the atlas contains no third-party
artwork. ICON0.PNG and PIC1.PNG use the same mark_mask() geometry.

Usage: python3 tools/gen_ui_atlas.py > src/psp/ui_atlas.h
"""

import math
import sys

from PIL import Image, ImageChops, ImageDraw

SS = 16  # supersampling factor
ATLAS_W = 128  # Height is rounded up to a power of two.

# Cloud speech bubble in a 64-unit-wide box.
MARK_W, MARK_H = 64, 51


def canvas(w, h):
    return Image.new("L", (w * SS, h * SS), 0)


def fill_circle(img, cx, cy, r):
    ImageDraw.Draw(img).ellipse(
        [(cx - r) * SS, (cy - r) * SS, (cx + r) * SS - 1, (cy + r) * SS - 1],
        fill=255)


def fill_ellipse(img, cx, cy, rx, ry):
    ImageDraw.Draw(img).ellipse(
        [(cx - rx) * SS, (cy - ry) * SS, (cx + rx) * SS - 1, (cy + ry) * SS - 1],
        fill=255)


def fill_rrect(img, x0, y0, x1, y1, r):
    ImageDraw.Draw(img).rounded_rectangle(
        [x0 * SS, y0 * SS, x1 * SS - 1, y1 * SS - 1], radius=r * SS, fill=255)


def fill_poly(img, pts):
    ImageDraw.Draw(img).polygon([(x * SS, y * SS) for x, y in pts], fill=255)


def thick_line(img, x0, y0, x1, y1, w):
    nx, ny = y1 - y0, x0 - x1
    k = w / 2 / math.hypot(nx, ny)
    nx, ny = nx * k, ny * k
    fill_poly(img, [(x0 + nx, y0 + ny), (x1 + nx, y1 + ny),
                    (x1 - nx, y1 - ny), (x0 - nx, y0 - ny)])


def stroke(img, pts, w):
    """Polyline with round caps and joins."""
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        thick_line(img, x0, y0, x1, y1, w)
    for x, y in pts:
        fill_circle(img, x, y, w / 2)


def arc(cx, cy, r, a0, a1, n=12):
    """Points from angle a0 to a1 in degrees; y points down."""
    return [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * k / n)),
             cy + r * math.sin(math.radians(a0 + (a1 - a0) * k / n)))
            for k in range(n + 1)]


def finish(img):
    return img.reduce(SS)


def circle(d):
    img = canvas(d, d)
    fill_circle(img, d / 2, d / 2, d / 2)
    return finish(img)


def inverse(img):
    return ImageChops.invert(img)


def default_avatar(d):
    s = d / 24.0
    img = canvas(d, d)
    fill_circle(img, 12 * s, 9 * s, 4 * s)
    fill_ellipse(img, 12 * s, 22.5 * s, 8 * s, 7 * s)
    return ImageChops.multiply(finish(img), circle(d))


def mark_mask(w):
    """Returns the mark w texels wide, still supersampled by SS."""
    s = w / MARK_W
    img = Image.new("L", (int(round(w * SS)),
                          int(math.ceil(MARK_H * s)) * SS), 0)
    fill_rrect(img, 3 * s, 18 * s, 61 * s, 42 * s, 12 * s)
    fill_circle(img, 20 * s, 22 * s, 11 * s)
    fill_circle(img, 36 * s, 15 * s, 15 * s)
    fill_circle(img, 50 * s, 25 * s, 11 * s)
    fill_poly(img, [(11 * s, 35 * s), (29 * s, 40 * s), (5 * s, 51 * s)])
    return img


def mark(w):
    return finish(mark_mask(w))


# Repost and reply are drawn on a 24-unit grid at 12 texels. Stroke centres
# sit on odd units so that the 1-texel strokes land on texel centres; solid
# arrowheads stay legible at that size where open chevrons do not.
def repost(n):
    s = n / 24.0
    img = canvas(n, n)
    top = [(5, 13)] + arc(8, 10, 3, 180, 270) + [(16, 7)]
    bot = [(19, 11)] + arc(16, 14, 3, 0, 90) + [(8, 17)]
    for pts in (top, bot):
        stroke(img, [(x * s, y * s) for x, y in pts], 2 * s)
    fill_poly(img, [(15 * s, 3 * s), (21 * s, 7 * s), (15 * s, 11 * s)])
    fill_poly(img, [(9 * s, 13 * s), (3 * s, 17 * s), (9 * s, 21 * s)])
    return finish(img)


def reply(n):
    s = n / 24.0
    img = canvas(n, n)
    line = [(5, 4)] + arc(9, 11, 4, 180, 90) + [(16, 15)]
    stroke(img, [(x * s, y * s) for x, y in line], 2 * s)
    fill_poly(img, [(15 * s, 11 * s), (21 * s, 15 * s), (15 * s, 19 * s)])
    return finish(img)


def glyph_triangle(n):
    sw = 1.6
    top, bot = 0.8, n - 1.2
    side = (bot - top) * 2 / math.sqrt(3)
    c = n / 2
    tri = [(c, top), (c + side / 2, bot), (c - side / 2, bot)]
    # Insetting every edge by sw scales the triangle about its incentre.
    ix = sum(p[0] for p in tri) / 3
    iy = sum(p[1] for p in tri) / 3
    r = side / (2 * math.sqrt(3))
    k = (r - sw) / r
    img = canvas(n, n)
    fill_poly(img, tri)
    hole = canvas(n, n)
    fill_poly(hole, [(ix + (x - ix) * k, iy + (y - iy) * k) for x, y in tri])
    return finish(ImageChops.subtract(img, hole))


def glyph_circle(n):
    img = canvas(n, n)
    fill_circle(img, n / 2, n / 2, n / 2 - 0.6)
    hole = canvas(n, n)
    fill_circle(hole, n / 2, n / 2, n / 2 - 2.2)
    return finish(ImageChops.subtract(img, hole))


def glyph_cross(n):
    img = canvas(n, n)
    a, b = 1.6, n - 1.6
    thick_line(img, a, a, b, b, 1.7)
    thick_line(img, b, a, a, b, 1.7)
    return finish(img)


def glyph_square(n):
    img = canvas(n, n)
    a, b = 1.3, n - 1.3
    fill_poly(img, [(a, a), (b, a), (b, b), (a, b)])
    hole = canvas(n, n)
    a2, b2 = a + 1.6, b - 1.6
    fill_poly(hole, [(a2, a2), (b2, a2), (b2, b2), (a2, b2)])
    return finish(ImageChops.subtract(img, hole))


def folder(n):
    img = canvas(n, n)
    fill_poly(img, [(0.5, 2.0), (4.5, 2.0), (5.8, 3.4), (11.5, 3.4),
                    (11.5, 10.5), (0.5, 10.5)])
    return finish(img)


def picture(n):
    img = canvas(n, n)
    fill_poly(img, [(0.5, 1.5), (11.5, 1.5), (11.5, 10.5), (0.5, 10.5)])
    hole = canvas(n, n)
    fill_poly(hole, [(1.8, 2.8), (10.2, 2.8), (10.2, 9.2), (1.8, 9.2)])
    out = ImageChops.subtract(img, hole)
    fill_poly(out, [(1.8, 9.2), (4.6, 5.6), (6.6, 7.8), (7.8, 6.6),
                    (10.2, 9.2)])
    fill_circle(out, 8.2, 4.8, 1.0)
    return finish(out)


# Entry order defines the generated enum values.
ICONS = [
    ("CIRCLE32",  circle(32)),
    ("ICIRCLE32", inverse(circle(32))),
    ("AVATAR32",  default_avatar(32)),
    ("LOGO48",    mark(48)),
    ("LOGO20",    mark(20)),
    ("CIRCLE16",  circle(16)),
    ("ICIRCLE16", inverse(circle(16))),
    ("REPOST",    repost(12)),
    ("REPLY",     reply(12)),
    ("FOLDER",    folder(12)),
    ("PICTURE",   picture(12)),
    ("TRIANGLE",  glyph_triangle(10)),
    ("CIRCLE",    glyph_circle(10)),
    ("CROSS",     glyph_cross(10)),
    ("SQUARE",    glyph_square(10)),
]


def pack(icons):
    """Shelf packing, tallest first, with a one-texel border per icon."""
    order = sorted(range(len(icons)), key=lambda i: -icons[i][1].size[1])
    pos = [None] * len(icons)
    x = y = shelf = 0
    for i in order:
        w, h = icons[i][1].size
        if x + w + 2 > ATLAS_W:
            x, y, shelf = 0, y + shelf, 0
        pos[i] = (x + 1, y + 1)
        x += w + 2
        shelf = max(shelf, h + 2)
    rows = y + shelf
    height = 1
    while height < rows:
        height *= 2
    return pos, rows, height


def main():
    pos, rows, height = pack(ICONS)
    atlas = Image.new("L", (ATLAS_W, rows), 0)
    for (name, img), (x, y) in zip(ICONS, pos):
        w, h = img.size
        # Repeat edge texels, including corners, for bilinear filtering.
        padded = Image.new("L", (w + 2, h + 2), 0)
        padded.paste(img, (1, 1))
        padded.paste(img.crop((0, 0, w, 1)), (1, 0))
        padded.paste(img.crop((0, h - 1, w, h)), (1, h + 1))
        padded.paste(padded.crop((1, 0, 2, h + 2)), (0, 0))
        padded.paste(padded.crop((w, 0, w + 1, h + 2)), (w + 1, 0))
        atlas.paste(padded, (x - 1, y - 1))

    px = atlas.load()
    nib = []
    for yy in range(rows):
        for xx in range(0, ATLAS_W, 2):
            lo = (px[xx, yy] * 15 + 127) // 255
            hi = (px[xx + 1, yy] * 15 + 127) // 255
            nib.append(lo | (hi << 4))

    out = sys.stdout
    out.write("/* Generated by tools/gen_ui_atlas.py; do not edit by hand. */\n")
    out.write("#ifndef PSPSKY_UI_ATLAS_H\n#define PSPSKY_UI_ATLAS_H\n\n")
    out.write("/* Only UI_ATLAS_ROWS rows of the power-of-two texture are stored. */\n")
    out.write("#define UI_ATLAS_W %d\n#define UI_ATLAS_H %d\n#define UI_ATLAS_ROWS %d\n\n"
              % (ATLAS_W, height, rows))
    out.write("enum {\n")
    for i, (name, _) in enumerate(ICONS):
        out.write("    UI_ICON_%s%s,\n" % (name, " = 0" if i == 0 else ""))
    out.write("    UI_ICON_COUNT\n};\n\n")
    out.write("/* Icon rectangles: x, y, w, h in texels. */\n")
    out.write("static const unsigned char k_ui_icon_rect[UI_ICON_COUNT][4] = {\n")
    for (name, img), (x, y) in zip(ICONS, pos):
        out.write("    { %3d, %3d, %2d, %2d },  /* %s */\n"
                  % (x, y, img.size[0], img.size[1], name))
    out.write("};\n\n")
    out.write("/* 4-bit coverage; left texel in the low nibble. */\n")
    out.write("static const unsigned char k_ui_atlas_a4[UI_ATLAS_W * UI_ATLAS_ROWS / 2] = {\n")
    for i in range(0, len(nib), 16):
        out.write("    " + ",".join("0x%02x" % b for b in nib[i:i + 16]) + ",\n")
    out.write("};\n\n#endif /* PSPSKY_UI_ATLAS_H */\n")


if __name__ == "__main__":
    main()
