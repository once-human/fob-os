/* fob laptop simulator: runs onkar_gfx.inc + onkar_ui.inc, scripted input, writes PPM screenshots to out/.
   Every screenshot also checks the dirty-rect logic: the incrementally updated frame must equal a full redraw. */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include "onkar_gfx.inc"

static uint16_t fb[LCD_H][LCD_W];
static uint32_t t_ms, sent_pix, frames, sent_frames;
static uint8_t s_bright = 255;
static bool s_disp = true;
static uint32_t epoch0 = 1793614170u;     /* 2026-11-02 08:49:30 */
static double render_ns;
static int haptics;

static void gfx_out(int x0, int y0, int w, int h, const uint16_t *pix)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) { uint16_t v = pix[j * w + i]; fb[y0 + j][x0 + i] = (uint16_t)((v >> 8) | (v << 8)); }
    sent_pix += (uint32_t)(w * h);
}
static void gfx_out_wait(void) {}
static void gfx_frame_start(void) {}
static uint32_t plat_ms(void) { return t_ms; }
static void plat_bright(uint8_t v) { s_bright = v; }
static bool plat_bright_ready(void) { return true; }
static void plat_vsync(bool on) { (void)on; }
static void plat_display(bool on) { s_disp = on; }
static bool plat_bt(void) { return true; }
static uint32_t plat_now_s(void) { return epoch0 + t_ms / 1000; }
static bool plat_time_ok(void) { return true; }
static int saves;
static void plat_save(void) { saves++; }
static void plat_haptic(uint8_t id) { (void)id; haptics++; }
static int plat_batt(int *mv) { if (mv) { *mv = 3900; } return 78; }
static bool plat_info(int i, char *k, char *v)
{
    static const char *K[] = {"firmware", "running", "bluetooth", "touch", "frame rate"};
    static const char *V[] = {"fob 0.1", "0h 12m", "connected", "chsc5816", "58 fps"};
    if (i >= 5) { return false; }
    strcpy(k, K[i]); strcpy(v, V[i]);
    return true;
}

#include "onkar_ui.inc"

static T_UI_CFG cfg = {180, 1, 1, 1, 0, 0, 2, 0};
static int shots, bad_shots;

static void frame(void)
{
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    uint32_t before = sent_pix;
    ui_frame(t_ms);
    clock_gettime(CLOCK_MONOTONIC, &b);
    render_ns += (b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec);
    frames++;
    if (sent_pix != before) { sent_frames++; }
}
static void run(uint32_t ms) { uint32_t end = t_ms + ms; while (t_ms < end) { t_ms += 16; frame(); } }

static void write_ppm(const char *name, uint16_t (*buf)[LCD_W], bool lit)
{
    char path[128]; snprintf(path, sizeof path, "out/%s.ppm", name);
    FILE *f = fopen(path, "wb"); if (!f) { return; }
    fprintf(f, "P6 %d %d 255\n", LCD_W, LCD_H);
    for (int y = 0; y < LCD_H; y++)
        for (int x = 0; x < LCD_W; x++)
        {
            uint16_t v = buf[y][x];
            int r = (v >> 11) * 255 / 31, g = ((v >> 5) & 63) * 255 / 63, b = (v & 31) * 255 / 31;
            if (!lit) { r = g = b = 0; }
            fputc(r, f); fputc(g, f); fputc(b, f);
        }
    fclose(f);
}
static uint16_t ref[LCD_H][LCD_W], inc[LCD_H][LCD_W];
static void shot(const char *name)
{
    memcpy(inc, fb, sizeof fb);
    write_ppm(name, fb, s_disp);
    full_dirty = true; ui_frame(t_ms);
    memcpy(ref, fb, sizeof fb);
    int diff = 0, bx0 = 9999, by0 = 9999, bx1 = -1, by1 = -1;
    for (int y = 0; y < LCD_H; y++) for (int x = 0; x < LCD_W; x++) if (ref[y][x] != inc[y][x]) { diff++; if (x < bx0) bx0 = x; if (y < by0) by0 = y; if (x > bx1) bx1 = x; if (y > by1) by1 = y; }
    shots++;
    if (diff) { bad_shots++; printf("  %-22s STALE %d px in [%d,%d]-[%d,%d]\n", name, diff, bx0, by0, bx1, by1); }
    else { printf("  %-22s ok  scr %d bright %3d  prims %d drops %u\n", name, scr, s_bright, g_n, g_drops); }
}
static void crown(int d) { ui_crown(d, t_ms); frame(); }
static void press(void) { ui_button_edge(true, t_ms); t_ms += 60; ui_button_edge(false, t_ms); ui_button(false, t_ms); frame(); }
static void dbl(void) { press(); t_ms += 90; press(); }
static void touch(bool d, int x, int y) { ui_touch(d, x, y, t_ms); frame(); }
static void tap(int x, int y) { touch(true, x, y); t_ms += 16; touch(true, x, y); t_ms += 50; touch(false, x, y); }
static void drag(int x0, int y0, int x1, int y1, int ms)
{
    touch(true, x0, y0); int n = ms / 16;
    for (int i = 1; i <= n; i++) { t_ms += 16; touch(true, x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n); }
    t_ms += 16; touch(false, x1, y1);
}

