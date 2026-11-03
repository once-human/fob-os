/* ====================================================================================
 * fob shell. Portable C on top of onkar_gfx.inc (also runs in sim/sim.c).
 *
 * The crown is the only button: rotate = move, press = act, press twice = back, hold = screen off.
 *   carry (the resting screen, touch off): rotate = next scene, press = open the dial
 *   dial: rotate = choose a mode, press = enter it, double-press = back to carry
 *   mode: rotate/press = the mode's own controls, double-press = back to the dial
 * Every mode remembers its last page; carry resumes on the scene you left it on.
 * Platform (onkar_os.inc or the simulator) provides the plat_* functions below.
 * ==================================================================================== */

typedef struct { uint8_t bright, always_on, autostart, timeout, face, accent, haptic, spare2; } T_UI_CFG;   /* face = carry scene; haptic 0 off 1 light 2 medium 3 strong */

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

/* ------------------------------------------------------------------ palette: bone ink on black, one signal colour */
#define C_BG    0x0000
#define C_INK   RGB(236, 230, 218)
#define C_Q     RGB(142, 142, 142)
#define C_DIM   RGB(90, 90, 90)
#define C_RULE  RGB(74, 74, 74)
#define C_LN    RGB(38, 38, 38)
#define C_SF    RGB(24, 24, 24)
#define C_SIG   RGB(238, 75, 43)
#define CX      (LCD_W / 2)

#define N_ACCENT 1
#define N_TIMEOUT 5
#define N_SCENES 4
#define N_FACE   N_SCENES
#define N_MODES  9
static const char *const MODE_NAME[N_MODES] = {"carry", "cards", "music", "inbox", "remote", "claude", "play", "tools", "settings"};
static const char *const MODE_CAP[N_MODES] = {"ambient scenes: photo, clocks", "qr cards: you, upi, contact", "now playing, volume, seek", "notifications and calls",
                                              "camera shutter, clicker", "your laptop, answered from here", "coin, dice, tally, pick", "torch, timers, metronome, leash", "screen, haptics, about"};
static const uint16_t TIMEOUTS[] = {10, 15, 30, 60, 300};

/* ------------------------------------------------------------------ small helpers */
static char *u_num(char *p, uint32_t v) { char t[11]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n) { *p++ = t[--n]; } *p = 0; return p; }
static char *u_2(char *p, uint32_t v) { *p++ = (char)('0' + (v / 10) % 10); *p++ = (char)('0' + v % 10); *p = 0; return p; }
static char *u_s(char *p, const char *s) { while (*s) { *p++ = *s++; } *p = 0; return p; }
static float e_out(float t) { t = 1.f - t; return 1.f - t * t * t; }
static float e_inout(float t) { float u = 2.f - 2.f * t; return t < 0.5f ? 4.f * t * t * t : 1.f - u * u * u / 2.f; }
static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

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

/* ------------------------------------------------------------------ state */
enum { S_BOOT, S_CARRY, S_DIAL, S_MODE, S_LOW, S_N };
static T_UI_CFG *uc;
static uint8_t scr = S_BOOT, mode_cur;
static bool full_dirty = true;
static uint32_t boot_t0, last_input, last_sec = 0xFFFFFFFF, last_min = 0xFFFFFFFF, save_at;
static bool sleeping, sleep_pending, dimmed, lowbat;
static anim_t bright_an, face_an, dial_an, sc_an, fade_an;
static int8_t scene, scene_from, scene_dir;
static int dial_idx;
static uint8_t mem[N_MODES];                    /* per-mode memory: settings keeps its row here */
static uint8_t fade_ph, pend_scr, pend_mode;   /* 0 none, 1 fading out, 2 fading in */
static bool press_pend; static uint32_t press_t;
#define DBL_MS   300
#define BOOT_MS  2600

static void hap(uint8_t id) { if (uc && uc->haptic) { plat_haptic(id); } }
static void dirty(void) { full_dirty = true; }
static void time_parts(uint32_t *h, uint32_t *m, uint32_t *s) { uint32_t t = plat_now_s() % 86400u; *h = t / 3600; *m = (t / 60) % 60; *s = t % 60; }

