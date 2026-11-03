/* ====================================================================================
 * fob shell, v0.2. Portable C on top of onkar_gfx.inc (also runs in sim/sim.c).
 *
 * The crown and the touch panel do the same jobs; use either.
 *   rotate = move (scenes in carry, modes on the dial, pages inside a mode)
 *   press  = act (open the dial, enter a mode, the mode's main action)
 *   press twice = back (mode -> dial -> carry)        hold = screen off
 *   touch: swipe left/right = pages and scenes, swipe up on carry = dial, drag the dial, tap = the main action
 * carry is the resting screen and the always-on display: it dims to a glow, never switches off by itself.
 * Platform (onkar_os.inc or the simulator) provides the plat_* functions below.
 * ==================================================================================== */

typedef struct { uint8_t bright, always_on, autostart, timeout, face, accent, haptic, spare2; } T_UI_CFG;   /* face = carry scene; haptic 0 off 1 light 2 medium 3 strong */

#include "fob_ancs.h"

enum { H_TICK, H_SOFT, H_TAP, H_CLICK, H_DOUBLE, H_SUCCESS, H_LONG, H_BUMP, H_ALARM, H_BOOT, H_SCROLL, H_PRESS, H_RELEASE, H_N };
static void plat_haptic(uint8_t id);
static int plat_batt(int *mv);          /* 0..100 %, or -1 = no battery seen (charger-only supply) */
static uint32_t plat_ms(void);
static void plat_bright(uint8_t v);
static void plat_vsync(bool on);        /* true: send this frame in step with the panel's refresh (still pictures); false: as fast as possible (motion) */
static bool plat_bright_ready(void);    /* the panel has reached the requested brightness (the supply ramps it up after boot) */
static void plat_display(bool on);
static bool plat_bt(void);
static uint32_t plat_now_s(void);
static bool plat_time_ok(void);
static void plat_save(void);
static bool plat_info(int i, char *key, char *val);

/* ------------------------------------------------------------------ palette: bone ink on black, one signal colour */
#define C_BG    0x0000
#define C_INK   RGB(236, 230, 218)
#define C_Q     RGB(142, 142, 142)
#define C_DIM   RGB(96, 96, 96)
#define C_RULE  RGB(74, 74, 74)
#define C_LN    RGB(38, 38, 38)
#define C_SF    RGB(24, 24, 24)
#define C_SIG   RGB(238, 75, 43)
#define CX      (LCD_W / 2)
#define DOT     "`"             /* the font renders ` as a middle dot, ^ as a chevron */

#define N_ACCENT 1
#define N_TIMEOUT 5
#define N_SCENES 5
#define N_FACE   N_SCENES
#define N_MODES  9
enum { M_CARRY, M_CARDS, M_MUSIC, M_INBOX, M_REMOTE, M_CLAUDE, M_PLAY, M_TOOLS, M_SETTINGS };
static const char *const MODE_NAME[N_MODES] = {"carry", "cards", "music", "inbox", "remote", "claude", "play", "tools", "settings"};
static const char *const MODE_CAP[N_MODES] = {"scenes: me, pet, clocks", "qr cards: site, github", "now playing and volume", "notifications and calls",
                                              "camera shutter, clicker", "your laptop, from here", "coin, dice, tally", "torch, stopwatch, metronome", "screen, haptics, about"};
static const uint8_t MODE_WORD[N_MODES] = {W_CARRY, W_CARDS, W_MUSIC, W_INBOX, W_REMOTE, W_CLAUDE, W_PLAY, W_TOOLS, W_SETTINGS};
static const uint16_t TIMEOUTS[] = {10, 15, 30, 60, 300};
static const char *const WDAY[] = {"thu", "fri", "sat", "sun", "mon", "tue", "wed"};
static const char *const MON[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};

/* ------------------------------------------------------------------ small helpers */
static char *u_num(char *p, uint32_t v) { char t[11]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n) { *p++ = t[--n]; } *p = 0; return p; }
static char *u_2(char *p, uint32_t v) { *p++ = (char)('0' + (v / 10) % 10); *p++ = (char)('0' + v % 10); *p = 0; return p; }
static char *u_s(char *p, const char *s) { while (*s) { *p++ = *s++; } *p = 0; return p; }
static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static float e_out(float t) { t = 1.f - t; return 1.f - t * t * t; }
static float e_inout(float t) { float u = 2.f - 2.f * t; return t < 0.5f ? 4.f * t * t * t : 1.f - u * u * u / 2.f; }
static float exp_neg(float x) { float d = 1.f + x * (1.f + x * (0.5f + x * (0.16667f + x * 0.041667f))); return 1.f / d; }
static float spring(float t)            /* 0 -> 1 with a small overshoot, settles in ~0.8 s */
{
    if (t <= 0) { return 0; } if (t >= 1.2f) { return 1; }
    const float w = 15.f, z = 0.52f, wd = w * 0.8544f;
    return 1.f - exp_neg(z * w * t) * (cosf(wd * t) + (z * w / wd) * sinf(wd * t));
}

enum { E_OUT, E_INOUT, E_LIN };
typedef struct { float a, b; uint32_t t0; uint16_t dur; uint8_t ease; } anim_t;
static float an_val(const anim_t *an, uint32_t now)
{
    if (!an->dur || now - an->t0 >= an->dur) { return an->b; }
    float t = (float)(now - an->t0) / an->dur;
    float k = an->ease == E_OUT ? e_out(t) : an->ease == E_INOUT ? e_inout(t) : t;
    return an->a + (an->b - an->a) * k;
}
static bool an_busy(const anim_t *an, uint32_t now) { return an->dur && now - an->t0 < an->dur; }
static void an_go(anim_t *an, float to, uint16_t dur, uint32_t now, uint8_t ease) { an->a = an_val(an, now); an->b = to; an->t0 = now; an->dur = dur; an->ease = ease; }
static void an_set(anim_t *an, float v) { an->a = an->b = v; an->dur = 0; }

static void civil(uint32_t days, int *y, int *m, int *d)
{
    int32_t z = (int32_t)days + 719468, era = z / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097), yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1); *m = (int)(mp < 10 ? mp + 3 : mp - 9); *y = (int)((int32_t)yoe + era * 400 + (*m <= 2));
}

/* ------------------------------------------------------------------ state */
enum { S_BOOT, S_CARRY, S_DIAL, S_MODE, S_LOW, S_N };
static T_UI_CFG *uc;
static uint8_t scr = S_BOOT, mode_cur;
static bool full_dirty = true;
static uint32_t boot_t0, boot_wait0, last_input, last_sec = 0xFFFFFFFF, last_min = 0xFFFFFFFF, save_at;
static bool boot_run, boot_buzzed, sleeping, sleep_pending, dimmed, lowbat;
static anim_t bright_an, face_an, dial_an, car_an, fade_an;      /* car_an: how far the visible carry scene is dragged off centre, in pixels */
static int8_t scene;
static bool car_drag;
static int dial_idx;
static uint8_t mem[N_MODES];                    /* per-mode memory: the page or row each mode was left on */
static uint8_t fade_ph, pend_scr, pend_mode;    /* 0 none, 1 fading out, 2 fading in */
static bool press_pend; static uint32_t press_t;
static uint8_t set_sub;                          /* settings: 0 the list, 1 phone, 2 advanced, 3 forget this phone */
static uint8_t inb_open;                         /* inbox: 1 = reading one notification */
static uint32_t seen_gen, toast_t0, call_buzz; static bool toast_on, call_ui; static fob_note_t toast_n, call_n;
#define DBL_MS   300
#define WSC      4          /* beluga pixel size */
#define BOOT_END 4700

static void hap(uint8_t id) { if (uc && uc->haptic) { plat_haptic(id); } }
static void dirty(void) { full_dirty = true; }
static void time_parts(uint32_t *h, uint32_t *m, uint32_t *s) { uint32_t t = plat_now_s() % 86400u; *h = t / 3600; *m = (t / 60) % 60; *s = t % 60; }
static void save_soon(uint32_t now) { save_at = now + 1500; if (!save_at) { save_at = 1; } }

