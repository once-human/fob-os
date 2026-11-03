/* ====================================================================================
 * ONKAR OS graphics (v16). Portable C: also compiled on the laptop by sim/sim.c.
 *
 * No frame buffer (368x448x2 = 330 KB, the chip has ~48 KB for the app). Each frame is a display list of
 * anti-aliased primitives; only the dirty rectangle is rendered, strip by strip, into two small buffers:
 * the platform sends one strip (its own window + memory write) while the next one is rendered.
 * Geometry is float (Cortex-M4F), coverage per pixel from exact distances -> smooth edges everywhere.
 * Text: 4-bit anti-aliased glyphs, nibble-RLE (tools/gen_font2.py).
 *
 * Platform must provide:  gfx_out(x0, y0, w, h, pix) - send w x h big-endian RGB565 pixels (async ok),
 *                         gfx_out_wait() - wait until the previous gfx_out is done.
 * ==================================================================================== */
#include <math.h>
#include "onkar_font2.h"

#define LCD_W       368
#define LCD_H       448
#define STRIP_PIX   4416                 /* 368 x 12 */
#define DL_MAX      128
#define POOL_MAX    1024

#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

enum { FS, FM, FL, FD, FH, FI, FIS };
static const font2_t *const FONT_TAB[] = {&F_S, &F_M, &F_L, &F_D, &F_H, &F_I, &F_IS};

enum { G_RRECT, G_RING, G_ARC, G_LINE, G_TEXT, G_ICON, G_HOOK };

typedef struct { int16_t x0, y0, x1, y1; } grect_t;        /* x1, y1 exclusive */
typedef void (*ghook_t)(uint16_t *row, int y, int x0, int x1, int bx);   /* row[x - bx] */

typedef struct
{
    uint8_t t, a, f;            /* type, alpha 0..255, font */
    uint16_t c;
    float x, y, w, h, r, r2;    /* rrect: x y w h r | ring/arc: cx cy - - R Rin | line: x0 y0 x1 y1 halfwidth */
    float n0x, n0y, n1x, n1y;   /* arc half-plane normals; n0x > 9 = full ring */
    uint16_t s;                 /* text: pool offset | icon: glyph */
    ghook_t hook;
    grect_t bb;                 /* pixels touched, already clipped */
} gprim_t;

static gprim_t g_dl[DL_MAX];
static uint8_t g_n;
static char g_pool[POOL_MAX];
static uint16_t g_pool_n;
static uint16_t g_bg;
static grect_t g_dirty;
static grect_t g_clip = {0, 0, LCD_W, LCD_H};
static int g_ox, g_oy;          /* origin offset for everything added */
static uint8_t g_alpha = 255;   /* global alpha multiplier */
static uint16_t gfx_buf[2][STRIP_PIX] __attribute__((aligned(4)));   /* DMA reads words */
static bool g_full_rows;        /* send whole rows (platform option) */
static uint32_t g_stat_frames, g_stat_pix;

