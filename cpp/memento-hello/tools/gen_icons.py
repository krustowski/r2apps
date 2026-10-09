#!/usr/bin/env python3
#
#  gen_icons.py --- the desktop's icons: the `icons` table in
#  windows/desktop_window.cpp.
#
#  Usage: tools/gen_icons.py [--preview icons.png]
#
#  Each icon is drawn here from shapes --- discs, polygons, strokes --- on the
#  30x30 grid the desktop paints at one cell to a device pixel, and its rows
#  are written over the table in desktop_window.cpp: '#' the light colour,
#  '+' the mid tone, '.' the tile.  A pixel takes a shape's colour when at
#  least half of it is covered (4x4 samples), so edges stay hard: there is no
#  in-between colour to blur them with.  Later shapes paint over earlier
#  ones, which is how the gaps and outlines are made.
#
#  To change an icon, change its function and run this.  To add one, add a
#  function where it belongs in launch order (openApp() takes the index) and
#  raise ICONS in desktop_window.cpp to match.  With --preview it also writes
#  a sheet of them all, eight times over and at their real size, for looking
#  at while drawing (needs Pillow).
#
import math
import os
import sys

N = 30   # the grid: ART in desktop_window.cpp
SS = 4   # sub-samples per pixel, each way

W, M, B = '#', '+', '.'   # light, mid tone, tile

SOURCE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'windows', 'desktop_window.cpp')


class Canvas:
    def __init__(self):
        self.p = [[B] * N for _ in range(N)]

    # A shape, as a test on a point in grid units: (0, 0) is the top left
    # corner of the top left pixel.
    def fill(self, inside, ch):
        for y in range(N):
            for x in range(N):
                n = 0
                for sy in range(SS):
                    for sx in range(SS):
                        if inside(x + (sx + .5) / SS, y + (sy + .5) / SS):
                            n += 1
                if n * 2 >= SS * SS:
                    self.p[y][x] = ch

    # Single pixels and rectangles, in whole pixels, ends included.
    def px(self, pts, ch):
        for x, y in pts:
            if 0 <= x < N and 0 <= y < N:
                self.p[y][x] = ch

    def rect(self, x0, y0, x1, y1, ch):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                self.p[y][x] = ch

    def hline(self, x0, x1, y, ch):
        self.rect(min(x0, x1), y, max(x0, x1), y, ch)

    def vline(self, x, y0, y1, ch):
        self.rect(x, min(y0, y1), x, max(y0, y1), ch)

    def rows(self):
        return [''.join(r) for r in self.p]


# --- shapes ---------------------------------------------------------------

def disc(cx, cy, r):
    return lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def ellipse(cx, cy, rx, ry, rot=0.0):
    c, s = math.cos(rot), math.sin(rot)

    def f(x, y):
        dx, dy = x - cx, y - cy
        u, v = dx * c + dy * s, -dx * s + dy * c
        return (u / rx) ** 2 + (v / ry) ** 2 <= 1
    return f


# The ring between radii r0 and r1, from angle a0 to a1: degrees, 0 east,
# counter-clockwise as on paper (y up).
def arc(cx, cy, r0, r1, a0, a1):
    def f(x, y):
        dx, dy = x - cx, cy - y
        d2 = dx * dx + dy * dy
        if not (r0 * r0 <= d2 <= r1 * r1):
            return False
        a = math.degrees(math.atan2(dy, dx)) % 360
        lo, hi = a0 % 360, a1 % 360
        return lo <= a <= hi if lo <= hi else (a >= lo or a <= hi)
    return f


def box(x0, y0, x1, y1):
    return lambda x, y: x0 <= x <= x1 and y0 <= y <= y1


def rbox(x0, y0, x1, y1, r):
    def f(x, y):
        if not (x0 <= x <= x1 and y0 <= y <= y1):
            return False
        cx = min(max(x, x0 + r), x1 - r)
        cy = min(max(y, y0 + r), y1 - r)
        return (x - cx) ** 2 + (y - cy) ** 2 <= r * r
    return f


# A stroke w wide from one point to another, with round ends.
def seg(x0, y0, x1, y1, w):
    def f(x, y):
        dx, dy = x1 - x0, y1 - y0
        L = dx * dx + dy * dy
        t = 0 if L == 0 else max(0, min(1, ((x - x0) * dx + (y - y0) * dy) / L))
        px, py = x0 + t * dx, y0 + t * dy
        return (x - px) ** 2 + (y - py) ** 2 <= (w / 2) ** 2
    return f


