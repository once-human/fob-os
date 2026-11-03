"""Fonts for fob -> onkar_font2.h    python3 tools/gen_font.py OUT.h    (system python, needs PIL)
Archivo 400/800 (SIL OFL, from the portfolio's own font files) + the hand-cut star mark drawn here.
Each glyph is cropped to its ink, 16 alpha levels (4 bit), stored as a nibble stream:
   1..14  one pixel of that alpha     0 n  n+1 transparent pixels (1..16)     15 n  n+1 opaque pixels (1..16)
Every 8th row starts at a checkpoint (u16 nibble offset, table in front of the stream) so the strip renderer can start mid-glyph.
Giant type is tracked tight (-4.5 % of the size), like the website."""
import math, os, sys
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
A400, A800 = os.path.join(HERE, "fonts", "archivo-400.woff"), os.path.join(HERE, "fonts", "archivo-800.woff")
ASCII = [chr(c) for c in range(32, 127)]
GAMMA = 0.85          # >1 thinner, <1 bolder edges (AMOLED + sRGB blending makes thin text look starved)
LOWER = set("abcdefghijklmnopqrstuvwxyz0123456789 .,:;%+-/()'?!&<>=`^")
FONTS = [  # name, file, px, chars, keep, tabular digits, tracking
    ("F_N", A800, 112, [chr(c) for c in range(37, 59)], set("%0123456789:"), True, -0.045),
    ("F_T", A800, 30, ASCII, LOWER, False, -0.02),
    ("F_B", A400, 17, ASCII, LOWER, False, 0),
    ("F_L", A400, 12, ASCII, LOWER, False, 0),
]


def encode(img):
    w, h = img.size
    px = img.load()
    rows = [[(int(round(((px[x, y] / 255.0) ** GAMMA) * 15)) if px[x, y] else 0) for x in range(w)] for y in range(h)]
    nib, cps = [], []
    for y, r in enumerate(rows):
        if y % 8 == 0:
            cps.append(len(nib))
        x = 0
        while x < w:
            v = r[x]
            if v in (0, 15):
                n = 1
                while x + n < w and r[x + n] == v and n < 16:
                    n += 1
                nib += [v, n - 1]
                x += n
            else:
                nib.append(v)
                x += 1
    if len(nib) % 2:
        nib.append(0)
    data = bytearray()
    for cp in cps:
        data += cp.to_bytes(2, "little")
    data += bytes((nib[i] | (nib[i + 1] << 4)) for i in range(0, len(nib), 2))
    return bytes(data)


def emit(name, glyphs, blob, first, count, line_h, asc, defs=""):
    ln = name.lower()
    return (f"static const gl_t {ln}_g[{len(glyphs)}] = {{" + ",".join(f"{{{o},{w},{h},{a},{x},{y}}}" for o, w, h, x, y, a in glyphs) + "};\n"
            f"static const uint8_t {ln}_d[{len(blob)}] = {{" + ",".join(str(v) for v in blob) + "};\n"
            f"static const font2_t {name} = {{{ln}_g, {ln}_d, {first}, {count}, {line_h}, {asc}}};\n" + defs), len(blob) + 12 * len(glyphs)


def build(name, path, px, chars, keep, tab, track):
    f = ImageFont.truetype(path, px)
    asc, desc = f.getmetrics()
    cell = max(round(f.getlength(c)) for c in "0123456789") if tab else 0
    glyphs, blob = [], bytearray()
    for ch in chars:
        src = {'`': '\u00b7', '^': '\u203a'}.get(ch, ch)
        l, t, r, b = f.getbbox(src, anchor="ls")
        adv = max(1, round(f.getlength(src) + track * px))
        if r <= l or b <= t or (keep is not None and ch not in keep):
            glyphs.append((len(blob), 0, 0, 0, 0, adv))
            continue
        w, h = r - l, b - t
        im = Image.new("L", (w, h), 0)
        ImageDraw.Draw(im).text((-l, -t), src, font=f, fill=255, anchor="ls")
        ox, oy = l, t + asc
        if tab and ch.isdigit():
            ox, adv = (cell - w) // 2, round(cell + track * px)
        off = len(blob)
        blob += encode(im)
        glyphs.append((off, w, h, ox, oy, adv))
    return emit(name, glyphs, blob, ord(chars[0]), len(chars), asc + desc, asc)


