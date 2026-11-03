/* ====================================================================================
 * ONKAR OS user interface (v19: scroll detents, crown press/release clicks, low-battery mode, always starts). Portable C on top of onkar_gfx.inc (also runs in sim/sim.c).
 *
 * Navigation (watchOS-like):
 *   watch face  -- swipe left/right: next face (follows the finger) -- swipe up from the bottom: control centre
 *               -- button: app list
 *   app list    -- drag / crown: scroll with inertia -- tap: open app -- button: back to the face
 *   apps        -- swipe right (from anywhere that isn't a canvas): back, follows the finger -- button: face
 *   button long press: screen off. Screen off: tap, button or crown wakes it.
 *   battery < 6% (real battery only): low-battery mode = dark, touch/crown/haptics off, button shows the battery for 4 s.
 * Platform (onkar_os.inc or the simulator) provides the plat_* functions below.
 * ==================================================================================== */

typedef struct { uint8_t bright, always_on, autostart, timeout, face, accent, haptic, spare2; } T_UI_CFG;   /* haptic 0 off 1 light 2 medium 3 strong */

/* haptic patterns (the platform plays them; 1 ms accurate on the watch) */
enum { H_TICK, H_SOFT, H_TAP, H_CLICK, H_DOUBLE, H_SUCCESS, H_LONG, H_BUMP, H_ALARM, H_BOOT, H_SCROLL, H_PRESS, H_RELEASE, H_N };
static void plat_haptic(uint8_t id);
static int plat_batt(int *mv);          /* 0..100 %, or -1 = no battery seen (charger-only supply) */

static uint32_t plat_ms(void);
static void plat_bright(uint8_t v);
static void plat_display(bool on);
static bool plat_bt(void);
static uint32_t plat_now_s(void);
static bool plat_time_ok(void);
static void plat_save(void);
static bool plat_info(int i, char *key, char *val);

/* ------------------------------------------------------------------ palette (Apple dark) */
#define C_BG      0x0000
#define C_WHITE   0xFFFF
#define C_CARD    RGB(28, 28, 30)
#define C_CARD2   RGB(44, 44, 46)
#define C_CARD3   RGB(58, 58, 60)
#define C_SEC     RGB(142, 142, 147)
#define C_TER     RGB(99, 99, 102)
#define C_BLUE    RGB(10, 132, 255)
#define C_GREEN   RGB(48, 209, 88)
#define C_ORANGE  RGB(255, 159, 10)
#define C_RED     RGB(255, 69, 58)
#define C_YELLOW  RGB(255, 214, 10)
#define C_PINK    RGB(255, 55, 95)
#define C_PURPLE  RGB(191, 90, 242)
#define C_TEAL    RGB(64, 200, 224)
#define C_MINT    RGB(99, 230, 190)
#define C_INDIGO  RGB(94, 92, 230)
#define CX        (LCD_W / 2)

static const uint16_t ACCENTS[] = {C_ORANGE, C_BLUE, C_GREEN, C_PINK, C_PURPLE, C_TEAL};
#define N_ACCENT 6
static const char *const WDAY[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *const MON[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const uint16_t TIMEOUTS[] = {10, 15, 30, 60, 300};
#define N_TIMEOUT 5
static const char *const FACE_NAMES[] = {"Stack", "Analog", "Modular"};
#define N_FACE 3

static T_UI_CFG *uc;
#define ACC (ACCENTS[uc->accent % N_ACCENT])

/* ------------------------------------------------------------------ small helpers */
static char *u_num(char *p, uint32_t v) { char t[11]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n) { *p++ = t[--n]; } *p = 0; return p; }
static char *u_2(char *p, uint32_t v) { *p++ = (char)('0' + (v / 10) % 10); *p++ = (char)('0' + v % 10); *p = 0; return p; }
static char *u_s(char *p, const char *s) { while (*s) { *p++ = *s++; } *p = 0; return p; }

static float e_out(float t) { t = 1.f - t; return 1.f - t * t * t; }
static float e_inout(float t) { float u = 2.f - 2.f * t; return t < 0.5f ? 4.f * t * t * t : 1.f - u * u * u / 2.f; }
static float pow_n(float b, float n) { float r = 1.f; for (int k = (int)(n + 0.5f); k; k >>= 1, b *= b) { if (k & 1) { r *= b; } } return r; }
static float e_back(float t) { const float c1 = 1.2f, c3 = c1 + 1.f; float u = t - 1.f; return 1.f + c3 * u * u * u + c1 * u * u; }
static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

enum { E_OUT, E_INOUT, E_BACK, E_LIN };
typedef struct { float a, b; uint32_t t0; uint16_t dur; uint8_t ease; } anim_t;
static float an_val(const anim_t *an, uint32_t now)
{
    if (!an->dur || now - an->t0 >= an->dur) { return an->b; }
    float t = (float)(now - an->t0) / an->dur;
    float k = an->ease == E_OUT ? e_out(t) : an->ease == E_INOUT ? e_inout(t) : an->ease == E_BACK ? e_back(t) : t;
    return an->a + (an->b - an->a) * k;
}
static bool an_busy(const anim_t *an, uint32_t now) { return an->dur && now - an->t0 < an->dur; }
static void an_go(anim_t *an, float to, uint16_t dur, uint32_t now, uint8_t ease)
{
    an->a = an_val(an, now); an->b = to; an->t0 = now; an->dur = dur; an->ease = ease;
}
static void an_set(anim_t *an, float v) { an->a = an->b = v; an->dur = 0; }

static void civil(uint32_t days, int *y, int *m, int *d)    /* days since 1970-01-01 -> date */
{
    int32_t z = (int32_t)days + 719468, era = z / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)((int32_t)yoe + era * 400 + (*m <= 2));
}

static void inval(int x0, int y0, int x1, int y1) { gfx_invalidate(x0 + g_ox, y0 + g_oy, x1 + g_ox, y1 + g_oy); }

/* ------------------------------------------------------------------ UI state */
enum { S_BOOT, S_FACE, S_APPS, S_STOPWATCH, S_TIMER, S_SKETCH, S_FLASH, S_BREATHE, S_SETTINGS, S_ABOUT, S_LOW, S_N };
static uint8_t scr = S_BOOT, scr_prev;
static bool full_dirty = true;
static uint32_t boot_t0, last_input, last_sec = 0xFFFFFFFF, last_min = 0xFFFFFFFF;
static bool sleeping, aod, sleep_pending;
static bool lowbat;             /* low-battery mode (set by the platform) */
static anim_t bright_an;
static bool last_bt;

/* transitions: 0 = none, 1 = push (new from the right), 2 = pop (back), 3 = rise (apps over face), 4 = fall */
static uint8_t tr_kind;
static anim_t tr;               /* 0..1 */
static bool tr_live;            /* progress follows the finger */
static uint8_t tr_from, tr_to;

/* touch gesture */
enum { TS_IDLE, TS_PRESS, TS_DRAG, TS_DONE };
static uint8_t ts;
static int t_x0, t_y0, t_x, t_y, t_axis;          /* axis 1 = horizontal, 2 = vertical */
static uint32_t t_t0;
static int t_hx[6], t_hy[6]; static uint32_t t_ht[6]; static uint8_t t_hn;
static bool swallow;            /* touch that woke the screen */

/* scroll lists */
typedef struct { float pos, vel, max, d0, rowh; anim_t an; bool drag, anim, edge; uint32_t lt, ht; int row; } scroll_t;
static scroll_t sc_apps, sc_set, sc_about;

/* face paging + control centre */
static anim_t face_an;          /* fractional face index while swiping */
static int face_det;            /* face the finger is over (detent tick when it changes) */
static bool face_drag;
static anim_t cc_an;            /* control centre 0 hidden .. 1 shown */
static bool cc_drag;
static int press_id = -1;       /* highlighted button while finger is down */

/* app state */
static bool sw_run; static uint32_t sw_acc, sw_t0, sw_laps[3]; static uint8_t sw_nlap;
static uint8_t tm_state, tm_min = 5; static uint32_t tm_total, tm_end, tm_left; static uint32_t tm_last_q;
#define SK_MAX 500
static int16_t sk_x[SK_MAX], sk_y[SK_MAX]; static uint8_t sk_c[SK_MAX]; static uint16_t sk_n; static uint8_t sk_col; static float sk_w = 5.f;
static uint8_t fl_mode; static uint32_t fl_t0;
static uint8_t br_state, br_min = 1; static uint32_t br_t0;
static uint32_t save_at;
static float an_sec_from, an_sec_to; static uint32_t an_sec_t0; static float an_drawn[3] = {-999, -999, -999};

static uint32_t hz_alarm, hz_breath;     /* next repeat of the alarm / next breathe tap */
static uint8_t hz_slider = 0xFF;         /* brightness slider: tick at every 10% */

static void ui_go(uint8_t s, uint8_t kind, uint32_t now);
static uint16_t sk_color(uint8_t i);

/* ------------------------------------------------------------------ common pieces */
static void time_hm(uint32_t *h, uint32_t *m, uint32_t *s)
{
    uint32_t t = plat_now_s() % 86400u;
    *h = t / 3600; *m = (t / 60) % 60; *s = t % 60;
}

static void draw_header(const char *title, uint16_t c)
{
    uint32_t h, m, s; char b[8];
    time_hm(&h, &m, &s);
    char *p = u_num(b, h); *p++ = ':'; u_2(p, m);
    g_text(26, 16, FM, c, 255, title);
    g_text_r(LCD_W - 26, 16, FM, C_WHITE, 255, b);
}

static void draw_pill_btn(float x, float y, float w, float h, uint16_t bg, uint8_t bga, const char *label, uint16_t fg, bool pressed)
{
    g_rrect(x, y, w, h, h / 2, bg, (uint8_t)(pressed ? bga / 2 : bga));
    g_text_c((int)(x + w / 2), (int)(y + (h - FONT_TAB[FM]->line_h) / 2), FM, fg, pressed ? 160 : 255, label);
}