static void go(uint8_t to, uint8_t mode, uint32_t now)
{
    pend_scr = to; pend_mode = mode;
    if (fade_ph == 1) { return; }
    fade_ph = 1; an_go(&fade_an, 0, 100, now, E_LIN);
}
static void wake(uint32_t now)
{
    if (sleeping) { plat_display(true); sleeping = false; an_set(&bright_an, 0); }
    sleep_pending = false; dimmed = false; last_input = now;
    an_go(&bright_an, uc->bright, 240, now, E_OUT);
    dirty();
}
static void go_sleep(uint32_t now) { sleep_pending = true; an_go(&bright_an, 0, 240, now, E_OUT); }
static void touched(uint32_t now) { last_input = now; if (dimmed) { dimmed = false; an_go(&bright_an, uc->bright, 200, now, E_OUT); dirty(); } }

/* ------------------------------------------------------------------ drawing helpers */
static void word(int x, int base, int fi, const char *s, uint16_t c, uint8_t a) { g_text(x, base - FONT_TAB[fi]->asc, fi, c, a, s); }
static void word_r(int rx, int base, int fi, const char *s, uint16_t c, uint8_t a) { g_text_r(rx, base - FONT_TAB[fi]->asc, fi, c, a, s); }
static void word_c(int cx, int base, int fi, const char *s, uint16_t c, uint8_t a) { g_text_c(cx, base - FONT_TAB[fi]->asc, fi, c, a, s); }
static void hair(int x0, int x1, int y, uint16_t c) { g_rect((float)x0, (float)y, (float)(x1 - x0), 1.f, c, 255); }
static void layer(int fi, const int16_t (*off)[2], int idx, int x, int y, uint16_t c, uint8_t a) { g_icon(x + off[idx][0], y + off[idx][1], fi, idx, c, a); }
static void bigword(int w, int cy, uint8_t a) { const gl_t *g = &f_v_g[w]; g_icon(CX - g->w / 2, cy - g->h / 2, FV, w, C_INK, a); }

/* the book's chrome: a top row of three hairline cells (the time, where you are, what is connected) and a caption row of two */
static void top(const char *mid)
{
    char t[8]; uint32_t h, m, s; time_parts(&h, &m, &s);
    char *p = u_2(t, h); *p++ = ':'; u_2(p, m);
    hair(20, 120, 30, C_RULE); hair(130, 230, 30, C_RULE); hair(240, 348, 30, C_RULE);
    word(20, 47, FL, t, C_INK, 255); word(130, 47, FL, mid, C_Q, 255);
    char b[24]; int mv; int pc = plat_batt(&mv); p = u_s(b, fob_ancs_state() == FA_READY ? "iphone" : (plat_bt() ? "bt" : "visible"));
    if (pc >= 0) { p = u_s(p, " " DOT " "); p = u_num(p, (uint32_t)pc); u_s(p, "%"); }
    word(240, 47, FL, b, C_Q, 255);
}
static void foot(const char *a, const char *b)
{
    hair(20, 180, 414, C_RULE); hair(190, 348, 414, C_RULE);
    word(20, 431, FL, a, C_Q, 255); word(190, 431, FL, b, C_Q, 255);
}
static void segs(int n, int on, int y)
{
    float gap = 4.f, w = (328.f - gap * (n - 1)) / n;
    for (int k = 0; k < n; k++) { g_rect(20.f + k * (w + gap), (float)y, w, 2.f, k == on ? C_INK : RGB(58, 58, 58), 255); }
}

/* ------------------------------------------------------------------ boot: rings draw, the star lands, the beluga swims in */
static const uint16_t WHC[7] = {RGB(255, 251, 236), RGB(245, 240, 217), RGB(232, 227, 200), RGB(211, 207, 177), RGB(181, 181, 155), RGB(74, 74, 42), 0x0000};
static void draw_whale(int x, int y, int frame, uint8_t a)
{
    for (int i = 0; i < 7; i++) { layer(FW, f_w_off, frame * 7 + i, x, y, WHC[i], a); }
}
static void draw_boot(uint32_t now)
{
    if (!boot_run) { return; }
    float t = (now - boot_t0) / 1000.f;
    static const uint16_t RC[3] = {RGB(236, 230, 218), RGB(142, 142, 142), RGB(74, 74, 74)};
    float rf = t > 1.9f ? clampf(1.f - (t - 1.9f) / 0.5f, 0, 1) : 1.f;
    for (int i = 0; i < 3 && rf > 0; i++)
    {
        float R = 130.f - i * 26.f, k = clampf((t - 0.05f - i * 0.16f) / 0.95f, 0, 1);
        uint8_t a = (uint8_t)(255 * rf);
        if (k >= 1.f) { g_ring(CX, 190, R, R - 1.6f, RC[i], a); }
        else if (k > 0.f) { g_arc(CX, 190, R, R - 1.6f, 0, 360.f * e_inout(k), RC[i], a, false); }
    }
    float sa = clampf((t - 0.9f) / 0.45f, 0, 1) * (t > 2.0f ? clampf(1.f - (t - 2.0f) / 0.35f, 0, 1) : 1.f);
    if (sa > 0.01f) { g_icon_c(CX, 190, FX, F_X_STAR, C_INK, (uint8_t)(255 * sa)); }
    if (t > 2.0f)
    {
        float rise = spring((t - 2.0f) / 0.9f), wa = clampf((t - 2.0f) / 0.3f, 0, 1);
        int bob = t > 3.0f ? (int)(sinf((t - 3.0f) * 2.6f) * 2.5f) : 0;
        int flick = (t > 3.0f && ((int)((t - 3.0f) / 0.45f) & 1)) ? 1 : 0;
        int wx = CX - (28 * WSC) / 2, wy = 150 + (int)((1.f - rise) * 120.f) + bob - WSC;
        draw_whale(wx, wy, flick, (uint8_t)(255 * wa));
        if (t > 3.1f && t < 4.4f) { for (int i = 0; i < 3; i++) { float k = ((t - 3.1f) * 0.8f + i * 0.33f); k -= (int)k; g_circle(wx + 27.f * WSC + sinf(k * 9 + i) * 5, wy + 11.f * WSC - k * 60.f, 1.8f + i * 0.5f, C_Q, (uint8_t)(200 * (1.f - k))); } }
    }
    if (t > 2.6f) { float k = spring((t - 2.6f) / 0.9f), a = clampf((t - 2.6f) / 0.35f, 0, 1); g_icon_c(CX, 372 + (int)((1.f - k) * 30.f), FV, W_FOB, C_INK, (uint8_t)(255 * a)); }
}

/* ------------------------------------------------------------------ the pet's room */
static const uint16_t PETC[9] = {RGB(150, 140, 120), RGB(207, 198, 179), RGB(239, 232, 218), RGB(227, 167, 154), RGB(110, 103, 87), C_SIG, 0x0000, 0xFFFF, RGB(43, 43, 43)};
#define PSC 5
static uint8_t pet_state(uint32_t now)
{
    if (dimmed) { return PS_SLEEP; }
    uint32_t t = now / 250, ph = (t / 40) % 3;           /* every ~10 s: idle, idle, coding */
    if (ph == 2) { return PS_CODING; }
    return (t % 17 == 0) ? PS_BLINK : PS_IDLE;
}
static void sc_pet(uint32_t now)
{
    uint8_t st = pet_state(now); uint32_t tk = now / 250;
    uint32_t h, m, s; time_parts(&h, &m, &s);
    bool night = h < 6 || h >= 19 || st == PS_SLEEP;
    g_rect(252, 84, 84, 66, RGB(38, 38, 38), 255);
    g_rect(257, 89, 74, 56, night ? RGB(12, 18, 28) : RGB(45, 61, 77), 255);
    if (night) { g_rect(262, 100, 3, 3, RGB(207, 214, 224), 200); g_rect(300, 96, 3, 3, RGB(207, 214, 224), 255); g_rect(318, 122, 3, 3, RGB(207, 214, 224), 160); g_rect(304, 104, 12, 12, RGB(239, 232, 218), 255); g_rect(309, 102, 11, 11, RGB(12, 18, 28), 255); }
    else { g_rect(304, 98, 14, 14, RGB(232, 217, 168), 255); g_rect(266, 124, 36, 6, RGB(106, 127, 144), 255); }
    g_rect(293, 89, 2, 56, RGB(38, 38, 38), 255); g_rect(257, 114, 74, 2, RGB(38, 38, 38), 255);
    int px = CX - (26 * PSC) / 2, py = 158 + ((st == PS_SLEEP && (tk & 2)) ? 3 : 0);
    for (int i = 0; i < PET_BODY_N; i++) { layer(FP, f_p_off, PET_BODY[i], px, py, PETC[i], 255); }
    for (int i = 0; i < PET_FACE[st].n; i++) { layer(FP, f_p_off, PET_FACE[st].l[i], px, py, PETC[PET_FACE[st].t[i]], 255); }
    if (st == PS_SLEEP) { g_text(272, 124 - ((int)(tk & 3)) * 4 - FONT_TAB[FT]->asc, FT, C_Q, 255, "z"); g_text(292, 100 - ((int)(tk & 3)) * 4 - FONT_TAB[FT]->asc, FT, C_DIM, 255, "z"); }
    g_rect(20, 330, 328, 14, RGB(74, 63, 51), 255); g_rect(20, 344, 328, 3, RGB(44, 37, 29), 255);
    g_rect(20, 347, 328, 70, RGB(26, 22, 18), 255); g_rect(158, 368, 52, 6, RGB(44, 37, 29), 255); g_rect(176, 370, 16, 2, RGB(125, 117, 101), 255);
    g_rect(96, 296, 176, 34, RGB(43, 43, 43), 255); g_rect(96, 296, 176, 3, RGB(58, 58, 58), 255);
    g_icon_c(184, 313, FX, F_X_STAR_S, C_SIG, st == PS_SLEEP ? 70 : (uint8_t)(170 + (tk % 3) * 40));
    if (st == PS_CODING) { int t2 = (int)(tk & 1); g_rect(112.f, 288.f + t2 * 5, 18, 10, RGB(239, 232, 218), 255); g_rect(238.f, 288.f + (1 - t2) * 5, 18, 10, RGB(239, 232, 218), 255); }
    g_rect(298, 306, 26, 26, RGB(201, 194, 180), 255); g_rect(324, 312, 6, 14, RGB(201, 194, 180), 255); g_rect(300, 308, 22, 4, RGB(58, 47, 42), 255);
    if (st != PS_SLEEP) { g_rect(306, 292 - (int)(tk % 3) * 4, 3, 3, C_Q, 180); g_rect(314, 284 - (int)((tk + 1) % 3) * 4, 3, 3, C_Q, 120); }
    g_rect(34, 262, 5, 68, RGB(59, 59, 59), 255); g_rect(34, 256, 44, 7, RGB(59, 59, 59), 255); g_rect(70, 263, 8, 6, RGB(255, 217, 160), 220);
}