static void go(uint8_t to, uint8_t mode, uint32_t now)
{
    if (fade_ph == 1) { pend_scr = to; pend_mode = mode; return; }
    pend_scr = to; pend_mode = mode; fade_ph = 1;
    an_go(&fade_an, 0, 110, now, E_LIN);
}

static void wake(uint32_t now)
{
    if (sleeping) { plat_display(true); sleeping = false; an_set(&bright_an, 0); }
    sleep_pending = false; dimmed = false; last_input = now;
    an_go(&bright_an, uc->bright, 240, now, E_OUT);
    dirty();
}
static void go_sleep(uint32_t now) { sleep_pending = true; an_go(&bright_an, 0, 240, now, E_OUT); }
static void touched(uint32_t now)       /* any input: undo ambient dimming */
{
    last_input = now;
    if (dimmed) { dimmed = false; an_go(&bright_an, uc->bright, 200, now, E_OUT); }
}

/* ------------------------------------------------------------------ drawing helpers */
static void word(int x, int base, int fi, const char *s, uint16_t c, uint8_t a) { g_text(x, base - FONT_TAB[fi]->asc, fi, c, a, s); }
static void word_r(int rx, int base, int fi, const char *s, uint16_t c, uint8_t a) { g_text_r(rx, base - FONT_TAB[fi]->asc, fi, c, a, s); }
static void hair(int x0, int x1, int y, uint16_t c) { g_rect((float)x0, (float)y, (float)(x1 - x0), 1.f, c, 255); }

static void chrome(const char *mid, const char *right)
{
    char t[8]; uint32_t h, m, s; time_parts(&h, &m, &s);
    if (plat_time_ok()) { char *p = u_2(t, h); *p++ = ':'; u_2(p, m); } else { u_s(t, "--:--"); }
    hair(20, 120, 30, C_RULE); hair(130, 230, 30, C_RULE); hair(240, 348, 30, C_RULE);
    word(20, 47, FL, t, C_INK, 255); word(130, 47, FL, mid, C_Q, 255); word(240, 47, FL, right, C_Q, 255);
}
static void foot(const char *a, const char *b)
{
    hair(20, 180, 414, C_RULE); hair(190, 348, 414, C_RULE);
    word(20, 431, FL, a, C_Q, 255); word(190, 431, FL, b, C_Q, 255);
}
static void segs(int n, int on, int y)
{
    float gap = 4.f, w = (328.f - gap * (n - 1)) / n;
    for (int k = 0; k < n; k++) { g_rect(20.f + k * (w + gap), (float)y, w, 2.f, k == on ? C_INK : RGB(60, 60, 60), 255); }
}

/* ------------------------------------------------------------------ boot */
static void draw_boot(uint32_t now)
{
    uint32_t t = now - boot_t0;
    static const uint16_t RC[3] = {RGB(236, 230, 218), RGB(142, 142, 142), RGB(74, 74, 74)};
    for (int i = 0; i < 3; i++)
    {
        float R = 130.f - i * 26.f, k = clampf(((float)t - 60.f - i * 160.f) / 900.f, 0, 1);
        if (k >= 1.f) { g_ring(CX, 190, R, R - 1.6f, RC[i], 255); }
        else if (k > 0.f) { g_arc(CX, 190, R, R - 1.6f, 0, 360.f * e_inout(k), RC[i], 255, false); }
    }
    float sa = clampf(((float)t - 850.f) / 500.f, 0, 1), wa = clampf(((float)t - 1050.f) / 500.f, 0, 1);
    if (sa > 0) { g_icon_c(CX, 190, FX, F_X_STAR, C_INK, (uint8_t)(255 * e_out(sa))); }
    if (wa > 0) { g_text_c(CX, 424 - FONT_TAB[FG]->asc, FG, C_INK, (uint8_t)(255 * e_out(wa)), "fob"); }
}