def polyline(pts, w):
    return union(*[seg(*pts[i], *pts[i + 1], w) for i in range(len(pts) - 1)])


def poly(pts):
    def f(x, y):
        inside = False
        n = len(pts)
        for i in range(n):
            xa, ya = pts[i]
            xb, yb = pts[(i + 1) % n]
            if (ya > y) != (yb > y):
                xi = xa + (y - ya) * (xb - xa) / (yb - ya)
                if x < xi:
                    inside = not inside
        return inside
    return f


def union(*fs):
    return lambda x, y: any(f(x, y) for f in fs)


def minus(a, b):
    return lambda x, y: a(x, y) and not b(x, y)


def inter(a, b):
    return lambda x, y: a(x, y) and b(x, y)


# --- the icons, in launch order -------------------------------------------

ICONS = []


# `note` becomes a comment over the icon's rows.
def icon(label, note=None):
    def deco(fn):
        ICONS.append((label, note, fn))
        return fn
    return deco


@icon("Clock")
def clock(c):
    c.fill(disc(15, 15, 13), W)              # bezel
    c.fill(disc(15, 15, 11.2), M)            # its inner edge, in shade
    c.fill(disc(15, 15, 10.2), W)            # face
    for a in range(0, 360, 30):              # hour marks, longer at the quarters
        r0, r1 = (7.4, 9.6) if a % 90 == 0 else (8.4, 9.6)
        ca, sa = math.cos(math.radians(a)), math.sin(math.radians(a))
        w = 2 if a % 90 == 0 else 1.2
        c.fill(seg(15 + ca * r0, 15 - sa * r0, 15 + ca * r1, 15 - sa * r1, w), B)
    c.fill(seg(15, 15, 10.0, 11.0, 2.2), B)  # hour hand, at ten
    c.fill(seg(15, 15, 20.6, 8.6, 1.5), B)   # minute hand, at two
    c.fill(disc(15, 15, 1.7), B)


@icon("Shell")
def shell(c):
    c.fill(rbox(1, 3, 29, 27, 2), W)         # window
    c.fill(box(2.5, 8, 27.5, 25.5), B)       # screen
    for x in (4, 7, 10):                     # title bar buttons
        c.rect(x, 5, x + 1, 6, B)
    c.fill(polyline([(5.5, 11.5), (10, 15.5), (5.5, 19.5)], 2.2), W)   # >
    c.rect(12, 18, 17, 19, W)                # _
    c.rect(19, 18, 20, 19, M)                # cursor
    c.rect(12, 12, 17, 12, M)                # earlier output
    c.rect(19, 12, 24, 12, M)
    c.rect(12, 15, 21, 15, M)
    c.rect(5, 22, 23, 22, M)


@icon("Net", "An Ethernet plug on its cable.")
def net(c):
    cable = [(25, -1), (24.5, 3), (21.5, 6.5), (17, 8.5), (15, 11)]
    c.fill(polyline(cable, 6.4), B)
    c.fill(polyline(cable, 4.4), M)
    c.fill(poly([(11, 10), (19, 10), (20, 14), (10, 14)]), W)   # boot
    c.fill(rbox(6, 14, 24, 28, 1.5), W)                          # body
    c.rect(11, 15, 19, 19, M)                                    # latch
    c.hline(11, 19, 20, B)
    for x in range(8, 24, 2):                                    # the eight contacts
        c.vline(x, 22, 27, B)