/* ------------------------------------------------------------------ carry scenes */
static const uint32_t *qr_m; static int qr_n, qr_x, qr_y, qr_s;
static void qr_hook(uint16_t *row, int y, int x0, int x1, int bx)
{
    if (y < qr_y) { return; }
    int my = (y - qr_y) / qr_s; if (my >= qr_n) { return; }
    uint32_t bits = qr_m[my];
    for (int x = x0 < qr_x ? qr_x : x0; x < x1; x++) { int mx = (x - qr_x) / qr_s; if (mx < qr_n && ((bits >> mx) & 1)) { row[x - bx] = 0x0000; } }
}
static void draw_qr(int idx, int x, int y, int s)
{
    qr_m = idx ? QR1 : QR0; qr_n = idx ? QR1_N : QR0_N; qr_x = x + g_ox; qr_y = y + g_oy; qr_s = s;      /* the hook works in screen pixels: it must follow the offset */
    g_hook(x, y, x + qr_n * s, y + qr_n * s, qr_hook);
}
static void sc_me(void)
{
    bigword(W_ONKAR, 142, 255);
    word(20, 232, FB, "i build tiny hardware", C_INK, 255);
    word(20, 256, FB, "and the software for it.", C_Q, 255);
    hair(20, 348, 292, C_LN);
    word(20, 326, FL, "onkaryaglewad.in", C_Q, 255);
    word(20, 346, FL, "scan, or tap a phone to the back", C_DIM, 255);
    g_rrect(236, 300, 102, 102, 0, 0xFFFF, 255); draw_qr(0, 240, 304, 3);
}
static void sc_stack(void)
{
    uint32_t h, m, s; time_parts(&h, &m, &s); char b[16];
    u_2(b, h); word(24, 206, FN, b, C_INK, 255);
    u_2(b, m); word(24, 366, FN, b, C_Q, 255);
    uint32_t days = plat_now_s() / 86400u; int y, mo, d; civil(days, &y, &mo, &d);
    char *p = u_s(b, WDAY[days % 7]); *p++ = ' '; p = u_num(p, (uint32_t)d); *p++ = ' '; u_s(p, MON[mo - 1]);
    word(20, 400, FB, b, C_Q, 255);
}
static void sc_ring(void)
{
    uint32_t h, m, s; time_parts(&h, &m, &s);
    const float cx = CX, cy = 238.f, r = 150.f;
    for (int i = 0; i < 60; i++)
    {
        float a = i * 0.104720f, sn = sinf(a), cs = cosf(a), l = (i % 5) ? 6.f : 15.f;
        g_line(cx + sn * (r - l), cy - cs * (r - l), cx + sn * r, cy - cs * r, (i % 5) ? 0.5f : 0.9f, (i % 5) ? C_DIM : C_INK, 255);
    }
    float ha = ((h % 12) + m / 60.f) * 0.523599f, ma = (m + s / 60.f) * 0.104720f, sa = s * 0.104720f;
    g_line(cx, cy, cx + sinf(ha) * 78, cy - cosf(ha) * 78, 2.6f, C_INK, 255);
    g_line(cx, cy, cx + sinf(ma) * 122, cy - cosf(ma) * 122, 1.6f, C_INK, 255);
    if (!dimmed) { g_line(cx, cy, cx + sinf(sa) * 136, cy - cosf(sa) * 136, 0.8f, C_SIG, 255); }      /* the always-on picture does not tick */
    g_circle(cx, cy, 5, C_SIG, 255);
}
static void sc_day(void)
{
    uint32_t h, m, s; time_parts(&h, &m, &s); char b[8];
    char *p = u_2(b, h); *p++ = ':'; u_2(p, m);
    word_c(CX, 150, FT, b, C_INK, 255);
    word_c(CX, 176, FL, "the day, in 24 hours", C_Q, 255);
    for (int i = 0; i < 24; i++)
    {
        float sun = sinf((i - 6) * 0.2618f); if (sun < 0) { sun = 0; }
        float hh = 26.f + sun * 120.f, x = 30.f + i * 13.0f;
        g_rect(x, 348.f - hh, 9.6f, hh, (uint32_t)i == h ? C_INK : (sun > 0.05f ? C_Q : RGB(58, 58, 58)), 255);
    }
    g_circle(30.f + h * 13.0f + 4.8f, 366.f, 2.8f, C_SIG, 255);
    word(20, 394, FL, "midnight", C_Q, 255); word_c(CX, 394, FL, "noon", C_Q, 255); word_r(348, 394, FL, "midnight", C_Q, 255);
}
static void draw_scene(int i, uint32_t now)
{
    switch (i) { case 0: sc_me(); break; case 1: sc_pet(now); break; case 2: sc_stack(); break; case 3: sc_ring(); break; default: sc_day(); break; }
}
static const char *const SCENE_NAME[N_SCENES] = {"me", "pet", "stack", "hairline", "day"};
static void draw_carry(uint32_t now)
{
    top(SCENE_NAME[scene]);
    segs(N_SCENES, scene, 60);
    int bx = g_ox, by = g_oy, o = (int)an_val(&car_an, now);
    g_ox = bx + o; draw_scene(scene, now);
    if (o != 0)
    {
        int nb = scene + (o < 0 ? 1 : -1);
        if (nb >= 0 && nb < N_SCENES) { g_ox = bx + o + (o < 0 ? LCD_W : -LCD_W); draw_scene(nb, now); }
    }
    g_ox = bx; g_oy = by;
}

/* ------------------------------------------------------------------ the dial (the book's: a giant word, neighbours fading, a tick scale) */
static void draw_dial(uint32_t now)
{
    float p = an_val(&dial_an, now);
    top("dial");
    for (int i = 0; i < N_MODES; i++)
    {
        float d = i - p, ad = d < 0 ? -d : d;
        if (ad >= 1.7f) { continue; }
        int yc = 226 + (int)(d * 120.f);
        float ag = clampf(1.f - ad * 1.25f, 0, 1), as = clampf(ad * 2.f, 0, 1) * clampf(1.7f - ad, 0, 1) * 0.6f;
        if (ag > 0.02f) { bigword(MODE_WORD[i], yc, (uint8_t)(255 * ag)); }
        if (as > 0.02f) { word(54, yc + 10, FT, MODE_NAME[i], C_INK, (uint8_t)(255 * as)); }
    }
    for (int i = 0; i < N_MODES; i++)    /* the tick scale */
    {
        float d = i - p, ad = d < 0 ? -d : d; bool on = ad < 0.5f;
        g_rect(on ? 30.f : 34.f, 96.f + i * 28.f, on ? 16.f : 8.f, on ? 2.f : 1.f, on ? C_SIG : RGB(90, 90, 90), 255);
    }
    int k = (int)(p + 0.5f); if (k < 0) { k = 0; } if (k >= N_MODES) { k = N_MODES - 1; }
    float near = clampf(1.f - (p > k ? p - k : k - p) * 2.f, 0, 1);
    hair(54, 348, 350, C_LN);
    word(54, 376, FB, MODE_CAP[k], C_Q, (uint8_t)(255 * near));
    foot("crown " DOT " rotate, or drag", "press " DOT " or tap to enter");
}