/* ------------------------------------------------------------------ carry scenes */
static void sc_me(void)
{
    g_rect(20, 76, 328, 226, RGB(29, 26, 23), 255);
    word(30, 98, FL, "a photograph goes here", RGB(107, 98, 90), 255);
    word(14, 392, FG, "onkar", C_INK, 255);
}
static void sc_stack(void)
{
    uint32_t h, m, s; time_parts(&h, &m, &s); char b[4];
    if (!plat_time_ok()) { word(14, 190, FN, "--", C_INK, 255); word(14, 360, FN, "--", C_Q, 255); return; }
    u_2(b, h); word(14, 190, FN, b, C_INK, 255);
    u_2(b, m); word(14, 360, FN, b, C_Q, 255);
}
static void sc_ring(void)
{
    uint32_t h, m, s; time_parts(&h, &m, &s);
    const float cx = CX, cy = 236.f, r = 150.f;
    for (int i = 0; i < 60; i++)
    {
        float a = i * 0.104720f, sn = sinf(a), cs = cosf(a), l = (i % 5) ? 6.f : 15.f;
        g_line(cx + sn * (r - l), cy - cs * (r - l), cx + sn * r, cy - cs * r, (i % 5) ? 0.5f : 0.9f, (i % 5) ? C_DIM : C_INK, 255);
    }
    if (!plat_time_ok()) { return; }
    float ha = ((h % 12) + m / 60.f) * 0.523599f, ma = (m + s / 60.f) * 0.104720f, sa = s * 0.104720f;
    g_line(cx, cy, cx + sinf(ha) * 78, cy - cosf(ha) * 78, 2.6f, C_INK, 255);
    g_line(cx, cy, cx + sinf(ma) * 122, cy - cosf(ma) * 122, 1.6f, C_INK, 255);
    g_line(cx, cy, cx + sinf(sa) * 136, cy - cosf(sa) * 136, 0.8f, C_SIG, 255);
    g_circle(cx, cy, 5, C_SIG, 255);
}
static void sc_day(void)
{
    uint32_t h, m, s; time_parts(&h, &m, &s); char b[8];
    if (plat_time_ok()) { char *p = u_2(b, h); *p++ = ':'; u_2(p, m); } else { u_s(b, "--:--"); }
    word(14, 200, FG, b, C_INK, 255);
    word(20, 250, FB, plat_time_ok() ? "the day, in 24 segments" : "clock not set", C_Q, 255);
    for (int i = 0; i < 24; i++)
    {
        bool day = i >= 6 && i < 19;
        float x = 20.f + i * 13.6f, hh = day ? 46.f : 30.f;
        g_rect(x, 346.f - hh, 10.4f, hh, (plat_time_ok() && (uint32_t)i == h) ? C_INK : (day ? C_Q : RGB(58, 58, 58)), 255);
    }
    if (plat_time_ok()) { g_circle(20.f + h * 13.6f + 5.2f, 364.f, 2.6f, C_SIG, 255); }
    word(20, 388, FL, "midnight", C_Q, 255); word(190, 388, FL, "noon", C_Q, 255); word_r(348, 388, FL, "midnight", C_Q, 255);
}
static void draw_scene(int i)
{
    switch (i) { case 0: sc_me(); break; case 1: sc_stack(); break; case 2: sc_ring(); break; default: sc_day(); break; }
}
static const char *const SCENE_NAME[N_SCENES] = {"carry · me", "carry · stack", "carry · hairline", "carry · day"};
static void draw_carry(uint32_t now)
{
    chrome(SCENE_NAME[scene], "");
    segs(N_SCENES, scene, 60);
    if (an_busy(&sc_an, now))
    {
        float e = an_val(&sc_an, now);                         /* 0..1: the new scene slides in */
        g_ox = (int)(-scene_dir * LCD_W * e);   draw_scene(scene_from);
        g_ox = (int)(scene_dir * LCD_W * (1.f - e)); draw_scene(scene);
        g_ox = 0;
    }
    else { draw_scene(scene); }
}