static void draw_toggle(float x, float y, bool on, float k)          /* k 0..1 knob position */
{
    g_rrect(x, y, 64, 38, 19, g_mix(C_CARD3, C_GREEN, k), 255);
    g_circle(x + 19 + 26 * k, y + 19, 15.5f, C_WHITE, 255);
    (void)on;
}

/* ------------------------------------------------------------------ boot animation */
static const uint16_t BOOT_C[3] = {C_ORANGE, C_PINK, C_PURPLE};
static void draw_boot(uint32_t now)
{
    uint32_t t = now - boot_t0;
    for (int i = 0; i < 3; i++)
    {
        float R = 132.f - i * 30.f, k = clampf(((float)t - 80.f - i * 110.f) / 950.f, 0, 1);
        g_ring(CX, 200, R, R - 22, BOOT_C[i], 46);
        g_arc(CX, 200, R, R - 22, 0, 360.f * e_inout(k), BOOT_C[i], 255, true);
    }
    float wa = clampf(((float)t - 900.f) / 450.f, 0, 1);
    g_text_c(CX, 360, FL, C_WHITE, (uint8_t)(255 * e_out(wa)), "ONKAR");
    g_text_c(CX, 404, FS, C_SEC, (uint8_t)(255 * e_out(clampf(((float)t - 1100.f) / 450.f, 0, 1))), "OS 19");
    inval(CX - 140, 60, CX + 140, 340);
    if (t >= 900 && t < 1600) { inval(0, 350, LCD_W, 440); }
}

/* ------------------------------------------------------------------ watch faces */
static void face_stack(uint32_t now, bool live)
{
    uint32_t h, m, s; char b[24], *p;
    time_hm(&h, &m, &s);
    uint32_t t = plat_now_s(); int yy, mo, d;
    civil(t / 86400u, &yy, &mo, &d);
    if (plat_time_ok())
    {
        p = u_s(b, WDAY[(t / 86400u + 4) % 7]); p = u_s(p, " "); p = u_num(p, (uint32_t)d); *p++ = ' '; u_s(p, MON[mo - 1]);
        g_text_c(CX, 22, FM, ACC, 255, b);
    }
    else { g_text_c(CX, 22, FM, C_SEC, 255, "Time not synced"); }
    const font2_t *f = FONT_TAB[FH];
    int oy = f->g['0' - f->first].oy, cw = f->g['0' - f->first].adv;
    u_2(b, h);
    g_text(CX - cw, 70 - oy, FH, C_WHITE, 255, b);
    u_2(b, m);
    g_text(CX - cw, 236 - oy, FH, ACC, 255, b);
    bool bt = plat_bt();
    g_icon_c(CX, 418, FIS, bt ? F_IS_BLUETOOTH : F_IS_BLUETOOTH_DISABLED, bt ? C_BLUE : C_TER, 255);
    (void)now; (void)live;
}

static void hand(float cx, float cy, float ang, float r0, float r1, float hw, uint16_t c)
{
    float sn = sinf(ang * 0.0174533f), cs = cosf(ang * 0.0174533f);
    g_line(cx + sn * r0, cy - cs * r0, cx + sn * r1, cy - cs * r1, hw, c, 255);
}

static void hand_box(float ang, float r0, float r1, float hw, int *bx)
{
    float sn = sinf(ang * 0.0174533f), cs = cosf(ang * 0.0174533f);
    float xa = CX + sn * r0, ya = 224 - cs * r0, xb = CX + sn * r1, yb = 224 - cs * r1;
    int x0 = (int)fminf(xa, xb) - (int)hw - 3, y0 = (int)fminf(ya, yb) - (int)hw - 3;
    int x1 = (int)fmaxf(xa, xb) + (int)hw + 4, y1 = (int)fmaxf(ya, yb) + (int)hw + 4;
    bx[0] = g_min(bx[0], x0); bx[1] = g_min(bx[1], y0); bx[2] = g_max(bx[2], x1); bx[3] = g_max(bx[3], y1);
}

static void face_analog(uint32_t now, bool live)
{
    const float cx = CX, cy = 224;
    uint32_t h, m, s;
    time_hm(&h, &m, &s);
    for (int i = 0; i < 60; i++)
    {
        float a = i * 6.f * 0.0174533f, sn = sinf(a), cs = cosf(a);
        if (i % 5) { g_line(cx + sn * 160, cy - cs * 160, cx + sn * 170, cy - cs * 170, 1.1f, C_TER, 255); }
        else { g_line(cx + sn * 142, cy - cs * 142, cx + sn * 169, cy - cs * 169, 3.2f, C_WHITE, 255); }
    }
    uint32_t t = plat_now_s(); int yy, mo, d; char b[8];
    civil(t / 86400u, &yy, &mo, &d);
    const char *wd = WDAY[(t / 86400u + 4) % 7];
    b[0] = wd[0]; b[1] = wd[1]; b[2] = wd[2]; b[3] = 0;
    g_text_c((int)cx + 82, (int)cy - 40, FS, C_SEC, 255, b);
    u_num(b, (uint32_t)d);
    g_text_c((int)cx + 82, (int)cy - 16, FL, ACC, 255, b);
    float sa = an_val(&(anim_t){an_sec_from, an_sec_to, an_sec_t0, 260, E_BACK}, now);
    sa = roundf(sa * 20.f) / 20.f;                    /* quantised so 'unchanged' is exact (no sub-pixel drift left on screen) */
    float ma = m * 6.f + s * 0.1f, ha = (h % 12) * 30.f + m * 0.5f;
    hand(cx, cy, ha, 0, 22, 2.5f, C_WHITE);
    hand(cx, cy, ha, 22, 96, 6.5f, C_WHITE);
    hand(cx, cy, ma, 0, 22, 2.5f, C_WHITE);
    hand(cx, cy, ma, 22, 152, 5.5f, C_WHITE);
    g_circle(cx, cy, 7.5f, C_WHITE, 255);
    hand(cx, cy, sa, -30, 162, 1.6f, ACC);
    g_circle(cx, cy, 5.5f, ACC, 255);
    g_circle(cx, cy, 2.2f, C_BG, 255);
    if (!live)
    {
        int bx[4] = {9999, 9999, -9999, -9999};
        bool any = false;
        float now_a[3] = {sa, ma, ha};
        for (int i = 0; i < 3; i++)
        {
            if (now_a[i] == an_drawn[i]) { continue; }
            float r1 = i == 0 ? 162 : (i == 1 ? 152 : 96), hw = i == 0 ? 6 : 7;
            hand_box(now_a[i], i == 0 ? -30 : 0, r1, hw, bx);
            if (an_drawn[i] > -900) { hand_box(an_drawn[i], i == 0 ? -30 : 0, r1, hw, bx); }
            an_drawn[i] = now_a[i];
            any = true;
        }
        if (any) { gfx_invalidate(bx[0] + g_ox, bx[1] + g_oy, bx[2] + g_ox, bx[3] + g_oy); }
    }
}

static void card_line(int y, int icon, uint16_t ic, const char *k, const char *v, uint16_t vc)
{
    g_rrect(16, (float)y, LCD_W - 32, 80, 24, C_CARD, 255);
    g_circle(52, y + 40, 22, ic, 50);
    g_icon_c(52, y + 40, FIS, icon, ic, 255);
    g_text(90, y + 9, FS, C_SEC, 255, k);
    g_text(90, y + 36, FM, vc, 255, v);
}

static void fmt_ms(char *p, uint32_t ms, bool cs)            /* m:ss(.cc) */
{
    uint32_t sec = ms / 1000;
    p = u_2(p, sec / 60); *p++ = ':'; p = u_2(p, sec % 60);
    if (cs) { *p++ = '.'; u_2(p, (ms / 10) % 100); }
}

static uint32_t sw_now(uint32_t now) { return sw_acc + (sw_run ? now - sw_t0 : 0); }
static uint32_t tm_remaining(uint32_t now) { return tm_state == 1 ? (tm_end > now ? tm_end - now : 0) : tm_left; }

static void face_modular(uint32_t now, bool live)
{
    uint32_t h, m, s; char b[24], *p;
    time_hm(&h, &m, &s);
    uint32_t t = plat_now_s(); int yy, mo, d;
    civil(t / 86400u, &yy, &mo, &d);
    const char *wd = WDAY[(t / 86400u + 4) % 7];
    p = b; *p++ = wd[0]; *p++ = wd[1]; *p++ = wd[2]; *p++ = ' '; p = u_num(p, (uint32_t)d); *p++ = ' '; u_s(p, MON[mo - 1]);
    g_text(24, 18, FM, plat_time_ok() ? ACC : C_SEC, 255, plat_time_ok() ? b : "Not synced");
    bool bt = plat_bt();
    g_icon_c(LCD_W - 40, 36, FIS, bt ? F_IS_BLUETOOTH : F_IS_BLUETOOTH_DISABLED, bt ? C_BLUE : C_TER, 255);
    p = u_num(b, h); *p++ = ':'; u_2(p, m);
    int w = gfx_text_w(FD, b);
    g_text(20, 52, FD, C_WHITE, 255, b);
    u_2(b, s);
    g_text(20 + w + 8, 96, FL, C_SEC, 255, b);
    if (!live) { inval(20 + w, 90, LCD_W, 150); }

    if (sw_run || sw_acc) { fmt_ms(b, sw_now(now), false); } else { u_s(b, "Ready"); }
    card_line(176, F_IS_TIMER, C_ORANGE, "Stopwatch", b, sw_run ? C_ORANGE : C_WHITE);
    if (tm_state == 1 || tm_state == 2) { fmt_ms(b, tm_remaining(now) + 999, false); } else { u_s(b, "Not running"); }
    card_line(264, F_IS_TIMER, C_YELLOW, "Timer", b, tm_state == 1 ? C_YELLOW : C_WHITE);
    uint32_t up = now / 1000;
    p = u_num(b, up / 3600); p = u_s(p, "h "); p = u_num(p, (up / 60) % 60); u_s(p, "m");
    card_line(352, F_IS_WATCH, C_MINT, "Running for", b, C_WHITE);
    if (!live && (sw_run || tm_state == 1)) { inval(16, 176, LCD_W - 16, 344); }
}