def star_img(size):
    """The hand-cut mark: five irregular points, an imperfect flat paper shape."""
    outer, inner, jit = [0.50, 0.43, 0.54, 0.40, 0.48], [0.21, 0.18, 0.22, 0.19, 0.20], [-3, 5, -4, 6, -2]
    c, pts = size / 2, []
    for i in range(5):
        a = math.radians(i * 72 - 90 + jit[i]); b = math.radians(i * 72 - 54 + jit[i] * 0.5)
        pts += [(c + math.cos(a) * outer[i] * size, c + math.sin(a) * outer[i] * size), (c + math.cos(b) * inner[i] * size, c + math.sin(b) * inner[i] * size)]
    big = Image.new("L", (size * 4, size * 4), 0)
    ImageDraw.Draw(big).polygon([(x * 4, y * 4) for x, y in pts], fill=255)
    im = big.resize((size, size), Image.LANCZOS)
    return im.crop(im.getbbox())


def star():
    glyphs, blob = [], bytearray()
    for size in (150, 40):
        im = star_img(size); w, h = im.size
        glyphs.append((len(blob), w, h, 0, 0, w)); blob += encode(im)
    return emit("F_X", glyphs, blob, 0, 2, 0, 0, "#define F_X_STAR 0\n#define F_X_STAR_S 1\n")



import subprocess

def word_bitmap(word, width, cap=118, track=-0.045):
    """A word set in Archivo 800, tracked tight, sized so its ink is `width` px wide (the book's fit-to-width giant type)."""
    lo, hi = 20, cap
    def ink(px):
        f = ImageFont.truetype(A800, px); x = 0.0; l0 = None; r1 = 0
        for ch in word:
            bl, bt, br, bb = f.getbbox(ch, anchor="ls")
            if l0 is None: l0 = x + bl
            r1 = x + br
            x += f.getlength(ch) + track * px
        return l0, r1
    for _ in range(14):
        mid = (lo + hi) // 2
        l0, r1 = ink(mid)
        if r1 - l0 > width: hi = mid - 1
        else: lo = mid + 1
    px = max(20, min(cap, hi))
    f = ImageFont.truetype(A800, px); l0, r1 = ink(px)
    asc, desc = f.getmetrics()
    im = Image.new("L", (int(r1 - l0) + 4, asc + desc + 4), 0)
    d = ImageDraw.Draw(im); x = -l0 + 2
    for ch in word:
        d.text((x, asc + 2), ch, font=f, fill=255, anchor="ls"); x += f.getlength(ch) + track * px
    box = im.getbbox(); im = im.crop(box)
    return im, px

def layers_font(name, images, defs=""):
    """images: list of (PIL L image, (x, y) offset) -> one glyph each; the offsets go in a side table."""
    glyphs, blob, offs = [], bytearray(), []
    for im, off in images:
        w, h = im.size; o = len(blob); blob += encode(im)
        glyphs.append((o, w, h, 0, 0, w)); offs.append(off)
    out, n = emit(name, glyphs, blob, 0, len(glyphs), 0, 0, defs)
    out += f"static const int16_t {name.lower()}_off[{len(offs)}][2] = {{" + ",".join(f"{{{x},{y}}}" for x, y in offs) + "};\n"
    return out, n

def grid_layers(grid, scale, tones):
    """grid: rows of chars -> {tone: (image, (x, y))} cropped to ink, pixels scaled."""
    res = {}
    for t in tones:
        cells = [(x, y) for y, row in enumerate(grid) for x, c in enumerate(row) if c == t]
        if not cells: continue
        x0 = min(c[0] for c in cells); x1 = max(c[0] for c in cells); y0 = min(c[1] for c in cells); y1 = max(c[1] for c in cells)
        im = Image.new("L", ((x1 - x0 + 1) * scale, (y1 - y0 + 1) * scale), 0)
        d = ImageDraw.Draw(im)
        for x, y in cells: d.rectangle([(x - x0) * scale, (y - y0) * scale, (x - x0 + 1) * scale - 1, (y - y0 + 1) * scale - 1], fill=255)
        res[t] = (im, (x0 * scale, y0 * scale))
    return res

