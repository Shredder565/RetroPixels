#!/usr/bin/env python3
# Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
"""Draws PixelU-Wii's own pictures: the program icon, the Sys/Resources images, the icon
theme in Sys/Themes and the pictures of the Windows dark style.

Everything is drawn here from shapes (no image is read), so the output is ours and can be
remade at any time:

  python3 pixelu/wii/art/make_art.py           write every file
  python3 pixelu/wii/art/make_art.py --check   redraw into memory and compare with the files
                                              in the tree (pixels for PNG/ICO, bytes for SVG)

Needs Pillow (python3 -m pip install Pillow). Output files:

  pixelu/wii/Source/Core/DolphinQt/PixelU-Wii.ico        program icon (DolphinQt.rc, DolphinTool.rc)
  pixelu/wii/Data/Sys/Resources/*.png                    same names and sizes as the images the
                                                         program loads from Sys/Resources
  pixelu/wii/Data/Sys/Themes/PixelU/*.png                the toolbar and debugger icons
                                                         (32 x 32, with @2x and @4x copies)
  pixelu/wii/Source/Core/DolphinQt/Styles/Dark/*.svg     check boxes, arrows and so on used by
                                                         dark.qss
"""

import io
import math
import os
import sys

from PIL import Image, ImageChops, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
WII = os.path.dirname(HERE)
ICO_PATH = os.path.join(WII, "Source", "Core", "DolphinQt", "PixelU-Wii.ico")
RES_DIR = os.path.join(WII, "Data", "Sys", "Resources")
DARK_DIR = os.path.join(WII, "Source", "Core", "DolphinQt", "Styles", "Dark")
# The one bundled icon theme (DEFAULT_THEME_DIR in Source/Core/Common/CommonPaths.h).
THEME_NAME = "PixelU"
THEME_DIR = os.path.join(WII, "Data", "Sys", "Themes", THEME_NAME)


def rgb(h, a=255):
    h = h.lstrip("#")
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16), a)


def mix(c1, c2, t):
    t = max(0.0, min(1.0, t))
    return tuple(int(round(c1[i] + (c2[i] - c1[i]) * t)) for i in range(4))


# PixelU-Wii palette.
NAVY_TOP = rgb("1c2f60")
NAVY_BOTTOM = rgb("0a1124")
NAVY_EDGE = rgb("3557b4")
CYAN = rgb("3fd2ff")
BLUE = rgb("2a62f0")
DISC_EDGE = rgb("1b3fa8")
HUB = rgb("a6ecff")
SHINE = rgb("f0fcff")
PINK = rgb("ff4d8d")
WHITE = rgb("ffffff")


# ---------------------------------------------------------------- program icon


def icon_grid(g):
    """The icon as a g x g pixel-art grid: a stepped rounded tile with a pixel disc on it
    and a small pixel spark in the top right corner."""
    im = Image.new("RGBA", (g, g), (0, 0, 0, 0))
    px = im.load()
    s = g / 32.0
    margin = 1 if g >= 24 else 0
    corner = 6.5 * s
    lo, hi = margin, g - 1 - margin

    def in_tile(x, y):
        if x < lo or x > hi or y < lo or y > hi:
            return False
        cx = min(max(x + 0.5, lo + corner), hi + 1 - corner)
        cy = min(max(y + 0.5, lo + corner), hi + 1 - corner)
        return math.hypot(x + 0.5 - cx, y + 0.5 - cy) <= corner

    for y in range(g):
        for x in range(g):
            if not in_tile(x, y):
                continue
            edge = not all(in_tile(x + dx, y + dy) for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
            px[x, y] = NAVY_EDGE if edge else mix(NAVY_TOP, NAVY_BOTTOM, y / (g - 1))

    # The disc, a little left of and below the centre so the spark has room.
    dcx, dcy = 14.5 * s, 17.0 * s
    r_out, r_hub, r_hole = 11.0 * s, 4.6 * s, 2.0 * s
    for y in range(g):
        for x in range(g):
            dx, dy = x + 0.5 - dcx, y + 0.5 - dcy
            r = math.hypot(dx, dy)
            if r > r_out:
                continue
            ang = (math.degrees(math.atan2(dy, dx)) + 360.0) % 360.0
            if r <= r_hole:
                px[x, y] = NAVY_BOTTOM
            elif r <= r_hub:
                px[x, y] = HUB if r > r_hole + 0.9 * s else mix(HUB, BLUE, 0.5)
            elif r > r_out - 1.0 * max(s, 1.0):
                px[x, y] = DISC_EDGE
            else:
                t = (math.sin(math.radians(ang - 30.0)) + 1.0) / 2.0
                c = mix(CYAN, BLUE, t)
                if int(r / max(s * 2.0, 1.0)) % 2 == 1:
                    c = mix(c, NAVY_BOTTOM, 0.12)
                if 200.0 <= ang <= 250.0 and r_hub + 1.2 * s < r < r_out - 1.6 * s:
                    c = SHINE
                px[x, y] = c

    # Spark: a plus of pink pixels with a white centre.
    sx, sy = int(round(25.0 * s)), int(round(6.5 * s))
    arm = 2 if g >= 32 else 1
    for i in range(-arm, arm + 1):
        for (x, y) in ((sx + i, sy), (sx, sy + i)):
            if 0 <= x < g and 0 <= y < g:
                px[x, y] = PINK
    px[sx, sy] = WHITE
    return im


def render_icon(size):
    g = 16 if size <= 24 else 32
    grid = icon_grid(g)
    k = max(1, math.ceil(size / g))
    big = grid.resize((g * k, g * k), Image.NEAREST)
    if big.size[0] != size:
        big = big.resize((size, size), Image.LANCZOS)
    return big


ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]


def make_ico():
    frames = [render_icon(s) for s in ICO_SIZES]
    buf = io.BytesIO()
    frames[-1].save(buf, format="ICO", sizes=[(s, s) for s in ICO_SIZES], append_images=frames[:-1])
    return buf.getvalue()


# ---------------------------------------------------------------- drawing helpers


SS = 4  # supersampling for the smooth pictures