/* ------------------------------------------------------------------ dial */
static void draw_dial(uint32_t now)
{
    float p = an_val(&dial_an, now);
    chrome("dial", "turn · press");
    for (int i = 0; i < N_MODES; i++)
    {
        float d = i - p, ad = d < 0 ? -d : d;
        if (ad >= 1.7f) { continue; }
        int yb = 262 + (int)(d * 118.f);
        float ag = clampf(1.f - ad * 1.2f, 0, 1), as = clampf(ad * 2.f, 0, 1) * clampf(1.7f - ad, 0, 1) * 0.55f;
        if (ag > 0.02f) { word(18, yb, FG, MODE_NAME[i], C_INK, (uint8_t)(255 * ag)); }
        if (as > 0.02f) { word(20, yb - 8, FT, MODE_NAME[i], C_INK, (uint8_t)(255 * as)); }
    }
    int k = (int)(p + 0.5f); if (k < 0) { k = 0; } if (k >= N_MODES) { k = N_MODES - 1; }
    float near = clampf(1.f - (p > k ? p - k : k - p) * 2.f, 0, 1);
    hair(20, 348, 296, C_RULE);
    word(20, 322, FL, MODE_CAP[k], C_Q, (uint8_t)(255 * near));
    char b[10]; char *q = u_2(b, (uint32_t)k + 1); q = u_s(q, " / 09"); (void)q;
    word_r(348, 322, FL, b, C_Q, 255);
    foot("crown · rotate", "press · enter");
}

/* ------------------------------------------------------------------ modes */
static const uint8_t BRIGHTS[] = {40, 80, 120, 160, 200, 255};
static const char *const HAP_NAME[] = {"off", "light", "medium", "strong"};
static int scr_val(void) { if (uc->always_on) { return 0; } return 1 + (uc->timeout > 4 ? 4 : uc->timeout); }
static void draw_settings(uint32_t now)
{
    (void)now; char b[24]; int row = mem[8];
    chrome("settings", "");
    static const char *const LBL[3] = {"brightness", "haptics", "screen"};
    for (int i = 0; i < 3; i++)
    {
        int y = 64 + i * 56; bool sel = i == row;
        hair(20, 348, y, C_LN);
        if (sel) { g_circle(14, (float)y + 28, 3.5f, C_SIG, 255); }
        word(20, y + 38, FT, LBL[i], sel ? C_INK : RGB(154, 148, 136), 255);
        if (i == 0) { char *p = u_num(b, (uint32_t)uc->bright * 100u / 255u); u_s(p, " %"); }
        else if (i == 1) { u_s(b, HAP_NAME[uc->haptic & 3]); }
        else { int v = scr_val(); if (v == 0) { u_s(b, "always on"); } else { char *p = u_num(b, TIMEOUTS[v - 1] >= 60 ? TIMEOUTS[v - 1] / 60 : TIMEOUTS[v - 1]); u_s(p, TIMEOUTS[v - 1] >= 60 ? " min" : " s"); } }
        word_r(348, y + 36, FB, b, sel ? C_SIG : C_Q, 255);
    }
    hair(20, 348, 232, C_LN);
    char k[24], v[32]; char line[64];
    for (int i = 0; plat_info(i, k, v) && i < 9; i++)
    {
        char *p = u_s(line, k); p = u_s(p, "  "); u_s(p, v);
        word(20, 256 + i * 17, FL, line, C_Q, 255);
    }
    foot("turn · move", "press · change");
}
static void draw_mode_stub(void)
{
    chrome(MODE_NAME[mode_cur], "not built yet");
    word(14, 190, FG, MODE_NAME[mode_cur], C_INK, 255);
    word(20, 232, FB, MODE_CAP[mode_cur], C_Q, 255);
    word(20, 262, FB, "designed in the book, coming next.", RGB(110, 110, 110), 255);
    foot("dbl-press · dial", "");
}
static void draw_low(void)
{
    char b[8]; int mv; int pc = plat_batt(&mv); if (pc < 0) { pc = 0; }
    chrome("battery", "low");
    g_circle(24, 80, 4, C_SIG, 255); word(34, 85, FL, "plug in soon", C_SIG, 255);
    char *p = u_num(b, (uint32_t)pc); u_s(p, "%");
    word(14, 270, FN, b, C_SIG, 255);
    word(20, 330, FL, "the screen dims. bluetooth stays on.", C_Q, 255);
}

/* ------------------------------------------------------------------ input */
static void scene_step(int d, uint32_t now)
{
    int ns = scene + d;
    if (ns < 0 || ns >= N_SCENES) { hap(H_BUMP); return; }
    scene_from = scene; scene = (int8_t)ns; scene_dir = (int8_t)(d > 0 ? 1 : -1);
    an_set(&sc_an, 0); an_go(&sc_an, 1, 300, now, E_OUT);
    uc->face = (uint8_t)scene; an_set(&face_an, scene);
    save_at = now + 1500; if (!save_at) { save_at = 1; }
    hap(H_TICK); dirty();
}

