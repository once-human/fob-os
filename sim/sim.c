/* ONKAR OS v16 laptop simulator: runs onkar_gfx.inc + onkar_ui.inc, scripted input, writes PPM screenshots.
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

static void gfx_out(int x0, int y0, int w, int h, const uint16_t *pix)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
        {
            uint16_t v = pix[j * w + i];
            fb[y0 + j][x0 + i] = (uint16_t)((v >> 8) | (v << 8));
        }
    sent_pix += (uint32_t)(w * h);
}
static void gfx_out_wait(void) {}
static void gfx_frame_start(void) {}
static uint32_t plat_ms(void) { return t_ms; }
static void plat_bright(uint8_t v) { s_bright = v; }
static void plat_display(bool on) { s_disp = on; }
static bool sim_bt = true;
static bool plat_bt(void) { return sim_bt; }
static uint32_t plat_now_s(void) { return epoch0 + t_ms / 1000; }
static bool plat_time_ok(void) { return true; }
static int saves;
static const char *const HN[] = {"tick", "soft", "tap", "click", "double", "success", "long", "bump", "alarm", "boot", "scroll", "press", "release"};
static int sim_pct = -1;
static int plat_batt(int *mv) { *mv = sim_pct < 0 ? 3290 : 3500 + sim_pct * 7; return sim_pct; }
static void plat_haptic(uint8_t id) { printf("t=%u haptic %s\n", t_ms, HN[id]); }
static void plat_save(void) { saves++; }
static bool plat_info(int i, char *k, char *v)
{
    static const char *K[] = {"Firmware", "Running for", "Bluetooth", "Touch", "Frame rate", "Last frame", "Last restart"};
    static const char *V[] = {"ONKAR OS 16", "0h 12m", "Connected", "CHSC5816 (0x2E)", "58 fps", "6.1 ms render, 9.8 ms send", "Power on"};
    if (i >= 7) { return false; }
    strcpy(k, K[i]); strcpy(v, V[i]);
    return true;
}

#include "onkar_ui.inc"

static T_UI_CFG cfg = {180, 0, 1, 1, 0, 0, 0, 0};
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
    char path[128];
    snprintf(path, sizeof path, "out/%s.ppm", name);
    FILE *f = fopen(path, "wb");
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
    /* full redraw at the same time -> must be identical */
    full_dirty = true;
    ui_frame(t_ms);
    memcpy(ref, fb, sizeof fb);
    int diff = 0, dx0 = 9999, dy0 = 9999, dx1 = -1, dy1 = -1;
    for (int y = 0; y < LCD_H; y++)
        for (int x = 0; x < LCD_W; x++)
            if (ref[y][x] != inc[y][x]) { diff++; if (x < dx0) dx0 = x; if (y < dy0) dy0 = y; if (x > dx1) dx1 = x; if (y > dy1) dy1 = y; }
    shots++;
    if (diff)
    {
        bad_shots++;
        printf("  %-22s STALE %d px in [%d,%d]-[%d,%d]\n", name, diff, dx0, dy0, dx1, dy1);
        char n2[64]; snprintf(n2, sizeof n2, "%s_ref", name); write_ppm(n2, ref, true);
    }
    else { printf("  %-22s ok  (bright %3d)\n", name, s_bright); }
}

static void touch(bool d, int x, int y) { ui_touch(d, x, y, t_ms); frame(); }
static void tap(int x, int y) { touch(true, x, y); t_ms += 16; touch(true, x, y); t_ms += 60; touch(true, x, y); t_ms += 16; touch(false, x, y); }
static void drag(int x0, int y0, int x1, int y1, int ms, bool lift)
{
    touch(true, x0, y0);
    int n = ms / 16;
    for (int i = 1; i <= n; i++) { t_ms += 16; touch(true, x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n); }
    if (lift) { t_ms += 16; touch(false, x1, y1); }
}
static void crown(int d) { ui_crown(d, t_ms); frame(); }
static void button(bool lp) { if (!lp) { ui_button_edge(true, t_ms); ui_button_edge(false, t_ms); } ui_button(lp, t_ms); frame(); }

