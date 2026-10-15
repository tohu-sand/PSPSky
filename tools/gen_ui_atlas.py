#!/usr/bin/env python3
"""Generate the UI alpha-mask atlas; requires Pillow.

Masks are supersampled and box-filtered. Repeated edge texels prevent
bilinear filtering from sampling adjacent icons.

Logo, avatar, repost and reply paths: bluesky-social/social-app (MIT License,
Copyright 2023-2026 Bluesky Social PBC). See THIRD_PARTY_LICENSES.md.

Usage: python3 tools/gen_ui_atlas.py > src/psp/ui_atlas.h
"""

import math
import re
import sys

from PIL import Image, ImageChops, ImageDraw

SS = 16  # supersampling factor
ATLAS_W = 128  # Height is rounded up to a power of two.

# src/view/icons/Logo.tsx, viewBox 0 0 64 57
LOGO_PATH = (
    "M13.873 3.805C21.21 9.332 29.103 20.537 32 26.55v15.882c0-.338-.13.044"
    "-.41.867-1.512 4.456-7.418 21.847-20.923 7.944-7.111-7.32-3.819-14.64 "
    "9.125-16.85-7.405 1.264-15.73-.825-18.014-9.015C1.12 23.022 0 8.51 0 "
    "6.55 0-3.268 8.579-.182 13.873 3.805ZM50.127 3.805C42.79 9.332 34.897 "
    "20.537 32 26.55v15.882c0-.338.13.044.41.867 1.512 4.456 7.418 21.847 "
    "20.923 7.944 7.111-7.32 3.819-14.64-9.125-16.85 7.405 1.264 15.73-.825 "
    "18.014-9.015C62.88 23.022 64 8.51 64 6.55c0-9.818-8.578-6.732-13.873"
    "-2.745Z")

# src/view/com/util/UserAvatar.tsx DefaultAvatar, viewBox 0 0 24 24.
# Head: circle at (12, 9.5), r = 3.5. This path defines the shoulders.
AVATAR_BODY_PATH = (
    "M 12.058 22.784 C 9.422 22.784 7.007 21.836 5.137 20.262 C 5.667 17.988 "
    "8.534 16.25 11.99 16.25 C 15.494 16.25 18.391 18.036 18.864 20.357 C "
    "17.01 21.874 14.64 22.784 12.058 22.784 Z")

# src/components/icons/Repost.tsx Repost_Stroke2_Corner3_Rounded,
# viewBox 0 0 24 24
REPOST_PATH = (
    "M16.793 2.293a1 1 0 0 1 1.414 0L20.5 4.586a2 2 0 0 1 0 2.828l-2.293 "
    "2.293a1 1 0 0 1-1.414-1.414L18.086 7H7a2 2 0 0 0-2 2v2a1 1 0 1 1-2 0V9a4 "
    "4 0 0 1 4-4h11.086l-1.293-1.293a1 1 0 0 1 0-1.414ZM20 12a1 1 0 0 1 1 "
    "1v2a4 4 0 0 1-4 4H5.914l1.293 1.293a1 1 0 1 1-1.414 1.414L3.5 19.414a2 2 "
    "0 0 1 0-2.828l2.293-2.293a1 1 0 0 1 1.414 1.414L5.914 17H17a2 2 0 0 0 "
    "2-2v-2a1 1 0 0 1 1-1Z")

# src/components/icons/Arrow.tsx ArrowCornerDownRight_Stroke2_Corner2_Rounded,
# viewBox 0 0 24 24
REPLY_PATH = (
    "M15.793 10.293a1 1 0 0 1 1.338-.068l.076.068 3.293 3.293a2 2 0 0 1 "
    ".138 2.677l-.138.151-3.293 3.293a1 1 0 1 1-1.414-1.414L18.086 16H8a5 5 "
    "0 0 1-5-5V5a1 1 0 0 1 2 0v6a3 3 0 0 0 3 3h10.086l-2.293-2.293-.068-.076"
    "a1 1 0 0 1 .068-1.338Z")

_NUM = re.compile(r"[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?")