class Canvas:
    """Draws in a w x h coordinate space at SS times the largest output size, then scales down."""

    def __init__(self, w, h, scale, bg=(0, 0, 0, 0)):
        self.w, self.h, self.k = w, h, scale * SS
        self.im = Image.new("RGBA", (int(w * self.k), int(h * self.k)), bg)
        self.d = ImageDraw.Draw(self.im)

    def p(self, pts):
        return [(x * self.k, y * self.k) for x, y in pts]

    def rect(self, x0, y0, x1, y1, fill, r=0.0, outline=None, width=0.0):
        box = (x0 * self.k, y0 * self.k, x1 * self.k - 1, y1 * self.k - 1)
        w = int(round(width * self.k)) if outline else 0
        if r > 0:
            self.d.rounded_rectangle(box, radius=r * self.k, fill=fill, outline=outline, width=w)
        else:
            self.d.rectangle(box, fill=fill, outline=outline, width=w)

    def poly(self, pts, fill):
        self.d.polygon(self.p(pts), fill=fill)

    def circle(self, cx, cy, r, fill=None, outline=None, width=0.0):
        k = self.k
        self.d.ellipse((
            (cx - r) * k, (cy - r) * k, (cx + r) * k, (cy + r) * k), fill=fill, outline=outline,
            width=int(round(width * k)))

    def line(self, pts, fill, width):
        self.d.line(self.p(pts), fill=fill, width=max(1, int(round(width * self.k))), joint="curve")
        r = width / 2.0
        for x, y in (pts[0], pts[-1]):
            self.circle(x, y, r, fill=fill)

    def star(self, cx, cy, r, fill, points=5, inner=0.4, rot=-90.0):
        pts = []
        for i in range(points * 2):
            rr = r if i % 2 == 0 else r * inner
            a = math.radians(rot + i * 180.0 / points)
            pts.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
        self.poly(pts, fill)

    def out(self, w, h):
        return self.im.resize((w, h), Image.LANCZOS)


def scaled_set(draw, base_w, base_h, scales=(1, 2, 4)):
    """draw(canvas) draws in base_w x base_h units; returns {scale: image}."""
    c = Canvas(base_w, base_h, max(scales))
    draw(c)
    return {s: c.out(base_w * s, base_h * s) for s in scales}


# ---------------------------------------------------------------- flags (32 x 22)


FW, FH = 32.0, 22.0


def stripes_h(c, colours):
    n = len(colours)
    for i, col in enumerate(colours):
        c.rect(0, FH * i / n, FW, FH * (i + 1) / n, rgb(col))


def stripes_v(c, colours):
    n = len(colours)
    for i, col in enumerate(colours):
        c.rect(FW * i / n, 0, FW * (i + 1) / n, FH, rgb(col))


def union_flag(c, x0, y0, w, h):
    """The Union Flag in the box (used in the Australian flag's canton)."""
    blue, red, white = rgb("012169"), rgb("c8102e"), rgb("ffffff")
    c.rect(x0, y0, x0 + w, y0 + h, blue)
    t = h / 5.0
    for (a, b) in (((x0, y0), (x0 + w, y0 + h)), ((x0 + w, y0), (x0, y0 + h))):
        c.d.line(c.p([a, b]), fill=white, width=int(t * c.k))
    for (a, b) in (((x0, y0), (x0 + w, y0 + h)), ((x0 + w, y0), (x0, y0 + h))):
        c.d.line(c.p([a, b]), fill=red, width=max(1, int(t * 0.4 * c.k)))
    c.rect(x0 + w / 2 - t, y0, x0 + w / 2 + t, y0 + h, white)
    c.rect(x0, y0 + h / 2 - t, x0 + w, y0 + h / 2 + t, white)
    c.rect(x0 + w / 2 - t * 0.6, y0, x0 + w / 2 + t * 0.6, y0 + h, red)
    c.rect(x0, y0 + h / 2 - t * 0.6, x0 + w, y0 + h / 2 + t * 0.6, red)


def flag_australia(c):
    c.rect(0, 0, FW, FH, rgb("012169"))
    union_flag(c, 0, 0, FW / 2, FH / 2)
    white = rgb("ffffff")
    c.star(FW / 4, FH * 0.76, 2.6, white, points=7, inner=0.45)
    for (x, y, r) in ((24.0, 4.0, 1.5), (20.5, 9.5, 1.5), (27.5, 8.5, 1.5), (24.0, 18.0, 1.7), (25.8, 11.6, 0.8)):
        c.star(x, y, r, white, points=7 if r > 1 else 5, inner=0.45)


def flag_europe(c):
    c.rect(0, 0, FW, FH, rgb("003399"))
    for i in range(12):
        a = math.radians(i * 30.0)
        c.star(FW / 2 + 6.6 * math.sin(a), FH / 2 - 6.6 * math.cos(a), 1.25, rgb("ffcc00"))


def flag_france(c):
    stripes_v(c, ["0055a4", "ffffff", "ef4135"])


def flag_germany(c):
    stripes_h(c, ["000000", "dd0000", "ffce00"])


def flag_italy(c):
    stripes_v(c, ["009246", "ffffff", "ce2b37"])


def flag_netherlands(c):
    stripes_h(c, ["ae1c28", "ffffff", "21468b"])


def flag_russia(c):
    stripes_h(c, ["ffffff", "0039a6", "d52b1e"])


def flag_japan(c):
    c.rect(0, 0, FW, FH, rgb("ffffff"))
    c.circle(FW / 2, FH / 2, FH * 0.3, fill=rgb("bc002d"))


def flag_spain(c):
    c.rect(0, 0, FW, FH, rgb("aa151b"))
    c.rect(0, FH / 4, FW, FH * 3 / 4, rgb("f1bf00"))
    # A plain shield for the arms, left of centre.
    x, y = 8.0, 7.0
    c.rect(x, y, x + 4.6, y + 6.5, rgb("aa151b"), r=1.2)
    c.rect(x + 1.2, y + 1.2, x + 3.4, y + 4.0, rgb("f1bf00"))
    c.rect(x - 1.2, y + 0.5, x - 0.4, y + 7.0, rgb("8a8a8a"))
    c.rect(x + 5.0, y + 0.5, x + 5.8, y + 7.0, rgb("8a8a8a"))


def flag_taiwan(c):
    c.rect(0, 0, FW, FH, rgb("fe0000"))
    c.rect(0, 0, FW / 2, FH / 2, rgb("000095"))
    cx, cy = FW / 4, FH / 4
    c.star(cx, cy, 4.2, rgb("ffffff"), points=12, inner=0.62)
    c.circle(cx, cy, 2.2, fill=rgb("000095"))
    c.circle(cx, cy, 1.8, fill=rgb("ffffff"))