static void draw_face_n(int i, uint32_t now, bool live)
{
    switch (i)
    {
    case 0: face_stack(now, live); break;
    case 1: face_analog(now, live); break;
    default: face_modular(now, live); break;
    }
}

static void draw_faces(uint32_t now)
{
    float f = an_val(&face_an, now);
    bool live = face_drag || an_busy(&face_an, now);
    if (!live) { draw_face_n(uc->face, now, false); return; }
    int i0 = (int)floorf(f);
    float frac = f - i0;
    int ox0 = g_ox;
    for (int k = 0; k < 2; k++)
    {
        int i = i0 + k;
        if (i < 0 || i >= N_FACE) { continue; }
        int off = (int)lroundf((k - frac) * LCD_W);
        if (off <= -LCD_W || off >= LCD_W) { continue; }
        g_ox = ox0 + off;
        g_set_clip(0, 0, LCD_W, LCD_H);
        draw_face_n(i, now, true);
    }
    g_ox = ox0; g_reset_clip();
    for (int i = 0; i < N_FACE; i++)            /* page dots */
    {
        float k = 1.f - clampf(fabsf(f - i), 0, 1);
        g_circle(CX - 18 + i * 18, 438, 4.f, g_mix(C_CARD3, C_WHITE, k), 255);
    }
    inval(0, 0, LCD_W, LCD_H);
}

/* ------------------------------------------------------------------ control centre */
enum { CC_FLASH, CC_AOD, CC_SLEEP, CC_BT, CC_FACE, CC_SET, CC_N };
static void cc_btn_pos(int i, float *x, float *y) { *x = 74.f + (i % 3) * 110.f; *y = 132.f + (i / 3) * 112.f; }

static void draw_cc(uint32_t now)
{
    float s = an_val(&cc_an, now);
    float top = (1.f - s) * LCD_H;
    int oy0 = g_oy;
    g_rrect(0, top, LCD_W, LCD_H + 40, 34, C_BG, 255);
    g_rrect(CX - 22, top + 10, 44, 6, 3, C_CARD3, 255);
    g_oy = oy0 + (int)lroundf(top);
    char b[8]; uint32_t h, m, sec; time_hm(&h, &m, &sec);
    char *p = u_num(b, h); *p++ = ':'; u_2(p, m);
    g_text_c(CX, 30, FM, C_WHITE, 255, b);
    {
        int mv, pc = plat_batt(&mv);
        if (pc >= 0) { p = u_num(b, (uint32_t)pc); u_s(p, "%"); }
        g_text_r(LCD_W - 34, 34, FS, pc < 0 ? C_SEC : pc < 20 ? C_RED : C_GREEN, 255, pc < 0 ? "Plugged" : b);
    }
    for (int i = 0; i < CC_N; i++)
    {
        float x, y; cc_btn_pos(i, &x, &y);
        bool on = (i == CC_AOD && uc->always_on) || (i == CC_BT && plat_bt());
        uint16_t bg = on ? (i == CC_BT ? C_BLUE : C_WHITE) : C_CARD2;
        uint16_t fg = on ? (i == CC_BT ? C_WHITE : C_BG) : C_WHITE;
        bool pr = press_id == 100 + i;
        g_circle(x, y, pr ? 44 : 48, bg, 255);
        int ic = i == CC_FLASH ? F_I_FLASHLIGHT_ON : i == CC_AOD ? F_I_DARK_MODE : i == CC_SLEEP ? F_I_POWER_SETTINGS_NEW :
                 i == CC_BT ? (plat_bt() ? F_I_BLUETOOTH : F_I_BLUETOOTH_DISABLED) : i == CC_FACE ? F_I_WATCH : F_I_SETTINGS;
        g_icon_c((int)x, (int)y, FI, ic, fg, 255);
    }
    /* brightness slider */
    float k = clampf((uc->bright - 16) / 239.f, 0, 1);
    g_rrect(24, 336, LCD_W - 48, 64, 32, C_CARD2, 255);
    g_rrect(24, 336, 64 + (LCD_W - 48 - 64) * k, 64, 32, C_WHITE, 255);
    g_icon_c(56, 368, FI, F_I_LIGHT_MODE, C_BG, 255);
    g_oy = oy0;
    inval(0, (int)top - 2, LCD_W, LCD_H);
}

/* ------------------------------------------------------------------ app list */
typedef struct { const char *name; uint16_t c; uint8_t icon; uint8_t scr; uint16_t fg; } app_t;
static const app_t APPS[] = {
    {"Stopwatch", C_ORANGE, 0, S_STOPWATCH, C_WHITE},
    {"Timer", C_YELLOW, 1, S_TIMER, C_BG},
    {"Sketch", C_PINK, 2, S_SKETCH, C_WHITE},
    {"Flashlight", C_CARD3, 3, S_FLASH, C_YELLOW},
    {"Breathe", C_MINT, 4, S_BREATHE, C_BG},
    {"Settings", RGB(99, 99, 102), 5, S_SETTINGS, C_WHITE},
    {"About", C_BLUE, 6, S_ABOUT, C_WHITE},
};
#define N_APPS 7
#define APP_ROW 92
#define APP_TOP 64

static float edge_fade(float cy)
{
    float a = clampf((cy - 8.f) / 70.f, 0, 1), b = clampf((LCD_H - 8.f - cy) / 70.f, 0, 1);
    return fminf(a, b);
}

static void draw_apps(uint32_t now)
{
    (void)now;
    float pos = sc_apps.pos;
    for (int i = 0; i < N_APPS; i++)
    {
        float cy = APP_TOP + 46 + i * APP_ROW - pos;
        if (cy < -50 || cy > LCD_H + 50) { continue; }
        float f = edge_fade(cy);
        uint8_t a = (uint8_t)(255 * (0.12f + 0.88f * f));
        float ind = (1.f - f) * 22.f;
        bool pr = press_id == i;
        float r = pr ? 32 : 35;
        g_circle(26 + 35 + ind, cy, r, APPS[i].c, a);
        g_icon_c((int)(61 + ind), (int)cy, FI, APPS[i].icon, APPS[i].fg, a);
        g_text((int)(114 + ind), (int)cy - FONT_TAB[FM]->line_h / 2, FM, C_WHITE, (uint8_t)(pr ? a / 2 : a), APPS[i].name);
    }
    g_rect(0, 0, LCD_W, 56, C_BG, 255);
    draw_header("", C_WHITE);
}

/* ------------------------------------------------------------------ scroll physics */
static float rubber(float over) { return over * 0.45f / (1.f + over / 260.f); }

/* detent: a light tick each time a row passes (finger or inertia), at most one per 30 ms */
static void sc_tick(scroll_t *s, uint32_t now)
{
    if (s->pos < 0 || s->pos > s->max) { return; }
    int r = (int)(s->pos / s->rowh);
    if (r != s->row) { s->row = r; if (now - s->ht > 30) { s->ht = now; plat_haptic(H_SCROLL); } }
}

static void sc_drag(scroll_t *s, float dy, bool start)
{
    uint32_t now = plat_ms();
    if (start) { s->d0 = s->pos; s->drag = true; s->anim = false; s->vel = 0; s->edge = false; s->row = (int)(clampf(s->pos, 0, s->max) / s->rowh); }
    float raw = s->d0 - dy;
    bool over = raw < 0 || raw > s->max;
    if (over && !s->edge && s->max > 0) { plat_haptic(H_BUMP); }        /* finger pulls past the end */
    s->edge = over;
    if (raw < 0) { s->pos = -rubber(-raw); }
    else if (raw > s->max) { s->pos = s->max + rubber(raw - s->max); }
    else { s->pos = raw; }
    if (!start) { sc_tick(s, now); }
}

static void sc_release(scroll_t *s, float vy, uint32_t now)
{
    s->drag = false;
    s->vel = -vy;
    if (s->vel > 3.f) { s->vel = 3.f; }
    if (s->vel < -3.f) { s->vel = -3.f; }
    s->lt = now;
}

static void sc_crown(scroll_t *s, int d, uint32_t now)
{
    float from = s->anim ? s->an.b : s->pos, target = clampf(from + d * 46.f, 0, s->max);
    plat_haptic(fabsf(target - from) < 0.5f ? H_BUMP : H_TICK);
    s->vel = 0; s->anim = true;
    an_set(&s->an, s->pos);
    an_go(&s->an, target, 200, now, E_OUT);
}

static bool sc_step(scroll_t *s, uint32_t now)       /* true while moving */
{
    if (s->drag) { return true; }
    if (s->anim)
    {
        s->pos = an_val(&s->an, now);
        s->row = (int)(clampf(s->pos, 0, s->max) / s->rowh);    /* crown already ticked */
        if (!an_busy(&s->an, now)) { s->anim = false; }
        return true;
    }
    float dt = (float)(now - s->lt);
    s->lt = now;
    if (dt > 50) { dt = 50; }
    if (s->pos < -0.5f || s->pos > s->max + 0.5f)
    {
        if (fabsf(s->vel) > 0.3f) { plat_haptic(H_BUMP); }
        s->vel = 0; s->anim = true;
        an_set(&s->an, s->pos);
        an_go(&s->an, s->pos < 0 ? 0 : s->max, 360, now, E_OUT);
        return true;
    }
    if (fabsf(s->vel) < 0.02f) { s->vel = 0; return false; }
    s->pos += s->vel * dt;
    sc_tick(s, now);
    s->vel *= pow_n(0.9965f, dt);
    if (s->pos < 0 || s->pos > s->max) { s->vel *= pow_n(0.9f, dt); }
    return true;
}