/* ------------------------------------------------------------------ modes */
static const uint8_t BRIGHTS[] = {40, 80, 120, 160, 200, 255};
static const char *const HAP_NAME[] = {"off", "light", "medium", "strong"};
static int scr_choice(void) { return uc->always_on ? 0 : (uc->timeout < 2 ? 1 : (uc->timeout > 4 ? 3 : uc->timeout - 1)); }     /* 0 always on, 1 30 s, 2 1 min, 3 5 min */
#define SL_X0 40
#define SL_X1 328
static void pill(int x, int y, int w, const char *s, bool on)
{
    g_rrect((float)x, (float)y, (float)w, 38.f, 19.f, on ? C_INK : C_SF, 255);
    word_c(x + w / 2, y + 26, FB, s, on ? 0x0000 : C_Q, 255);
}
static void draw_phone_page(void);
static void draw_settings(uint32_t now)
{
    if (set_sub) { draw_phone_page(); return; }
    (void)now; char b[24]; int row = mem[M_SETTINGS];
    top("settings");
    int y = 70; if (row == 0) { g_circle(9, (float)y + 14, 3.5f, C_SIG, 255); }
    word(20, y + 22, FT, "brightness", row == 0 ? C_INK : RGB(154, 148, 136), 255);
    char *p = u_num(b, (uint32_t)uc->bright * 100u / 255u); u_s(p, "%"); word_r(348, y + 20, FB, b, C_SIG, 255);
    float k = (float)(uc->bright - 16) / 239.f; if (k < 0) { k = 0; }
    g_rect(SL_X0, y + 50, SL_X1 - SL_X0, 3, RGB(52, 52, 52), 255); g_rect(SL_X0, y + 50, k * (SL_X1 - SL_X0), 3, C_INK, 255);
    g_circle(SL_X0 + k * (SL_X1 - SL_X0), y + 51.5f, 13, C_INK, 255);
    y = 160; if (row == 1) { g_circle(9, (float)y + 14, 3.5f, C_SIG, 255); }
    word(20, y + 22, FT, "haptics", row == 1 ? C_INK : RGB(154, 148, 136), 255);
    for (int i = 0; i < 4; i++) { pill(30 + i * 78, y + 36, 72, HAP_NAME[i], (uc->haptic & 3) == i); }
    y = 258; if (row == 2) { g_circle(9, (float)y + 14, 3.5f, C_SIG, 255); }
    word(20, y + 22, FT, "screen", row == 2 ? C_INK : RGB(154, 148, 136), 255);
    int sc = scr_choice();
    pill(30, y + 36, 112, "always on", sc == 0); pill(150, y + 36, 58, "30s", sc == 1); pill(214, y + 36, 58, "1m", sc == 2); pill(278, y + 36, 60, "5m", sc == 3);
    y = 338; hair(20, 348, y, C_LN); if (row == 3) { g_circle(9, (float)y + 28, 3.5f, C_SIG, 255); }
    word(20, y + 38, FT, "phone", row == 3 ? C_INK : RGB(154, 148, 136), 255);
    { int fs = fob_ancs_state(); word_r(348, y + 36, FB, fs == FA_READY ? "iphone, connected" : (fs == FA_DISCOVERING ? "pairing..." : (fs == FA_DENIED ? "allow notifications" : "not paired")), fs == FA_READY ? C_SIG : C_Q, 255); }
    { char k2[24], v[32], line[64]; if (plat_info(0, k2, v)) { p = u_s(line, k2); p = u_s(p, "  "); u_s(p, v); word(20, 398, FL, line, C_DIM, 255); } }
    foot("crown " DOT " move", "press " DOT " change, or touch");
}

static void draw_cards(uint32_t now)
{
    (void)now; int pg = mem[M_CARDS] & 1;
    top("cards"); segs(2, pg, 60);
    g_rrect(36, 82, 296, 296, 14, 0xFFFF, 255);
    int s = pg ? 8 : 10, n = pg ? QR1_N : QR0_N; draw_qr(pg, 36 + (296 - n * s) / 2, 82 + (296 - n * s) / 2, s);
    word_c(CX, 408, FT, pg ? "github.com/once-human" : "onkaryaglewad.in", C_INK, 255);
}

static uint32_t play_t0; static uint8_t play_res; static uint16_t tally;
static const char *const PLAY_NAME[4] = {"coin", "d6", "d20", "tally"};
static uint32_t rnd_s = 2463534242u;
static uint32_t rnd(void) { rnd_s ^= rnd_s << 13; rnd_s ^= rnd_s >> 17; rnd_s ^= rnd_s << 5; return rnd_s; }
static void draw_play(uint32_t now)
{
    int pg = mem[M_PLAY] & 3; char b[8];
    top("play"); segs(4, pg, 60);
    bool rolling = play_t0 && now - play_t0 < 650;
    if (pg == 0)
    {
        g_circle(CX, 224, 104, C_INK, 255); g_ring(CX, 224, 90, 88, 0x0000, 70);
        word_c(CX, 236, FT, rolling ? ((now / 90) & 1 ? "heads" : "tails") : (play_res ? "tails" : "heads"), 0x0000, 255);
    }
    else if (pg == 3) { u_num(b, tally); word_c(CX, 270, FN, b, C_INK, 255); }
    else { int mx = pg == 1 ? 6 : 20; u_num(b, rolling ? 1 + (now / 70) % mx : play_res); g_ring(CX, 224, 118, 117, C_RULE, 255); word_c(CX, 270, FN, b, C_INK, 255); }
    word_c(CX, 380, FL, pg == 3 ? "press or tap: +1   hold: reset" : "press or tap to roll", C_Q, 255);
    for (int i = 0; i < 4; i++) { word(36 + i * 80, 432, FL, PLAY_NAME[i], pg == i ? C_INK : C_DIM, 255); }
}
static void play_act(uint32_t now)
{
    int pg = mem[M_PLAY] & 3;
    if (pg == 3) { tally++; hap(H_CLICK); }
    else { play_t0 = now; play_res = (uint8_t)(pg == 0 ? (rnd() & 1) : 1 + rnd() % (pg == 1 ? 6 : 20)); hap(H_DOUBLE); }
    dirty();
}