def flag_usa(c):
    red, white = rgb("b22234"), rgb("ffffff")
    for i in range(13):
        c.rect(0, FH * i / 13, FW, FH * (i + 1) / 13, red if i % 2 == 0 else white)
    cw, ch = FW * 0.4, FH * 7 / 13
    c.rect(0, 0, cw, ch, rgb("3c3b6e"))
    for row in range(9):
        cols = 6 if row % 2 == 0 else 5
        for col in range(cols):
            x = cw * ((col * 2 + (1 if row % 2 == 0 else 2)) / 12.0)
            y = ch * ((row + 1) / 10.0)
            c.circle(x, y, 0.42, fill=white)


def flag_korea(c):
    c.rect(0, 0, FW, FH, rgb("ffffff"))
    cx, cy, r = FW / 2, FH / 2, 5.4
    red, blue, black = rgb("cd2e3a"), rgb("0047a0"), rgb("000000")
    # The taegeuk: red over blue, split by an S curve, tilted along the flag's diagonal.
    tilt = math.degrees(math.atan2(FH, FW))
    k = c.k
    c.d.pieslice(((cx - r) * k, (cy - r) * k, (cx + r) * k, (cy + r) * k), 180 + tilt, 360 + tilt, fill=red)
    c.d.pieslice(((cx - r) * k, (cy - r) * k, (cx + r) * k, (cy + r) * k), tilt, 180 + tilt, fill=blue)
    ux, uy = math.cos(math.radians(tilt)), math.sin(math.radians(tilt))
    c.circle(cx - ux * r / 2, cy - uy * r / 2, r / 2, fill=red)
    c.circle(cx + ux * r / 2, cy + uy * r / 2, r / 2, fill=blue)
    # Trigrams: three bars each, some broken, in the four corners.
    patterns = [(0, 0, 0), (1, 1, 1), (0, 1, 0), (1, 0, 1)]
    corners = [(-1, -1), (1, 1), (1, -1), (-1, 1)]
    for (sx, sy), pat in zip(corners, patterns):
        n = math.hypot(FW, FH)
        nx, ny = sx * FW / n, sy * FH / n  # from the centre towards the corner
        ax, ay = -ny, nx  # along a bar
        d = 9.6
        px_, py_ = cx + nx * d, cy + ny * d
        for i, broken in enumerate(pat):
            off = (i - 1) * 1.3
            bx, by = px_ + nx * off, py_ + ny * off
            half = 2.4
            if broken:
                for a, b in ((-half, -0.3), (0.3, half)):
                    c.line([(bx + ax * a, by + ay * a), (bx + ax * b, by + ay * b)], black, 0.8)
            else:
                c.line([(bx - ax * half, by - ay * half), (bx + ax * half, by + ay * half)], black, 0.8)


def flag_international(c):
    c.rect(0, 0, FW, FH, rgb("4b92db"))
    white = rgb("ffffff")
    cx, cy, r = FW / 2, FH / 2, 6.0
    c.circle(cx, cy, r, outline=white, width=0.8)
    c.d.ellipse(((cx - r * 0.45) * c.k, (cy - r) * c.k, (cx + r * 0.45) * c.k, (cy + r) * c.k),
                outline=white, width=int(0.7 * c.k))
    c.line([(cx - r, cy), (cx + r, cy)], white, 0.7)
    c.line([(cx, cy - r), (cx, cy + r)], white, 0.7)
    for yy in (-r * 0.55, r * 0.55):
        half = math.sqrt(r * r - yy * yy)
        c.line([(cx - half, cy + yy), (cx + half, cy + yy)], white, 0.6)


def flag_unknown(c):
    c.rect(0, 0, FW, FH, rgb("d9dce3"))
    c.rect(0.5, 0.5, FW - 0.5, FH - 0.5, None, outline=rgb("9aa0ad"), width=1.0)
    ink = rgb("5a6070")
    # A question mark from an arc and two strokes.
    k = c.k
    cx, cy = FW / 2, 8.2
    c.d.arc(((cx - 3.6) * k, (cy - 3.6) * k, (cx + 3.6) * k, (cy + 3.6) * k), 180, 400, fill=ink,
            width=int(1.8 * k))
    c.line([(cx + 2.7, cy + 2.4), (cx, cy + 4.6), (cx, cy + 6.6)], ink, 1.8)
    c.circle(cx, 18.2, 1.15, fill=ink)


FLAGS = {
    "Flag_Australia": flag_australia,
    "Flag_Europe": flag_europe,
    "Flag_France": flag_france,
    "Flag_Germany": flag_germany,
    "Flag_International": flag_international,
    "Flag_Italy": flag_italy,
    "Flag_Japan": flag_japan,
    "Flag_Korea": flag_korea,
    "Flag_Netherlands": flag_netherlands,
    "Flag_Russia": flag_russia,
    "Flag_Spain": flag_spain,
    "Flag_Taiwan": flag_taiwan,
    "Flag_USA": flag_usa,
    "Flag_Unknown": flag_unknown,
}


# ---------------------------------------------------------------- platform icons (32 x 32)


def platform_gamecube(c):
    # A plain isometric cube in violet.
    top, left, right, edge = rgb("a99bff"), rgb("6a55e0"), rgb("4a37b8"), rgb("2c2075")
    cx, cy, s = 16.0, 16.5, 11.0
    h = s * 0.58
    t = [(cx, cy - s), (cx + s * 0.87, cy - s + h), (cx, cy - s + 2 * h), (cx - s * 0.87, cy - s + h)]
    l = [t[3], t[2], (cx, cy + s), (cx - s * 0.87, cy + s - h)]
    r = [t[2], t[1], (cx + s * 0.87, cy + s - h), (cx, cy + s)]
    c.poly([(x, y) for x, y in t], top)
    c.poly(l, left)
    c.poly(r, right)
    c.line(t + [t[0]], edge, 1.0)
    c.line([l[0], l[3], l[2], r[2], r[1]], edge, 1.0)
    c.line([t[2], l[2]], edge, 1.0)


def platform_triforce(c):
    # An arcade cabinet.
    body, dark, screen, glow = rgb("f0a63a"), rgb("8a4f0e"), rgb("1b2340"), rgb("4fd8ff")
    c.poly([(9, 3), (23, 3), (24, 9), (22, 16), (25, 22), (25, 29), (7, 29), (7, 22), (10, 16), (8, 9)], body)
    c.rect(10, 5, 22, 8, dark, r=0.6)
    c.poly([(10.5, 10), (21.5, 10), (20.5, 16), (11.5, 16)], screen)
    c.rect(13, 12, 19, 14, glow, r=0.5)
    c.rect(8, 19, 24, 22, dark, r=0.8)
    c.circle(12, 20.5, 1.1, fill=rgb("ff4d8d"))
    c.circle(19, 20.5, 0.9, fill=glow)
    c.circle(21.5, 20.5, 0.9, fill=glow)
    c.rect(13, 24, 19, 27, dark, r=0.5)