/* ------------------------------------------------------------------ stopwatch */
static void draw_stopwatch(uint32_t now)
{
    draw_header("Stopwatch", C_ORANGE);
    char b[16];
    uint32_t ms = sw_now(now);
    fmt_ms(b, ms, false);
    char c[4]; c[0] = '.'; u_2(c + 1, (ms / 10) % 100);
    int w1 = gfx_text_w(FD, b), w2 = gfx_text_w(FL, c), x = CX - (w1 + w2) / 2;
    g_text(x, 78, FD, C_WHITE, 255, b);
    g_text(x + w1, 118, FL, C_WHITE, 255, c);
    if (sw_run) { inval(0, 80, LCD_W, 186); }
    for (int i = 0; i < sw_nlap; i++)
    {
        int y = 196 + i * 42;
        char k[10], *p = u_s(k, "Lap "); u_num(p, (uint32_t)(sw_nlap - i));
        g_text(30, y, FM, C_SEC, 255, k);
        fmt_ms(b, sw_laps[i], true);
        g_text_r(LCD_W - 30, y, FM, C_WHITE, 255, b);
        g_rect(30, y + 39, LCD_W - 60, 1, C_CARD2, 255);
    }
    bool lap_ok = sw_run || sw_acc;
    g_circle(92, 376, press_id == 1 ? 46 : 50, C_CARD2, 255);
    g_text_c(92, 376 - 17, FM, C_WHITE, (uint8_t)(lap_ok ? 255 : 90), sw_run || !sw_acc ? "Lap" : "Reset");
    uint16_t c2 = sw_run ? C_RED : C_GREEN;
    g_circle(LCD_W - 92, 376, press_id == 2 ? 46 : 50, c2, 64);
    g_text_c(LCD_W - 92, 376 - 17, FM, c2, 255, sw_run ? "Stop" : "Start");
}

static void stopwatch_tap(int x, int y, uint32_t now)
{
    if (y < 310) { return; }
    if (x > CX)
    {
        if (sw_run) { sw_acc += now - sw_t0; sw_run = false; }
        else { sw_t0 = now; sw_run = true; }
        plat_haptic(H_CLICK);
    }
    else if (sw_run)
    {
        uint32_t t = sw_now(now), prev = 0;
        for (int i = 0; i < 3; i++) { (void)i; }
        static uint32_t lap_base;
        if (!sw_nlap) { lap_base = 0; }
        prev = lap_base; lap_base = t;
        memmove(&sw_laps[1], &sw_laps[0], 2 * sizeof(uint32_t));
        sw_laps[0] = t - prev;
        if (sw_nlap < 3) { sw_nlap++; }
        plat_haptic(H_TAP);
    }
    else if (sw_acc) { sw_acc = 0; sw_nlap = 0; plat_haptic(H_DOUBLE); }
    full_dirty = true;
}

/* ------------------------------------------------------------------ timer */
static const uint8_t TM_PRESET[6] = {1, 3, 5, 10, 15, 30};
static void tm_preset_box(int i, float *x, float *y) { *x = 24.f + (i % 3) * 110.f; *y = 216.f + (i / 3) * 70.f; }

static void draw_timer(uint32_t now)
{
    draw_header("Timer", C_YELLOW);
    char b[16];
    if (tm_state == 0)
    {
        b[0] = 0; char *p = u_2(b, tm_min); u_s(p, ":00");
        g_text_c(CX, 80, FD, C_WHITE, 255, b);
        g_text_c(CX, 180, FS, C_SEC, 255, "Turn the crown to adjust");
        for (int i = 0; i < 6; i++)
        {
            float x, y; tm_preset_box(i, &x, &y);
            bool sel = TM_PRESET[i] == tm_min;
            g_rrect(x, y, 100, 60, 30, sel ? C_YELLOW : C_CARD2, (uint8_t)(press_id == 10 + i ? 150 : 255));
            u_num(b, TM_PRESET[i]);
            g_text_c((int)x + 50, (int)y + 12, FM, sel ? C_BG : C_WHITE, 255, b);
        }
        draw_pill_btn(24, 362, LCD_W - 48, 68, C_YELLOW, 255, "Start", C_BG, press_id == 1);
        return;
    }
    if (tm_state == 3)
    {
        g_circle(CX, 190, 86, C_YELLOW, 255);
        g_icon_c(CX, 190, FI, 1, C_BG, 255);
        g_text_c(CX, 300, FL, C_WHITE, 255, "Time's up");
        g_text_c(CX, 350, FS, C_SEC, 255, "Tap to dismiss");
        return;
    }
    uint32_t q = (tm_remaining(now) + 249) / 250, left = q * 250;     /* quarter seconds: what's drawn = what was invalidated */
    float k = tm_total ? (float)left / tm_total : 0;
    g_ring(CX, 214, 150, 134, C_CARD2, 255);
    g_arc(CX, 214, 150, 134, 360.f * (1.f - k), 360.f, C_YELLOW, 255, true);
    fmt_ms(b, left + 750, false);
    g_text_c(CX, 166, FD, C_WHITE, 255, b);
    char c[16], *p = u_s(c, "of "); p = u_num(p, tm_total / 60000); u_s(p, " min");
    g_text_c(CX, 262, FS, tm_state == 2 ? C_YELLOW : C_SEC, 255, tm_state == 2 ? "Paused" : c);
    draw_pill_btn(24, 382, 150, 56, C_CARD2, 255, "Cancel", C_WHITE, press_id == 2);
    draw_pill_btn(LCD_W - 174, 382, 150, 56, C_YELLOW, 70, tm_state == 2 ? "Resume" : "Pause", C_YELLOW, press_id == 3);
    if (q != tm_last_q) { tm_last_q = q; inval(CX - 160, 58, CX + 160, 372); }
}

static void timer_tap(int x, int y, uint32_t now)
{
    if (tm_state == 0)
    {
        for (int i = 0; i < 6; i++)
        {
            float bx, by; tm_preset_box(i, &bx, &by);
            if (x >= bx && x < bx + 100 && y >= by && y < by + 60) { tm_min = TM_PRESET[i]; full_dirty = true; plat_haptic(H_TICK); return; }
        }
        if (y >= 350)
        {
            tm_total = tm_min * 60000u; tm_end = now + tm_total; tm_state = 1; full_dirty = true; plat_haptic(H_SUCCESS);
        }
        return;
    }
    if (tm_state == 3) { tm_state = 0; plat_bright(uc->bright); full_dirty = true; plat_haptic(H_TAP); return; }
    if (y >= 370)
    {
        plat_haptic(H_CLICK);
        if (x < CX) { tm_state = 0; }
        else if (tm_state == 1) { tm_left = tm_remaining(now); tm_state = 2; }
        else { tm_end = now + tm_left; tm_state = 1; }
        full_dirty = true;
    }
}

/* ------------------------------------------------------------------ sketch */
static uint16_t sk_color(uint8_t i)
{
    static const uint16_t P[6] = {C_WHITE, C_ORANGE, C_YELLOW, C_GREEN, C_TEAL, C_PINK};
    return P[i % 6];
}

static void sk_hook(uint16_t *row, int y, int x0, int x1, int bx)
{
    gprim_t p;
    p.t = G_LINE; p.a = 255;
    for (int i = 1; i < sk_n; i++)
    {
        if (sk_c[i] & 0x80) { continue; }
        float hw = (sk_c[i] & 0x70) / 16.f + 2.f;
        int ya = g_min(sk_y[i - 1], sk_y[i]) - (int)hw - 2, yb = g_max(sk_y[i - 1], sk_y[i]) + (int)hw + 2;
        if (y < ya || y > yb) { continue; }
        p.x = sk_x[i - 1]; p.y = sk_y[i - 1]; p.w = sk_x[i]; p.h = sk_y[i]; p.r = hw; p.c = sk_color(sk_c[i] & 15);
        g_row_line(&p, row - bx, y, x0, x1);
    }
    if (sk_n && (sk_c[sk_n - 1] & 0x80))           /* single dot just started */
    {
        float hw = (sk_c[sk_n - 1] & 0x70) / 16.f + 2.f;
        p.x = p.w = sk_x[sk_n - 1]; p.y = p.h = sk_y[sk_n - 1]; p.r = hw; p.c = sk_color(sk_c[sk_n - 1] & 15);
        g_row_line(&p, row - bx, y, x0, x1);
    }
}

static void draw_sketch(uint32_t now)
{
    (void)now;
    g_hook(0, 0, LCD_W, LCD_H, sk_hook);
    g_circle(42, 42, 26, C_CARD2, 220);
    g_icon_c(42, 42, FIS, F_IS_DELETE, C_WHITE, 255);
    for (int i = 0; i < 6; i++)
    {
        float x = 64.f + i * 48.f;
        g_circle(x, 414, 15, sk_color((uint8_t)i), 255);
        if (i == sk_col) { g_ring(x, 414, 22, 19, C_WHITE, 255); }
    }
}

static void sk_add(int x, int y, bool start)
{
    if (sk_n >= SK_MAX) { memmove(sk_x, sk_x + 100, (SK_MAX - 100) * 2); memmove(sk_y, sk_y + 100, (SK_MAX - 100) * 2); memmove(sk_c, sk_c + 100, SK_MAX - 100); sk_n -= 100; sk_c[0] |= 0x80; full_dirty = true; }
    int wq = (int)clampf((sk_w - 2.f) * 16.f, 0, 112) & 0x70;
    sk_x[sk_n] = (int16_t)x; sk_y[sk_n] = (int16_t)y; sk_c[sk_n] = (uint8_t)(sk_col | wq | (start ? 0x80 : 0));
    int hw = (int)sk_w + 4;
    if (start || !sk_n) { gfx_invalidate(x - hw, y - hw, x + hw, y + hw); }
    else
    {
        int px = sk_x[sk_n - 1], py = sk_y[sk_n - 1];
        gfx_invalidate(g_min(px, x) - hw, g_min(py, y) - hw, g_max(px, x) + hw, g_max(py, y) + hw);
    }
    sk_n++;
}