static bool torch_on; static uint32_t torch_t0, sw_t0, sw_acc, met_next; static bool sw_run, met_run; static uint16_t met_bpm = 100; static uint8_t torch_lim;
static const uint16_t TORCH_LIM[4] = {120, 600, 0, 0};
static const char *const TORCH_NAME[4] = {"2 min", "10 min", "until i leave", "stay on"};
static void draw_tools(uint32_t now)
{
    int pg = mem[M_TOOLS] % 3; char b[16];
    if (pg == 0 && torch_on)
    {
        g_rect(0, 0, LCD_W, LCD_H, 0xFFFF, 255);
        if (TORCH_LIM[torch_lim]) { uint32_t el = (now - torch_t0) / 1000u, left = el < TORCH_LIM[torch_lim] ? TORCH_LIM[torch_lim] - el : 0; char *p = u_num(b, left / 60); *p++ = ':'; u_2(p, left % 60); word(40, 50, FL, b, 0x0000, 255); }
        word_c(CX, 428, FL, "tap: change limit " DOT " press: off", RGB(90, 90, 90), 255);
        word_r(328, 50, FL, TORCH_NAME[torch_lim], 0x0000, 255);
        return;
    }
    top("tools"); segs(3, pg, 60);
    if (pg == 0)
    {
        g_ring(CX, 214, 100, 99, C_RULE, 255); g_circle(CX, 214, 26, C_INK, 255);
        word_c(CX, 346, FT, "torch", C_INK, 255); word_c(CX, 372, FB, "press or tap: light", C_Q, 255); word_c(CX, 398, FL, TORCH_NAME[torch_lim], C_DIM, 255);
    }
    else if (pg == 1)
    {
        uint32_t ms = sw_acc + (sw_run ? now - sw_t0 : 0), sec = ms / 1000;
        u_2(b, (sec / 60) % 100); word(24, 206, FN, b, C_INK, 255);
        u_2(b, sec % 60); word(24, 366, FN, b, sw_run ? C_Q : C_DIM, 255);
        char *p = u_s(b, "."); u_num(p, (ms / 100) % 10); word(250, 366, FT, b, C_Q, 255);
        word_c(CX, 408, FL, sw_run ? "press or tap: stop" : "press or tap: start   hold: reset", C_Q, 255);
    }
    else
    {
        u_num(b, met_bpm); word_c(CX, 250, FN, b, C_INK, 255); word_c(CX, 284, FT, "bpm", C_Q, 255);
        for (int i = 0; i < 4; i++) { g_rect(30.f + i * 78, 330, 72, 6, (met_run && ((now / (60000u / met_bpm)) & 3) == (uint32_t)i) ? C_SIG : RGB(58, 58, 58), 255); }
        word_c(CX, 380, FL, met_run ? "press or tap: stop" : "press or tap: start", C_Q, 255); word_c(CX, 402, FL, "left of the screen: slower " DOT " right: faster", C_DIM, 255);
    }
}
static void tools_act(uint32_t now, int x)
{
    int pg = mem[M_TOOLS] % 3;
    if (pg == 0)
    {
        if (torch_on && x >= 0) { torch_lim = (uint8_t)((torch_lim + 1) & 3); torch_t0 = now; hap(H_CLICK); }
        else { torch_on = !torch_on; torch_t0 = now; if (torch_on) { plat_bright(255); an_set(&bright_an, 255); } else { plat_bright(uc->bright); an_set(&bright_an, uc->bright); } hap(H_TAP); }
    }
    else if (pg == 1) { if (sw_run) { sw_acc += now - sw_t0; sw_run = false; } else { sw_t0 = now; sw_run = true; } hap(H_CLICK); }
    else
    {
        if (x >= 0 && x < 100) { met_bpm = met_bpm > 40 ? met_bpm - 5 : 40; hap(H_TICK); }
        else if (x > 268) { met_bpm = met_bpm < 220 ? met_bpm + 5 : 220; hap(H_TICK); }
        else { met_run = !met_run; met_next = now; hap(H_TAP); }
    }
    dirty();
}

/* ------------------------------------------------------------------ the iPhone: pairing, inbox, pop-ups, calls */
static void fit_text(char *dst, const char *src, int fi, int maxw, int cap)
{
    const font2_t *f = FONT_TAB[fi]; int w = 0, o = 0;
    for (; *src && o < cap - 3; src++)
    {
        const gl_t *g = g_glyph(f, (uint8_t)*src); int a = g ? g->adv : 0;
        if (w + a > maxw) { dst[o++] = '.'; dst[o++] = '.'; break; }
        dst[o++] = *src; w += a;
    }
    dst[o] = 0;
}
static void ago(char *b, uint32_t t_ms, uint32_t now) { uint32_t m = (now - t_ms) / 60000u; if (m == 0) { u_s(b, "now"); } else if (m < 60) { char *p = u_num(b, m); u_s(p, "m"); } else { char *p = u_num(b, m / 60); u_s(p, "h"); } }
static void step(int n, int y, const char *t) { char d[4]; u_num(d, (uint32_t)n); word(20, y, FT, d, C_SIG, 255); word(52, y - 1, FB, t, C_INK, 255); }
static void draw_phone_guide(const char *where)
{
    int st = fob_ancs_state();
    top(where);
    if (st == FA_DISCOVERING) { word(20, 130, FT, "pairing", C_INK, 255); word(20, 162, FB, "looking for your notifications...", C_Q, 255); foot("hold on", "dbl-press " DOT " back"); return; }
    if (st == FA_DENIED)
    {
        word(20, 112, FT, "allow notifications", C_INK, 255); word(20, 142, FB, "the phone is paired but not sharing them.", C_Q, 255);
        step(1, 214, "open settings, then bluetooth"); step(2, 258, "tap the (i) next to fob"); step(3, 302, "switch on show notifications"); word(20, 352, FL, "then press the crown to try again", C_Q, 255);
        foot("waiting for the phone", "dbl-press " DOT " back"); return;
    }
    word(20, 112, FT, "pair your phone", C_INK, 255); word(20, 142, FB, "no app needed. it uses the iphone itself.", C_Q, 255);
    step(1, 214, "open settings, then bluetooth"); step(2, 258, "tap fob under other devices"); step(3, 302, "pair, and allow notifications");
    g_circle(24, 372, 4, C_SIG, 255); word(36, 377, FL, plat_bt() ? "a device is connected, waiting for pairing" : "visible now, waiting for your phone", C_Q, 255);
    foot("crown " DOT " nothing to do", "dbl-press " DOT " back");
}
static void draw_inbox(uint32_t now)
{
    if (fob_ancs_state() != FA_READY) { draw_phone_guide("inbox"); return; }
    int n = fob_ancs_count(), sel = mem[M_INBOX]; if (sel >= n) { sel = n ? n - 1 : 0; mem[M_INBOX] = (uint8_t)sel; }
    top("inbox");
    if (!n) { word(20, 140, FT, "nothing waiting", C_INK, 255); word(20, 172, FB, "you are all caught up.", C_Q, 255); foot("dbl-press " DOT " dial", ""); return; }
    fob_note_t t; char b[120], tm[8];
    if (inb_open && fob_ancs_get((uint8_t)sel, &t))
    {
        g_circle(24, 80, 4, t.unread ? C_SIG : C_DIM, 255); word(36, 85, FL, t.app, C_Q, 255); ago(tm, t.t_ms, now); word_r(348, 85, FL, tm, C_Q, 255);
        fit_text(b, t.title, FT, 328, sizeof b); word(20, 130, FT, b, C_INK, 255);
        const char *p = t.msg; int y = 170;                                   /* word-wrapped message */
        while (*p && y < 380)
        {
            int w = 0, o = 0, lastsp = -1; char line[64];
            while (p[o] && o < 62) { const gl_t *g = g_glyph(FONT_TAB[FB], (uint8_t)p[o]); w += g ? g->adv : 0; if (w > 328) { break; } if (p[o] == ' ') { lastsp = o; } o++; }
            if (p[o] && lastsp > 0 && w > 328) { o = lastsp; }
            memcpy(line, p, (size_t)o); line[o] = 0; word(20, y, FB, line, C_INK, 255); y += 26; p += o; while (*p == ' ') { p++; }
        }
        foot("dbl-press " DOT " list", ""); return;
    }
    int first = sel < 2 ? 0 : sel - 1;
    for (int r = 0; r < 3 && first + r < n; r++)
    {
        int i = first + r, y = 66 + r * 112; bool on = i == sel;
        if (!fob_ancs_get((uint8_t)i, &t)) { continue; }
        hair(20, 348, y, C_LN);
        if (on) { g_rect(8, (float)y + 12, 3, 82, C_SIG, 255); }
        word(20, y + 28, FL, t.app, t.unread ? C_SIG : C_Q, 255); ago(tm, t.t_ms, now); word_r(348, y + 28, FL, tm, C_Q, 255);
        fit_text(b, t.title[0] ? t.title : t.app, FT, 328, sizeof b); word(20, y + 62, FT, b, on ? C_INK : RGB(154, 148, 136), 255);
        fit_text(b, t.msg, FB, 328, sizeof b); word(20, y + 90, FB, b, C_Q, 255);
    }
    foot("crown " DOT " scroll", "press " DOT " read");
}
static void draw_toast(uint32_t now)
{
    char b[120], tm[8]; ago(tm, toast_n.t_ms, now);
    g_rect(14, 54, 340, 92, C_SF, 255); g_rect(14, 54, 3, 92, C_SIG, 255);
    word(28, 78, FL, toast_n.app, C_SIG, 255); word_r(342, 78, FL, tm, C_Q, 255);
    fit_text(b, toast_n.title[0] ? toast_n.title : toast_n.app, FT, 312, sizeof b); word(28, 110, FT, b, C_INK, 255);
    fit_text(b, toast_n.msg, FB, 312, sizeof b); word(28, 135, FB, b, C_Q, 255);
}
static void draw_call(void)
{
    char b[120];
    top("call"); g_circle(24, 96, 4, C_SIG, 255); word(36, 101, FL, call_n.cat == 1 ? "incoming call" : "call", C_SIG, 255);
    fit_text(b, call_n.title[0] ? call_n.title : call_n.msg, FT, 328, sizeof b); word_c(CX, 236, FT, b, C_INK, 255);
    word_c(CX, 270, FB, call_n.app, C_Q, 255);
    foot("press " DOT " answer", "hold " DOT " decline");
}
static void draw_phone_page(void)
{
    int st = fob_ancs_state();
    if (set_sub == 1 && st != FA_READY) { draw_phone_guide("phone"); return; }
    top("phone");
    if (set_sub == 1)
    {
        word(20, 112, FT, "connected", C_INK, 255); word(20, 142, FB, "your iphone is sharing notifications.", C_Q, 255);
        hair(20, 348, 190, C_LN); word(20, 224, FT, "inbox", C_INK, 255); { char b[24]; char *p = u_num(b, fob_ancs_count()); u_s(p, " waiting"); word_r(348, 222, FB, b, C_Q, 255); }
        hair(20, 348, 250, C_LN); g_circle(9, 282, 3.5f, C_SIG, 255); word(20, 292, FT, "advanced", C_INK, 255); word_r(348, 291, FB, ">", C_Q, 255);
        foot("press " DOT " open", "dbl-press " DOT " back");
    }
    else if (set_sub == 2)
    {
        word(20, 112, FT, "advanced", C_INK, 255); word(20, 142, FB, "you rarely need these.", C_Q, 255);
        hair(20, 348, 190, C_LN); g_circle(9, 222, 3.5f, C_SIG, 255); word(20, 232, FT, "forget this phone", C_SIG, 255);
        foot("press " DOT " open", "dbl-press " DOT " back");
    }
    else
    {
        word(20, 112, FT, "forget your iphone?", C_INK, 255);
        word(20, 150, FB, "notifications stop. your settings stay.", C_Q, 255); word(20, 176, FB, "you can pair it again at any time.", C_Q, 255);
        g_ring(CX, 292, 54, 52, RGB(52, 52, 52), 255); word_c(CX, 298, FB, "hold", C_INK, 255);
        foot("hold the crown " DOT " forget", "dbl-press " DOT " cancel");
    }
}