def platform_wii(c):
    # A motion remote seen from the front.
    body, line, btn = rgb("eef2f8"), rgb("8c96a8"), rgb("3a7bff")
    c.rect(11, 2, 21, 30, line, r=4.5)
    c.rect(12, 3, 20, 29, body, r=3.8)
    c.rect(14.6, 6, 17.4, 12, rgb("4d5566"), r=0.4)
    c.rect(13, 7.6, 19, 10.4, rgb("4d5566"), r=0.4)
    c.circle(16, 15.5, 2.0, fill=btn)
    for y in (20.5, 23.0):
        c.circle(16, y, 0.9, fill=line)
    c.rect(14.4, 26, 17.6, 27, btn, r=0.4)


def platform_wad(c):
    # A channel: a rounded screen tile with a play mark.
    c.rect(3, 6, 29, 26, rgb("1aa3c9"), r=4)
    c.rect(4.5, 7.5, 27.5, 24.5, rgb("52d3f2"), r=3)
    c.poly([(13, 11), (21.5, 16), (13, 21)], rgb("ffffff"))
    c.rect(10, 27.5, 22, 29, rgb("1aa3c9"), r=0.7)


def platform_file(c):
    paper, edge, fold = rgb("e6e9ef"), rgb("8890a0"), rgb("bfc5d1")
    c.poly([(7, 3), (19.5, 3), (26, 9.5), (26, 29), (7, 29)], edge)
    c.poly([(8, 4), (19, 4), (25, 10), (25, 28), (8, 28)], paper)
    c.poly([(19, 4), (25, 10), (19, 10)], fold)
    for y in (15, 19, 23):
        c.line([(11, y), (22, y)], edge, 1.0)


PLATFORMS = {
    "Platform_File": platform_file,
    "Platform_Gamecube": platform_gamecube,
    "Platform_Triforce": platform_triforce,
    "Platform_Wad": platform_wad,
    "Platform_Wii": platform_wii,
}


# ---------------------------------------------------------------- other resources


def nobanner(c):
    # 96 x 32: shown when a game has no banner. A dark slot with a dashed frame and a disc.
    c.rect(0, 0, 96, 32, rgb("2a2f3b"), r=3)
    k = c.k
    dash = rgb("596277")
    for x in range(4, 92, 6):
        c.line([(x, 2.5), (min(x + 3, 92), 2.5)], dash, 1.0)
        c.line([(x, 29.5), (min(x + 3, 92), 29.5)], dash, 1.0)
    for y in range(6, 28, 6):
        c.line([(2.5, y), (2.5, min(y + 3, 27))], dash, 1.0)
        c.line([(93.5, y), (93.5, min(y + 3, 27))], dash, 1.0)
    c.circle(48, 16, 9, fill=rgb("3f4859"))
    c.circle(48, 16, 3.2, fill=rgb("6b7790"))
    c.circle(48, 16, 1.4, fill=rgb("2a2f3b"))
    c.d.arc(((48 - 6.5) * k, (16 - 6.5) * k, (48 + 6.5) * k, (16 + 6.5) * k), 200, 250,
            fill=rgb("8e9ab3"), width=int(1.6 * k))


ACH_BG = rgb("3a4152")
ACH_FG = rgb("aab3c5")


def ach_game(c):
    c.rect(0, 0, 96, 96, ACH_BG)
    # A game pad.
    c.rect(18, 34, 78, 66, ACH_FG, r=14)
    bg = ACH_BG
    c.rect(28, 46, 42, 52, bg, r=1)
    c.rect(32, 42, 38, 56, bg, r=1)
    c.circle(62, 45, 3.6, fill=bg)
    c.circle(69, 52, 3.6, fill=bg)
    c.rect(44, 38, 52, 41, bg, r=1)


def ach_player(c):
    c.rect(0, 0, 96, 96, ACH_BG)
    c.circle(48, 36, 15, fill=ACH_FG)
    c.d.chord(((20) * c.k, (56) * c.k, (76) * c.k, (112) * c.k), 180, 360, fill=ACH_FG)


def padlock(c, open_):
    c.rect(0, 0, 64, 64, ACH_BG)
    k = c.k
    if open_:
        c.d.arc(((26) * k, (12) * k, (46) * k, (36) * k), 180, 360, fill=ACH_FG, width=int(5 * k))
        c.rect(41, 22, 46, 31, ACH_FG)
        c.rect(14, 29, 46, 54, ACH_FG, r=4)
    else:
        c.d.arc(((21) * k, (12) * k, (43) * k, (38) * k), 180, 360, fill=ACH_FG, width=int(5 * k))
        c.rect(21, 24, 26, 31, ACH_FG)
        c.rect(38, 24, 43, 31, ACH_FG)
        c.rect(16, 29, 48, 54, ACH_FG, r=4)
    hx = 30 if open_ else 32
    c.circle(hx, 39, 3.6, fill=ACH_BG)
    c.rect(hx - 1.6, 40, hx + 1.6, 47, ACH_BG, r=0.8)


def iso_disc(c):
    c.circle(8, 8, 7.2, fill=rgb("7fa6c9"))
    c.circle(8, 8, 6.4, fill=rgb("d7e6f3"))
    k = c.k
    c.d.pieslice(((8 - 6.4) * k, (8 - 6.4) * k, (8 + 6.4) * k, (8 + 6.4) * k), 200, 250, fill=rgb("ffffff"))
    c.d.pieslice(((8 - 6.4) * k, (8 - 6.4) * k, (8 + 6.4) * k, (8 + 6.4) * k), 20, 70, fill=rgb("a9c6e0"))
    c.circle(8, 8, 2.4, fill=rgb("7fa6c9"))
    c.circle(8, 8, 1.2, fill=(0, 0, 0, 0))


def iso_file(c):
    edge, paper = rgb("7a8294"), rgb("f2f4f8")
    c.poly([(3, 1), (10, 1), (14, 5), (14, 15), (3, 15)], edge)
    c.poly([(4, 2), (9.6, 2), (13, 5.4), (13, 14), (4, 14)], paper)
    c.poly([(9.6, 2), (13, 5.4), (9.6, 5.4)], rgb("c9ced8"))