/* ------------------------------------------------------------------ helpers */
static inline int g_min(int a, int b) { return a < b ? a : b; }
static inline int g_max(int a, int b) { return a > b ? a : b; }
static inline float g_clampf(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
static inline float g_fmin(float a, float b) { return a < b ? a : b; }
static inline float g_fmax(float a, float b) { return a > b ? a : b; }
static inline float g_floor(float x) { int i = (int)x; return (float)(i - (x < (float)i)); }
static inline float g_ceil(float x) { int i = (int)x; return (float)(i + (x > (float)i)); }
#if defined(__ARM_FP)
static inline float g_sqrt(float x) { float r; __asm__("vsqrt.f32 %0, %1" : "=t"(r) : "t"(x)); return r; }   /* 14 cycles, no libm call */
#define GFX_HOT __attribute__((section(".app.data_ram.text"), noinline))   /* inner loops run from RAM, not flash */
#else
static inline float g_sqrt(float x) { return sqrtf(x); }
#define GFX_HOT
#endif

static inline uint16_t g_blend(uint16_t d, uint16_t s, uint32_t a5)      /* a5 0..32 */
{
    uint32_t D = (d | ((uint32_t)d << 16)) & 0x07E0F81Fu;
    uint32_t S = (s | ((uint32_t)s << 16)) & 0x07E0F81Fu;
    uint32_t R = ((((S - D) * a5) >> 5) + D) & 0x07E0F81Fu;
    return (uint16_t)(R | (R >> 16));
}

static uint16_t g_mix(uint16_t a, uint16_t b, float t)                    /* colour a -> b */
{
    int k = (int)(t * 32.f + 0.5f);
    return g_blend(a, b, (uint32_t)(k < 0 ? 0 : (k > 32 ? 32 : k)));
}

static grect_t g_isect(grect_t a, grect_t b)
{
    grect_t r = {(int16_t)g_max(a.x0, b.x0), (int16_t)g_max(a.y0, b.y0), (int16_t)g_min(a.x1, b.x1), (int16_t)g_min(a.y1, b.y1)};
    return r;
}
static bool g_empty(grect_t r) { return r.x1 <= r.x0 || r.y1 <= r.y0; }

/* ------------------------------------------------------------------ frame / dirty area */
static void gfx_begin(uint16_t bg)
{
    g_n = 0; g_pool_n = 0; g_bg = bg;
    g_ox = g_oy = 0; g_alpha = 255;
    g_clip.x0 = 0; g_clip.y0 = 0; g_clip.x1 = LCD_W; g_clip.y1 = LCD_H;
}

static void gfx_invalidate(int x0, int y0, int x1, int y1)
{
    x0 = g_max(x0, 0); y0 = g_max(y0, 0); x1 = g_min(x1, LCD_W); y1 = g_min(y1, LCD_H);
    if (x1 <= x0 || y1 <= y0) { return; }
    if (g_empty(g_dirty)) { g_dirty.x0 = x0; g_dirty.y0 = y0; g_dirty.x1 = x1; g_dirty.y1 = y1; return; }
    g_dirty.x0 = g_min(g_dirty.x0, x0); g_dirty.y0 = g_min(g_dirty.y0, y0);
    g_dirty.x1 = g_max(g_dirty.x1, x1); g_dirty.y1 = g_max(g_dirty.y1, y1);
}
static void gfx_invalidate_all(void) { gfx_invalidate(0, 0, LCD_W, LCD_H); }
static void g_set_clip(int x0, int y0, int x1, int y1)
{
    g_clip.x0 = (int16_t)g_max(x0 + g_ox, 0); g_clip.y0 = (int16_t)g_max(y0 + g_oy, 0);
    g_clip.x1 = (int16_t)g_min(x1 + g_ox, LCD_W); g_clip.y1 = (int16_t)g_min(y1 + g_oy, LCD_H);
}
static void g_reset_clip(void) { g_clip.x0 = 0; g_clip.y0 = 0; g_clip.x1 = LCD_W; g_clip.y1 = LCD_H; }

static gprim_t *g_add(uint8_t t, uint16_t c, uint8_t a, float bx0, float by0, float bx1, float by1)
{
    if (g_n >= DL_MAX) { return NULL; }
    uint8_t ga = (uint8_t)(((uint32_t)a * g_alpha + 127) / 255);
    if (!ga) { return NULL; }
    grect_t bb = {(int16_t)g_floor(bx0 + g_ox), (int16_t)g_floor(by0 + g_oy), (int16_t)g_ceil(bx1 + g_ox), (int16_t)g_ceil(by1 + g_oy)};
    bb = g_isect(bb, g_clip);
    if (g_empty(bb)) { return NULL; }
    gprim_t *p = &g_dl[g_n++];
    p->t = t; p->c = c; p->a = ga; p->bb = bb;
    return p;
}

/* ------------------------------------------------------------------ primitives (coordinates in px, float) */
static void g_rrect(float x, float y, float w, float h, float r, uint16_t c, uint8_t a)
{
    if (w <= 0 || h <= 0) { return; }
    if (r > w / 2) { r = w / 2; }
    if (r > h / 2) { r = h / 2; }
    gprim_t *p = g_add(G_RRECT, c, a, x - 1, y - 1, x + w + 1, y + h + 1);
    if (!p) { return; }
    p->x = x + g_ox; p->y = y + g_oy; p->w = w; p->h = h; p->r = r;
}
static void g_rect(float x, float y, float w, float h, uint16_t c, uint8_t a) { g_rrect(x, y, w, h, 0, c, a); }

static void g_ring(float cx, float cy, float R, float Rin, uint16_t c, uint8_t a)     /* Rin 0 = filled circle */
{
    if (R <= 0) { return; }
    gprim_t *p = g_add(G_RING, c, a, cx - R - 1, cy - R - 1, cx + R + 1, cy + R + 1);
    if (!p) { return; }
    p->x = cx + g_ox; p->y = cy + g_oy; p->r = R; p->r2 = Rin > 0 ? Rin : 0;
}
static void g_circle(float cx, float cy, float R, uint16_t c, uint8_t a) { g_ring(cx, cy, R, 0, c, a); }

/* arc of a ring, angles in degrees clockwise from 12 o'clock, a1 > a0; round = rounded ends */
static void g_arc(float cx, float cy, float R, float Rin, float a0, float a1, uint16_t c, uint8_t a, bool round)
{
    float sweep = a1 - a0;
    if (sweep <= 0.05f) { return; }
    if (sweep >= 360.f) { g_ring(cx, cy, R, Rin, c, a); return; }
    gprim_t *p = g_add(G_ARC, c, a, cx - R - 1, cy - R - 1, cx + R + 1, cy + R + 1);
    if (!p) { return; }
    float t0 = a0 * 0.0174533f, t1 = a1 * 0.0174533f;
    p->x = cx + g_ox; p->y = cy + g_oy; p->r = R; p->r2 = Rin;
    p->n0x = cosf(t0); p->n0y = sinf(t0);
    p->n1x = -cosf(t1); p->n1y = -sinf(t1);
    p->w = sweep > 180.f ? 1.f : 0.f;
    if (round)
    {
        float rm = (R + Rin) / 2, rc = (R - Rin) / 2;
        g_circle(cx + rm * sinf(t0), cy - rm * cosf(t0), rc, c, a);
        g_circle(cx + rm * sinf(t1), cy - rm * cosf(t1), rc, c, a);
    }
}

static void g_line(float x0, float y0, float x1, float y1, float hw, uint16_t c, uint8_t a)  /* capsule */
{
    gprim_t *p = g_add(G_LINE, c, a, g_fmin(x0, x1) - hw - 1, g_fmin(y0, y1) - hw - 1, g_fmax(x0, x1) + hw + 1, g_fmax(y0, y1) + hw + 1);
    if (!p) { return; }
    p->x = x0 + g_ox; p->y = y0 + g_oy; p->w = x1 + g_ox; p->h = y1 + g_oy; p->r = hw;
}

/* custom per-row painter (sketch strokes) over a box */
static void g_hook(int x0, int y0, int x1, int y1, ghook_t fn)
{
    gprim_t *p = g_add(G_HOOK, 0, 255, (float)x0, (float)y0, (float)x1, (float)y1);
    if (p) { p->hook = fn; }
}

/* ------------------------------------------------------------------ text */
static const gl_t *g_glyph(const font2_t *f, uint8_t ch)
{
    if (ch < f->first || ch >= f->first + f->count) { return NULL; }
    return &f->g[ch - f->first];
}

static int gfx_text_w(int fi, const char *s)
{
    const font2_t *f = FONT_TAB[fi];
    int w = 0;
    for (; *s; s++) { const gl_t *g = g_glyph(f, (uint8_t)*s); if (g) { w += g->adv; } }
    return w;
}

/* y = top of the line box (font ascent below it) */
static void g_text(int x, int y, int fi, uint16_t c, uint8_t a, const char *s)
{
    const font2_t *f = FONT_TAB[fi];
    int n = (int)strlen(s);
    if (!n || g_pool_n + n + 1 > POOL_MAX) { return; }
    int w = gfx_text_w(fi, s);
    gprim_t *p = g_add(G_TEXT, c, a, (float)x - 4, (float)y, (float)(x + w + 4), (float)(y + f->line_h + 4));
    if (!p) { return; }
    p->x = (float)(x + g_ox); p->y = (float)(y + g_oy); p->f = (uint8_t)fi; p->s = g_pool_n;
    memcpy(&g_pool[g_pool_n], s, n + 1);
    g_pool_n += n + 1;
}
static void g_text_c(int cx, int y, int fi, uint16_t c, uint8_t a, const char *s) { g_text(cx - gfx_text_w(fi, s) / 2, y, fi, c, a, s); }
static void g_text_r(int rx, int y, int fi, uint16_t c, uint8_t a, const char *s) { g_text(rx - gfx_text_w(fi, s), y, fi, c, a, s); }

static void g_icon(int x, int y, int fi, int idx, uint16_t c, uint8_t a)      /* x, y = top-left */
{
    const font2_t *f = FONT_TAB[fi];
    if (idx < 0 || idx >= f->count) { return; }
    const gl_t *g = &f->g[idx];
    gprim_t *p = g_add(G_ICON, c, a, (float)x, (float)y, (float)(x + g->w), (float)(y + g->h));
    if (!p) { return; }
    p->x = (float)(x + g_ox); p->y = (float)(y + g_oy); p->f = (uint8_t)fi; p->s = (uint16_t)idx;
}
static void g_icon_c(int cx, int cy, int fi, int idx, uint16_t c, uint8_t a)
{
    const gl_t *g = &FONT_TAB[fi]->g[idx];
    g_icon(cx - g->w / 2, cy - g->h / 2, fi, idx, c, a);
}

/* ------------------------------------------------------------------ rasteriser: one row of one primitive */
static inline uint32_t g_a5(float cov, uint32_t pa)       /* coverage * prim alpha -> 0..32 */
{
    return (uint32_t)(cov * (float)pa * (32.f / 255.f) + 0.5f);
}

static inline __attribute__((always_inline)) void g_put(uint16_t *px, uint16_t c, uint32_t a5)
{
    if (a5 >= 32) { *px = c; }
    else if (a5) { *px = g_blend(*px, c, a5); }
}

GFX_HOT static void g_span(uint16_t *row, int x0, int x1, uint16_t c, uint32_t a5)
{
    if (a5 >= 32) { for (int x = x0; x < x1; x++) { row[x] = c; } }
    else if (a5) { for (int x = x0; x < x1; x++) { row[x] = g_blend(row[x], c, a5); } }
}

/* rounded box signed distance -> coverage */
GFX_HOT static float g_rr_cov(const gprim_t *p, float px, float py)
{
    float hw = p->w * 0.5f, hh = p->h * 0.5f;
    float qx = fabsf(px - (p->x + hw)) - (hw - p->r), qy = fabsf(py - (p->y + hh)) - (hh - p->r);
    float mx = qx > 0 ? qx : 0, my = qy > 0 ? qy : 0;
    float d = g_sqrt(mx * mx + my * my) + g_fmin(g_fmax(qx, qy), 0.f) - p->r;
    return g_clampf(0.5f - d);
}

GFX_HOT static void g_row_rrect(const gprim_t *p, uint16_t *row, int y, int x0, int x1)
{
    float cy = y + 0.5f;
    float top = p->y, bot = p->y + p->h, r = p->r;
    float vy = g_clampf(g_fmin(cy + 0.5f, bot) - g_fmax(cy - 0.5f, top));
    if (vy <= 0) { return; }
    float dy = g_fmax(g_fmax(top + r - cy, cy - (bot - r)), 0.f);
    float k = r > 0 ? r - g_sqrt(g_fmax(r * r - dy * dy, 0.f)) : 0.f;    /* inset at this row */
    float l = p->x + k, rr = p->x + p->w - k;
    int a = g_max((int)g_floor(l) - 1, x0), b = g_min((int)g_ceil(rr) + 1, x1);
    int fa = (int)g_ceil(l + 1.f), fb = (int)g_floor(rr - 1.f);            /* fully covered horizontally */
    bool straight = dy <= 0.f;
    for (int x = a; x < b; x++)
    {
        if (x >= fa && x < fb)
        {
            int e = g_min(fb, b);
            uint32_t a5 = g_a5(straight ? vy : g_clampf(vy), p->a);
            if (!straight && dy > 0.5f) { break; }
            g_span(row, x, e, p->c, a5);
            x = e - 1;
            continue;
        }
        float cov;
        if (straight) { cov = g_clampf(g_fmin(x + 1.f, p->x + p->w) - g_fmax((float)x, p->x)) * vy; }
        else { cov = g_rr_cov(p, x + 0.5f, cy); }
        g_put(&row[x], p->c, g_a5(cov, p->a));
    }
    if (!straight && dy > 0.5f)          /* corner rows: exact everywhere inside [a, b) not yet done */
    {
        for (int x = g_max(a, fa); x < b; x++)
        {
            if (x < fb && dy < r - 1.f)                   /* inside: one span */
            {
                int e = g_min(fb, b);
                g_span(row, x, e, p->c, g_a5(1.f, p->a));
                x = e - 1;
                continue;
            }
            g_put(&row[x], p->c, g_a5(g_rr_cov(p, x + 0.5f, cy), p->a));
        }
    }
}

GFX_HOT static void g_row_ring(const gprim_t *p, uint16_t *row, int y, int x0, int x1, bool arc)
{
    float dy = y + 0.5f - p->y, R = p->r, Ri = p->r2, dy2 = dy * dy;
    float ro = R + 0.5f;
    if (dy2 >= ro * ro) { return; }
    float xo = g_sqrt(ro * ro - dy2);
    float rf = R - 0.5f, xf = dy2 < rf * rf ? g_sqrt(rf * rf - dy2) : -1.f;          /* fully inside outer edge */
    float hin = -1.f, hout = -1.f;
    if (Ri > 0)
    {
        float ra = Ri - 0.5f, rb = Ri + 0.5f;
        hin = dy2 < ra * ra ? g_sqrt(ra * ra - dy2) : -1.f;                          /* fully inside the hole */
        hout = dy2 < rb * rb ? g_sqrt(rb * rb - dy2) : 0.f;                          /* beyond: fully outside hole */
    }
    int a = g_max((int)g_floor(p->x - xo), x0), b = g_min((int)g_ceil(p->x + xo), x1);
    for (int x = a; x < b; x++)
    {
        float dx = x + 0.5f - p->x, adx = fabsf(dx);
        if (adx < hin)                                       /* jump over the hole */
        {
            int nx = (int)g_ceil(p->x + hin - 0.5f);
            if (nx > x) { x = nx - 1; }
            continue;
        }
        float cov;
        if (!arc && adx < xf && adx > hout)                  /* solid part of the row: one span */
        {
            float endc = (dx < 0 && hout > 0) ? p->x - hout : p->x + xf;
            int e = g_min((int)g_ceil(endc - 0.5f), b);
            if (e > x + 1)
            {
                g_span(row, x, e, p->c, g_a5(1.f, p->a));
                x = e - 1;
                continue;
            }
        }
        if (adx < xf && adx > hout) { cov = 1.f; }
        else
        {
            float d = g_sqrt(dx * dx + dy2);
            cov = g_clampf(R + 0.5f - d);
            if (Ri > 0) { cov *= g_clampf(d - Ri + 0.5f); }
        }
        if (arc && cov > 0)
        {
            float d0 = dx * p->n0x + dy * p->n0y, d1 = dx * p->n1x + dy * p->n1y;
            float s = p->w > 0.5f ? g_fmax(d0, d1) : g_fmin(d0, d1);
            cov *= g_clampf(s + 0.5f);
        }
        if (cov > 0) { g_put(&row[x], p->c, g_a5(cov, p->a)); }
    }
}

GFX_HOT static void g_row_line(const gprim_t *p, uint16_t *row, int y, int x0, int x1)
{
    float ax = p->x, ay = p->y, bx = p->w, by = p->h, hw = p->r;
    float vx = bx - ax, vy = by - ay, L2 = vx * vx + vy * vy;
    float cy = y + 0.5f;
    int a = x0, b = x1;
    if (L2 > 1e-3f)
    {
        float L = g_sqrt(L2), nx = -vy / L, ny = vx / L;                 /* x range of the band at this row */
        if (fabsf(nx) > 0.02f)
        {
            float c0 = nx * ax + ny * ay, e = hw + 1.5f;
            float xa = (c0 - e - ny * cy) / nx, xb = (c0 + e - ny * cy) / nx;
            if (xa > xb) { float t = xa; xa = xb; xb = t; }
            a = g_max(a, (int)g_floor(xa)); b = g_min(b, (int)g_ceil(xb) + 1);
        }
    }
    for (int x = a; x < b; x++)
    {
        float px = x + 0.5f - ax, py = cy - ay;
        float t = L2 > 1e-3f ? (px * vx + py * vy) / L2 : 0.f;
        t = t < 0 ? 0 : (t > 1 ? 1 : t);
        float dx = px - t * vx, dy = py - t * vy;
        float cov = g_clampf(hw + 0.5f - g_sqrt(dx * dx + dy * dy));
        if (cov > 0) { g_put(&row[x], p->c, g_a5(cov, p->a)); }
    }
}

/* glyph decoder: walks the nibble stream row by row (a strip decodes each glyph row exactly once) */
typedef struct { const uint8_t *st; uint32_t n; } gdec_t;

GFX_HOT static void gdec_row(gdec_t *d, int w, uint16_t *row, int gx, int x0, int x1, uint16_t c, const uint8_t *lut)
{
    const uint8_t *st = d->st;
    uint32_t n = d->n;
    int x = 0;
    while (x < w)
    {
        uint32_t v = (n & 1) ? (st[n >> 1] >> 4) : (st[n >> 1] & 15); n++;
        if (v == 0 || v == 15)
        {
            uint32_t k = (n & 1) ? (st[n >> 1] >> 4) : (st[n >> 1] & 15); n++;
            if (v == 15 && row)
            {
                int s0 = g_max(gx + x, x0), s1 = g_min(gx + x + (int)k + 1, x1);
                if (s0 < s1) { g_span(row, s0, s1, c, lut[15]); }
            }
            x += (int)k + 1;
        }
        else
        {
            int px = gx + x;
            if (row && px >= x0 && px < x1) { g_put(&row[px], c, lut[v]); }
            x++;
        }
    }
    d->n = n;
}

/* rows [ya, yb) of one glyph whose top-left is (gx, gy); row(y) = buf + (y - sy0) * bw - sx0 */
GFX_HOT static void g_strip_glyph(const font2_t *f, const gl_t *g, int gx, int gy, uint16_t *buf, int bw, int sx0, int sy0,
                                  int ya, int yb, int xa, int xb, uint16_t c, const uint8_t *lut)
{
    int r0 = g_max(ya - gy, 0), r1 = g_min(yb - gy, g->h);
    if (r0 >= r1 || !g->w || gx >= xb || gx + g->w <= xa) { return; }
    const uint8_t *base = f->d + g->off;
    gdec_t d;
    d.st = base + ((g->h + 7) >> 3) * 2;
    d.n = base[(r0 >> 3) * 2] | (base[(r0 >> 3) * 2 + 1] << 8);
    for (int k = r0 & 7; k; k--) { gdec_row(&d, g->w, NULL, 0, 0, 0, 0, lut); }
    for (int r = r0; r < r1; r++) { gdec_row(&d, g->w, buf + (gy + r - sy0) * bw - sx0, gx, xa, xb, c, lut); }
}

GFX_HOT static void g_strip_text(const gprim_t *p, uint16_t *buf, int bw, int sx0, int sy0, int ya, int yb, int xa, int xb)
{
    const font2_t *f = FONT_TAB[p->f];
    uint8_t lut[16];
    for (int i = 0; i < 16; i++) { lut[i] = (uint8_t)((i * (uint32_t)p->a * 32 + 15 * 255 / 2) / (15 * 255)); }
    int px = (int)p->x, py = (int)p->y;
    if (p->t == G_ICON)
    {
        g_strip_glyph(f, &f->g[p->s], px, py, buf, bw, sx0, sy0, ya, yb, xa, xb, p->c, lut);
        return;
    }
    for (const char *s = &g_pool[p->s]; *s && px < xb; s++)
    {
        const gl_t *g = g_glyph(f, (uint8_t)*s);
        if (!g) { continue; }
        g_strip_glyph(f, g, px + g->ox, py + g->oy, buf, bw, sx0, sy0, ya, yb, xa, xb, p->c, lut);
        px += g->adv;
    }
}

/* ------------------------------------------------------------------ frame output */
GFX_HOT static void gfx_render_strip(uint16_t *buf, int sx0, int sx1, int sy0, int sy1)
{
    int w = sx1 - sx0;
    uint32_t bg2 = g_bg | ((uint32_t)g_bg << 16), *wp = (uint32_t *)buf;
    for (int i = 0, n = (w * (sy1 - sy0)) >> 1; i < n; i++) { wp[i] = bg2; }
    for (int i = 0; i < g_n; i++)
    {
        const gprim_t *p = &g_dl[i];
        int ya = g_max(p->bb.y0, sy0), yb = g_min(p->bb.y1, sy1);
        int xa = g_max(p->bb.x0, sx0), xb = g_min(p->bb.x1, sx1);
        if (ya >= yb || xa >= xb) { continue; }
        if (p->t == G_TEXT || p->t == G_ICON) { g_strip_text(p, buf, w, sx0, sy0, ya, yb, xa, xb); continue; }
        for (int y = ya; y < yb; y++)
        {
            uint16_t *row = buf + (y - sy0) * w - sx0;        /* row[x] valid for x in [sx0, sx1) */
            switch (p->t)
            {
            case G_RRECT: g_row_rrect(p, row, y, xa, xb); break;
            case G_RING: g_row_ring(p, row, y, xa, xb, false); break;
            case G_ARC: g_row_ring(p, row, y, xa, xb, true); break;
            case G_LINE: g_row_line(p, row, y, xa, xb); break;
            case G_HOOK: p->hook(row + sx0, y, xa, xb, sx0); break;
            default: break;
            }
        }
    }
    for (int i = 0, n = (w * (sy1 - sy0)) >> 1; i < n; i++)    /* to the panel's byte order */
    {
        uint32_t v = wp[i];
        wp[i] = ((v & 0xFF00FF00u) >> 8) | ((v & 0x00FF00FFu) << 8);
    }
}

static void gfx_out(int x0, int y0, int w, int h, const uint16_t *pix);
static void gfx_out_wait(void);
static void gfx_frame_start(void);     /* platform: e.g. wait for the panel's tearing signal */

/* render + send the dirty area; returns pixels sent */
static uint32_t gfx_flush(void)
{
    grect_t d = g_dirty;
    g_dirty.x0 = g_dirty.y0 = g_dirty.x1 = g_dirty.y1 = 0;
    if (g_empty(d)) { return 0; }
    if (g_full_rows) { d.x0 = 0; d.x1 = LCD_W; }
    d.x0 &= ~1; d.y0 &= ~1;                       /* panel wants even window starts and sizes */
    if (d.x1 & 1) { d.x1++; }
    if (d.y1 & 1) { d.y1++; }
    int w = d.x1 - d.x0;
    int rows = (STRIP_PIX / w) & ~1;
    int k = 0;
    for (int y = d.y0; y < d.y1; y += rows, k ^= 1)
    {
        int y1 = g_min(y + rows, d.y1);
        gfx_render_strip(gfx_buf[k], d.x0, d.x1, y, y1);
        gfx_out_wait();
        if (y == d.y0) { gfx_frame_start(); }
        gfx_out(d.x0, y, w, y1 - y, gfx_buf[k]);
    }
    gfx_out_wait();
    g_stat_frames++;
    g_stat_pix = (uint32_t)w * (d.y1 - d.y0);
    return g_stat_pix;
}