static void draw_mode_stub(void)
{
    top(MODE_NAME[mode_cur]);
    bigword(MODE_WORD[mode_cur], 160, 255);
    word(20, 250, FB, MODE_CAP[mode_cur], C_Q, 255);
    word(20, 280, FB, "designed in the book, built next.", RGB(110, 110, 110), 255);
    foot("dbl-press " DOT " dial", "swipe in from the left");
}
static void draw_low(void)
{
    char b[8]; int mv; int pc = plat_batt(&mv); if (pc < 0) { pc = 0; }
    top("battery");
    g_circle(36, 92, 4, C_SIG, 255); word(46, 97, FL, "plug in soon", C_SIG, 255);
    char *p = u_num(b, (uint32_t)pc); u_s(p, "%"); word(24, 286, FN, b, C_SIG, 255);
    word(20, 340, FL, "the screen dims. bluetooth stays on.", C_Q, 255);
}

/* ------------------------------------------------------------------ input */
static void scene_step(int d, uint32_t now)
{
    int ns = scene + d;
    if (ns < 0 || ns >= N_SCENES) { hap(H_BUMP); return; }
    scene = (int8_t)ns;
    an_set(&car_an, (float)(d > 0 ? LCD_W : -LCD_W)); an_go(&car_an, 0, 340, now, E_OUT);
    uc->face = (uint8_t)scene; an_set(&face_an, scene); save_soon(now); hap(H_TICK); dirty();
}
static void page_step(int mode, int n, int d, uint32_t now)
{
    (void)now; int np = mem[mode] + d;
    if (np < 0 || np >= n) { hap(H_BUMP); return; }
    mem[mode] = (uint8_t)np; hap(H_TICK); dirty();
}
static void back_to_dial2(uint32_t now);
static void dial_go(int ni, uint32_t now) { if (ni < 0 || ni >= N_MODES) { hap(H_BUMP); return; } dial_idx = ni; an_go(&dial_an, (float)ni, 280, now, E_OUT); hap(H_TICK); dirty(); }
static void open_dial(uint32_t now) { dial_idx = mode_cur; an_set(&dial_an, (float)dial_idx); hap(H_TAP); go(S_DIAL, 0, now); }
static void enter_mode(uint32_t now) { hap(H_TAP); mode_cur = (uint8_t)dial_idx; if (dial_idx == M_CARRY) { go(S_CARRY, 0, now); } else { go(S_MODE, (uint8_t)dial_idx, now); } }
static void back_to_dial(uint32_t now)
{
    if (mode_cur == M_SETTINGS && set_sub) { set_sub--; hap(H_SOFT); dirty(); return; }
    if (mode_cur == M_INBOX && inb_open) { inb_open = 0; hap(H_SOFT); dirty(); return; }
    back_to_dial2(now);
}
static void back_to_dial2(uint32_t now) { hap(H_SOFT); dial_idx = mode_cur; an_set(&dial_an, (float)dial_idx); if (torch_on) { torch_on = false; plat_bright(uc->bright); an_set(&bright_an, uc->bright); } go(S_DIAL, 0, now); }
static void settings_apply(int row, int v, uint32_t now)       /* v < 0: the next value */
{
    if (row == 0) { uint8_t nb = BRIGHTS[0]; for (int i = 0; i < 6; i++) { if (BRIGHTS[i] > uc->bright) { nb = BRIGHTS[i]; break; } } uc->bright = v >= 16 ? (uint8_t)v : nb; plat_bright(uc->bright); an_set(&bright_an, uc->bright); }
    else if (row == 1) { uc->haptic = (uint8_t)(v >= 0 ? v & 3 : (uc->haptic + 1) & 3); }
    else { int s = v >= 0 ? v : (scr_choice() + 1) % 4; if (s == 0) { uc->always_on = 1; } else { uc->always_on = 0; uc->timeout = (uint8_t)(s + 1); } }
    save_soon(now); if (uc->haptic) { plat_haptic(H_CLICK); } dirty();
}
static void main_action(uint32_t now, int x)
{
    if (scr == S_CARRY) { open_dial(now); }
    else if (scr == S_DIAL) { enter_mode(now); }
    else if (scr == S_MODE)
    {
        switch (mode_cur)
        {
        case M_SETTINGS:
            if (set_sub == 1) { if (fob_ancs_state() == FA_READY) { set_sub = 2; hap(H_TAP); dirty(); } else { hap(H_TAP); } }
            else if (set_sub == 2) { set_sub = 3; hap(H_TAP); dirty(); }
            else if (set_sub == 3) { hap(H_BUMP); }
            else if (mem[M_SETTINGS] == 3) { set_sub = 1; hap(H_TAP); dirty(); }
            else { settings_apply(mem[M_SETTINGS], -1, now); }
            break;
        case M_INBOX: if (fob_ancs_state() == FA_READY && fob_ancs_count()) { if (!inb_open) { inb_open = 1; fob_ancs_mark_read(); hap(H_TAP); dirty(); } } break;
        case M_PLAY: play_act(now); break;
        case M_TOOLS: tools_act(now, x); break;
        case M_CARDS: page_step(M_CARDS, 2, mem[M_CARDS] ? -1 : 1, now); break;
        default: hap(H_TAP); break;
        }
    }
}