int main(void)
{
    ui_init(&cfg, 0);
    ui_boot_lit(0);
    for (int i = 0; i < 20; i++) { run(250); char n[32]; snprintf(n, sizeof n, "boot_%02d", i); if (i % 2 == 1 || i < 6) { write_ppm(n, fb, s_disp); } }
    run(800); shot("carry_me");
    crown(1); run(120); write_ppm("carry_slide_mid", fb, true); run(500); shot("carry_pet");
    crown(1); run(600); shot("carry_stack");
    crown(1); run(600); shot("carry_hairline");
    crown(1); run(600); shot("carry_day");
    crown(-4); run(900);
    drag(300, 220, 80, 224, 160); run(500); shot("swipe_to_pet");
    drag(184, 400, 184, 200, 160); run(700); shot("dial_by_swipe_up");
    drag(184, 330, 184, 90, 200); run(500); shot("dial_dragged");
    tap(184, 230); run(700); shot("mode_entered_by_tap");
    dbl(); run(700);
    for (int i = 0; i < 5; i++) { crown(1); run(320); } shot("dial_remote");
    press(); run(700); shot("mode_stub");
    dbl(); run(700);
    crown(-4); run(900); shot("dial_cards");
    press(); run(700); shot("cards_site");
    crown(1); run(300); shot("cards_github");
    dbl(); run(700); crown(5); run(900); press(); run(700); shot("play_coin");
    press(); run(300); run(900); shot("play_result"); crown(1); run(300); crown(1); run(300); crown(1); run(300); press(); run(200); shot("play_tally");
    dbl(); run(700); crown(1); run(320); press(); run(700); shot("tools_torch");
    press(); run(300); shot("torch_on"); press(); run(300);
    crown(1); run(300); shot("tools_stopwatch"); press(); run(1300); shot("stopwatch_running");
    crown(1); run(300); shot("tools_metronome");
    dbl(); run(700); crown(1); run(320); press(); run(700); shot("settings");
    tap(200, 124); run(300); shot("settings_slider_tap");
    drag(60, 124, 300, 124, 200); run(300); shot("settings_slider_drag");
    tap(160, 220); run(300); shot("settings_haptics_tap"); tap(246, 312); run(300); shot("settings_screen_pill");
    dbl(); run(700); dbl(); run(700); shot("carry_back");
    printf("  carry resumed on scene %d (expected 4)\n", scene);
    run(12000); shot("carry_dimmed");
    { int sc0 = scene; uint8_t b0 = s_bright; tap(184, 224); run(300); drag(300, 220, 80, 224, 160); run(400);
      printf("  dimmed + touch: scene %d->%d, brightness %d->%d (expected unchanged)\n", sc0, scene, b0, s_bright);
      crown(1); run(500); printf("  dimmed + first crown turn: scene %d->%d, brightness %d->%d (expected: same scene, brighter)\n", sc0, scene, b0, s_bright);
      shot("carry_woken"); crown(1); run(500); printf("  second turn: scene now %d\n", scene); }
    touch(true, 300, 220); for (int i = 1; i <= 8; i++) { t_ms += 16; touch(true, 300 - i * 14, 224); } write_ppm("carry_finger_follow", fb, true); t_ms += 16; touch(false, 190, 224); run(500);
    ui_lowbat(true, t_ms); run(800); shot("lowbat");
    ui_lowbat(false, t_ms); run(900);
    ui_dfu(); write_ppm("dfu", fb, true);
    for (int sc = 0; sc < 5; sc++)
    {
        crown(sc - scene > 0 ? 1 : -1); run(500); while (scene != sc) { crown(sc > scene ? 1 : -1); run(500); }
        run(13000); char n[32]; snprintf(n, sizeof n, "dim_%d", sc); write_ppm(n, fb, true);
        crown(1); run(400); if (scene == 4) { crown(-1); run(400); }
    }
    printf("frames %u (%u sent), avg ui_frame %.1f us on this laptop, saves %d, haptics %d, %d/%d screenshots consistent\n",
           frames, sent_frames, render_ns / frames / 1000.0, saves, haptics, shots - bad_shots, shots);
    return bad_shots ? 1 : 0;
}