def iso_folder(c):
    c.poly([(1, 3), (6, 3), (7.5, 4.5), (15, 4.5), (15, 14), (1, 14)], rgb("c98f1d"))
    c.poly([(1, 6.5), (15, 6.5), (15, 14), (1, 14)], rgb("f2c14e"))


def logo_image(size):
    return render_icon(size)


def resources():
    """{file name: image} for everything in Sys/Resources that is a picture."""
    out = {}
    for name, fn in sorted(FLAGS.items()):
        for s, im in scaled_set(fn, 32, 22).items():
            out[name + (".png" if s == 1 else "@%dx.png" % s)] = im
    for name, fn in sorted(PLATFORMS.items()):
        for s, im in scaled_set(fn, 32, 32).items():
            out[name + (".png" if s == 1 else "@%dx.png" % s)] = im
    for s, im in scaled_set(nobanner, 96, 32).items():
        out["nobanner" + (".png" if s == 1 else "@%dx.png" % s)] = im
    out["dolphin_logo.png"] = logo_image(200)
    out["dolphin_logo@2x.png"] = logo_image(400)
    out["achievements_game.png"] = scaled_set(ach_game, 96, 96, (1,))[1]
    out["achievements_player.png"] = scaled_set(ach_player, 96, 96, (1,))[1]
    out["achievements_locked.png"] = scaled_set(lambda c: padlock(c, False), 64, 64, (1,))[1]
    out["achievements_unlocked.png"] = scaled_set(lambda c: padlock(c, True), 64, 64, (1,))[1]
    out["isoproperties_disc.png"] = scaled_set(iso_disc, 16, 16, (1,))[1]
    out["isoproperties_file.png"] = scaled_set(iso_file, 16, 16, (1,))[1]
    out["isoproperties_folder.png"] = scaled_set(iso_folder, 16, 16, (1,))[1]
    return out


# ---------------------------------------------------------------- icon theme (32 x 32)

# One mid-tone ink reads on light and dark toolbars alike; a few icons carry an accent.
T_INK = rgb("5b8def")
T_DEEP = rgb("2f5fc4")
T_SOFT = rgb("a9c4f7")
T_GO = rgb("2fbf71")
T_HALT = PINK
T_WARN = rgb("f2a93b")
T_W = 2.6  # stroke width


def t_head(c, tip, direction, col, size=5.2):
    """An arrowhead with its point at tip, pointing along direction (dx, dy)."""
    dx, dy = direction
    n = math.hypot(dx, dy) or 1.0
    dx, dy = dx / n, dy / n
    px, py = -dy, dx
    back = (tip[0] - dx * size, tip[1] - dy * size)
    c.poly([tip, (back[0] + px * size * 0.62, back[1] + py * size * 0.62),
            (back[0] - px * size * 0.62, back[1] - py * size * 0.62)], col)


def t_arrow(c, pts, col, w=T_W, size=5.2):
    """A polyline that ends in an arrowhead (the line stops short so the point stays sharp)."""
    (x0, y0), (x1, y1) = pts[-2], pts[-1]
    dx, dy = x1 - x0, y1 - y0
    n = math.hypot(dx, dy) or 1.0
    stop = (x1 - dx / n * size * 0.8, y1 - dy / n * size * 0.8)
    c.line(pts[:-1] + [stop], col, w)
    t_head(c, (x1, y1), (dx, dy), col, size)


def t_arc_arrow(c, cx, cy, r, a0, a1, col, w=T_W, size=5.2):
    """An arc from a0 to a1 (degrees, clockwise from three o'clock) ending in an arrowhead."""
    k = c.k
    trim = math.degrees(size * 0.7 / r)
    end = a1 - trim if a1 > a0 else a1 + trim
    lo, hi = (a0, end) if a1 > a0 else (end, a0)
    c.d.arc(((cx - r - w / 2) * k, (cy - r - w / 2) * k, (cx + r + w / 2) * k, (cy + r + w / 2) * k),
            lo, hi, fill=col, width=max(1, int(round(w * k))))
    start = math.radians(a0)
    c.circle(cx + r * math.cos(start), cy + r * math.sin(start), w / 2, fill=col)
    a = math.radians(a1)
    tip = (cx + r * math.cos(a), cy + r * math.sin(a))
    sign = 1.0 if a1 > a0 else -1.0
    t_head(c, tip, (-math.sin(a) * sign, math.cos(a) * sign), col, size)


def t_outline_rect(c, x0, y0, x1, y1, col, r=2.5, w=T_W):
    c.rect(x0, y0, x1, y1, None, r=r, outline=col, width=w)


def t_folder(c, col, open_=False):
    c.poly([(4, 8), (12, 8), (14.5, 10.5), (27, 10.5), (27, 25), (4, 25)], col)
    if open_:
        c.poly([(8, 14), (30, 14), (26, 25), (4, 25)], T_SOFT)
    else:
        c.rect(4, 13, 27, 25, T_SOFT, r=1.2)


def t_page(c, col=T_INK, fill=None):
    pts = [(8, 3.5), (19.5, 3.5), (25, 9), (25, 28.5), (8, 28.5)]
    c.poly(pts, col)
    c.poly([(10.4, 5.9), (18.6, 5.9), (22.6, 9.9), (22.6, 26.1), (10.4, 26.1)], fill or rgb("ffffff", 0))
    c.poly([(19, 3.5), (25, 9.5), (19, 9.5)], T_DEEP)


def t_floppy(c, col=T_INK):
    c.poly([(4.5, 4.5), (23.5, 4.5), (27.5, 8.5), (27.5, 27.5), (4.5, 27.5)], col)
    c.rect(9, 4.5, 21, 12, T_SOFT, r=0.8)
    c.rect(17, 6, 19.5, 10.5, T_DEEP, r=0.4)
    c.rect(8, 16, 24, 27.5, rgb("ffffff"), r=1.0)
    for y in (19.5, 23):
        c.line([(11, y), (21, y)], T_SOFT, 1.4)


def t_dot(c, cx, cy, r, col):
    c.circle(cx, cy, r, fill=col)


def t_plus_badge(c, cx=24.5, cy=24.5):
    c.circle(cx, cy, 6.2, fill=T_GO)
    c.line([(cx - 3.2, cy), (cx + 3.2, cy)], rgb("ffffff"), 2.0)
    c.line([(cx, cy - 3.2), (cx, cy + 3.2)], rgb("ffffff"), 2.0)


def t_code_lines(c, x0=6, x1=26, ys=(8, 14, 20, 26)):
    for i, y in enumerate(ys):
        c.line([(x0 + (4 if i % 2 else 0), y), (x1 - (6 if i == len(ys) - 1 else 0), y)], T_SOFT, 2.0)