static int check(void)
{
    memcpy(inc, fb, sizeof fb);
    full_dirty = true; ui_frame(t_ms); memcpy(ref, fb, sizeof fb);
    int diff = 0;
    for (int y = 0; y < LCD_H; y++) for (int x = 0; x < LCD_W; x++) if (ref[y][x] != inc[y][x]) { if (diff < 6) printf("   (%d,%d) inc %04x ref %04x\n", x, y, inc[y][x], ref[y][x]); diff++; }
    return diff;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "analog"))
    {
        cfg.face = 1; cfg.timeout = 4;
        ui_init(&cfg, 0); run(2500);
        for (int i = 0; i < 400; i++)
        {
            t_ms += 16; frame();
            int d = check();
            if (d) { printf("t=%u sec=%u snap+%u: %d px stale  sa_from %.1f to %.1f drawn %.2f %.2f %.2f\n", t_ms, plat_now_s() % 60, t_ms - an_sec_t0, d, an_sec_from, an_sec_to, an_drawn[0], an_drawn[1], an_drawn[2]); }
        }
        return 0;
    }
    ui_init(&cfg, 0);
    for (int i = 0; i < 12; i++) { run(130); char n[32]; snprintf(n, sizeof n, "boot_%02d", i); write_ppm(n, fb, s_disp); }
    run(1000);
    shot("face_stack");
    drag(300, 220, 120, 224, 160, false); run(16);
    write_ppm("face_swipe_mid", fb, true);
    t_ms += 16; touch(false, 120, 224); run(500);
    shot("face_analog");
    run(300); write_ppm("analog_tick_mid", fb, true); run(900);
    shot("face_analog_later");
    drag(300, 220, 100, 224, 120, true); run(500);
    shot("face_modular");
    drag(60, 220, 300, 224, 120, true); run(500);
    drag(60, 220, 300, 224, 120, true); run(500);
    shot("face_back_to_stack");
    drag(184, 430, 184, 200, 200, false); write_ppm("cc_drag", fb, true);
    t_ms += 16; touch(false, 184, 200); run(500);
    shot("control_centre");
    crown(-2); run(100); shot("cc_dimmer");
    button(false); run(400);
    shot("cc_closed");
    button(false); run(150); write_ppm("apps_rise_mid", fb, true); run(400);
    shot("apps");
    drag(184, 380, 184, 180, 150, true); run(60); write_ppm("apps_fling", fb, true); run(1500);
    shot("apps_scrolled");
    crown(-9); run(600); shot("apps_crown_top");
    tap(150, APP_TOP + 46); run(150); write_ppm("push_mid", fb, true); run(400);
    shot("stopwatch");
    tap(276, 376); run(3250); shot("stopwatch_running");
    tap(92, 376); run(1100); tap(92, 376); run(700); shot("stopwatch_laps");
    tap(276, 376); run(100); shot("stopwatch_stopped");
    drag(20, 200, 200, 200, 150, false); write_ppm("back_swipe_mid", fb, true);
    t_ms += 16; touch(false, 200, 200); run(500);
    shot("back_to_apps");
    tap(150, APP_TOP + 46 + APP_ROW); run(500); shot("timer_setup");
    crown(3); run(100); shot("timer_crown");
    tap(184, 396); run(2000); shot("timer_running");
    tap(276, 410); run(300); shot("timer_paused");
    button(false); run(500); shot("face_from_timer");
    button(false); run(500);
    tap(150, APP_TOP + 46 + 2 * APP_ROW); run(500);
    drag(80, 120, 280, 300, 300, true); drag(100, 300, 300, 120, 200, true); run(100);
    tap(64 + 48 * 3, 414); drag(150, 150, 220, 250, 200, true); run(100);
    shot("sketch");
    button(false); run(500); button(false); run(500);
    tap(150, APP_TOP + 46 + 3 * APP_ROW); run(500); shot("flashlight");
    tap(184, 224); run(2500); shot("flashlight_warm");
    button(false); run(500); button(false); run(500);
    drag(184, 380, 184, 150, 150, true); run(1500);
    shot("apps_bottom");
    tap(150, (int)(APP_TOP + 46 + 4 * APP_ROW - sc_apps.pos)); run(500); shot("breathe");
    tap(184, 388); run(2000); shot("breathe_in");
    run(3000); shot("breathe_hold");
    run(3000); shot("breathe_out");
    button(false); run(500); button(false); run(500);
    drag(184, 380, 184, 150, 150, true); run(1500);
    tap(150, (int)(APP_TOP + 46 + 5 * APP_ROW - sc_apps.pos)); run(500); shot("settings");
    tap(320, (int)set_row_y(ST_AOD) + 40); run(80); write_ppm("toggle_mid", fb, true); run(400); shot("settings_aod_on");
    tap(50 + 54 * 2, (int)set_row_y(ST_ACCENT) + 58); run(200); shot("settings_green");
    crown(3); run(400); shot("settings_scrolled");
    button(false); run(500); shot("face_green_accent");
    button(false); run(500);
    drag(184, 380, 184, 150, 150, true); run(1500);
    tap(150, (int)(APP_TOP + 46 + 6 * APP_ROW - sc_apps.pos)); run(500); shot("about");
    button(false); run(600);
    run(20000); shot("aod_dim");
    tap(184, 224); run(400); shot("woken");
    cfg.always_on = 0;
    run(20000); printf("  display after timeout: %s, bright %d\n", s_disp ? "ON" : "off", s_bright);
    button(false); run(400); shot("woken_button");
    button(true); run(600); printf("  after long press: display %s\n", s_disp ? "ON" : "off");
    sim_pct = 72; button(false); run(400); drag(184, 430, 184, 200, 200, true); run(500); shot("cc_battery");
    button(false); run(400);
    sim_pct = 4; ui_lowbat(true, t_ms); run(500); shot("lowbat");
    tap(184, 224); crown(2); run(100); printf("  low mode, after tap+crown: screen %d (S_LOW=%d)\n", scr, S_LOW);
    run(5000); printf("  low mode after 5 s: display %s\n", s_disp ? "ON" : "off");
    button(false); run(400); shot("lowbat_button");
    sim_pct = 40; ui_lowbat(false, t_ms); run(600); shot("lowbat_exit");
    ui_dfu(); write_ppm("dfu", fb, true);
    printf("frames %u (%u sent), avg ui_frame %.1f us on this laptop, saves %d, %d/%d screenshots consistent\n",
           frames, sent_frames, render_ns / frames / 1000.0, saves, shots - bad_shots, shots);
    return bad_shots ? 1 : 0;
}