# ---- the beluga (recreated from the reference pixel art; 2 tail frames) ----
WSP = [[1,0,0,'T'],[1,1,2,'W'],[2,0,0,'T'],[2,1,2,'C'],[2,3,4,'W'],[3,1,1,'T'],[3,2,3,'C'],[3,4,4,'W'],[3,7,7,'C'],[3,8,8,'W'],[3,9,9,'C'],[4,1,2,'T'],[4,3,3,'C'],[4,4,5,'W'],[4,6,9,'C'],[5,2,4,'T'],[5,5,5,'W'],[5,6,7,'C'],[5,8,8,'T'],[6,3,5,'C'],[6,6,7,'S'],[7,3,5,'C'],[8,2,3,'T'],[8,4,4,'C'],[8,5,5,'W'],[9,2,4,'T'],[9,5,7,'W'],[10,3,4,'T'],[10,5,6,'C'],[10,7,9,'W'],
 [8,21,24,'W'],[9,18,20,'W'],[9,21,24,'C'],[9,25,26,'W'],[10,18,20,'W'],[10,21,25,'C'],[10,26,27,'W'],
 [11,4,5,'T'],[11,6,9,'C'],[11,10,12,'W'],[11,13,14,'T'],[11,15,17,'W'],[11,18,26,'C'],[11,27,27,'W'],[11,22,22,'o'],
 [12,5,5,'T'],[12,6,12,'C'],[12,13,13,'T'],[12,14,26,'C'],[12,22,22,'e'],[12,24,27,'o'],[12,27,27,'W'],
 [13,5,5,'T'],[13,6,19,'C'],[13,20,20,'T'],[13,21,23,'C'],[13,24,26,'k'],
 [14,7,8,'T'],[14,9,19,'C'],[14,20,20,'S'],[14,21,26,'C'],
 [15,7,7,'D'],[15,8,11,'T'],[15,12,19,'S'],[15,20,20,'T'],[15,21,23,'C'],[15,24,24,'S'],
 [16,8,10,'D'],[16,11,11,'T'],[16,12,16,'S'],[16,17,18,'W'],[16,19,19,'S'],[16,20,21,'C'],[16,22,23,'D'],
 [17,10,12,'D'],[17,13,14,'S'],[17,15,15,'D'],[17,16,16,'W'],[17,17,17,'C'],[17,18,18,'W'],[17,19,20,'S'],[17,21,22,'D'],
 [18,12,13,'D'],[18,14,15,'W'],[18,16,17,'C'],[18,18,20,'D'],[18,21,21,'C'],[18,22,22,'W'],
 [19,13,14,'W'],[19,15,16,'C'],[19,20,20,'S'],[19,21,23,'C'],
 [20,13,15,'C'],[20,21,21,'D'],[20,22,23,'C']]
WTONES = ['W', 'C', 'T', 'S', 'D', 'o', 'e']        # layer order; 'e' (black eye, mouth line) is drawn last
def whale_grid(flick):
    g = [['.'] * 30 for _ in range(24)]
    for r, a, b, ch in WSP:
        for c in range(a, b + 1):
            rr = r + (flick if (c <= 9 and r <= 7) else 0)
            g[rr][c] = 'e' if ch == 'k' else ch
    return g