/* touch: tap, swipe, drag */
static bool tdown, tmoved, tdrag; static int tx0, ty0, tx, ty, taxis; static uint32_t tt0; static float t_dial0;
static void ui_touch(bool down, int x, int y, uint32_t now)
{
    if (lowbat || scr == S_BOOT || fade_ph) { return; }
    if (sleeping || dimmed) { tdown = false; car_drag = false; return; }          /* asleep or in the always-on glow: only the crown wakes it */
    if (down)
    {
        if (!tdown) { tdown = true; tmoved = false; tdrag = false; taxis = 0; tx0 = x; ty0 = y; tt0 = now; t_dial0 = an_val(&dial_an, now); touched(now); }
        tx = x; ty = y; last_input = now;
        int dx = x - tx0, dy = y - ty0;
        if (!taxis && (dx * dx + dy * dy) > 14 * 14) { taxis = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? 1 : 2; tmoved = true; }
        if (scr == S_DIAL && taxis == 2) { an_set(&dial_an, clampf(t_dial0 - dy / 120.f, 0, N_MODES - 1)); tdrag = true; dirty(); }
        if (scr == S_CARRY && taxis == 1)
        {
            float off = (float)dx; if ((scene == 0 && off > 0) || (scene == N_SCENES - 1 && off < 0)) { off *= 0.3f; }      /* rubber band at the ends */
            an_set(&car_an, off); car_drag = true; dirty();
        }
        if (scr == S_MODE && mode_cur == M_SETTINGS && !set_sub && ty0 >= 100 && ty0 < 150 && taxis != 2)         /* the brightness slider */
        {
            int v = 16 + (int)(clampf((float)(x - SL_X0) / (SL_X1 - SL_X0), 0, 1) * 239.f);
            if (v != uc->bright) { static uint8_t lastd; if (v / 24 != lastd) { lastd = (uint8_t)(v / 24); hap(H_TICK); } uc->bright = (uint8_t)v; plat_bright(uc->bright); an_set(&bright_an, uc->bright); save_soon(now); dirty(); }
        }
        return;
    }
    if (!tdown) { return; }
    tdown = false;
    int dx = tx - tx0, dy = ty - ty0, adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy; uint32_t dt = now - tt0;
    if (scr == S_DIAL && tdrag) { dial_idx = (int)(an_val(&dial_an, now) + 0.5f); dial_idx = dial_idx < 0 ? 0 : (dial_idx >= N_MODES ? N_MODES - 1 : dial_idx); an_go(&dial_an, (float)dial_idx, 220, now, E_OUT); hap(H_TICK); dirty(); return; }
    bool swipe_h = adx > 56 && adx > ady * 1.3f, swipe_v = ady > 56 && ady > adx * 1.3f;
    if (scr == S_MODE && mode_cur == M_SETTINGS && !set_sub && ty0 >= 100 && ty0 < 150 && tx0 >= 24) { return; }       /* a slider drag is not a swipe */
    if (scr == S_CARRY)
    {
        if (car_drag)
        {
            car_drag = false; float off = an_val(&car_an, now); int d = off < 0 ? 1 : -1; bool can = scene + d >= 0 && scene + d < N_SCENES;
            if (can && (adx > 100 || (adx > 40 && dt < 260)) && (dx < 0) == (d > 0))
            {
                scene = (int8_t)(scene + d); uc->face = (uint8_t)scene; an_set(&face_an, scene); save_soon(now); hap(H_TICK);
                an_set(&car_an, off + (d > 0 ? LCD_W : -LCD_W)); an_go(&car_an, 0, 300, now, E_OUT);
            }
            else { if (!can && adx > 60) { hap(H_BUMP); } an_go(&car_an, 0, 240, now, E_OUT); }
            dirty(); return;
        }
        if (swipe_v && dy < 0) { open_dial(now); }
        return;
    }
    if (swipe_h && dx > 0 && tx0 < 60 && scr == S_MODE) { back_to_dial(now); return; }
    if (scr == S_DIAL) { if (swipe_h && dx > 0) { hap(H_SOFT); mode_cur = 0; go(S_CARRY, 0, now); } else if (!tmoved && dt < 450 && ty0 > 130 && ty0 < 330) { enter_mode(now); } return; }
    if (scr == S_MODE)
    {
        if (swipe_h)
        {
            int d = dx < 0 ? 1 : -1;
            if (mode_cur == M_CARDS) { page_step(M_CARDS, 2, d, now); } else if (mode_cur == M_PLAY) { page_step(M_PLAY, 4, d, now); } else if (mode_cur == M_TOOLS) { page_step(M_TOOLS, 3, d, now); }
            else if (dx > 0) { back_to_dial(now); }
            return;
        }
        if (tmoved) { return; }
        if (mode_cur == M_PLAY && mem[M_PLAY] == 3 && dt > 800) { tally = 0; hap(H_LONG); dirty(); return; }
        if (mode_cur == M_TOOLS && mem[M_TOOLS] == 1 && dt > 800) { sw_acc = 0; sw_run = false; hap(H_LONG); dirty(); return; }
        if (mode_cur == M_SETTINGS && set_sub) { main_action(now, tx0); return; }
        if (mode_cur == M_SETTINGS)
        {
            if (ty0 >= 338 && ty0 < 382) { mem[M_SETTINGS] = 3; main_action(now, tx0); return; }
            if (ty0 >= 196 && ty0 < 244) { int i = (tx0 - 30) / 78; if (i >= 0 && i < 4) { settings_apply(1, i, now); mem[M_SETTINGS] = 1; } }
            else if (ty0 >= 294 && ty0 < 334) { int c = tx0 < 146 ? 0 : tx0 < 210 ? 1 : tx0 < 274 ? 2 : 3; settings_apply(2, c, now); mem[M_SETTINGS] = 2; }
            return;
        }
        main_action(now, tx0);
    }
}

static void ui_crown(int d, uint32_t now)
{
    if (sleeping) { wake(now); return; }
    if (lowbat || scr == S_BOOT) { return; }
    if (dimmed) { touched(now); return; }          /* from the always-on glow, the first turn just wakes it */
    touched(now);
    if (fade_ph) { return; }
    if (scr == S_CARRY) { scene_step(d, now); }
    else if (scr == S_DIAL) { dial_go(dial_idx + d, now); }
    else if (scr == S_MODE)
    {
        switch (mode_cur)
        {
        case M_SETTINGS: { if (set_sub) { break; } int nr = mem[M_SETTINGS] + d; if (nr < 0 || nr > 3) { hap(H_BUMP); } else { mem[M_SETTINGS] = (uint8_t)nr; hap(H_TICK); dirty(); } break; }
        case M_INBOX: { int n = fob_ancs_count(); if (inb_open || fob_ancs_state() != FA_READY) { break; } int nr = mem[M_INBOX] + d; if (nr < 0 || nr >= n) { hap(H_BUMP); } else { mem[M_INBOX] = (uint8_t)nr; hap(H_TICK); dirty(); } break; }
        case M_CARDS: page_step(M_CARDS, 2, d, now); break;
        case M_PLAY: page_step(M_PLAY, 4, d, now); break;
        case M_TOOLS: page_step(M_TOOLS, 3, d, now); break;
        default: break;
        }
    }
}

static void ui_button_edge(bool down, uint32_t now)
{
    (void)now;
    if (scr == S_BOOT || lowbat || sleeping) { return; }
    hap(down ? H_PRESS : H_RELEASE);
}

static void ui_button(bool long_press, uint32_t now)
{
    if (sleeping) { wake(now); return; }
    if (lowbat) { if (!long_press) { wake(now); dirty(); } return; }
    if (scr == S_BOOT) { return; }
    if (call_ui) { touched(now); if (long_press) { fob_ancs_action(call_n.uid, false); hap(H_LONG); } else { fob_ancs_action(call_n.uid, true); hap(H_SUCCESS); } call_ui = false; dirty(); return; }
    if (dimmed && !long_press) { touched(now); return; }          /* a press from the glow only wakes it */
    touched(now);
    if (long_press)
    {
        press_pend = false;
        if (scr == S_MODE && mode_cur == M_SETTINGS && set_sub == 3) { fob_ancs_forget(); set_sub = 1; hap(H_LONG); dirty(); return; }
        if (scr == S_MODE && mode_cur == M_PLAY && mem[M_PLAY] == 3) { tally = 0; hap(H_LONG); dirty(); return; }
        hap(H_LONG); if (torch_on) { torch_on = false; } go_sleep(now); return;
    }
    if (fade_ph) { return; }
    if (press_pend && now - press_t < DBL_MS) { press_pend = false; if (scr == S_DIAL) { hap(H_SOFT); mode_cur = 0; go(S_CARRY, 0, now); } else if (scr == S_MODE) { back_to_dial(now); } return; }
    press_pend = true; press_t = now;
}

/* ------------------------------------------------------------------ frame */
static void ui_init(T_UI_CFG *c, uint32_t now)
{
    uc = c;
    if (uc->bright < 16) { uc->bright = 160; }
    scene = (int8_t)(uc->face % N_SCENES); an_set(&face_an, scene);
    boot_t0 = now; scr = S_BOOT; full_dirty = true; last_input = now; boot_run = false; boot_buzzed = false;
    an_set(&bright_an, uc->bright); plat_bright(uc->bright);
    an_set(&fade_an, 1); fade_ph = 0; press_pend = false; mode_cur = 0;
}