def icon_play(c):
    c.poly([(9, 5), (27, 16), (9, 27)], T_GO)


def icon_pause(c):
    c.rect(7.5, 5, 13.5, 27, T_INK, r=1.8)
    c.rect(18.5, 5, 24.5, 27, T_INK, r=1.8)


def icon_stop(c):
    c.rect(6, 6, 26, 26, T_HALT, r=3.5)


def icon_open(c):
    t_folder(c, T_INK, open_=True)


def icon_browse(c):
    t_folder(c, T_INK)
    c.circle(20, 19, 5.4, fill=rgb("ffffff"), outline=T_DEEP, width=2.4)
    c.line([(23.8, 22.8), (28.5, 27.5)], T_DEEP, 3.0)


def icon_refresh(c):
    t_arc_arrow(c, 16, 16, 10, 200, 520 - 40, T_INK, w=3.0, size=6.5)


def icon_fullscreen(c):
    w = 3.0
    for sx, sy in ((1, 1), (-1, 1), (1, -1), (-1, -1)):
        x = 16 - sx * 11
        y = 16 - sy * 11
        c.line([(x, y + sy * 7), (x, y), (x + sx * 7, y)], T_INK, w)
    c.rect(11, 11, 21, 21, T_SOFT, r=1.5)


def icon_screenshot(c):
    c.rect(3.5, 9, 28.5, 26.5, T_INK, r=3)
    c.rect(11, 5.5, 21, 10, T_INK, r=1.2)
    c.circle(16, 17.5, 6.2, fill=rgb("ffffff"))
    c.circle(16, 17.5, 4.0, fill=T_DEEP)
    c.circle(24.5, 12.5, 1.3, fill=T_SOFT)


def icon_config(c):
    cx, cy = 16, 16
    teeth = 8
    pts = []
    for i in range(teeth * 4):
        a = math.radians(i * 360.0 / (teeth * 4) + 360.0 / (teeth * 8))
        r = 13.0 if (i % 4) in (0, 1) else 10.0
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    c.poly(pts, T_INK)
    c.circle(cx, cy, 4.2, fill=(0, 0, 0, 0))


def icon_graphics(c):
    c.rect(3, 5, 29, 23, T_INK, r=2.5)
    c.rect(5.5, 7.5, 26.5, 20.5, T_SOFT, r=1.0)
    c.poly([(7, 19), (13, 12), (17, 16.5), (20, 13.5), (25, 19)], T_DEEP)
    c.circle(21.5, 10.5, 1.8, fill=rgb("ffffff"))
    c.rect(13, 23, 19, 26, T_INK)
    c.rect(9, 26, 23, 28.5, T_INK, r=1.0)


def icon_classic(c):
    # A two-handled pad: d-pad on the left, four buttons on the right.
    c.rect(2.5, 9, 29.5, 24, T_INK, r=7.5)
    c.rect(7, 14.8, 13, 18.2, rgb("ffffff"), r=0.6)
    c.rect(8.3, 13.5, 11.7, 19.5, rgb("ffffff"), r=0.6)
    for (x, y) in ((22, 13.6), (25, 16.5), (19, 16.5), (22, 19.4)):
        c.circle(x, y, 1.6, fill=T_SOFT)
    c.rect(14.5, 12, 17.5, 13.6, T_SOFT, r=0.6)


def icon_gcpad(c):
    # A rounder pad with a big main button and a stick.
    c.poly([(5, 10), (27, 10), (30, 22), (26, 28), (21, 24), (11, 24), (6, 28), (2, 22)], T_INK)
    c.circle(9.5, 15.5, 3.4, fill=T_SOFT)
    c.circle(9.5, 15.5, 1.6, fill=T_DEEP)
    c.circle(22.5, 16, 3.0, fill=T_GO)
    c.circle(26.5, 13, 1.4, fill=rgb("ffffff"))
    c.circle(18.6, 13, 1.4, fill=rgb("ffffff"))
    c.circle(19.5, 20.5, 2.0, fill=T_WARN)


def icon_wiimote(c):
    c.rect(11, 2.5, 21, 29.5, T_INK, r=4.5)
    c.rect(14.6, 5.8, 17.4, 11.8, rgb("ffffff"), r=0.4)
    c.rect(13, 7.4, 19, 10.2, rgb("ffffff"), r=0.4)
    c.circle(16, 15.8, 2.1, fill=T_SOFT)
    for y in (21, 24.5):
        c.circle(16, y, 1.1, fill=rgb("ffffff"))


def icon_assembler_new(c):
    t_page(c)
    t_plus_badge(c)


def icon_assembler_openasm(c):
    t_folder(c, T_INK, open_=True)
    c.line([(15, 15.5), (12, 19), (15, 22.5)], T_DEEP, 2.2)
    c.line([(21, 15.5), (24, 19), (21, 22.5)], T_DEEP, 2.2)


def icon_assembler_save(c):
    t_floppy(c)


def icon_assembler_assemble(c):
    # A spanner (build the code). Drawing with a clear colour cuts the jaw out of the head.
    clear = (0, 0, 0, 0)
    c.line([(10, 10), (26.5, 26.5)], T_INK, 5.0)
    c.circle(10, 10, 7.5, fill=T_INK)
    c.poly([(2, 7), (7, 2), (12.5, 7.5), (7.5, 12.5)], clear)
    c.circle(26.5, 26.5, 1.4, fill=T_DEEP)


def icon_assembler_inject(c):
    # Code going into the chip.
    c.rect(7, 16, 25, 28, T_INK, r=2)
    for x in (10, 14, 18, 22):
        c.line([(x, 28), (x, 30.5)], T_INK, 1.6)
    c.rect(11, 19, 21, 25, T_SOFT, r=1)
    t_arrow(c, [(16, 2.5), (16, 14.5)], T_GO, w=3.0, size=6.0)


def icon_assembler_clipboard(c):
    c.rect(6, 5, 26, 29.5, T_INK, r=2.5)
    c.rect(8.8, 8.5, 23.2, 26.8, rgb("ffffff"), r=1)
    c.rect(11, 2.5, 21, 7.5, T_DEEP, r=1.5)
    for y in (13, 17.5, 22):
        c.line([(11.5, y), (20.5, y)], T_SOFT, 1.8)


def icon_debugger_breakpoint(c):
    t_dot(c, 16, 16, 10.5, T_HALT)
    c.circle(12.5, 12.5, 2.6, fill=mix(T_HALT, WHITE, 0.6))