class _Reader:
    def __init__(self, d):
        self.d = d
        self.i = 0

    def _skip(self):
        while self.i < len(self.d) and self.d[self.i] in " ,\t\r\n":
            self.i += 1

    def at_number(self):
        self._skip()
        return self.i < len(self.d) and (self.d[self.i] in "+-." or
                                         self.d[self.i].isdigit())

    def command(self):
        self._skip()
        if self.i >= len(self.d):
            return None
        c = self.d[self.i]
        self.i += 1
        return c

    def number(self):
        self._skip()
        m = _NUM.match(self.d, self.i)
        if not m:
            raise ValueError("number expected at %d" % self.i)
        self.i = m.end()
        return float(m.group(0))

    def flag(self):
        # Arc flags are single digits and may be written without separators.
        self._skip()
        c = self.d[self.i]
        self.i += 1
        return c == "1"


def _bezier(p0, p1, p2, p3, n=24):
    out = []
    for k in range(1, n + 1):
        t = k / n
        u = 1 - t
        out.append((u * u * u * p0[0] + 3 * u * u * t * p1[0] +
                    3 * u * t * t * p2[0] + t * t * t * p3[0],
                    u * u * u * p0[1] + 3 * u * u * t * p1[1] +
                    3 * u * t * t * p2[1] + t * t * t * p3[1]))
    return out


def _arc(p0, rx, ry, phi_deg, large, sweep, p1):
    """SVG arc, endpoint form converted per SVG 1.1 appendix F.6.5."""
    if rx == 0 or ry == 0 or p0 == p1:
        return [p1]
    rx, ry = abs(rx), abs(ry)
    phi = math.radians(phi_deg)
    cp, sp = math.cos(phi), math.sin(phi)
    dx, dy = (p0[0] - p1[0]) / 2, (p0[1] - p1[1]) / 2
    x1 = cp * dx + sp * dy
    y1 = -sp * dx + cp * dy
    lam = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry)
    if lam > 1:
        rx *= math.sqrt(lam)
        ry *= math.sqrt(lam)
    num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1
    den = rx * rx * y1 * y1 + ry * ry * x1 * x1
    co = math.sqrt(max(0.0, num / den)) if den else 0.0
    if large == sweep:
        co = -co
    cx1 = co * rx * y1 / ry
    cy1 = -co * ry * x1 / rx
    cx = cp * cx1 - sp * cy1 + (p0[0] + p1[0]) / 2
    cy = sp * cx1 + cp * cy1 + (p0[1] + p1[1]) / 2

    def ang(ux, uy, vx, vy):
        a = math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)
        return a

    t1 = ang(1, 0, (x1 - cx1) / rx, (y1 - cy1) / ry)
    dt = ang((x1 - cx1) / rx, (y1 - cy1) / ry,
             (-x1 - cx1) / rx, (-y1 - cy1) / ry)
    if not sweep and dt > 0:
        dt -= 2 * math.pi
    elif sweep and dt < 0:
        dt += 2 * math.pi
    n = max(4, int(abs(dt) / (math.pi / 32)) + 1)
    out = []
    for k in range(1, n + 1):
        t = t1 + dt * k / n
        x = rx * math.cos(t)
        y = ry * math.sin(t)
        out.append((cp * x - sp * y + cx, sp * x + cp * y + cy))
    return out