@icon("Mount", "A CD, and a diskette in front of it.")
def mount(c):
    cx, cy = 18.5, 11
    c.fill(disc(cx, cy, 10.6), W)
    c.fill(arc(cx, cy, 6.4, 7.4, 20, 80), M)        # sheen
    c.fill(arc(cx, cy, 6.4, 7.4, 200, 260), M)
    c.fill(disc(cx, cy, 3.6), M)                    # hub
    c.fill(disc(cx, cy, 1.9), B)                    # hole
    x0, y0, x1, y1 = 1, 14, 15, 28                  # the diskette, cut off at one corner
    c.fill(poly([(x0 - 1, y0 - 1), (x1 - 1.5, y0 - 1), (x1 + 2, y0 + 2.5), (x1 + 2, y1 + 2), (x0 - 1, y1 + 2)]), B)
    c.fill(poly([(x0, y0), (x1 - 1.5, y0), (x1 + 1, y0 + 2.5), (x1 + 1, y1 + 1), (x0, y1 + 1)]), M)
    c.rect(x0 + 3, y0, x0 + 10, y0 + 4, W)          # shutter
    c.rect(x0 + 7, y0 + 1, x0 + 8, y0 + 3, B)       # its window
    c.rect(x0 + 2, y0 + 8, x1 - 1, y1, W)           # label
    c.hline(x0 + 4, x1 - 3, y0 + 10, M)
    c.hline(x0 + 4, x1 - 3, y0 + 12, M)
    c.rect(x0 + 1, y1 - 1, x0 + 1, y1, B)           # write-protect hole


@icon("Tasks")
def tasks(c):
    c.fill(rbox(1, 3, 29, 27, 2), W)
    c.fill(box(3, 5, 27, 25), B)
    pts = [(3, 19.5), (6.5, 17.5), (9.5, 20), (13, 11.5), (16, 18.5), (19, 14.5),
           (21.5, 16.5), (24.5, 8.5), (27, 12)]

    def under(x, y):
        for (xa, ya), (xb, yb) in zip(pts, pts[1:]):
            if xa <= x <= xb:
                return y >= ya + (yb - ya) * (x - xa) / (xb - xa)
        return False
    c.fill(inter(box(3, 5, 27, 25), under), M)
    for y in (10, 15, 20):                  # grid, through the area only
        for x in range(4, 27, 3):
            if c.p[y][x] == M:
                c.p[y][x] = B
    c.fill(polyline(pts, 1.7), W)


@icon("Chat")
def chat(c):
    c.fill(rbox(11, 2, 29, 17, 4.5), M)     # the answer, behind
    c.fill(union(rbox(-0.2, 6.8, 23.2, 25.2, 6.5), poly([(4, 21), (1.8, 30), (12.5, 22)])), B)
    c.fill(union(rbox(1, 8, 22, 24, 5.5), poly([(5, 22), (3, 28.5), (11, 23)])), W)
    for x in (6, 11, 16):
        c.fill(disc(x + .5, 16, 1.6), B)


@icon("Calc")
def calc(c):
    c.fill(rbox(4, 1, 26, 29, 2.5), W)
    c.fill(box(7, 4, 23, 10), B)            # display, showing 10
    c.px([(16, 6), (15, 7), (16, 7), (16, 8), (16, 9)], W)
    c.px([(19, 6), (20, 6), (21, 6), (19, 7), (21, 7), (19, 8), (21, 8), (19, 9), (20, 9), (21, 9)], W)
    for row in range(4):
        for col in range(4):
            x = 7 + col * 4 + (col > 2)
            y = 13 + row * 4
            if col == 3 and row == 3:
                continue
            ch = M if col == 3 else B
            if col == 3 and row == 2:
                c.rect(x, y, x + 2, y + 6, ch)   # tall =
            else:
                c.rect(x, y, x + 2, y + 2, ch)


@icon("IRC")
def irc(c):
    sl = 0.22                               # italic slant

    def v(x):
        return seg(x + 11.5 * sl, 4, x - 11.5 * sl, 27, 3.2)
    c.fill(union(v(12.5), v(20.5),
                 seg(4.5, 11, 26.5, 11, 3.0),
                 seg(3.5, 20, 25.5, 20, 3.0)), W)


@icon("Music")
def music(c):
    c.fill(poly([(10.6, 5), (27.1, 1.5), (27.1, 6.5), (10.6, 10)]), W)  # beam
    c.fill(box(10.6, 6, 12.6, 23), W)
    c.fill(box(25.1, 3, 27.1, 20), W)
    c.fill(ellipse(8.2, 23.4, 4.5, 3.3, math.radians(25)), W)
    c.fill(ellipse(22.7, 20.4, 4.5, 3.3, math.radians(25)), W)