/* the screen supply is up: wait for the brightness to be there, then play the boot animation (so it is never seen half dark) */
static void ui_boot_lit(uint32_t now)
{
    scr = S_BOOT; full_dirty = true; last_input = now; boot_run = false; boot_buzzed = false; boot_wait0 = now;
    an_set(&bright_an, uc->bright); plat_bright(uc->bright);
}

static void ui_lowbat(bool on, uint32_t now)
{
    if (on == lowbat) { return; }
    lowbat = on; press_pend = false; fade_ph = 0; an_set(&fade_an, 1);
    if (on) { hap(H_DOUBLE); scr = S_LOW; wake(now); an_go(&bright_an, uc->bright < 70 ? uc->bright : 70, 300, now, E_OUT); }
    else { scr = S_CARRY; wake(now); plat_haptic(H_SUCCESS); }
    dirty();
}

static bool ui_frame(uint32_t now)
{
    bool busy = false;
    if (an_busy(&bright_an, now) || bright_an.dur)
    {
        float b = an_val(&bright_an, now);
        plat_bright((uint8_t)lroundf(b));
        if (an_busy(&bright_an, now)) { busy = true; }
        else { bright_an.dur = 0; if (sleep_pending && b < 0.5f) { plat_display(false); sleeping = true; } sleep_pending = false; }
    }
    if (save_at && (int32_t)(now - save_at) >= 0) { save_at = 0; plat_save(); }
    if (sleeping) { return false; }

    if (scr == S_BOOT)
    {
        if (!boot_run) { busy = true; if (plat_bright_ready() || now - boot_wait0 > 6000) { boot_run = true; boot_t0 = now; } }
        else
        {
            busy = true; dirty();
            if (!boot_buzzed && now - boot_t0 >= 1000) { boot_buzzed = true; hap(H_BOOT); }
            if (now - boot_t0 >= BOOT_END && !fade_ph) { go(S_CARRY, 0, now); last_input = now; }
        }
    }
    if (press_pend) { if (now - press_t >= DBL_MS) { press_pend = false; if (!fade_ph) { main_action(now, -1); } } else { busy = true; } }
    if (fade_ph == 1 && !an_busy(&fade_an, now))
    {
        scr = pend_scr; if (scr == S_MODE) { mode_cur = pend_mode; } if (scr == S_CARRY) { an_set(&car_an, 0); car_drag = false; }
        an_go(&fade_an, 1, 240, now, E_OUT); fade_ph = 2; dirty();
    }
    if (fade_ph == 2 && !an_busy(&fade_an, now)) { fade_ph = 0; an_set(&fade_an, 1); dirty(); }
    if (fade_ph) { busy = true; dirty(); }

    /* idle: carry dims to an ambient glow and stays on (unless the screen setting says otherwise); every other screen returns to carry */
    if (!lowbat && !fade_ph && scr != S_BOOT)
    {
        uint32_t idle = now - last_input;
        if (scr == S_CARRY)
        {
            if (!dimmed && idle > 10000) { dimmed = true; uint8_t lo = (uint8_t)(uc->bright / 3); if (lo < 40) { lo = 40; } if (lo > uc->bright) { lo = uc->bright; } an_go(&bright_an, lo, 1200, now, E_INOUT); dirty(); }
            if (!uc->always_on && idle > (uint32_t)TIMEOUTS[uc->timeout > 4 ? 4 : uc->timeout] * 1000u) { go_sleep(now); }
        }
        else if (idle > 25000 && !torch_on && !met_run && !sw_run) { go(S_CARRY, 0, now); }
        if (torch_on && TORCH_LIM[torch_lim] && now - torch_t0 > TORCH_LIM[torch_lim] * 1000u) { torch_on = false; plat_bright(uc->bright); an_set(&bright_an, uc->bright); dirty(); }
    }
    if (lowbat && now - last_input > 4000) { go_sleep(now); last_input = now; }

    /* the iPhone: a new notification pops up, a call takes over the screen */
    { uint32_t g = fob_ancs_gen();
      if (g != seen_gen)
      {
          seen_gen = g; dirty();
          fob_note_t n;
          if (fob_ancs_get(0, &n) && n.unread && n.uid != toast_n.uid && !lowbat && scr != S_BOOT)
          {
              toast_n = n; toast_on = true; toast_t0 = now;
              if (sleeping) { wake(now); } else { touched(now); }
              uint8_t h = (uint8_t)(n.app[0] + n.app[1]); hap(h % 3 == 0 ? H_DOUBLE : (h % 3 == 1 ? H_TAP : H_SUCCESS));
          }
          call_ui = fob_ancs_call_active(&call_n);
          if (call_ui) { if (sleeping) { wake(now); } else { touched(now); } call_buzz = now; }
      }
      if (toast_on && now - toast_t0 > 4000) { toast_on = false; dirty(); }
      if (toast_on) { busy = busy; }
      if (call_ui && (int32_t)(now - call_buzz) >= 0) { hap(H_ALARM); call_buzz = now + 1500; } }

    /* things that tick: the clock, the pet, the metronome, stopwatch, torch countdown */
    uint32_t h, m, s; time_parts(&h, &m, &s); uint32_t sec = plat_now_s();
    if (sec != last_sec)
    {
        last_sec = sec;
        if (scr == S_CARRY && scene == 3 && !dimmed) { dirty(); }
        if (scr == S_MODE && ((mode_cur == M_TOOLS && (torch_on || mem[M_TOOLS] % 3 == 1)) || (mode_cur == M_PLAY && play_t0))) { dirty(); }
    }
    if (m != last_min) { last_min = m; dirty(); }
    if (scr == S_CARRY && scene == 1 && !dimmed) { static uint32_t pt; if (now - pt >= 250) { pt = now; dirty(); } }
    if (scr == S_CARRY && scene == 1) { static bool wasd; if (wasd != dimmed) { wasd = dimmed; dirty(); } }
    if (scr == S_MODE && mode_cur == M_PLAY && play_t0 && now - play_t0 < 700) { busy = true; dirty(); }
    if (scr == S_MODE && mode_cur == M_TOOLS && mem[M_TOOLS] % 3 == 1 && sw_run) { static uint32_t st; if (now - st >= 100) { st = now; dirty(); } }
    if (met_run && now >= met_next) { met_next += 60000u / met_bpm; hap(H_TICK); if (scr == S_MODE && mode_cur == M_TOOLS) { dirty(); } }
    { static bool was_moving; bool moving = an_busy(&car_an, now) || an_busy(&dial_an, now) || tdrag || car_drag; if (moving) { busy = true; } if (moving || was_moving) { dirty(); } was_moving = moving; }

    if (!full_dirty) { return busy; }
    gfx_begin(C_BG);
    gfx_invalidate_all(); full_dirty = false;
    if (fade_ph) { g_alpha = (uint8_t)(255.f * clampf(an_val(&fade_an, now), 0, 1)); }
    if (dimmed && scr == S_CARRY) { g_ox = (int)(m % 3) * 2 - 2; g_oy = (int)((m / 3) % 3) * 2 - 2; }          /* burn-in guard: the picture drifts a few pixels */
    if (call_ui) { draw_call(); } else
    switch (scr)
    {
    case S_BOOT: draw_boot(now); break;
    case S_CARRY: draw_carry(now); break;
    case S_DIAL: draw_dial(now); break;
    case S_MODE:
        switch (mode_cur)
        {
        case M_SETTINGS: draw_settings(now); break;
        case M_INBOX: draw_inbox(now); break;
        case M_CARDS: draw_cards(now); break;
        case M_PLAY: draw_play(now); break;
        case M_TOOLS: draw_tools(now); break;
        default: draw_mode_stub(); break;
        }
        break;
    default: draw_low(); break;
    }
    g_alpha = 255; g_ox = g_oy = 0;
    if (toast_on && !call_ui && scr != S_BOOT) { draw_toast(now); }
    plat_vsync(!busy);
    gfx_flush();
    return busy;
}

static void ui_dfu(void)
{
    if (sleeping) { plat_display(true); sleeping = false; }
    plat_bright(uc ? uc->bright : 160);
    gfx_begin(C_BG);
    word(20, 190, FT, "updating", C_INK, 255);
    word(20, 232, FB, "keep it near the laptop.", C_Q, 255);
    word(20, 258, FB, "fob restarts by itself.", RGB(110, 110, 110), 255);
    gfx_invalidate_all();
    gfx_flush();
}