static void do_single(uint32_t now)
{
    if (scr == S_CARRY) { dial_idx = mode_cur; an_set(&dial_an, (float)dial_idx); hap(H_TAP); go(S_DIAL, 0, now); }
    else if (scr == S_DIAL)
    {
        hap(H_TAP); mode_cur = (uint8_t)dial_idx;
        if (dial_idx == 0) { go(S_CARRY, 0, now); } else { go(S_MODE, (uint8_t)dial_idx, now); }
    }
    else if (scr == S_MODE && mode_cur == 8)
    {
        int row = mem[8];
        if (row == 0) { uint8_t nb = BRIGHTS[0]; for (int i = 0; i < 6; i++) { if (BRIGHTS[i] > uc->bright) { nb = BRIGHTS[i]; break; } } uc->bright = nb; plat_bright(nb); an_set(&bright_an, nb); }
        else if (row == 1) { uc->haptic = (uint8_t)((uc->haptic + 1) & 3); }
        else { int v = (scr_val() + 1) % 6; if (v == 0) { uc->always_on = 1; } else { uc->always_on = 0; uc->timeout = (uint8_t)(v - 1); } }
        save_at = now + 1500; if (!save_at) { save_at = 1; }
        if (uc->haptic) { plat_haptic(H_CLICK); }
        dirty();
    }
    else { hap(H_TAP); }
}
static void do_double(uint32_t now)
{
    if (scr == S_DIAL) { hap(H_SOFT); go(S_CARRY, 0, now); mode_cur = 0; }
    else if (scr == S_MODE) { hap(H_SOFT); dial_idx = mode_cur; an_set(&dial_an, (float)dial_idx); go(S_DIAL, 0, now); }
}

static void ui_touch(bool down, int x, int y, uint32_t now)
{
    (void)x; (void)y;
    if (sleeping || lowbat) { return; }
    if (down && scr != S_CARRY) { touched(now); }       /* touch has no job on carry: it stays off there */
}