/* ------------------------------------------------------------------ flashlight */
static void draw_flash(uint32_t now)
{
    static const uint16_t FL_C[3] = {C_WHITE, RGB(255, 196, 128), RGB(255, 30, 20)};
    g_rect(0, 0, LCD_W, LCD_H, FL_C[fl_mode % 3], 255);
    float a = 1.f - clampf(((float)(now - fl_t0) - 1400.f) / 500.f, 0, 1);
    if (a > 0) { g_text_c(CX, 404, FS, RGB(90, 90, 90), (uint8_t)(255 * a), "Tap to change colour"); inval(0, 396, LCD_W, 436); }
}

/* ------------------------------------------------------------------ breathe */
static void draw_breathe(uint32_t now)
{
    float e = 0.f;
    const char *msg = "";
    float ma = 1.f;
    if (br_state == 1)
    {
        uint32_t t = now - br_t0, cyc = t % 9600u;
        if (cyc < 4000) { e = e_inout(cyc / 4000.f); msg = "Breathe in"; ma = clampf(cyc / 600.f, 0, 1); }
        else if (cyc < 4600) { e = 1.f; msg = "Hold"; }
        else { e = 1.f - e_inout((cyc - 4600) / 5000.f); msg = "Breathe out"; ma = clampf((cyc - 4600) / 600.f, 0, 1); }
        if (t >= br_min * 60000u) { br_state = 2; full_dirty = true; plat_haptic(H_SUCCESS); }
    }
    else if (br_state == 0) { e = 0.35f; }
    float pr = 34.f + 40.f * e, cd = 10.f + 62.f * e, rot = 60.f * e * 0.0174533f;
    for (int i = 0; i < 6; i++)
    {
        float a = rot + i * 1.0471976f;
        uint16_t c = g_mix(C_MINT, C_TEAL, (float)(i % 2) * 0.7f);
        g_circle(CX + cd * sinf(a), 200 - cd * cosf(a), pr, c, 120);
    }
    if (br_state == 1)
    {
        g_text_c(CX, 368, FL, C_WHITE, (uint8_t)(255 * ma), msg);
        inval(CX - 160, 40, CX + 160, 420);
        return;
    }
    if (br_state == 2)
    {
        g_rect(0, 0, LCD_W, LCD_H, C_BG, 170);
        g_text_c(CX, 172, FL, C_WHITE, 255, "Well done");
        g_text_c(CX, 216, FS, C_SEC, 255, "Tap to go again");
        return;
    }
    draw_header("Breathe", C_MINT);
    char b[12], *p = u_num(b, br_min); u_s(p, " min");
    g_text_c(CX, 314, FS, C_SEC, 255, "Turn the crown to set the length");
    draw_pill_btn(24, 352, LCD_W - 48, 72, C_MINT, 255, b, C_BG, press_id == 1);
}

/* ------------------------------------------------------------------ settings */
enum { ST_BRIGHT, ST_AOD, ST_TIMEOUT, ST_HAPTIC, ST_FACE, ST_ACCENT, ST_N };       /* v19: no "start on power": always on */
static const char *const HAP_NAMES[4] = {"Off", "Light", "Medium", "Strong"};
static anim_t tg_an[3];
#define SET_TOP 66
static float set_row_y(int i) { return SET_TOP + i * 96.f - sc_set.pos; }

static void draw_settings(uint32_t now)
{
    char b[24];
    for (int i = 0; i < ST_N; i++)
    {
        float y = set_row_y(i);
        if (y < -100 || y > LCD_H) { continue; }
        float f = edge_fade(y + 42);
        uint8_t a = (uint8_t)(255 * (0.15f + 0.85f * f));
        g_rrect(14, y, LCD_W - 28, 86, 26, C_CARD, (uint8_t)(press_id == 20 + i ? a / 2 : a));
        switch (i)
        {
        case ST_BRIGHT:
            g_text(34, (int)y + 8, FS, C_SEC, a, "Brightness");
            {
                float k = clampf((uc->bright - 16) / 239.f, 0, 1);
                g_rrect(34, y + 46, LCD_W - 68, 22, 11, C_CARD3, a);
                g_rrect(34, y + 46, 22 + (LCD_W - 68 - 22) * k, 22, 11, ACC, a);
            }
            break;
        case ST_AOD:
            g_text(34, (int)y + 25, FM, C_WHITE, a, "Always on");
            {
                uint8_t v = uc->always_on;
                anim_t *an = &tg_an[0];
                if (an->b != v) { an_go(an, v, 220, now, E_OUT); }
                float k = an_val(an, now);
                g_rrect(LCD_W - 34 - 64, y + 24, 64, 38, 19, g_mix(C_CARD3, C_GREEN, k), a);
                g_circle(LCD_W - 34 - 64 + 19 + 26 * k, y + 43, 15.5f, C_WHITE, a);
                if (an_busy(an, now)) { inval(LCD_W - 110, (int)y + 20, LCD_W - 20, (int)y + 66); }
            }
            break;
        case ST_TIMEOUT:
            g_text(34, (int)y + 25, FM, C_WHITE, a, "Screen off after");
            {
                uint16_t t = TIMEOUTS[uc->timeout % N_TIMEOUT];
                char *p = u_num(b, t >= 60 ? t / 60 : t); u_s(p, t >= 60 ? " min" : " s");
                g_text_r(LCD_W - 34, (int)y + 25, FM, ACC, a, b);
            }
            break;
        case ST_HAPTIC:
            g_text(34, (int)y + 25, FM, C_WHITE, a, "Haptics");
            g_text_r(LCD_W - 34, (int)y + 25, FM, ACC, a, HAP_NAMES[uc->haptic & 3]);
            break;
        case ST_FACE:
            g_text(34, (int)y + 25, FM, C_WHITE, a, "Watch face");
            g_text_r(LCD_W - 34, (int)y + 25, FM, ACC, a, FACE_NAMES[uc->face % N_FACE]);
            break;
        case ST_ACCENT:
            g_text(34, (int)y + 8, FS, C_SEC, a, "Colour");
            for (int k = 0; k < N_ACCENT; k++)
            {
                float x = 50.f + k * 54.f;
                g_circle(x, y + 58, 15, ACCENTS[k], a);
                if (k == uc->accent) { g_ring(x, y + 58, 22, 19, C_WHITE, a); }
            }
            break;
        default: break;
        }
    }
    g_rect(0, 0, LCD_W, 58, C_BG, 255);
    draw_header("Settings", C_SEC);
}

static void settings_change(void) { save_at = plat_ms() + 1500; if (!save_at) { save_at = 1; } full_dirty = true; }

static void settings_tap(int x, int y, uint32_t now)
{
    (void)now;
    for (int i = 0; i < ST_N; i++)
    {
        float ry = set_row_y(i);
        if (y < ry || y >= ry + 86) { continue; }
        switch (i)
        {
        case ST_BRIGHT:
            {
                int v = (int)(16 + 239.f * clampf((x - 34.f) / (LCD_W - 68.f), 0, 1));
                uc->bright = (uint8_t)v; plat_bright(uc->bright);
            }
            plat_haptic(H_TICK);
            break;
        case ST_AOD: uc->always_on ^= 1; plat_haptic(H_CLICK); break;
        case ST_TIMEOUT: uc->timeout = (uint8_t)((uc->timeout + 1) % N_TIMEOUT); plat_haptic(H_TICK); break;
        case ST_HAPTIC: uc->haptic = (uint8_t)((uc->haptic + 1) & 3); plat_haptic(H_CLICK); break;    /* plays at the new strength */
        case ST_FACE: uc->face = (uint8_t)((uc->face + 1) % N_FACE); an_set(&face_an, uc->face); plat_haptic(H_TICK); break;
        case ST_ACCENT:
            {
                int k = (int)lroundf((x - 50.f) / 54.f);
                if (k >= 0 && k < N_ACCENT) { uc->accent = (uint8_t)k; plat_haptic(H_TICK); }
            }
            break;
        default: break;
        }
        settings_change();
        return;
    }
}

/* ------------------------------------------------------------------ about */
static void draw_about(uint32_t now)
{
    (void)now;
    char k[24], v[32];
    for (int i = 0; plat_info(i, k, v); i++)
    {
        float y = 70 + i * 74.f - sc_about.pos;
        if (y < -80 || y > LCD_H) { continue; }
        uint8_t a = (uint8_t)(255 * (0.15f + 0.85f * edge_fade(y + 30)));
        g_text(26, (int)y, FS, C_SEC, a, k);
        g_text(26, (int)y + 26, FM, C_WHITE, a, v);
        g_rect(26, y + 66, LCD_W - 52, 1, C_CARD2, a);
    }
    g_rect(0, 0, LCD_W, 58, C_BG, 255);
    draw_header("About", C_BLUE);
    if ((now / 1000) != last_sec) { inval(0, 58, LCD_W, LCD_H); }
}

/* ------------------------------------------------------------------ low battery */
static void draw_low(uint32_t now)
{
    (void)now;
    int mv, pc = plat_batt(&mv);
    if (pc < 0) { pc = 0; }
    char b[8]; char *p = u_num(b, (uint32_t)pc); u_s(p, "%");
    g_rrect(CX - 70, 120, 140, 230, 26, C_CARD3, 255);
    g_rrect(CX - 62, 128, 124, 214, 20, C_BG, 255);
    g_rrect(CX - 26, 104, 52, 18, 7, C_CARD3, 255);
    float h = 198.f * clampf(pc / 100.f, 0.04f, 1.f);
    g_rrect(CX - 54, 136 + 198 - h, 108, h, 14, C_RED, 255);
    g_text_c(CX, 362, FM, C_WHITE, 255, b);
    g_text_c(CX, 412, FS, C_SEC, 255, "Low battery - charge");
    gfx_invalidate_all();
}