# ---- the pet (26 x 24): body layers + per-state face layers ----
PW, PH = 26, 24
def pet_grid(state, tick=1, face=True):
    g = [['.'] * PW for _ in range(PH)]
    def put(x, y, c):
        x = round(x); y = round(y)
        if 0 <= x < PW and 0 <= y < PH: g[y][x] = c
    def ell(cx, cy, rx, ry, f):
        for y in range(PH):
            for x in range(PW):
                dx, dy = (x + .5 - cx) / rx, (y + .5 - cy) / ry; d = dx * dx + dy * dy
                if d <= 1: f(x, y, d, dx, dy)
    def tri(a, b, c, ch):
        (x0, y0), (x1, y1), (x2, y2) = a, b, c; dd = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
        for y in range(PH):
            for x in range(PW):
                px, py = x + .5, y + .5
                l1 = ((y1 - y2) * (px - x2) + (x2 - x1) * (py - y2)) / dd; l2 = ((y2 - y0) * (px - x2) + (x0 - x2) * (py - y2)) / dd
                if l1 >= 0 and l2 >= 0 and l1 + l2 <= 1: put(x, y, ch)
    ell(13, 20.5, 9.2, 4.6, lambda x, y, d, dx, dy: put(x, y, 'O' if d > .78 else ('S' if (dy > .4 or dx > .55) else 'B')))
    for m in (0, 1):
        X = (lambda x: 26 - x) if m else (lambda x: x)
        tri([X(3), 6.5], [X(4.6), 0], [X(10), 3.6], 'O'); tri([X(4.4), 5.4], [X(5.2), 1.8], [X(8.6), 3.8], 'B'); tri([X(5.2), 4.8], [X(5.6), 2.7], [X(7.4), 3.9], 'P')
    ell(13, 11.3, 11.3, 8.1, lambda x, y, d, dx, dy: put(x, y, 'O' if d > .8 else ('S' if (dy > .62 or dx > .74) else 'B')))
    for x in range(7, 20):
        put(x, 19, 'A')
        if 8 < x < 18: put(x, 20, 'A')
    put(17, 21, 'A'); put(18, 22, 'A'); put(17, 22, 'A')
    for y in (13, 15):
        for x in (0, 1, 24, 25): put(x, y, 'O')
    if not face: return g
    eyes = {'coding': 'down', 'claude': 'down', 'music': 'happy', 'notify': 'wide', 'sleep': 'sleep', 'idle': 'open', 'blink': 'blink'}[state]
    def eye(ex, k):
        if k == 'open':
            for dy in range(4):
                for dx in range(4): put(ex + dx, 10 + dy, 'E')
            put(ex, 10, 'W'); put(ex + 1, 10, 'W'); put(ex, 11, 'W'); put(ex + 3, 13, 'W')
        if k == 'blink':
            for dx in range(4): put(ex + dx, 12, 'E')
        if k == 'happy': put(ex, 12, 'E'); put(ex + 1, 11, 'E'); put(ex + 2, 11, 'E'); put(ex + 3, 12, 'E')
        if k == 'sleep': put(ex, 11, 'E'); put(ex + 1, 12, 'E'); put(ex + 2, 12, 'E'); put(ex + 3, 11, 'E')
        if k == 'wide':
            for dy in range(5):
                for dx in range(4): put(ex + dx, 9 + dy, 'E')
            put(ex, 9, 'W'); put(ex + 1, 9, 'W'); put(ex, 10, 'W'); put(ex + 2, 11, 'W')
        if k == 'down':
            for dy in range(3):
                for dx in range(4): put(ex + dx, 11 + dy, 'E')
            put(ex, 11, 'W'); put(ex + 1, 11, 'W')
    eye(8, eyes); eye(17, eyes)
    if state in ('coding', 'claude'):
        for x in list(range(7, 13)) + list(range(16, 22)): put(x, 9, 'G'); put(x, 14, 'G')
        for y in range(9, 15):
            for x in (7, 12, 16, 21): put(x, y, 'G')
        for x in (13, 14, 15): put(x, 11, 'G')
        for x in (2, 3, 22, 23): put(x, 11, 'G')
    if state == 'notify':
        for dx in range(4): put(8 + dx, 7, 'D'); put(17 + dx, 7, 'D')
    put(12, 14, 'D'); put(13, 14, 'D')
    if state == 'music':
        for x in (11, 12, 13, 14): put(x, 16, 'D')
        put(12, 17, 'P'); put(13, 17, 'P')
        for x in (4, 5, 6): put(x, 14, 'P'); put(x, 15, 'P'); put(x + 16, 14, 'P'); put(x + 16, 15, 'P')
        for a in range(190, 351, 4):
            r = math.radians(a); put(13 + math.cos(r) * 11.9, 11.3 + math.sin(r) * 8.6, 'G'); put(13 + math.cos(r) * 11.9, 12.3 + math.sin(r) * 8.6, 'G')
        ell(1.6, 12, 1.8, 3.2, lambda x, y, *a: put(x, y, 'A')); ell(24.4, 12, 1.8, 3.2, lambda x, y, *a: put(x, y, 'A'))
    elif state == 'notify':
        for x, y in ((12, 16), (13, 16), (12, 17), (13, 17)): put(x, y, 'D')
        for x in (4, 5, 20, 21): put(x, 14, 'P')
    elif state == 'sleep':
        put(12, 16, 'D'); put(13, 16, 'D')
        for x in (4, 5, 20, 21): put(x, 14, 'P')
    else:
        put(11, 16, 'D'); put(12, 17, 'D'); put(13, 17, 'D'); put(14, 16, 'D')
        for x in (4, 5, 20, 21): put(x, 14, 'P')
    return g
PSTATES = ['idle', 'blink', 'coding', 'claude', 'music', 'notify', 'sleep']
PTONES = ['O', 'S', 'B', 'P', 'D', 'A', 'E', 'W', 'G']

WORDS = ["carry", "cards", "music", "inbox", "remote", "claude", "play", "tools", "settings", "onkar", "fob"]
def qr_matrix(text):
    out = subprocess.run(["qrencode", "-t", "ASCII", "-m", "0", "-l", "M", "-o", "-", text], capture_output=True, text=True, check=True).stdout.splitlines()
    return [[ch == "#" for ch in line[::2]] for line in out if line.strip() != ""]