def icon_debugger_add_breakpoint(c):
    t_dot(c, 13.5, 13.5, 9.5, T_HALT)
    t_plus_badge(c)


def icon_debugger_add_memorycheck(c):
    c.rect(4, 6, 22, 21, T_INK, r=2)
    for x in (7.5, 11.5, 15.5, 19):
        c.line([(x, 21), (x, 24)], T_INK, 1.5)
        c.line([(x, 6), (x, 3)], T_INK, 1.5)
    c.rect(7.5, 9.5, 18.5, 17.5, T_SOFT, r=1)
    t_plus_badge(c)


def icon_debugger_clear(c):
    # An eraser on a stroke.
    c.poly([(5, 20), (17, 7), (27, 16), (17.5, 26), (10, 26)], T_INK)
    c.poly([(5, 20), (10.5, 14), (19, 22), (15, 26), (10, 26)], T_SOFT)
    c.line([(4, 29), (28, 29)], T_DEEP, 2.0)


def icon_debugger_delete(c):
    c.rect(5, 6.5, 27, 9.5, T_INK, r=1.2)
    c.rect(12.5, 3.5, 19.5, 7, T_INK, r=1.2)
    c.poly([(7.5, 10.5), (24.5, 10.5), (23, 29), (9, 29)], T_INK)
    for x in (12.5, 16, 19.5):
        c.line([(x, 14), (x, 25.5)], T_SOFT, 1.8)


def icon_debugger_load(c):
    # Into the tray.
    c.line([(5, 19), (5, 27), (27, 27), (27, 19)], T_INK, 3.0)
    t_arrow(c, [(16, 3.5), (16, 22)], T_DEEP, w=3.2, size=7.0)


def icon_debugger_save(c):
    t_floppy(c)
    c.circle(25, 25, 5.5, fill=T_GO)
    t_arrow(c, [(25, 28), (25, 21.5)], rgb("ffffff"), w=1.8, size=3.6)


def icon_debugger_set_pc(c):
    t_code_lines(c, 13, 28)
    t_arrow(c, [(3, 14), (11.5, 14)], T_WARN, w=3.0, size=6.0)


def icon_debugger_show_pc(c):
    c.circle(16, 16, 10.5, outline=T_INK, width=2.6)
    for (a, b) in (((16, 1.5), (16, 8)), ((16, 24), (16, 30.5)), ((1.5, 16), (8, 16)), ((24, 16), (30.5, 16))):
        c.line([a, b], T_INK, 2.6)
    c.circle(16, 16, 3.4, fill=T_WARN)


def icon_debugger_skip(c):
    t_code_lines(c, 8, 26, ys=(14, 20, 26))
    t_arc_arrow(c, 16, 15, 8.5, 200, 340, T_WARN, w=2.8, size=5.6)


def icon_debugger_step_in(c):
    t_arrow(c, [(16, 2.5), (16, 21)], T_INK, w=3.0, size=6.5)
    t_dot(c, 16, 26.5, 3.4, T_WARN)


def icon_debugger_step_out(c):
    t_dot(c, 16, 26.5, 3.4, T_WARN)
    t_arrow(c, [(16, 21), (16, 2.5)], T_INK, w=3.0, size=6.5)


def icon_debugger_step_over(c):
    t_arc_arrow(c, 16, 18, 10, 190, 350, T_INK, w=3.0, size=6.2)
    t_dot(c, 16, 26, 3.4, T_WARN)


THEME_ICONS = {
    "assembler_assemble": icon_assembler_assemble,
    "assembler_clipboard": icon_assembler_clipboard,
    "assembler_inject": icon_assembler_inject,
    "assembler_new": icon_assembler_new,
    "assembler_openasm": icon_assembler_openasm,
    "assembler_save": icon_assembler_save,
    "browse": icon_browse,
    "classic": icon_classic,
    "config": icon_config,
    "debugger_add_breakpoint": icon_debugger_add_breakpoint,
    "debugger_add_memorycheck": icon_debugger_add_memorycheck,
    "debugger_breakpoint": icon_debugger_breakpoint,
    "debugger_clear": icon_debugger_clear,
    "debugger_delete": icon_debugger_delete,
    "debugger_load": icon_debugger_load,
    "debugger_save": icon_debugger_save,
    "debugger_set_pc": icon_debugger_set_pc,
    "debugger_show_pc": icon_debugger_show_pc,
    "debugger_skip": icon_debugger_skip,
    "debugger_step_in": icon_debugger_step_in,
    "debugger_step_out": icon_debugger_step_out,
    "debugger_step_over": icon_debugger_step_over,
    "fullscreen": icon_fullscreen,
    "gcpad": icon_gcpad,
    "graphics": icon_graphics,
    "open": icon_open,
    "pause": icon_pause,
    "play": icon_play,
    "refresh": icon_refresh,
    "screenshot": icon_screenshot,
    "stop": icon_stop,
    "wiimote": icon_wiimote,
}


def theme_icons():
    """{file name: image} for the icon theme: every name at 32, 64 and 128 pixels."""
    out = {}
    for name, fn in sorted(THEME_ICONS.items()):
        for s, im in scaled_set(fn, 32, 32).items():
            out[name + (".png" if s == 1 else "@%dx.png" % s)] = im
    return out


# ---------------------------------------------------------------- dark style pictures

# Colours shared with dark.qss.
D_TEXT = "#e4e7ed"
D_MUTED = "#6f7684"
D_FRAME = "#8a93a3"
D_ACCENT = "#3fa9f5"
D_ACCENT_DIM = "#3b4f63"
D_MARK = "#0f1520"


def svg(w, h, body):
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">\n%s\n</svg>\n'
            % (w, h, w, h, body))


def check_box(state, disabled):
    frame = D_MUTED if disabled else D_FRAME
    fill = D_ACCENT_DIM if disabled else D_ACCENT
    mark = "#9aa2b0" if disabled else D_MARK
    if state == "empty":
        return svg(16, 16, '<rect x="1.5" y="1.5" width="13" height="13" rx="3" fill="none" stroke="%s" '
                   'stroke-width="1.5"/>' % frame)
    box = '<rect x="1" y="1" width="14" height="14" rx="3" fill="%s"/>' % fill
    if state == "checked":
        return svg(16, 16, box + '\n<path d="M4.2 8.3 L7 11 L11.9 5.2" fill="none" stroke="%s" '
                   'stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>' % mark)
    return svg(16, 16, box + '\n<rect x="4" y="7" width="8" height="2" rx="1" fill="%s"/>' % mark)