# Coastlines as (longitude, latitude), roughly: detail under a pixel is
# wasted.  Only what faces the Web icon's view needs to be here.
AFRICA = [(-6, 36), (10, 37), (11, 33), (20, 31), (25, 32), (32, 31), (34, 28), (38, 20), (43, 12),
          (51, 11), (48, 5), (42, -1), (40, -10), (40, -16), (35, -22), (33, -27), (28, -33),
          (20, -35), (18, -32), (15, -27), (12, -17), (13, -10), (9, -1), (9, 4), (5, 5), (-4, 5),
          (-8, 4), (-13, 8), (-17, 14), (-17, 21), (-10, 29)]
EUROPE = [(-9, 43), (-9, 37), (-6, 36), (-2, 37), (3, 42), (5, 43), (9, 44), (12, 42), (16, 39),
          (18, 40), (14, 45), (19, 42), (23, 37), (26, 40), (29, 41), (36, 41), (41, 41), (42, 47),
          (40, 55), (30, 60), (26, 65), (30, 70), (20, 70), (12, 65), (5, 60), (8, 57), (10, 54),
          (8, 54), (5, 52), (2, 51), (-2, 48), (-4, 48), (-1, 46)]
BRITAIN = [(-5, 50), (1, 51), (0, 53), (-2, 56), (-5, 58), (-6, 56), (-3, 54)]
ARABIA = [(34, 28), (35, 33), (36, 36), (44, 37), (48, 30), (50, 26), (56, 26), (57, 23), (59, 22),
          (55, 17), (52, 16), (45, 13), (43, 13), (38, 20)]
ASIA = [(42, 47), (50, 46), (53, 37), (57, 26), (62, 25), (70, 22), (73, 18), (77, 8), (80, 14),
        (88, 22), (90, 50), (60, 70), (40, 68), (40, 55)]
MADAGASCAR = [(44, -25), (47, -25), (50, -16), (49, -12), (44, -17)]
S_AMERICA = [(-35, -7), (-39, -14), (-41, -22), (-48, -26), (-53, -33), (-58, -38), (-65, -42),
             (-68, -50), (-75, -50), (-73, -40), (-70, -18), (-81, -6), (-77, 7), (-72, 11),
             (-62, 10), (-52, 5), (-50, 0), (-44, -2)]
ICELAND = [(-17, 66), (-24, 66), (-20, 63)]
LANDS = [AFRICA, EUROPE, BRITAIN, ARABIA, ASIA, MADAGASCAR, S_AMERICA, ICELAND]


# Orthographic projection: the globe seen from far away over (lon0, lat0).
def ortho(lon0, lat0, cx, cy, R):
    l0, p0 = math.radians(lon0), math.radians(lat0)

    def f(lon, lat):
        l, p = math.radians(lon), math.radians(lat)
        x = math.cos(p) * math.sin(l - l0)
        y = math.cos(p0) * math.sin(p) - math.sin(p0) * math.cos(p) * math.cos(l - l0)
        return cx + R * x, cy - R * y
    return f


@icon("Web", "The Earth over Africa, the night side at the lower right.")
def web(c):
    R, cx, cy = 13.3, 15, 15
    proj = ortho(18, 12, cx, cy, R)
    c.fill(disc(cx, cy, R), M)                       # sea
    land = union(*[poly([proj(lo, la) for lo, la in shape]) for shape in LANDS])
    c.fill(inter(disc(cx, cy, R - 0.6), land), W)
    c.fill(minus(disc(cx, cy, R), disc(cx - 2.2, cy - 2.2, R)), B)


@icon("Editor")
def editor(c):
    c.fill(poly([(3, 1), (17, 1), (24, 8), (24, 29), (3, 29)]), W)   # page
    c.fill(poly([(17, 1), (17, 8), (24, 8)]), M)                     # dog-ear
    c.hline(17, 24, 8, B)
    c.vline(17, 1, 8, B)
    c.fill(seg(17, 1, 24, 8, 1.1), B)
    for y, x1 in ((6, 13), (10, 20), (14, 20), (18, 15), (22, 11)):
        c.hline(6, x1, y, B)
    # A pencil at 45 degrees, laid out in whole pixels rather than shapes
    # (stripes one pixel wide come out as a chequer at this angle): t = x - y
    # runs along it towards the upper right, u = x + y across it, centred on
    # u = 40.
    for y in range(N):
        for x in range(N):
            t, u = x - y, x + y
            a = abs(u - 40)
            if t < -16 or t > 15 or a > 4:
                continue
            if t <= -13:                      # lead
                ch = B if a <= (t + 16) // 2 + 1 else None
            elif t <= -8:                     # sharpened wood, widening
                half = (t + 15) // 2
                ch = W if a < half else (B if a <= half else None)
            elif t >= 12:                     # eraser
                ch = W if a <= 2 else B
            elif t >= 10:                     # ferrule
                ch = B if a <= 3 else None
            else:                             # painted body
                ch = B if a == 3 else M
            if ch:
                c.p[y][x] = ch