/* ------------------------------------------------------------------ scene dispatch */
static void draw_scene(uint8_t s, uint32_t now)
{
    switch (s)
    {
    case S_BOOT: draw_boot(now); break;
    case S_FACE: draw_faces(now); break;
    case S_APPS: draw_apps(now); break;
    case S_STOPWATCH: draw_stopwatch(now); break;
    case S_TIMER: draw_timer(now); break;
    case S_SKETCH: draw_sketch(now); break;
    case S_FLASH: draw_flash(now); break;
    case S_BREATHE: draw_breathe(now); break;
    case S_SETTINGS: draw_settings(now); break;
    case S_ABOUT: draw_about(now); break;
    case S_LOW: draw_low(now); break;
    default: break;
    }
}

static void ui_go(uint8_t s, uint8_t kind, uint32_t now)
{
    if (scr == S_FLASH && s != S_FLASH) { plat_bright(uc->bright); }
    tr_from = scr; tr_to = s; tr_kind = kind; tr_live = false;
    an_set(&tr, 0);
    if (kind) { an_go(&tr, 1, kind == 3 || kind == 4 ? 340 : 300, now, E_OUT); }
    scr_prev = scr; scr = s;
    press_id = -1;
    if (s == S_FLASH) { fl_t0 = now; plat_bright(255); }
    if (s == S_APPS && kind == 3) { sc_apps.pos = 0; sc_apps.vel = 0; }
    if (s == S_ABOUT) { sc_about.pos = 0; }
    full_dirty = true;
}

static uint8_t parent_of(uint8_t s) { return s == S_APPS ? S_FACE : (s == S_FACE || s == S_BOOT ? S_FACE : S_APPS); }

static void draw_transition(uint32_t now)
{
    float p = tr_live ? tr.b : an_val(&tr, now);
    int ox = g_ox, oy = g_oy;
    switch (tr_kind)
    {
    case 1: case 2:
        {
            bool push = tr_kind == 1;
            uint8_t under = push ? tr_from : tr_to, over = push ? tr_to : tr_from;
            float k = push ? p : 1.f - p;                       /* how far "over" has come in */
            g_ox = ox - (int)lroundf(k * LCD_W * 0.3f);
            g_alpha = (uint8_t)(255 * (1.f - 0.6f * k));
            draw_scene(under, now);
            g_alpha = 255;
            g_ox = ox + (int)lroundf((1.f - k) * LCD_W);
            g_rect(-24, 0, 24, LCD_H, C_BG, 140);
            g_rect(0, 0, LCD_W, LCD_H, C_BG, 255);
            draw_scene(over, now);
        }
        break;
    case 3: case 4:
        {
            float k = tr_kind == 3 ? p : 1.f - p;             /* app list rising over the face */
            g_alpha = (uint8_t)(255 * (1.f - k));
            draw_scene(S_FACE, now);
            g_alpha = (uint8_t)(255 * clampf(k * 1.4f, 0, 1));
            g_oy = oy + (int)lroundf((1.f - k) * 90.f);
            draw_scene(S_APPS, now);
            g_alpha = 255;
        }
        break;
    default: break;
    }
    g_ox = ox; g_oy = oy;
    gfx_invalidate_all();
}

/* ------------------------------------------------------------------ input */
static bool asleep(void) { return sleeping || aod || sleep_pending; }

static void wake(uint32_t now)
{
    last_input = now;
    if (!sleeping && !aod && !sleep_pending) { return; }
    float from = sleeping ? 0.f : an_val(&bright_an, now);
    if (sleeping) { plat_display(true); }
    sleeping = false; aod = false; sleep_pending = false;
    an_set(&bright_an, from);
    an_go(&bright_an, lowbat ? g_min(uc->bright, 70) : uc->bright, 260, now, E_OUT);
    full_dirty = true;
}

static void go_sleep(uint32_t now)
{
    if (scr != S_FACE && scr != S_BOOT && scr != S_LOW && !(scr == S_TIMER && tm_state) && !sw_run) { ui_go(S_FACE, 0, now); }
    if (cc_an.b > 0) { an_set(&cc_an, 0); }
    if (uc->always_on && !lowbat) { aod = true; an_go(&bright_an, g_max(uc->bright / 6, 6), 500, now, E_INOUT); }
    else { an_go(&bright_an, 0, 300, now, E_INOUT); sleep_pending = true; }
}

static int hit_press(int x, int y)       /* which button the finger is on (for the pressed look) */
{
    if (cc_an.b > 0.5f)
    {
        for (int i = 0; i < CC_N; i++)
        {
            float bx, by; cc_btn_pos(i, &bx, &by);
            if ((x - bx) * (x - bx) + (y - by) * (y - by) < 52 * 52) { return 100 + i; }
        }
        return -1;
    }
    switch (scr)
    {
    case S_APPS:
        {
            int i = (int)floorf((y + sc_apps.pos - APP_TOP) / APP_ROW);
            return (y > 56 && i >= 0 && i < N_APPS) ? i : -1;
        }
    case S_STOPWATCH: return y > 320 ? (x > CX ? 2 : 1) : -1;
    case S_TIMER:
        if (tm_state == 0)
        {
            for (int i = 0; i < 6; i++) { float bx, by; tm_preset_box(i, &bx, &by); if (x >= bx && x < bx + 100 && y >= by && y < by + 60) { return 10 + i; } }
            return y > 350 ? 1 : -1;
        }
        return y > 372 ? (x < CX ? 2 : 3) : -1;
    case S_BREATHE: return br_state == 0 && y > 340 ? 1 : -1;
    case S_SETTINGS:
        for (int i = 1; i < ST_N - 1; i++) { float ry = set_row_y(i); if (y >= ry && y < ry + 86) { return 20 + i; } }
        return -1;
    default: return -1;
    }
}

static void on_tap(int x, int y, uint32_t now)
{
    if (cc_an.b > 0.5f)
    {
        for (int i = 0; i < CC_N; i++)
        {
            float bx, by; cc_btn_pos(i, &bx, &by);
            if ((x - bx) * (x - bx) + (y - by) * (y - by) > 52 * 52) { continue; }
            plat_haptic(H_CLICK);
            switch (i)
            {
            case CC_FLASH: an_set(&cc_an, 0); ui_go(S_FLASH, 1, now); break;
            case CC_AOD: uc->always_on ^= 1; settings_change(); break;
            case CC_SLEEP: an_set(&cc_an, 0); go_sleep(now); break;
            case CC_FACE: uc->face = (uint8_t)((uc->face + 1) % N_FACE); an_set(&face_an, uc->face); settings_change(); break;
            case CC_SET: an_set(&cc_an, 0); ui_go(S_SETTINGS, 1, now); break;
            default: break;
            }
            full_dirty = true;
            return;
        }
        if (y > 330 && y < 410) { uc->bright = (uint8_t)(16 + 239.f * clampf((x - 56.f) / (LCD_W - 112.f), 0, 1)); plat_bright(uc->bright); settings_change(); plat_haptic(H_TICK); }
        return;
    }
    switch (scr)
    {
    case S_APPS:
        {
            int i = hit_press(x, y);
            if (i >= 0) { ui_go(APPS[i].scr, 1, now); plat_haptic(H_TAP); }
        }
        break;
    case S_STOPWATCH: stopwatch_tap(x, y, now); break;
    case S_TIMER: timer_tap(x, y, now); break;
    case S_SKETCH:
        if (x < 80 && y < 80) { sk_n = 0; full_dirty = true; plat_haptic(H_CLICK); break; }
        if (y > 390) { int k = (int)lroundf((x - 64.f) / 48.f); if (k >= 0 && k < 6) { sk_col = (uint8_t)k; full_dirty = true; plat_haptic(H_TICK); break; } }
        sk_add(x, y, true);
        break;
    case S_FLASH: fl_mode = (uint8_t)((fl_mode + 1) % 3); fl_t0 = now; full_dirty = true; plat_haptic(H_TICK); break;
    case S_BREATHE:
        if (br_state == 0 && y > 340) { br_state = 1; br_t0 = now; full_dirty = true; hz_breath = now + 300; plat_haptic(H_SUCCESS); }
        else if (br_state == 2) { br_state = 0; full_dirty = true; plat_haptic(H_TAP); }
        break;
    case S_SETTINGS: settings_tap(x, y, now); break;
    default: break;
    }
}