def flatten(d):
    """Returns the subpaths of SVG path data d as lists of points."""
    r = _Reader(d)
    subs = []
    cur = []
    pos = (0.0, 0.0)
    start = pos
    last_ctrl = None
    cmd = None
    while True:
        if r.at_number() and cmd is not None:
            c = cmd
            if c in "Mm":  # implicit lineto after moveto
                c = "L" if c == "M" else "l"
        else:
            c = r.command()
            if c is None:
                break
        rel = c.islower()
        C = c.upper()
        ox, oy = pos if rel else (0.0, 0.0)
        ctrl = None
        if C == "M":
            if cur:
                subs.append(cur)
            pos = (ox + r.number(), oy + r.number())
            start = pos
            cur = [pos]
        elif C == "L":
            pos = (ox + r.number(), oy + r.number())
            cur.append(pos)
        elif C == "H":
            pos = ((ox if rel else 0.0) + r.number(), pos[1])
            cur.append(pos)
        elif C == "V":
            pos = (pos[0], (oy if rel else 0.0) + r.number())
            cur.append(pos)
        elif C == "C":
            p1 = (ox + r.number(), oy + r.number())
            p2 = (ox + r.number(), oy + r.number())
            p3 = (ox + r.number(), oy + r.number())
            cur.extend(_bezier(pos, p1, p2, p3))
            ctrl, pos = p2, p3
        elif C == "S":
            p1 = pos if last_ctrl is None else (2 * pos[0] - last_ctrl[0],
                                                 2 * pos[1] - last_ctrl[1])
            p2 = (ox + r.number(), oy + r.number())
            p3 = (ox + r.number(), oy + r.number())
            cur.extend(_bezier(pos, p1, p2, p3))
            ctrl, pos = p2, p3
        elif C == "Q":
            q = (ox + r.number(), oy + r.number())
            p3 = (ox + r.number(), oy + r.number())
            p1 = (pos[0] + 2 / 3 * (q[0] - pos[0]), pos[1] + 2 / 3 * (q[1] - pos[1]))
            p2 = (p3[0] + 2 / 3 * (q[0] - p3[0]), p3[1] + 2 / 3 * (q[1] - p3[1]))
            cur.extend(_bezier(pos, p1, p2, p3))
            pos = p3
        elif C == "A":
            rx, ry, phi = r.number(), r.number(), r.number()
            large, sweep = r.flag(), r.flag()
            p3 = (ox + r.number(), oy + r.number())
            cur.extend(_arc(pos, rx, ry, phi, large, sweep, p3))
            pos = p3
        elif C == "Z":
            pos = start
            if cur:
                subs.append(cur)
            cur = []
        else:
            raise ValueError("unsupported path command " + c)
        last_ctrl = ctrl
        cmd = c
    if cur:
        subs.append(cur)
    return subs


def canvas(w, h):
    return Image.new("L", (w * SS, h * SS), 0)


def fill_path(img, d, scale, dx=0.0, dy=0.0):
    """Subpath union matches nonzero filling for these icons."""
    draw = ImageDraw.Draw(img)
    for sub in flatten(d):
        pts = [((x * scale + dx) * SS, (y * scale + dy) * SS) for x, y in sub]
        if len(pts) >= 3:
            draw.polygon(pts, fill=255)


def fill_circle(img, cx, cy, r):
    ImageDraw.Draw(img).ellipse(
        [(cx - r) * SS, (cy - r) * SS, (cx + r) * SS - 1, (cy + r) * SS - 1],
        fill=255)


def fill_poly(img, pts):
    ImageDraw.Draw(img).polygon([(x * SS, y * SS) for x, y in pts], fill=255)


def thick_line(img, x0, y0, x1, y1, w):
    nx, ny = y1 - y0, x0 - x1
    k = w / 2 / math.hypot(nx, ny)
    nx, ny = nx * k, ny * k
    fill_poly(img, [(x0 + nx, y0 + ny), (x1 + nx, y1 + ny),
                    (x1 - nx, y1 - ny), (x0 - nx, y0 - ny)])


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
    fill_circle(img, 12 * s, 9.5 * s, 3.5 * s)
    fill_path(img, AVATAR_BODY_PATH, s)
    return ImageChops.multiply(finish(img), circle(d))


def logo(w):
    h = int(math.ceil(57 * w / 64))
    img = canvas(w, h)
    fill_path(img, LOGO_PATH, w / 64)
    return finish(img)


def icon24(path, size):
    img = canvas(size, size)
    fill_path(img, path, size / 24)
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
    ("LOGO48",    logo(48)),
    ("LOGO20",    logo(20)),
    ("CIRCLE16",  circle(16)),
    ("ICIRCLE16", inverse(circle(16))),
    ("REPOST",    icon24(REPOST_PATH, 12)),
    ("REPLY",     icon24(REPLY_PATH, 12)),
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
    out.write("/*\n"
              " * Generated by tools/gen_ui_atlas.py; do not edit by hand.\n"
              " *\n"
              " * The logo, default avatar, repost and reply paths come from\n"
              " * bluesky-social/social-app (MIT License, Copyright 2023-2026\n"
              " * Bluesky Social PBC); see THIRD_PARTY_LICENSES.md.\n"
              " */\n")
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