@icon("Snake")
def snake(c):
    pts = []
    for i in range(0, 81):
        t = i / 80
        pts.append((2.5 + t * 16.5, 25.5 - t * 12.5 - 5.0 * math.sin(t * 2.2 * math.pi)))
    for i in range(len(pts) - 1):             # thin at the tail
        w = 1.3 + 3.2 * min(1, i / 30)
        c.fill(seg(*pts[i], *pts[i + 1], w), W)
    hx, hy = pts[-1]
    c.fill(ellipse(hx + 2.6, hy - 1.6, 4.6, 3.3, math.radians(-25)), W)
    c.fill(disc(hx + 3.4, hy - 3.1, 0.95), B)                     # eye
    tx, ty = hx + 7.0, hy - 3.8                                   # tongue
    c.fill(seg(tx, ty, tx + 2.2, ty - 1.1, 1.0), M)
    c.px([(int(tx + 2.6), int(ty - 2.3)), (int(tx + 2.9), int(ty - 0.7))], M)


@icon("Mines")
def mines(c):
    c.fill(rbox(1, 1, 29, 29, 2), W)        # an uncovered square
    cx, cy = 15, 15
    for a in range(0, 360, 45):
        ca, sa = math.cos(math.radians(a)), math.sin(math.radians(a))
        ln = 11.5 if a % 90 == 0 else 9.6
        c.fill(seg(cx, cy, cx + ca * ln, cy - sa * ln, 2.2), B)
    c.fill(disc(cx, cy, 7.4), B)
    c.fill(rbox(10.5, 10.5, 13.6, 13.6, 0.8), W)   # glint


@icon("Telegram")
def telegram(c):
    nose, tail, fold, low, keel = (28, 2.5), (1.5, 13.5), (12, 18.5), (12, 27), (21.5, 26.5)
    c.fill(poly([nose, fold, low, (15, 21.5)]), M)       # far wing, in shade
    c.fill(poly([nose, tail, fold]), W)                  # near wing
    c.fill(poly([nose, (15.5, 20.5), keel]), W)          # keel
    c.fill(seg(*nose, *fold, 1.0), B)
    c.fill(seg(*nose, 15.5, 20.5, 1.0), B)


@icon("Video")
def video(c):
    c.fill(rbox(3, 12, 27, 28, 1.2), W)                  # board
    c.fill(box(5, 14, 25, 26), B)
    c.fill(poly([(12, 16.5), (12, 24.5), (19.5, 20.5)]), W)   # play
    c.fill(box(3, 8, 27, 11), W)                         # the clapper: its lower bar
    c.fill(inter(box(3, 8, 27, 11), lambda x, y: (int(x) + int(y)) % 6 < 3), B)
    c.fill(box(3, 2, 27, 6), W)                          # and the stick, lifted
    c.fill(inter(box(3, 2, 27, 6), lambda x, y: (int(x) - int(y)) % 6 < 3), B)
    c.rect(3, 2, 3, 11, W)                               # hinge post


@icon("Spotify")
def spotify(c):
    c.fill(disc(15, 15, 13.5), W)
    # Three arcs, each the top of a circle: where it peaks, half its width,
    # how far its ends drop, and how thick it is.
    for peak, half, sag, w in ((9.4, 9.2, 3.6, 3.2), (14.8, 7.6, 2.9, 2.7), (19.9, 5.9, 2.2, 2.2)):
        R = (half * half + sag * sag) / (2 * sag)
        pts = []
        for i in range(13):
            x = 15 - half + i * 2 * half / 12
            pts.append((x, peak + R - math.sqrt(R * R - (x - 15) ** 2)))
        c.fill(polyline(pts, w), B)