static void ui_touch(bool down, int x, int y, uint32_t now)
{
    if (scr == S_BOOT || lowbat) { return; }
    if (down && ts == TS_IDLE && (sleeping || aod || sleep_pending)) { wake(now); swallow = true; }
    if (swallow) { if (!down) { swallow = false; } return; }
    if (down) { last_input = now; }
    if (down && ts == TS_IDLE)
    {
        ts = TS_PRESS; t_x0 = t_x = x; t_y0 = t_y = y; t_t0 = now; t_hn = 0; t_axis = 0;
        press_id = hit_press(x, y);
        if (press_id >= 0) { full_dirty = true; }
        if (scr == S_SKETCH && !(x < 80 && y < 80) && y <= 390 && cc_an.b < 0.5f) { sk_add(x, y, true); ts = TS_DRAG; t_axis = 3; }
        scroll_t *s = scr == S_APPS ? &sc_apps : scr == S_SETTINGS ? &sc_set : scr == S_ABOUT ? &sc_about : NULL;
        if (s && (fabsf(s->vel) > 0.05f || s->anim)) { s->vel = 0; s->anim = false; }
    }
    if (!down && ts == TS_IDLE) { return; }
    if (down)
    {
        t_x = x; t_y = y;
        if (t_hn == 6) { memmove(t_hx, t_hx + 1, 5 * sizeof(int)); memmove(t_hy, t_hy + 1, 5 * sizeof(int)); memmove(t_ht, t_ht + 1, 5 * sizeof(uint32_t)); t_hn = 5; }
        t_hx[t_hn] = x; t_hy[t_hn] = y; t_ht[t_hn] = now; t_hn++;
    }
    int dx = t_x - t_x0, dy = t_y - t_y0;
    if (ts == TS_PRESS && down)
    {
        if (abs(dx) > 12 || abs(dy) > 12)
        {
            ts = TS_DRAG;
            t_axis = abs(dx) > abs(dy) ? 1 : 2;
            if (press_id >= 0) { press_id = -1; full_dirty = true; }
            if (cc_an.b > 0.5f) { if (t_axis == 2) { cc_drag = true; } else if (t_y0 > 330) { t_axis = 4; } }
            else if (scr == S_FACE)
            {
                if (t_axis == 1) { face_drag = true; an_set(&face_an, uc->face); face_det = uc->face; }
                else if (t_y0 > LCD_H - 110 && dy < 0) { cc_drag = true; an_set(&cc_an, 0); }
            }
            else if (scr == S_SETTINGS && t_axis == 1 && t_y0 >= set_row_y(ST_BRIGHT) && t_y0 < set_row_y(ST_BRIGHT) + 86) { t_axis = 4; }
            else if (t_axis == 1 && dx > 0 && scr != S_APPS && scr != S_SKETCH) { tr_kind = 2; tr_from = scr; tr_to = parent_of(scr); tr_live = true; an_set(&tr, 0); }
            else if (t_axis == 1 && dx > 0 && scr == S_APPS) { tr_kind = 4; tr_from = scr; tr_to = S_FACE; tr_live = true; an_set(&tr, 0); }
            if (t_axis == 2) { scroll_t *s = scr == S_APPS ? &sc_apps : scr == S_SETTINGS ? &sc_set : scr == S_ABOUT ? &sc_about : NULL; if (s && !cc_drag) { sc_drag(s, 0, true); } }
        }
        else if (now - t_t0 > 600 && press_id < 0 && scr == S_FACE && cc_an.b < 0.5f)
        {
            ts = TS_DONE;                                  /* long press on the face: next face */
            an_set(&face_an, uc->face);
            uc->face = (uint8_t)((uc->face + 1) % N_FACE);
            an_go(&face_an, uc->face, 380, now, E_OUT);
            settings_change();
            plat_haptic(H_LONG);
        }
    }
    if (ts == TS_DRAG && down)
    {
        if (t_axis == 3) { sk_add(x, y, false); }
        else if (t_axis == 4)
        {
            float x0 = cc_an.b > 0.5f ? 56.f : 34.f, w = cc_an.b > 0.5f ? LCD_W - 112.f : LCD_W - 68.f;
            uc->bright = (uint8_t)(16 + 239.f * clampf((x - x0) / w, 0, 1)); plat_bright(uc->bright); full_dirty = true;
            uint8_t st = (uint8_t)((uc->bright - 16) * 10 / 240);
            if (hz_slider != 0xFF && st != hz_slider) { plat_haptic(st == 0 || st == 9 ? H_BUMP : H_TICK); }
            hz_slider = st;
        }
        else if (cc_drag)
        {
            float base = cc_an.b > 0.5f && !an_busy(&cc_an, now) ? 1.f : 0.f;
            if (base > 0.5f) { an_set(&cc_an, clampf(1.f - dy / (float)LCD_H, 0, 1)); cc_an.b = 0.99f; }
            else { an_set(&cc_an, clampf(-dy / (float)LCD_H, 0, 1)); }
        }
        else if (face_drag)
        {
            float f = uc->face - dx / (float)LCD_W;
            if (f < 0) { f = -rubber(-f * LCD_W) / LCD_W; }
            if (f > N_FACE - 1) { f = N_FACE - 1 + rubber((f - N_FACE + 1) * LCD_W) / LCD_W; }
            an_set(&face_an, f);
            int det = (int)lroundf(clampf(f, 0, N_FACE - 1));
            if (det != face_det) { face_det = det; plat_haptic(H_SCROLL); }    /* passing the half-way point */
        }
        else if (tr_live) { an_set(&tr, clampf(dx / (float)LCD_W, 0, 1)); }
        else if (t_axis == 2)
        {
            scroll_t *s = scr == S_APPS ? &sc_apps : scr == S_SETTINGS ? &sc_set : scr == S_ABOUT ? &sc_about : NULL;
            if (s) { sc_drag(s, (float)dy, false); }
        }
    }
    if (!down)
    {
        float vx = 0, vy = 0;
        if (t_hn >= 2)
        {
            int k = 0;
            while (k < t_hn - 2 && now - t_ht[k] > 90) { k++; }
            float dt = (float)(t_ht[t_hn - 1] - t_ht[k]);
            if (dt > 5) { vx = (t_hx[t_hn - 1] - t_hx[k]) / dt; vy = (t_hy[t_hn - 1] - t_hy[k]) / dt; }
            if (now - t_ht[t_hn - 1] > 120) { vx = vy = 0; }
        }
        if (ts == TS_PRESS && now - t_t0 < 600) { on_tap(t_x0, t_y0, now); }
        else if (ts == TS_DRAG)
        {
            if (cc_drag)
            {
                cc_drag = false;
                float s = cc_an.a;
                bool open = vy < -0.4f || (vy < 0.4f && s > 0.4f);
                if (open && s < 0.99f) { plat_haptic(H_SOFT); }
                an_set(&cc_an, s);
                an_go(&cc_an, open ? 1.f : 0.f, 300, now, E_OUT);
            }
            else if (face_drag)
            {
                face_drag = false;
                float f = an_val(&face_an, now);
                int target = (int)lroundf(f);
                if (vx < -0.35f) { target = (int)floorf(f) + 1; }
                if (vx > 0.35f) { target = (int)ceilf(f) - 1; }
                target = g_max(0, g_min(N_FACE - 1, target));
                if (target != uc->face) { uc->face = (uint8_t)target; settings_change(); plat_haptic(H_CLICK); }
                an_go(&face_an, target, 320, now, E_OUT);
            }
            else if (tr_live)
            {
                tr_live = false;
                float p = tr.b;
                bool done = vx > 0.45f || (vx > -0.3f && p > 0.35f);
                if (done)
                {
                    uint8_t to = tr_to;
                    uint8_t from = scr;
                    an_set(&tr, p); an_go(&tr, 1, 240, now, E_OUT);
                    plat_haptic(H_SOFT);
                    if (scr == S_FLASH) { plat_bright(uc->bright); }
                    scr_prev = from; scr = to;
                    if (to == S_FACE && tr_kind == 2) { tr_kind = 2; }
                }
                else { an_set(&tr, p); an_go(&tr, 0, 220, now, E_OUT); tr_to = tr_from; }
            }
            else if (t_axis == 2)
            {
                scroll_t *s = scr == S_APPS ? &sc_apps : scr == S_SETTINGS ? &sc_set : scr == S_ABOUT ? &sc_about : NULL;
                if (s) { sc_release(s, vy, now); }
            }
            else if (t_axis == 4) { settings_change(); hz_slider = 0xFF; }
        }
        if (press_id >= 0) { press_id = -1; full_dirty = true; }
        ts = TS_IDLE;
    }
}

static void ui_crown(int d, uint32_t now)
{
    if (scr == S_BOOT || lowbat) { return; }
    if (asleep()) { wake(now); plat_haptic(H_SOFT); return; }
    last_input = now;
    if (cc_an.b > 0.5f || scr == S_FLASH)
    {
        int b = uc->bright + d * 16;
        plat_haptic(b < 16 || b > 255 ? H_BUMP : H_TICK);
        uc->bright = (uint8_t)g_max(16, g_min(255, b)); plat_bright(uc->bright); settings_change(); return;
    }
    switch (scr)
    {
    case S_APPS: sc_crown(&sc_apps, d, now); break;
    case S_SETTINGS: sc_crown(&sc_set, d, now); break;
    case S_ABOUT: sc_crown(&sc_about, d, now); break;
    case S_TIMER:
        if (tm_state == 0) { int m = tm_min + d; plat_haptic(m < 1 || m > 99 ? H_BUMP : H_TICK); tm_min = (uint8_t)g_max(1, g_min(99, m)); full_dirty = true; }
        break;
    case S_BREATHE:
        if (br_state == 0) { int m = br_min + d; plat_haptic(m < 1 || m > 10 ? H_BUMP : H_TICK); br_min = (uint8_t)g_max(1, g_min(10, m)); full_dirty = true; }
        break;
    case S_SKETCH: sk_w = clampf(sk_w + d * 0.8f, 2.f, 9.f); plat_haptic(H_TICK); break;
    case S_FACE:
        if (!face_drag && cc_an.b < 0.5f)
        {
            int f = uc->face + d;
            if (f < 0 || f >= N_FACE) { plat_haptic(H_BUMP); break; }
            an_set(&face_an, an_val(&face_an, now));
            uc->face = (uint8_t)f; an_go(&face_an, f, 300, now, E_OUT); settings_change(); plat_haptic(H_TICK);
        }
        break;
    default: break;
    }
}

/* crown press: a firm click the moment it goes down, a lighter one when it comes back up (like a real switch) */
static void ui_button_edge(bool down, uint32_t now)
{
    (void)now;
    if (scr == S_BOOT || lowbat) { return; }
    plat_haptic(down ? H_PRESS : H_RELEASE);
}