def radio(checked, disabled):
    frame = D_MUTED if disabled else D_FRAME
    if not checked:
        return svg(16, 16, '<circle cx="8" cy="8" r="6.5" fill="none" stroke="%s" stroke-width="1.5"/>' % frame)
    ring = D_ACCENT_DIM if disabled else D_ACCENT
    dot = "#9aa2b0" if disabled else D_MARK
    return svg(16, 16, '<circle cx="8" cy="8" r="7" fill="%s"/>\n<circle cx="8" cy="8" r="3" fill="%s"/>'
               % (ring, dot))


def chevron(direction, w=16, h=16, colour=D_TEXT, size=4.0, stroke=1.6):
    cx, cy = w / 2.0, h / 2.0
    s = size
    pts = {
        "up": [(cx - s, cy + s / 2), (cx, cy - s / 2), (cx + s, cy + s / 2)],
        "down": [(cx - s, cy - s / 2), (cx, cy + s / 2), (cx + s, cy - s / 2)],
        "left": [(cx + s / 2, cy - s), (cx - s / 2, cy), (cx + s / 2, cy + s)],
        "right": [(cx - s / 2, cy - s), (cx + s / 2, cy), (cx - s / 2, cy + s)],
    }[direction]
    d = "M" + " L".join("%g %g" % p for p in pts)
    return svg(w, h, '<path d="%s" fill="none" stroke="%s" stroke-width="%g" stroke-linecap="round" '
               'stroke-linejoin="round"/>' % (d, colour, stroke))


def triangle(direction, w, h, colour=D_TEXT):
    cx, cy = w / 2.0, h / 2.0
    a = min(w, h) * 0.32
    pts = {
        "up": [(cx - a, cy + a / 2), (cx + a, cy + a / 2), (cx, cy - a / 2)],
        "down": [(cx - a, cy - a / 2), (cx + a, cy - a / 2), (cx, cy + a / 2)],
        "left": [(cx + a / 2, cy - a), (cx + a / 2, cy + a), (cx - a / 2, cy)],
        "right": [(cx - a / 2, cy - a), (cx - a / 2, cy + a), (cx + a / 2, cy)],
    }[direction]
    d = "M" + " L".join("%g %g" % p for p in pts) + " Z"
    return svg(w, h, '<path d="%s" fill="%s"/>' % (d, colour))


def dark_svgs():
    out = {}
    for disabled in (False, True):
        suffix = "-disabled" if disabled else ""
        for state in ("checked", "empty", "half"):
            out["checkbox-%s%s.svg" % (state, suffix)] = check_box(state, disabled)
        out["radiobutton-checked%s.svg" % suffix] = radio(True, disabled)
        out["radiobutton-empty%s.svg" % suffix] = radio(False, disabled)
    out["dockwidget-close.svg"] = svg(16, 16, '<path d="M4 4 L12 12 M12 4 L4 12" stroke="%s" '
                                      'stroke-width="1.6" stroke-linecap="round"/>' % D_TEXT)
    out["dockwidget-undock.svg"] = svg(16, 16, '<rect x="2.5" y="5.5" width="8" height="8" rx="1.5" fill="none" '
                                       'stroke="%s" stroke-width="1.3"/>\n<path d="M5.5 3.5 Q5.5 2.5 6.5 2.5 '
                                       'L12.5 2.5 Q13.5 2.5 13.5 3.5 L13.5 9.5 Q13.5 10.5 12.5 10.5" fill="none" '
                                       'stroke="%s" stroke-width="1.3"/>' % (D_TEXT, D_TEXT))
    out["dropdown-arrow.svg"] = chevron("down", 16, 16, size=3.6)
    out["down-triangle.svg"] = triangle("down", 16, 16)
    out["up-triangle.svg"] = triangle("up", 16, 16)
    out["down-triangle-spinbox.svg"] = triangle("down", 16, 8)
    out["up-triangle-spinbox.svg"] = triangle("up", 16, 8)
    out["left-triangle-tabbar.svg"] = triangle("left", 8, 16)
    out["right-triangle-tabbar.svg"] = triangle("right", 8, 16)
    for d in ("up", "down", "left", "right"):
        out["scrollbar-arrow-%s.svg" % d] = chevron(d, 16, 16, colour="#b9c0cc", size=3.2, stroke=1.5)
    out["table-header-sort-arrow-up.svg"] = chevron("up", 12, 8, size=3.4, stroke=1.4)
    out["table-header-sort-arrow-down.svg"] = chevron("down", 12, 8, size=3.4, stroke=1.4)
    return out


# ---------------------------------------------------------------- output


def png_bytes(im):
    buf = io.BytesIO()
    im.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


def all_outputs():
    files = {ICO_PATH: make_ico()}
    for name, im in resources().items():
        files[os.path.join(RES_DIR, name)] = png_bytes(im)
    for name, im in theme_icons().items():
        files[os.path.join(THEME_DIR, name)] = png_bytes(im)
    for name, text in dark_svgs().items():
        files[os.path.join(DARK_DIR, name)] = text.encode("utf-8")
    return files


def same_pixels(a, b):
    try:
        ia, ib = Image.open(io.BytesIO(a)), Image.open(io.BytesIO(b))
        frames_a, frames_b = [], []
        for im, frames in ((ia, frames_a), (ib, frames_b)):
            if im.format == "ICO":
                for size in sorted(im.ico.sizes()):
                    frames.append(im.ico.getimage(size).convert("RGBA"))
            else:
                frames.append(im.convert("RGBA"))
        if len(frames_a) != len(frames_b):
            return False
        return all(x.size == y.size and ImageChops.difference(x, y).getbbox() is None
                   for x, y in zip(frames_a, frames_b))
    except Exception:
        return False


def main():
    check = "--check" in sys.argv[1:]
    files = all_outputs()
    bad = 0
    for path, data in sorted(files.items()):
        rel = os.path.relpath(path, WII)
        if check:
            try:
                with open(path, "rb") as f:
                    have = f.read()
            except OSError:
                print("missing:", rel)
                bad += 1
                continue
            ok = have == data if path.endswith(".svg") else (have == data or same_pixels(have, data))
            if not ok:
                print("differs:", rel)
                bad += 1
        else:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)
    if check:
        print("%d files checked, %d differ or are missing" % (len(files), bad))
        return 1 if bad else 0
    print("wrote %d files" % len(files))
    return 0


if __name__ == "__main__":
    sys.exit(main())