static void ui_crown(int d, uint32_t now)
{
    if (sleeping) { wake(now); return; }
    if (lowbat) { return; }
    touched(now);
    if (scr == S_BOOT) { if (now - boot_t0 >= 1700) { go(S_CARRY, 0, now); } return; }
    if (fade_ph) { return; }
    if (scr == S_CARRY) { scene_step(d, now); }
    else if (scr == S_DIAL)
    {
        int ni = dial_idx + d;
        if (ni < 0 || ni >= N_MODES) { hap(H_BUMP); return; }
        dial_idx = ni; an_go(&dial_an, (float)dial_idx, 280, now, E_OUT); hap(H_TICK); dirty();
    }
    else if (scr == S_MODE && mode_cur == 8)
    {
        int nr = mem[8] + d; if (nr < 0 || nr > 2) { hap(H_BUMP); return; }
        mem[8] = (uint8_t)nr; hap(H_TICK); dirty();
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
    touched(now);
    if (scr == S_BOOT) { if (now - boot_t0 >= 1700) { go(S_CARRY, 0, now); } return; }
    if (long_press) { press_pend = false; hap(H_LONG); go_sleep(now); return; }
    if (fade_ph) { return; }
    if (press_pend && now - press_t < DBL_MS) { press_pend = false; do_double(now); return; }
    press_pend = true; press_t = now;
}

/* ------------------------------------------------------------------ frame */
static void ui_init(T_UI_CFG *c, uint32_t now)
{
    uc = c;
    if (uc->bright < 16) { uc->bright = 160; }
    scene = (int8_t)(uc->face % N_SCENES); an_set(&face_an, scene);
    boot_t0 = now; scr = S_BOOT; full_dirty = true; last_input = now;
    an_set(&bright_an, uc->bright); plat_bright(uc->bright);
    an_set(&fade_an, 1); fade_ph = 0; press_pend = false; mode_cur = 0;
}

/* boot logo already complete (drawn before the screen supply came up): hold it, brighten, then carry */
static void ui_boot_lit(uint32_t now)
{
    boot_t0 = now - 1700; scr = S_BOOT; full_dirty = true; last_input = now;
    an_set(&bright_an, 0);
    an_go(&bright_an, uc->bright, 500, now, E_OUT);
}

static void ui_lowbat(bool on, uint32_t now)
{
    if (on == lowbat) { return; }
    lowbat = on; press_pend = false; fade_ph = 0; an_set(&fade_an, 1);
    if (on) { hap(H_DOUBLE); scr = S_LOW; wake(now); an_go(&bright_an, uc->bright < 70 ? uc->bright : 70, 300, now, E_OUT); }
    else { scr = S_CARRY; wake(now); plat_haptic(H_SUCCESS); }
    dirty();
}

/* returns true while it wants the next frame immediately (animating) */
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

    if (press_pend) { if (now - press_t >= DBL_MS) { press_pend = false; if (!fade_ph) { do_single(now); } } else { busy = true; } }
    if (scr == S_BOOT && now - boot_t0 >= BOOT_MS && !fade_ph) { go(S_CARRY, 0, now); }
    if (fade_ph == 1 && !an_busy(&fade_an, now))
    {
        scr = pend_scr; mode_cur = scr == S_MODE ? pend_mode : mode_cur; if (scr == S_CARRY) { an_set(&sc_an, 1); }
        an_go(&fade_an, 1, 240, now, E_OUT); fade_ph = 2; dirty();
    }
    if (fade_ph == 2 && !an_busy(&fade_an, now)) { fade_ph = 0; an_set(&fade_an, 1); dirty(); }
    if (fade_ph) { busy = true; dirty(); }

    /* idle behaviour: carry dims to an ambient glow, then sleeps unless "always on"; anything else returns to carry */
    if (!lowbat && !fade_ph && scr != S_BOOT)
    {
        uint32_t idle = now - last_input;
        if (scr == S_CARRY)
        {
            if (!dimmed && idle > 10000) { dimmed = true; uint8_t lo = (uint8_t)(uc->bright / 4); an_go(&bright_an, lo < 16 ? 16 : lo, 1200, now, E_INOUT); }
            if (!uc->always_on && idle > (uint32_t)TIMEOUTS[uc->timeout > 4 ? 4 : uc->timeout] * 1000u) { go_sleep(now); }
        }
        else if (idle > 25000) { go(S_CARRY, 0, now); }
    }
    if (lowbat && now - last_input > 4000) { go_sleep(now); last_input = now; }

    /* the clock moves */
    uint32_t h, m, s; time_parts(&h, &m, &s); uint32_t sec = plat_now_s();
    if (sec != last_sec) { last_sec = sec; if (scr == S_CARRY && scene == 2) { dirty(); } }
    if (m != last_min) { last_min = m; dirty(); }
    { static bool was_moving; bool moving = an_busy(&sc_an, now) || an_busy(&dial_an, now); if (moving) { busy = true; } if (moving || was_moving) { dirty(); } was_moving = moving; }
    if (scr == S_BOOT && now - boot_t0 < 1700) { busy = true; dirty(); }

    if (!full_dirty) { return busy; }
    gfx_begin(C_BG);
    gfx_invalidate_all(); full_dirty = false;
    if (fade_ph) { g_alpha = (uint8_t)(255.f * clampf(an_val(&fade_an, now), 0, 1)); }
    switch (scr)
    {
    case S_BOOT: draw_boot(now); break;
    case S_CARRY: draw_carry(now); break;
    case S_DIAL: draw_dial(now); break;
    case S_MODE: if (mode_cur == 8) { draw_settings(now); } else { draw_mode_stub(); } break;
    default: draw_low(); break;
    }
    g_alpha = 255;
    gfx_flush();
    return busy;
}

static void ui_dfu(void)
{
    if (sleeping) { plat_display(true); sleeping = false; }
    plat_bright(uc ? uc->bright : 160);
    gfx_begin(C_BG);
    word(14, 190, FG, "updating", C_INK, 255);
    word(20, 232, FB, "keep it near the laptop.", C_Q, 255);
    word(20, 258, FB, "fob restarts by itself.", RGB(110, 110, 110), 255);
    gfx_invalidate_all();
    gfx_flush();
}