QRS = ["https://onkaryaglewad.in", "https://github.com/once-human"]

hdr = ["/* generated by tools/gen_font.py: Archivo (SIL OFL) + the fob star */\n",
       "typedef struct { uint32_t off; uint16_t w, h, adv; int16_t ox; int16_t oy; } gl_t;\n",
       "typedef struct { const gl_t *g; const uint8_t *d; uint8_t first, count, line_h, asc; } font2_t;\n"]
total = 0
for spec in FONTS:
    s, n = build(*spec)
    hdr.append(s); total += n
    print(f"{spec[0]:4s} {spec[2]:4d}px {n:6d} bytes")
s, n = star(); hdr.append(s); total += n; print(f"F_X  star  {n:6d} bytes")
# words (fit-to-width bitmaps)
wim = []
for w in WORDS:
    im, px = word_bitmap(w, 272 if w != "fob" else 200)
    wim.append((im, (0, 0))); print(f"word {w:9s} {px:3d}px {im.size}")
sv, n = layers_font("F_V", wim, "".join(f"#define W_{w.upper()} {i}\n" for i, w in enumerate(WORDS))); hdr.append(sv); total += n; print("F_V words", n, "bytes")
# beluga: 2 frames x 7 layers
wl = []
for fl in (0, -1):
    lay = grid_layers(whale_grid(fl), 5, WTONES)
    for t in WTONES: wl.append(lay[t] if t in lay else (Image.new("L", (1, 1), 0), (0, 0)))
sw, n = layers_font("F_W", wl, "#define WH_LAYERS 7\n"); hdr.append(sw); total += n; print("F_W whale", n, "bytes")
# pet: body layers (state 'none'), then per-state face overlays
pl, pmap = [], {}
base = pet_grid('idle', face=False)
for t in PTONES:
    lay = grid_layers(base, 6, [t])
    pmap[("body", t)] = len(pl); pl.append(lay[t] if t in lay else (Image.new("L", (1, 1), 0), (0, 0)))
for st in PSTATES:
    g = pet_grid(st)
    diff = [[(g[y][x] if g[y][x] != base[y][x] else '.') for x in range(PW)] for y in range(PH)]
    for t in PTONES:
        lay = grid_layers(diff, 6, [t])
        if t in lay: pmap[(st, t)] = len(pl); pl.append(lay[t])
sp, n = layers_font("F_P", pl); hdr.append(sp); total += n; print("F_P pet", n, "bytes,", len(pl), "layers")
body = ",".join(str(pmap[("body", t)]) for t in PTONES)
hdr.append(f"#define PET_BODY_N {len(PTONES)}\nstatic const uint8_t PET_BODY[{len(PTONES)}] = {{{body}}};   /* layer index per tone, order: O S B P D A E W G */\n")
hdr.append("typedef struct { uint8_t n; uint8_t l[6]; uint8_t t[6]; } pet_face_t;   /* t = tone index into PET_TONE */\n")
hdr.append("static const char PET_TONE[] = \"" + "".join(PTONES) + "\";\n")
fl = []
for st in PSTATES:
    items = [(pmap[(st, t)], PTONES.index(t)) for t in PTONES if (st, t) in pmap]
    fl.append("{%d,{%s},{%s}}" % (len(items), ",".join(str(a) for a, b in items) + ("," if False else ""), ",".join(str(b) for a, b in items)))
hdr.append("static const pet_face_t PET_FACE[%d] = {%s};\n" % (len(PSTATES), ",".join(fl)))
hdr.append("enum { PS_IDLE, PS_BLINK, PS_CODING, PS_CLAUDE, PS_MUSIC, PS_NOTIFY, PS_SLEEP };\n")
# QR codes: raw 1-bit module matrices (drawn by a row hook, a few hundred bytes)
qd = []
for q in QRS:
    m = qr_matrix(q); n_ = len(m)
    rows = [sum((1 << x) for x, v in enumerate(r) if v) for r in m]
    qd.append((n_, rows)); print("qr", q, n_, "modules")
for i, (n_, rows) in enumerate(qd):
    hdr.append(f"#define QR{i}_N {n_}\nstatic const uint32_t QR{i}[{n_}] = {{" + ",".join(f"0x{r:X}u" for r in rows) + "};\n")
hdr.append("enum { FN, FT, FB, FL, FX, FV, FW, FP };     /* font ids, in FONT_TAB order */\n")
open(sys.argv[1], "w").write("".join(hdr))
print("total", total, "bytes ->", sys.argv[1])