static void ui_button(bool long_press, uint32_t now)
{
    if (scr == S_BOOT) { return; }
    if (lowbat) { if (!long_press) { full_dirty = true; wake(now); } return; }     /* shows the battery, 4 s */
    if (asleep()) { wake(now); return; }
    last_input = now;
    if (long_press) { plat_haptic(H_LONG); go_sleep(now); return; }
    if (cc_an.b > 0.5f) { an_go(&cc_an, 0, 260, now, E_OUT); return; }
    if (scr == S_FACE) { ui_go(S_APPS, 3, now); }
    else if (scr == S_APPS) { ui_go(S_FACE, 4, now); }
    else if (scr != S_BOOT) { ui_go(S_FACE, 2, now); }
}

/* ------------------------------------------------------------------ frame */
static void ui_init(T_UI_CFG *c, uint32_t now)
{
    uc = c;
    if (uc->bright < 16) { uc->bright = 160; }
    boot_t0 = now; scr = S_BOOT; full_dirty = true;
    an_set(&face_an, uc->face % N_FACE);
    an_set(&bright_an, uc->bright);
    plat_bright(uc->bright);
    last_input = now;
    sc_apps.max = g_max(0, APP_TOP + N_APPS * APP_ROW + 20 - LCD_H);
    sc_set.max = g_max(0, SET_TOP + ST_N * 96 + 20 - LCD_H);
    sc_apps.rowh = APP_ROW; sc_set.rowh = 96; sc_about.rowh = 74;
    an_set(&tg_an[0], uc->always_on);
    char k[24], v[32]; int n = 0; while (plat_info(n, k, v)) { n++; }
    sc_about.max = (float)g_max(0, 70 + n * 74 + 20 - LCD_H);
}

/* boot logo already complete (drawn before the screen supply came up): hold it, brighten, then the face */
static void ui_boot_lit(uint32_t now)
{
    boot_t0 = now - 1600; scr = S_BOOT; full_dirty = true;
    an_set(&bright_an, 0);
    an_go(&bright_an, uc->bright, 500, now, E_OUT);
}


/* low-battery mode on/off (platform decides from the battery voltage) */
static void ui_lowbat(bool on, uint32_t now)
{
    if (on == lowbat) { return; }
    lowbat = on;
    if (on)
    {
        tm_state = 0; sw_run = false; br_state = 0; an_set(&cc_an, 0); ts = TS_IDLE;
        plat_haptic(H_DOUBLE);                       /* the last haptic until it is charged */
        ui_go(S_LOW, 0, now);
        wake(now);
        an_go(&bright_an, g_min(uc->bright, 70), 300, now, E_OUT);
    }
    else { ui_go(S_FACE, 0, now); wake(now); an_go(&bright_an, uc->bright, 300, now, E_OUT); plat_haptic(H_SUCCESS); }
}

/* returns true while it wants the next frame immediately (animating) */
static bool ui_frame(uint32_t now)
{
    bool busy = false;
    /* brightness animation (fades: wake, sleep, boot) */
    if (an_busy(&bright_an, now) || bright_an.dur)
    {
        float b = an_val(&bright_an, now);
        plat_bright((uint8_t)lroundf(b));
        if (an_busy(&bright_an, now)) { busy = true; }
        else
        {
            bright_an.dur = 0;
            if (sleep_pending && b < 0.5f) { plat_display(false); sleeping = true; }
            sleep_pending = false;
        }
    }
    if (save_at && (int32_t)(now - save_at) >= 0) { save_at = 0; plat_save(); }
    if (sleeping)
    {
        if (tm_state == 1 && tm_remaining(now) == 0) { wake(now); }
        else { return false; }
    }
    if (tm_state == 1 && tm_remaining(now) == 0)
    {
        tm_state = 3; wake(now); ui_go(S_TIMER, scr == S_TIMER ? 0 : 1, now); hz_alarm = now;
    }
    if (tm_state == 3) { plat_bright((uint8_t)(uc->bright * (0.55f + 0.45f * sinf(now * 0.008f)))); busy = true; }
    if (tm_state == 3 && (int32_t)(now - hz_alarm) >= 0) { plat_haptic(H_ALARM); hz_alarm = now + 1500; }
    if (scr == S_BREATHE && br_state == 1)
    {
        /* inhale: taps that speed up as the lungs fill (Apple-style); hold + exhale: quiet */
        uint32_t cyc = (now - br_t0) % 9600u;
        if (cyc >= 4000) { hz_breath = now + (9600 - cyc); }      /* first tap exactly when the next inhale starts */
        else if ((int32_t)(now - hz_breath) >= 0) { plat_haptic(H_TICK); hz_breath = now + 420 - 300 * cyc / 4000; }
    }

    /* timeouts */
    uint32_t to = lowbat ? 4000u : TIMEOUTS[uc->timeout % N_TIMEOUT] * 1000u;
    bool hold = asleep() || scr == S_FLASH || (scr == S_BREATHE && br_state == 1) || tm_state == 3 || ts != TS_IDLE;
    if (!hold && scr != S_BOOT && now - last_input > to) { go_sleep(now); }

    /* boot */
    if (scr == S_BOOT)
    {
        uint32_t t = now - boot_t0;
        if (t > 2500 && bright_an.b > 0 && !an_busy(&bright_an, now)) { an_go(&bright_an, 0, 220, now, E_INOUT); }
        if (t > 2750 && !an_busy(&bright_an, now))
        {
            if (!lowbat) { plat_haptic(H_BOOT); }
            scr = lowbat ? S_LOW : S_FACE; full_dirty = true;
            an_go(&bright_an, uc->bright, 420, now, E_OUT);
            last_input = now;
        }
        busy = true;
    }

    /* per-second / per-minute changes */
    uint32_t sec = plat_now_s();
    if (sec != last_sec)
    {
        if (scr == S_FACE && uc->face == 1) { an_sec_from = (float)((last_sec == 0xFFFFFFFF ? sec : sec - 1) % 60) * 6.f; an_sec_to = (float)(sec % 60) * 6.f; if (an_sec_to < an_sec_from) { an_sec_to += 360.f; } an_sec_t0 = now; }
        if (sec / 60 != last_min) { last_min = sec / 60; if (scr == S_FACE || scr == S_APPS || scr >= S_STOPWATCH) { inval(0, 0, LCD_W, 60); if (scr == S_FACE) { full_dirty = true; } } }
        bool bt = plat_bt();
        if (bt != last_bt) { last_bt = bt; full_dirty = true; }
        last_sec = sec;
    }
    if (scr == S_FACE && uc->face == 1 && now - an_sec_t0 < 300) { busy = true; }

    /* scrolling */
    if (scr == S_APPS && sc_step(&sc_apps, now)) { busy = true; full_dirty = true; }
    if (scr == S_SETTINGS && sc_step(&sc_set, now)) { busy = true; full_dirty = true; }
    if (scr == S_ABOUT && sc_step(&sc_about, now)) { busy = true; full_dirty = true; }

    bool trans = tr_kind && (tr_live || an_busy(&tr, now));
    if (tr_kind && !trans)
    {
        if (tr.b < 0.5f && tr_to == tr_from) { scr = tr_from; }      /* cancelled back-swipe */
        tr_kind = 0; full_dirty = true;
    }
    bool ccv = cc_an.b > 0 || an_busy(&cc_an, now) || cc_drag;
    if (cc_drag || an_busy(&cc_an, now)) { busy = true; }
    if (face_drag || an_busy(&face_an, now)) { busy = true; }
    if (trans) { busy = true; }
    if (ts != TS_IDLE) { busy = true; }
    if (sw_run && scr == S_STOPWATCH) { busy = true; }
    if (scr == S_BREATHE && br_state == 1) { busy = true; }
    if (scr == S_FLASH && now - fl_t0 < 2000) { busy = true; }
    if (scr == S_SETTINGS && (an_busy(&tg_an[0], now) || an_busy(&tg_an[1], now))) { busy = true; }

    /* when an animation/gesture finishes, redraw everything once: whatever partial areas the in-between
       frames updated, the settled screen is then exactly right */
    static bool was_moving;
    bool moving = ccv || face_drag || an_busy(&face_an, now) || trans || ts != TS_IDLE || (scr == S_FLASH && now - fl_t0 < 2000) ||
                  an_busy(&tg_an[0], now) || an_busy(&tg_an[1], now) || sc_apps.anim || sc_set.anim || sc_about.anim ||
                  fabsf(sc_apps.vel) > 0 || fabsf(sc_set.vel) > 0 || fabsf(sc_about.vel) > 0;
    if (was_moving && !moving) { full_dirty = true; }
    was_moving = moving;

    /* draw */
    gfx_begin(C_BG);
    if (full_dirty) { gfx_invalidate_all(); full_dirty = false; }
    float ccs = an_val(&cc_an, now);
    if (ccv && ccs >= 0.999f && !cc_drag) { draw_cc(now); }
    else
    {
        if (ccv) { g_alpha = (uint8_t)(255 * (1.f - 0.7f * ccs)); }
        if (trans) { draw_transition(now); }
        else { draw_scene(scr, now); }
        g_alpha = 255;
        if (ccv && ccs > 0.001f) { draw_cc(now); gfx_invalidate_all(); }
        else if (ccv && !cc_drag && !an_busy(&cc_an, now)) { cc_an.b = 0; gfx_invalidate_all(); }
    }
    gfx_flush();
    return busy;
}

static void ui_dfu(void)
{
    if (sleeping) { plat_display(true); sleeping = false; }
    plat_bright(uc ? uc->bright : 160);
    gfx_begin(C_BG);
    g_circle(CX, 176, 70, C_BLUE, 255);
    g_icon_c(CX, 176, FI, F_I_SYNC, C_WHITE, 255);
    g_text_c(CX, 276, FL, C_WHITE, 255, "Updating");
    g_text_c(CX, 324, FS, C_SEC, 255, "Keep the watch on the charger");
    gfx_invalidate_all();
    gfx_flush();
}