@icon("Jug", "Water pitcher: pouring lip, handle, water and a download arrow.")
def jug(c):
    c.fill(arc(21.5, 15, 4.2, 6.8, -90, 90), W)          # handle
    c.fill(poly([(2.5, 2), (21, 2), (19.5, 6), (21.5, 13), (21.5, 26), (19, 28.5),
                 (6, 28.5), (3.5, 26), (3.5, 13), (5.5, 6), (3.5, 4)]), W)
    inner = poly([(6.5, 4), (17.5, 4), (16.6, 6.3), (19.2, 13.3), (19.2, 25.3), (18, 26.5),
                  (7, 26.5), (5.8, 25.3), (5.8, 13.3), (8.4, 6.3)])
    c.fill(inner, B)
    c.fill(inter(inner, lambda x, y: y > 13.5 + 0.8 * math.sin((x - 5) * 0.9)), M)   # water
    c.fill(box(11.3, 9, 13.7, 18.5), W)                  # download arrow
    c.fill(poly([(7.5, 18), (17.5, 18), (12.5, 23.5)]), W)


# --- output ---------------------------------------------------------------

def draw():
    out = []
    for label, note, fn in ICONS:
        c = Canvas()
        fn(c)
        out.append((label, note, c.rows()))
    return out


def table(icons):
    lines = []
    for label, note, rows in icons:
        if note:
            lines.append('        // ' + note)
        lines.append('        {"%s", {' % label)
        lines += ['            "%s",' % r for r in rows]
        lines.append('        }},')
    return '\n'.join(lines) + '\n'


def write_source(icons):
    src = open(SOURCE).read()
    head = '    static constexpr Icon icons[ICONS] = {\n'
    start = src.find(head)
    if start < 0:
        sys.exit('gen_icons: no icon table in ' + SOURCE)
    start += len(head)
    end = src.find('\n    };\n', start)
    if end < 0:
        sys.exit('gen_icons: the icon table in %s does not end' % SOURCE)
    for name, want in (('ICONS', len(icons)), ('ART', N)):
        if 'static const int %s = %d;' % (name, want) not in src:
            sys.exit('gen_icons: %s in %s is not %d' % (name, SOURCE, want))
    new = src[:start] + table(icons) + src[end + 1:]
    if new != src:
        with open(SOURCE, 'w') as f:
            f.write(new)
        print('gen_icons: wrote %d icons to %s' % (len(icons), os.path.normpath(SOURCE)))
    else:
        print('gen_icons: %s is up to date' % os.path.normpath(SOURCE))


# The colours as the 16-colour VGA shows them: 0xFFE0E0FF comes out white.
COLOURS = {B: (0, 0, 170), W: (255, 255, 255), M: (85, 85, 255)}


def preview(icons, path):
    from PIL import Image, ImageDraw
    Z, G, cols = 8, 16, 6
    nrows = (len(icons) + cols - 1) // cols
    big_h = nrows * (N * Z + G + 14) + G
    img = Image.new('RGB', (cols * (N * Z + G) + G, big_h + nrows * (N + 18) * 3 + G), (255, 255, 255))
    d = ImageDraw.Draw(img)
    for i, (label, _, rows) in enumerate(icons):
        bx = G + (i % cols) * (N * Z + G)
        by = G + (i // cols) * (N * Z + G + 14)
        sx = G + (i % cols) * (N + 18) * 3
        sy = big_h + (i // cols) * (N + 18) * 3
        for y, r in enumerate(rows):
            for x, ch in enumerate(r):
                d.rectangle([bx + x * Z, by + y * Z, bx + x * Z + Z - 1, by + y * Z + Z - 1], COLOURS[ch])
                d.rectangle([sx + x * 3, sy + y * 3, sx + x * 3 + 2, sy + y * 3 + 2], COLOURS[ch])
        d.text((bx, by + N * Z + 1), label, fill=(0, 0, 0))
    img.save(path)
    print('gen_icons: preview in ' + path)


def main():
    args = sys.argv[1:]
    out = None
    if args[:1] == ['--preview'] and len(args) == 2:
        out = args[1]
    elif args:
        sys.exit('usage: tools/gen_icons.py [--preview icons.png]')
    icons = draw()
    write_source(icons)
    if out:
        preview(icons, out)


if __name__ == '__main__':
    main()
