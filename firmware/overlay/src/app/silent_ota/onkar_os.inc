/* ====================================================================================
 * ONKAR OS 19 - hardware core. Included into onkar_tool.c (shares its pin / SPIC2 / I2C helpers).
 * The look and behaviour live in onkar_gfx.inc (anti-aliased renderer) + onkar_ui.inc (screens, gestures),
 * which also run on the laptop (sim.c) - this file only connects them to the watch.
 *
 * Screen: 368 x 448 AMOLED, QSPI on SPIC2, RGB565. Only the changed rectangle is redrawn, in strips:
 *   each strip = its own window (2A/2B) + one 2C memory write fed by GDMA, while the CPU renders the next strip.
 * Power: P0_1 panel logic (soft start), P2_0 reset, P2_5 high-voltage supply (TIM5 PWM ramp, never switched hard).
 * Start: by itself 3 s after power-up (Bluetooth is advertising from the first second, so OTA always works).
 *   Every start is counted in FTL before it begins and cleared after 60 s of running. A start that killed the
 *   chip (power dip on the charger) therefore shows up as a count, and the next start waits longer and ramps the
 *   supply slower: count 0 -> 2 s / 120 ms steps, 1 -> 2 s / 250 ms, 2 -> 6 s / 500 ms, 3 -> 12 s, 4+ -> 30 s.
 *   v18: a failed start is a hang ~0.1 s after P2_5 reaches ~10% (supply glitch on charger power) -> 8 s watchdog.
 *   The ramp therefore runs with brightness 0 (no OLED current while the supply comes up), in fine steps through
 *   4..30%, and nothing else new (haptic timers) is started until the screen runs.
 *   It always tries again; the laptop can switch autostart off (0x32 01 00).
 *   v19: ALWAYS starts (no autostart setting; laptop stop = pause 60 s). Waits 0.3 s / 1.5 s / 3 s by failure count,
 *   15 s after 6+ failures in a row (time for a firmware update to connect). A DFU start freezes the OS (OS_DFU).
 *   Charger only (no battery) the chip supply is ~3.29 V and sags ~200 mV while P2_5 ramps: each step now waits
 *   (40 ms polls, max 1 s) while VBAT is >180 mV under its idle value. Haptic timers start 1 s into RUN.
 *   P2_5 stays on the PWM at 99.96% (never stopped: that glitch killed the watch right after every ramp).
 *   Brightness is capped from 0 after the ramp and rises +8 per tick only while VBAT holds (-24 when it sags), 5 s.
 *   Battery: VBAT read every 10 s; a real battery = VBAT ever >= 3.6 V. <6% for 30 s -> low-battery mode
 *   (screen dark, no touch/crown/haptics, button shows the battery 4 s, OS tick 100 ms while dark); >=10% leaves it.
 * Inputs: touch CHSC5816 at 0x2E on SCL P0_5 / SDA P1_2 (INT P2_4, low while touched), button P3_1 (low = pressed),
 *   crown = QDEC Y on P1_0 / P2_6 (2 counts per click).
 * Haptics (v18): motor on P0_0 (LOW = on). TIM2 25 kHz PWM = strength, TIM3 1 kHz tick (RAM ISR) = pattern timing.
 *   Crown mode 1 samples P1_0 / P2_6 in the same 1 kHz tick (software quadrature) instead of the QDEC block.
 * Loop: 20 ms timer tick on the app task; while something moves the next tick is posted at once (frame rate = as
 *   fast as render + send allow). Bluetooth events are queued in between, so they are never starved.
 * ==================================================================================== */
#include <stdlib.h>
#include "ftl.h"
#include "gap_msg.h"
#include "rtl876x_qdec.h"
#include "rtl876x_nvic.h"

extern T_GAP_CONN_STATE gap_conn_state;

#define OS_TICK_MS      20
#define OS_FTL_OFFSET   0x200
#define OS_MAGIC        0x354B4E4F      /* "ONK5": fob 0.2 starts from fresh defaults (always on) */

/* the renderer + UI: speed where it counts, size everywhere else */
#pragma GCC push_options
#pragma GCC optimize ("O2", "no-math-errno")
#include "onkar_gfx.inc"
#pragma GCC pop_options
#pragma GCC push_options
#pragma GCC optimize ("Os", "no-math-errno")
#include "onkar_ui.inc"
#pragma GCC pop_options

/* ------------------------------------------------------------------ settings (FTL) */
typedef struct
{
    uint32_t magic;
    uint8_t fails;          /* screen starts that never ran for 60 s */
    uint8_t ramp10;         /* minimum ms / 10 per P2_5 ramp step */
    uint8_t button_pin;     /* 0xFF = auto (P3_1 or P2_2 away from idle) */
    uint8_t touch_rst;      /* 0xFF = none */
    uint8_t touch_flags;    /* bit0 swap x/y, bit1 mirror x, bit2 mirror y, bit3 reverse crown */
    uint8_t te_sync;
    uint8_t full_rows;      /* 1 = always send whole rows (if partial-width windows ever show garbage) */
    uint8_t crown_mode;     /* 0 = QDEC block, 1 = software decoding of P1_0 / P2_6 at 1 kHz */
    uint8_t crown_div;      /* crown counts per click */
    uint8_t hap_inv;        /* 1 = motor on while the PWM output is HIGH (if haptics feel inverted) */
    uint8_t fastramp;       /* 1 = P2_5 from ~6% to 96% in one 0.4 s burst from RAM, interrupts off (no flash reads
                               while the OLED supply starts: the charger-only dips hang the chip at ~10%) */
    uint8_t spare;          /* v19: firmware version that last ran (new firmware -> failure count reset) */
    uint32_t epoch;         /* last known local time, saved every 5 min (no battery: power cuts lose the clock) */
    T_UI_CFG ui;            /* bright, always_on, autostart, timeout, face, accent */
} T_OS_CFG;

static T_OS_CFG cfg;

static void cfg_defaults(void)
{
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = OS_MAGIC;
    cfg.ramp10 = 10;
    cfg.button_pin = 0xFF;
    cfg.touch_rst = 0xFF;
    cfg.te_sync = 0;                /* TE wait cost ~25 ms per frame (14.5 fps vs 43 ms render) */
    cfg.ui.bright = 180;
    cfg.ui.autostart = 1;
    cfg.ui.always_on = 1;           /* carry is the always-on display */
    cfg.ui.timeout = 1;             /* 15 s */
    cfg.ui.accent = 0;              /* orange */
    cfg.ui.haptic = 2;              /* medium */
    cfg.crown_div = 2;
    cfg.crown_mode = 1;             /* v19: the QDEC block never counts on this board */
}

static void cfg_save(void) { ftl_save(&cfg, OS_FTL_OFFSET, sizeof(cfg)); }

static void cfg_load(void)
{
    if (ftl_load(&cfg, OS_FTL_OFFSET, sizeof(cfg)) != 0 || cfg.magic != OS_MAGIC) { cfg_defaults(); }
    if (cfg.ramp10 < 5) { cfg.ramp10 = 5; }
    if (cfg.button_pin != 0xFF && (!pin_valid(cfg.button_pin) || cfg.button_pin == P1_3 || cfg.button_pin == P1_4)) { cfg.button_pin = 0xFF; }
    if (cfg.ui.bright < 16) { cfg.ui.bright = 180; }
    if (cfg.spare != ONKAR_FW_VER) { cfg.spare = ONKAR_FW_VER; cfg.fails = 0; cfg.crown_mode = 1; cfg_save(); }
}

/* ------------------------------------------------------------------ pin helpers (not tracked by driven_mask:
   the laptop dead-man / disconnect release must never pull the screen's power) */
static void oos_out(uint8_t pin, uint8_t level)
{
    Pinmux_Config(pin, IDLE_MODE);
    Pad_Config(pin, PAD_SW_MODE, PAD_IS_PWRON, level ? PAD_PULL_UP : PAD_PULL_DOWN, PAD_OUT_ENABLE,
               level ? PAD_OUT_HIGH : PAD_OUT_LOW);
}

static void oos_gpio_in(uint8_t pin, PAD_Pull_Mode pull)
{
    RCC_PeriphClockCmd(APBPeriph_GPIO, APBPeriph_GPIO_CLOCK, ENABLE);
    Pinmux_Config(pin, DWGPIO);
    Pad_Config(pin, PAD_PINMUX_MODE, PAD_IS_PWRON, pull, PAD_OUT_DISABLE, PAD_OUT_LOW);
    GPIO_InitTypeDef g;
    GPIO_StructInit(&g);
    g.GPIO_Pin = GPIO_GetPin(pin);
    g.GPIO_Mode = GPIO_Mode_IN;
    g.GPIO_ITCmd = DISABLE;
    GPIO_Init(&g);
}

/* soft start a plain power-enable pin by GPIO PWM: ramp_ms, then held HIGH */
static void oos_soft_on(uint8_t pin, uint16_t ramp_ms)
{
    oos_gpio_in(pin, PAD_PULL_NONE);
    uint32_t m = GPIO_GetPin(pin);
    g_out(m, 0);
    uint32_t steps = (uint32_t)ramp_ms * 4;
    for (uint32_t k = 1; k <= steps; k++)
    {
        uint32_t on = (250 * k) / steps;
        g_set(m);
        platform_delay_us(on);
        if (k < steps) { g_clr(m); }
        platform_delay_us(250 - on);
    }
    oos_out(pin, 1);
}

/* ------------------------------------------------------------------ panel commands */
static void lcd_cmd(uint8_t hi, uint8_t lo, const uint8_t *d, uint8_t n)
{
    uint8_t h[4] = {0x02, 0x00, hi, lo};
    lcd_hw_frame(h, 4, d, n);
}
static void lcd_cmd1(uint8_t hi, uint8_t v) { lcd_cmd(hi, 0x00, &v, 1); }

typedef struct { uint8_t hi, lo, n; uint8_t d[16]; } T_LCD_INIT;
static const T_LCD_INIT LCD_INIT[] =          /* stock 0x83BBF4, byte for byte */
{
    {0xFF, 0x00, 3, {0x23, 0x08, 0x01}},
    {0xFF, 0x80, 2, {0x23, 0x08}},
    {0xC0, 0x83, 1, {0xA4}},
    {0xC0, 0x89, 1, {0xA4}},
    {0xC0, 0x93, 1, {0xA4}},
    {0xC0, 0x99, 1, {0xA4}},
    {0xB2, 0x83, 1, {0x12}},
    {0x3A, 0x00, 1, {0x55}},
    {0x51, 0x00, 2, {0x00, 0x00}},
    {0x35, 0x00, 1, {0x00}},
    {0xC0, 0xC4, 1, {0x80}},
    {0xC3, 0x80, 16, {0xF1, 0xF2, 0xF2, 0xF2, 0, 0, 0, 0, 0, 0, 0, 0, 0x12, 0, 0, 0}},
    {0xC3, 0x90, 16, {0xEE, 0xFF, 0xEE, 0xEE, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xEE, 0, 0, 0}},
    {0xC3, 0xA0, 16, {0xEF, 0x55, 0xAA, 0xAA, 0x20, 0, 0, 0, 0, 0, 0x10, 0x10, 0xEF, 0, 0, 0}},
    {0xC5, 0xDF, 1, {0x00}},
    {0x11, 0x00, 1, {0x00}},
    {0x29, 0x00, 1, {0x00}},
    {0xFF, 0x00, 4, {0x23, 0x08, 0x01, 0x01}},
    {0xC5, 0xE2, 1, {0xAF}},
    {0xC5, 0xE5, 1, {0x3B}},
    {0xC4, 0x82, 1, {0x52}},
    {0xC4, 0x89, 1, {0x81}},
};

/* ------------------------------------------------------------------ cycle counter (performance numbers) */
static void dwt_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}
static inline uint32_t cyc(void) { return DWT->CYCCNT; }
#define CYC_PER_US      90

static uint32_t st_wait_cyc, st_frame_us, st_wait_us, st_pix, st_frames, st_fps10, st_fps_n, st_fps_t0, st_max_us;

/* ------------------------------------------------------------------ frame streaming (gfx_out for onkar_gfx.inc) */
static void oos_dma_go(const void *src, uint32_t words)
{
    GDMA_InitTypeDef d;
    GDMA_StructInit(&d);
    d.GDMA_ChannelNum = 1;
    d.GDMA_DIR = GDMA_DIR_MemoryToPeripheral;
    d.GDMA_BufferSize = words;
    d.GDMA_SourceInc = DMA_SourceInc_Inc;
    d.GDMA_DestinationInc = DMA_DestinationInc_Fix;
    d.GDMA_SourceDataSize = GDMA_DataSize_Word;
    d.GDMA_DestinationDataSize = GDMA_DataSize_Word;
    d.GDMA_SourceMsize = GDMA_Msize_1;
    d.GDMA_DestinationMsize = GDMA_Msize_1;
    d.GDMA_SourceAddr = (uint32_t)src;
    d.GDMA_DestinationAddr = (uint32_t)&SPIC2->dr[0];
    d.GDMA_DestHandshake = GDMA_Handshake_SPIC2_TX;
    GDMA_Init(GDMA_Channel1, &d);
    GDMA_Cmd(1, ENABLE);
}

static bool oos_dma_wait(void)
{
    uint32_t guard = 0;
    while (GDMA_GetChannelStatus(1) && guard < 4000000) { guard++; }
    return guard < 4000000;
}

static void oos_stream_begin(void)
{
    SPIC2->ser = 1;
    SPIC2->ssienr = 0;
    SPIC2->ctrlr0 &= ~0x3F0000;
    SPIC2->ctrlr0 |= 0x80000;
    SPIC2->ctrlr0 &= ~0x300;
    SPIC2->imr |= 0x200;
    SPIC2->ctrlr2 |= 8;
    SPIC2->dr[0].word = 0x002C0032;     /* 32 00 2C 00 */
    SPIC2->dmacr = 2;
    SPIC2->dmatdlr = 4;
    SPIC2->ssienr = 1;
}

static void oos_stream_end(void)
{
    uint32_t guard = 0;
    while ((!(SPIC2->sr & BIT_TFE) || (SPIC2->sr & BIT_BUSY)) && guard < 400000) { guard++; }
    SPIC2->ssienr = 0;
    SPIC2->dmacr = 0;
    SPIC2->ctrlr2 &= ~8;
    SPIC2->imr &= ~0x200;
}

static void lcd_window(uint16_t x0, uint16_t x1, uint16_t y0, uint16_t y1)
{
    uint8_t xr[4] = {x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF};
    uint8_t yr[4] = {y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF};
    lcd_cmd(0x2A, 0x00, xr, 4);
    lcd_cmd(0x2B, 0x00, yr, 4);
}

/* start of a panel refresh (TE, P3_0), so a frame doesn't tear; max ~20 ms */
static void te_wait(void)
{
    if (!cfg.te_sync) { return; }
    uint32_t m = GPIO_GetPin(P3_0);
    uint32_t guard = 0;
    while ((g_read() & m) && guard < 20000) { platform_delay_us(1); guard++; }
    while (!(g_read() & m) && guard < 20000) { platform_delay_us(1); guard++; }
}

static bool out_busy;
static void gfx_frame_start(void) { te_wait(); }
static void gfx_out(int x0, int y0, int w, int h, const uint16_t *pix)
{
    lcd_window((uint16_t)x0, (uint16_t)(x0 + w - 1), (uint16_t)y0, (uint16_t)(y0 + h - 1));
    oos_stream_begin();
    oos_dma_go(pix, (uint32_t)w * h / 2);
    out_busy = true;
}
static void gfx_out_wait(void)
{
    if (!out_busy) { return; }
    uint32_t c = cyc();
    oos_dma_wait();
    oos_stream_end();
    out_busy = false;
    st_wait_cyc += cyc() - c;
}

/* ------------------------------------------------------------------ OS state */
enum { OS_OFF, OS_WAIT_BOOT, OS_RAMP, OS_RUN, OS_SLEEP, OS_DFU };
static uint8_t oos_state = OS_OFF;
static uint8_t ramp_i;
static uint32_t oos_next_ms, oos_run_since, boot_wait_ms, ramp_step_ms;
static bool fails_cleared;
static void *oos_timer = NULL;
static uint32_t tick_ms = OS_TICK_MS;
static volatile uint32_t oos_tick_sent, oos_tick_seen;
static bool oos_busy;                  /* UI wants the next frame now */

static const uint16_t RAMP[] = {10, 20, 50, 100, 130, 160, 180, 200, 215, 230, 245, 260, 280, 300, 330, 370, 420, 500,
                                600, 750, 1000, 1250, 1500, 2000, 2400, 2499};     /* 0.1 us of every 250 us. v19: ends at
    99.96% and STAYS on the PWM: switching to a plain HIGH output meant stopping the timer = P2_5 low for a moment =
    the converter restarting hard = the watch died right after the ramp (seen 3/3 on v18/v19) */
#define RAMP_N (sizeof(RAMP) / sizeof(RAMP[0]))
#define FAST_FROM       5                                       /* RAMP index where the RAM burst takes over */
#define P25_LC2         (*((volatile uint32_t *)0x400020C4UL))  /* TIM5 PWM high count (TIMER0_LOAD_COUNT2 + 5) */
static uint16_t RAMP_RAM[RAMP_N];                              /* RAM copy: the burst must not read flash */

/* runs from RAM with interrupts off; only register writes + the cycle counter. Returns with P2_5 at 96%. */
GFX_HOT static void p25_ram_burst(uint32_t step_cyc)
{
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    for (uint32_t i = FAST_FROM; i < RAMP_N - 1; i++)
    {
        uint32_t hc = (uint32_t)RAMP_RAM[i] * 4u;              /* 250 us period = 10000 counts */
        TIM5->LoadCount = 10000u - hc - 1u;
        P25_LC2 = hc - 1u;
        uint32_t c0 = DWT->CYCCNT;
        while (DWT->CYCCNT - c0 < step_cyc) { }
    }
    __set_PRIMASK(pm);
}

/* time: the laptop sends local time (0x31); restored from FTL after a power cut (then marked "not synced") */
static uint32_t epoch_at_set, ms_at_set, epoch_saved_ms;
static bool time_set, time_restored;

/* inputs */
static uint8_t btn_idle = 1, btn_cnt, btn_last;
static bool btn_down, btn_long_done;
static uint32_t btn_since, btn_presses;

typedef struct { uint32_t scl, sda; uint8_t scl_pin, sda_pin; } T_TBUS;
static T_TBUS tbus;
static uint8_t touch_addr;              /* 0 = not found */
static uint32_t touch_next_probe, touch_last_read;
static bool t_down;
static int16_t tp_x, tp_y;
static uint32_t touch_reads, touch_events;
static uint8_t touch_raw[8];

static const uint8_t MON_PIN[3] = {P2_2, P2_4, P3_1};
static uint8_t mon_lv[3], mon_idle[3];
static uint16_t mon_edges[3];
static uint16_t crown_last;
static int32_t crown_pos;
static int16_t crown_acc;
static int32_t crown_sw_last;

/* fake input from the laptop (0x33): a scripted finger */
static uint8_t fk_ev;
static int16_t fk_x0, fk_y0, fk_x1, fk_y1;
static uint32_t fk_t0;


/* ------------------------------------------------------------------ haptics + crown sampling (v18)
 * Motor = P0_0, LOW = motor on (stock: PWM2_N, idles HIGH). TIM2 = 25 kHz PWM on P0_0 (inaudible; duty = strength).
 * TIM3 = 1 kHz tick, ISR in RAM: steps through a pattern with 1 ms accuracy (the OS tick is 10 ms) and, in crown
 * mode 1, samples the crown pins. Between patterns P0_0 is a plain HIGH output again, so a stuck timer can never
 * leave the motor running. Patterns only play while the screen runs or is off - never during the P2_5 ramp. */
#define HAP_TIM         TIM2
#define HAP_TICK        TIM3
#define HAP_PERIOD      1600u                                      /* 40 MHz / 1600 = 25 kHz */
#define HAP_LC2         (*((volatile uint32_t *)0x400020B8UL))    /* TIM2 PWM high count (TIMER0_LOAD_COUNT2 + 2) */

typedef struct { uint8_t pct, ms; } hstep_t;
/* in RAM: the ISR must not read flash (it can fire while an FTL write has flash busy) */
static hstep_t HP_TICK_[] = {{100, 7}};
static hstep_t HP_SOFT_[] = {{70, 10}};
static hstep_t HP_TAP_[] = {{100, 14}};
static hstep_t HP_CLICK_[] = {{100, 22}};
static hstep_t HP_DOUBLE_[] = {{100, 16}, {0, 70}, {100, 16}};
static hstep_t HP_SUCCESS_[] = {{100, 12}, {0, 55}, {100, 30}};
static hstep_t HP_LONG_[] = {{100, 18}, {80, 70}};
static hstep_t HP_BUMP_[] = {{60, 16}};
static hstep_t HP_ALARM_[] = {{100, 200}, {0, 120}, {100, 200}, {0, 120}, {100, 200}};
static hstep_t HP_BOOT_[] = {{60, 25}, {0, 90}, {100, 35}};
static hstep_t HP_SCROLL_[] = {{100, 4}};             /* list rows / faces passing under the finger: barely there */
static hstep_t HP_PRESS_[] = {{100, 16}};             /* crown/button going down: firm */
static hstep_t HP_RELEASE_[] = {{75, 7}};             /* ... and coming back up: lighter */
typedef struct { hstep_t *p; uint8_t n; } hpat_t;
static const hpat_t HAPS[H_N] = {
    {HP_TICK_, 1}, {HP_SOFT_, 1}, {HP_TAP_, 1}, {HP_CLICK_, 1}, {HP_DOUBLE_, 3}, {HP_SUCCESS_, 3},
    {HP_LONG_, 2}, {HP_BUMP_, 1}, {HP_ALARM_, 5}, {HP_BOOT_, 3}, {HP_SCROLL_, 1}, {HP_PRESS_, 1}, {HP_RELEASE_, 1},
};
static const uint8_t HAP_DUTY[4] = {0, 60, 85, 100}, HAP_LEN[4] = {0, 70, 100, 130};   /* per level: % strength, % length */

static hstep_t *volatile hap_p;
static volatile uint8_t hap_n, hap_i, hap_kick, hap_cur, hap_sp, hap_sm, hap_invr;
static volatile uint16_t hap_left;
static volatile bool hap_on, hap_done = true;   /* on = P0_0 is on the PWM; done = nothing (more) to play */
static bool hap_ready;
static uint32_t hap_plays;

static volatile uint8_t crown_mode, crown_ab;
static volatile uint32_t crown_ma, crown_mb;
static volatile int32_t crown_sw;
static volatile uint16_t crown_edges[2];
static volatile int8_t CROWN_Q[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

__attribute__((always_inline)) static inline void hap_duty(uint8_t pct)        /* motor-on share of each 40 us period */
{
    uint32_t on = HAP_PERIOD * pct / 100u;
    if (on < 2) { on = 2; }
    if (on > HAP_PERIOD - 2) { on = HAP_PERIOD - 2; }
    if (hap_invr) { HAP_TIM->LoadCount = HAP_PERIOD - on - 1; HAP_LC2 = on - 1; }
    else { HAP_TIM->LoadCount = on - 1; HAP_LC2 = HAP_PERIOD - on - 1; }     /* LoadCount = low phase = motor on */
}

GFX_HOT static void onkar_tim3_isr(void)
{
    (void)HAP_TICK->EOI;
    if (crown_mode == 1)
    {
        uint32_t in = GPIO->DATAIN;
        uint8_t ab = (uint8_t)(((in & crown_ma) ? 2 : 0) | ((in & crown_mb) ? 1 : 0)), old = crown_ab;
        if (ab != old)
        {
            if ((ab ^ old) & 2) { crown_edges[0]++; }
            if ((ab ^ old) & 1) { crown_edges[1]++; }
            crown_sw += CROWN_Q[(old << 2) | ab];
            crown_ab = ab;
        }
    }
    if (!hap_on || hap_done) { return; }
    if (hap_kick && !--hap_kick) { hap_duty(hap_cur); }
    if (hap_left) { hap_left--; return; }
    if (hap_i >= hap_n) { hap_duty(0); hap_done = true; return; }
    hstep_t st = hap_p[hap_i];
    bool from_rest = hap_i == 0 || hap_p[hap_i - 1].pct == 0;
    hap_i++;
    uint32_t ms = (uint32_t)st.ms * (st.pct ? hap_sm : 100u) / 100u;
    hap_left = (uint16_t)(ms ? ms - 1 : 0);
    if (!st.pct) { hap_cur = 0; hap_kick = 0; hap_duty(0); return; }
    uint32_t pct = (uint32_t)st.pct * hap_sp / 100u;
    hap_cur = (uint8_t)(pct < 40 ? 40 : pct);
    if (from_rest && hap_cur < 100) { hap_kick = 3; hap_duty(100); }   /* ERM: full kick so short taps start crisp */
    else { hap_kick = 0; hap_duty(hap_cur); }
}

static void hap_init(void)
{
    RCC_PeriphClockCmd(APBPeriph_TIMER, APBPeriph_TIMER_CLOCK, ENABLE);
    TIM_TimeBaseInitTypeDef t;
    TIM_StructInit(&t);
    t.TIM_Mode = TIM_Mode_UserDefine;
    t.TIM_PWM_En = PWM_ENABLE;
    t.TIM_PWM_High_Count = HAP_PERIOD - 3;
    t.TIM_PWM_Low_Count = 1;
    TIM_TimeBaseInit(HAP_TIM, &t);                 /* not running yet; P0_0 stays a plain HIGH output */
    TIM_StructInit(&t);
    t.TIM_Mode = TIM_Mode_UserDefine;
    t.TIM_PWM_En = PWM_DISABLE;
    t.TIM_Period = 40000 - 1;                      /* 1 ms */
    TIM_TimeBaseInit(HAP_TICK, &t);
    RamVectorTableUpdate(Timer3_VECTORn, (IRQ_Fun)onkar_tim3_isr);
    NVIC_InitTypeDef n;
    n.NVIC_IRQChannel = TIMER3_IRQn;
    n.NVIC_IRQChannelPriority = 3;
    n.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&n);
    TIM_ClearINT(HAP_TICK);
    TIM_INTConfig(HAP_TICK, ENABLE);
    TIM_Cmd(HAP_TICK, ENABLE);
    hap_ready = true;
}

static void hap_park(void)          /* pattern over (or stop): P0_0 back to a plain HIGH output, PWM off */
{
    hap_done = true;
    Pad_Config(P0_0, PAD_SW_MODE, PAD_IS_PWRON, PAD_PULL_UP, PAD_OUT_ENABLE, PAD_OUT_HIGH);
    Pinmux_Config(P0_0, IDLE_MODE);
    TIM_Cmd(HAP_TIM, DISABLE);
    hap_on = false;
}

static bool hap_play(uint8_t id)
{
    uint8_t lv = cfg.ui.haptic & 3;
    if (!hap_ready || id >= H_N || !lv || oos_state == OS_RAMP || oos_state == OS_DFU) { return false; }
    hap_done = true;                               /* ISR leaves everything alone while we set up */
    hap_sp = HAP_DUTY[lv]; hap_sm = HAP_LEN[lv]; hap_invr = cfg.hap_inv;
    hap_p = HAPS[id].p; hap_n = HAPS[id].n; hap_i = 0; hap_left = 0; hap_kick = 0;
    if (!hap_on)
    {
        hap_duty(0);
        TIM_Cmd(HAP_TIM, ENABLE);
        Pinmux_Config(P0_0, timer_pwm2);
        Pad_Config(P0_0, PAD_PINMUX_MODE, PAD_IS_PWRON, PAD_PULL_UP, PAD_OUT_DISABLE, PAD_OUT_LOW);
        hap_on = true;
    }
    hap_plays++;
    hap_done = false;
    return true;
}

static void plat_haptic(uint8_t id) { hap_play(id); }

/* ------------------------------------------------------------------ battery (chip VBAT through the ADC) */
static uint16_t bat_mv, bat_raw_mv, v_idle, ramp_vmin = 0xFFFF;
static uint8_t bat_pct, low_cnt, ramp_holds, ramp_hold_n;
static bool bat_real, low_mode;
static uint32_t bat_next;

static uint16_t vbat_now(void) { return onkar_battery_read(NULL, NULL, NULL); }

static void bat_update(void)
{
    uint8_t pc; uint16_t mv = onkar_battery_read(NULL, &pc, NULL);
    if (mv < 2000 || mv > 5000) { return; }                     /* ADC glitch: ignore */
    bat_raw_mv = mv;
    bat_mv = bat_mv ? (uint16_t)((bat_mv * 3u + mv) / 4u) : mv;
    if (mv >= 3600) { bat_real = true; }                        /* charger-only supply sits at ~3.29 V */
    bat_pct = mv_to_pct(bat_mv);
}

static int plat_batt(int *mv) { *mv = bat_mv; return bat_real ? bat_pct : -1; }

/* ------------------------------------------------------------------ platform functions for onkar_ui.inc */
static uint32_t plat_ms(void) { return os_sys_time_get(); }
/* brightness = min(UI value, cap). The cap starts at 0 when the screen comes up and rises only while the supply holds
   (charger-only power can't take the OLED current all at once) */
static uint8_t bright_req, bright_cap = 255, bright_set = 0xFF;
static uint32_t cap_until;
static void bright_apply(void)
{
    uint8_t v = bright_req < bright_cap ? bright_req : bright_cap;
    if (v != bright_set && (oos_state == OS_RUN || oos_state == OS_SLEEP)) { bright_set = v; lcd_cmd1(0x51, v); }
}
static void plat_bright(uint8_t v) { bright_req = v; bright_apply(); }
static bool plat_bright_ready(void) { return bright_cap >= bright_req; }
static void plat_display(bool on)
{
    if (oos_state != OS_RUN && oos_state != OS_SLEEP) { return; }
    if (on) { lcd_cmd1(0x29, 0x00); oos_state = OS_RUN; }
    else { lcd_cmd1(0x51, 0); bright_set = 0; lcd_cmd1(0x28, 0x00); oos_state = OS_SLEEP; }
}
static bool plat_bt(void) { return gap_conn_state == GAP_CONN_STATE_CONNECTED; }
static uint32_t plat_now_s(void) { return epoch_at_set + (os_sys_time_get() - ms_at_set) / 1000; }
static bool plat_time_ok(void) { return time_set; }
static void plat_save(void) { cfg_save(); }

static bool plat_info(int i, char *k, char *v)
{
    char *p = v;
    uint32_t up = os_sys_time_get() / 1000;
    switch (i)
    {
    case 0: u_s(k, "Firmware"); u_s(v, "fob 0.2"); break;
    case 1: u_s(k, "Running for"); p = u_num(p, up / 3600); p = u_s(p, "h "); p = u_num(p, (up / 60) % 60); u_s(p, "m"); break;
    case 2: u_s(k, "Bluetooth"); u_s(v, plat_bt() ? "Connected" : "Visible, not connected"); break;
    case 3: u_s(k, "Clock"); u_s(v, time_set ? "Synced from laptop" : time_restored ? "Restored (may be behind)" : "Not set"); break;
    case 4: u_s(k, "Touch"); if (touch_addr) { p = u_s(p, "CHSC5816, "); p = u_num(p, touch_reads); u_s(p, " reads"); } else { u_s(v, "Not found"); } break;
    case 5: u_s(k, "Frame rate"); p = u_num(p, st_fps10 / 10); *p++ = '.'; p = u_num(p, st_fps10 % 10); u_s(p, " fps"); break;
    case 6: u_s(k, "Last frame"); p = u_num(p, st_frame_us / 1000); *p++ = '.'; p = u_num(p, (st_frame_us / 100) % 10); p = u_s(p, " ms, "); p = u_num(p, st_pix / 1000); u_s(p, "k px"); break;
    case 7:
        u_s(k, "Last restart");
        u_s(v, boot_reset_reason == 0 ? "Power on" : boot_reset_reason == 1 ? "Watchdog" : boot_reset_reason == 0xD4 ? "After update" : "Other");
        break;
    case 8: u_s(k, "Failed starts"); u_num(v, cfg.fails); break;
    case 9:
        u_s(k, "Crown");
        p = u_s(p, crown_mode == 1 ? "Pins, " : "QDEC, "); p = u_num(p, (uint32_t)(crown_pos < 0 ? -crown_pos : crown_pos)); u_s(p, " counts");
        break;
    case 10:
        u_s(k, "Power");
        p = u_s(p, bat_real ? "Battery " : "Charger only, ");
        if (bat_real) { p = u_num(p, bat_pct); p = u_s(p, "%, "); }
        p = u_num(p, bat_mv / 1000); *p++ = '.'; p = u_2(p, (bat_mv / 10) % 100); u_s(p, " V");
        break;
    default: return false;
    }
    return true;
}

/* ------------------------------------------------------------------ touch (I2C, bit-banged) */
static bool tb_w(uint8_t v)
{
    T_I2C b = {tbus.scl, tbus.sda};
    return i2c_wbyte(&b, v);
}

/* write nw bytes, then (repeated start) read n bytes; nw 0 = plain read */
static bool tb_xfer(uint8_t addr, const uint8_t *w, uint8_t nw, uint8_t *out, uint8_t n)
{
    T_I2C b = {tbus.scl, tbus.sda};
    i2c_start(&b);
    if (nw)
    {
        if (!tb_w(addr << 1)) { i2c_stop(&b); return false; }
        for (uint8_t i = 0; i < nw; i++) { if (!tb_w(w[i])) { i2c_stop(&b); return false; } }
        if (!n) { i2c_stop(&b); return true; }
        i2c_start(&b);
    }
    if (!tb_w((addr << 1) | 1)) { i2c_stop(&b); return false; }
    for (uint8_t i = 0; i < n; i++) { out[i] = i2c_rbyte(&b, i + 1 < n); }
    i2c_stop(&b);
    return true;
}

static void tbus_open(uint8_t scl, uint8_t sda)
{
    tbus.scl_pin = scl; tbus.sda_pin = sda;
    tbus.scl = GPIO_GetPin(scl); tbus.sda = GPIO_GetPin(sda);
    oos_gpio_in(scl, PAD_PULL_UP);
    oos_gpio_in(sda, PAD_PULL_UP);
    platform_delay_us(50);
}

static void touch_probe(void)
{
    tbus_open(P0_5, P1_2);
    touch_addr = 0;
    if (!(g_read() & tbus.scl) || !(g_read() & tbus.sda)) { return; }
    T_I2C b = {tbus.scl, tbus.sda};
    i2c_start(&b);
    bool ack = i2c_wbyte(&b, 0x2E << 1);          /* address ACK only: the chip NAKs a 1-byte register write */
    i2c_stop(&b);
    if (ack) { touch_addr = 0x2E; }
}

/* CHSC5816: read 8 bytes at 32-bit address 0x2000002C: FF n xl yl z (xh | yh << 4) (id | ev << 4) - */
static bool touch_poll(bool *down, int16_t *x, int16_t *y)
{
    uint8_t d[8];
    static const uint8_t reg[4] = {0x20, 0x00, 0x00, 0x2C};
    if (!tb_xfer(touch_addr, reg, 4, d, 8)) { return false; }
    memcpy(touch_raw, d, 8);
    touch_reads++;
    if (d[0] != 0xFF || d[1] > 2) { *down = false; return true; }
    *down = d[1] != 0 && (d[6] >> 4) != 1;
    int16_t rx = (int16_t)(((d[5] & 0x0F) << 8) | d[2]), ry = (int16_t)(((d[5] >> 4) << 8) | d[3]);
    if (cfg.touch_flags & 1) { int16_t t = rx; rx = ry; ry = t; }
    if (cfg.touch_flags & 2) { rx = LCD_W - 1 - rx; }
    if (cfg.touch_flags & 4) { ry = LCD_H - 1 - ry; }
    if (rx < 0) { rx = 0; } if (rx >= LCD_W) { rx = LCD_W - 1; }
    if (ry < 0) { ry = 0; } if (ry >= LCD_H) { ry = LCD_H - 1; }
    *x = rx; *y = ry;
    return true;
}

/* ------------------------------------------------------------------ screen power */
static void lcd_power_up(void)
{
    oos_out(P2_5, 0);
    spic2_stock_init(true);                  /* all 4 data lines: P0_6 = D2, P0_7 = D3 (stock + Realtek driver do this) */
    RCC_PeriphClockCmd(APBPeriph_IF8080, APBPeriph_IF8080_CLOCK, DISABLE);
    RCC_PeriphClockCmd(APBPeriph_IF8080, APBPeriph_IF8080_CLOCK, ENABLE);
    RCC_PeriphClockCmd(APBPeriph_GDMA, APBPeriph_GDMA_CLOCK, ENABLE);
    oos_out(P2_0, 1);
    oos_soft_on(P0_1, 100);
    platform_delay_ms(50);
    oos_out(P2_0, 0);
    platform_delay_ms(120);
    oos_out(P2_0, 1);
    platform_delay_ms(50);
    wdg_kick();
    for (uint8_t i = 0; i < sizeof(LCD_INIT) / sizeof(LCD_INIT[0]); i++)
    {
        lcd_cmd(LCD_INIT[i].hi, LCD_INIT[i].lo, LCD_INIT[i].d, LCD_INIT[i].n);
        if (LCD_INIT[i].hi == 0x11) { platform_delay_ms(120); }
    }
    platform_delay_ms(120);
    lcd_cmd1(0x29, 0x00);
    lcd_cmd1(0x51, 0);
    oos_gpio_in(P3_0, PAD_PULL_NONE);                    /* TE */
    gfx_begin(0);                                        /* clear the panel's memory */
    gfx_invalidate_all();
    gfx_flush();
    wdg_kick();
}

static void oos_frame(uint32_t now);

static void oos_p25_pwm(uint16_t high)
{
    if (high >= 2500)
    {
        pwm_stop();
        oos_out(P2_5, 1);
        return;
    }
    pwm_apply(P2_5, 250, high, false);
}

static void oos_start(void)
{
    if (oos_state == OS_RAMP || oos_state == OS_RUN || oos_state == OS_SLEEP || oos_state == OS_DFU) { return; }
    uint8_t f = cfg.fails;
    ramp_step_ms = bat_real ? 120 : 500;     /* charger only: 500 ms steps got through the ramp 3/3, 100-120 ms died ~10% */
    if (ramp_step_ms < (uint32_t)cfg.ramp10 * 10) { ramp_step_ms = (uint32_t)cfg.ramp10 * 10; }
    if (cfg.fails < 255) { cfg.fails++; }
    cfg_save();
    fails_cleared = false;
    { uint32_t a = 0; for (int i = 0; i < 4; i++) { a += vbat_now(); } v_idle = (uint16_t)(a / 4); }
    ramp_vmin = 0xFFFF; ramp_holds = 0; ramp_hold_n = 0;
    lcd_power_up();
    uint32_t t = os_sys_time_get();
    ui_init(&cfg.ui, t);
    ui_boot_lit(t);
    oos_frame(t);                           /* finished logo in panel memory, brightness stays 0 during the ramp */
    ramp_i = 0;
    oos_state = OS_RAMP;
    oos_next_ms = os_sys_time_get();
}

static void oos_stop(void)
{
    if (hap_on) { hap_park(); }
    if (oos_state == OS_OFF || oos_state == OS_WAIT_BOOT) { oos_state = OS_OFF; return; }
    lcd_cmd1(0x51, 0);
    lcd_cmd1(0x28, 0x00);
    platform_delay_ms(100);
    lcd_cmd1(0x10, 0x00);
    platform_delay_ms(120);
    pwm_stop();
    oos_out(P2_5, 0);
    platform_delay_ms(50);
    oos_out(P0_1, 0);
    oos_state = OS_OFF;
}

/* ------------------------------------------------------------------ inputs */
static void input_poll(uint32_t now)
{
    for (uint8_t i = 0; i < 3; i++)
    {
        uint8_t v = (g_read() & GPIO_GetPin(MON_PIN[i])) ? 1 : 0;
        if (v != mon_lv[i]) { mon_edges[i]++; mon_lv[i] = v; }
    }
    /* crown: QDEC Y (mode 0) or the 1 kHz pin sampler (mode 1); crown_div counts per click */
    int32_t dc;
    if (crown_mode == 1) { int32_t c = crown_sw; dc = c - crown_sw_last; crown_sw_last = c; }
    else { uint16_t cc = QDEC_GetAxisCount(QDEC, QDEC_AXIS_Y); dc = (int16_t)(cc - crown_last); crown_last = cc; }
    if (dc)
    {
        crown_pos += dc;
        crown_acc += (int16_t)((cfg.touch_flags & 8) ? -dc : dc);
    }
    int16_t dv = cfg.crown_div ? cfg.crown_div : 2;
    while (crown_acc >= dv) { crown_acc -= dv; ui_crown(1, now); }
    while (crown_acc <= -dv) { crown_acc += dv; ui_crown(-1, now); }

    /* button (debounced 2 ticks); long press 0.9 s */
    uint8_t lv;
    if (cfg.button_pin == 0xFF) { lv = (mon_lv[0] != mon_idle[0] || mon_lv[2] != mon_idle[2]) ? (uint8_t)(btn_idle ^ 1) : btn_idle; }
    else { lv = (g_read() & GPIO_GetPin(cfg.button_pin)) ? 1 : 0; }
    if (lv != btn_last) { btn_cnt = 0; btn_last = lv; }
    else if (btn_cnt < 3) { btn_cnt++; }
    if (btn_cnt == 2)
    {
        bool down = lv != btn_idle;
        if (down && !btn_down) { btn_down = true; btn_since = now; btn_long_done = false; ui_button_edge(true, now); }
        else if (!down && btn_down)
        {
            btn_down = false;
            btn_presses++;
            if (!btn_long_done) { ui_button_edge(false, now); ui_button(false, now); }
        }
    }
    if (btn_down && !btn_long_done && now - btn_since > 900) { btn_long_done = true; ui_button(true, now); }

    /* scripted finger from the laptop */
    if (fk_ev)
    {
        uint32_t t = now - fk_t0, dur = fk_ev == 1 ? 60 : (fk_ev == 6 ? 800 : 220);
        if (t < dur)
        {
            int x = fk_x0 + (int)((fk_x1 - fk_x0) * (int32_t)t / (int32_t)dur), y = fk_y0 + (int)((fk_y1 - fk_y0) * (int32_t)t / (int32_t)dur);
            ui_touch(true, x, y, now);
            oos_busy = true;
        }
        else { ui_touch(false, fk_x1, fk_y1, now); fk_ev = 0; }
        return;
    }

    if (low_mode) { return; }                   /* low battery: no touch reads at all */
    /* touch: read while INT (P2_4) is low, while the finger is down, and every 100 ms anyway */
    if (!touch_addr)
    {
        if (now >= touch_next_probe) { touch_probe(); touch_next_probe = now + 2000; }
        return;
    }
    bool intr = !(g_read() & GPIO_GetPin(P2_4));
    if (!intr && !t_down && now - touch_last_read < 100) { return; }
    touch_last_read = now;
    bool down; int16_t x, y;
    if (!touch_poll(&down, &x, &y)) { down = false; x = tp_x; y = tp_y; }
    if (down)
    {
        if (!t_down) { touch_events++; }
        t_down = true; tp_x = x; tp_y = y;
        ui_touch(true, x, y, now);
    }
    else if (t_down)
    {
        t_down = false;
        ui_touch(false, tp_x, tp_y, now);
    }
}

/* ------------------------------------------------------------------ one UI frame + numbers */
static void oos_frame(uint32_t now)
{
    uint32_t c0 = cyc();
    st_wait_cyc = 0;
    oos_busy = ui_frame(now) || oos_busy;
    uint32_t px = g_stat_pix;
    g_stat_pix = 0;
    if (px)
    {
        st_frame_us = (cyc() - c0) / CYC_PER_US;
        st_wait_us = st_wait_cyc / CYC_PER_US;
        st_pix = px;
        st_frames++;
        st_fps_n++;
        if (st_frame_us > st_max_us) { st_max_us = st_frame_us; }
        wdg_kick();
    }
    if (now - st_fps_t0 >= 1000) { st_fps10 = st_fps_n * 10000 / (now - st_fps_t0); st_fps_n = 0; st_fps_t0 = now; }
}

static void time_tick(uint32_t now)
{
    if ((time_set || time_restored) && now - epoch_saved_ms >= 300000)
    {
        epoch_saved_ms = now;
        cfg.epoch = plat_now_s();
        cfg_save();
    }
}

void onkar_os_tick(void)
{
    oos_tick_seen++;
    uint32_t now = os_sys_time_get();
    oos_busy = false;
    if (hap_on && hap_done) { hap_park(); }
    switch (oos_state)
    {
    case OS_WAIT_BOOT:
        if ((int32_t)(now - boot_wait_ms) >= 0) { oos_start(); }
        break;
    case OS_RAMP:
        if (now < oos_next_ms) { break; }
        if (cfg.fastramp && ramp_i == FAST_FROM)
        {
            for (uint32_t i = 0; i < RAMP_N; i++) { RAMP_RAM[i] = RAMP[i]; }
            wdg_kick();
            p25_ram_burst(20000u * CYC_PER_US);             /* 20 ms per step, ~0.4 s */
            wdg_kick();
            ramp_i = RAMP_N - 1;                            /* next tick: held fully on */
            oos_next_ms = now + 100;
            break;
        }
        if (ramp_i < RAMP_N)
        {
            uint16_t v = vbat_now();
            if (v > 2000 && v < ramp_vmin) { ramp_vmin = v; }
            if (v > 2000 && v + 180 < v_idle && ramp_hold_n < 25)
            {
                ramp_hold_n++; if (ramp_holds < 255) { ramp_holds++; }    /* supply sagging: let it recover first */
                oos_next_ms = now + 40;
                break;
            }
            ramp_hold_n = 0;
            oos_p25_pwm(RAMP[ramp_i]);
            lcd_cmd1(0x28, 0x00);           /* display off/on at every step, exactly as v17 (the ramp that worked) */
            lcd_cmd1(0x29, 0x00);
            ramp_i++;
            oos_next_ms = now + ramp_step_ms;
            break;
        }
        if (cfg.touch_rst != 0xFF && pin_valid(cfg.touch_rst) && cfg.touch_rst != P1_3 && cfg.touch_rst != P1_4)
        {
            oos_out(cfg.touch_rst, 0);
            platform_delay_ms(10);
            oos_out(cfg.touch_rst, 1);
            platform_delay_ms(60);
        }
        for (uint8_t i = 0; i < 3; i++) { mon_lv[i] = mon_idle[i] = (g_read() & GPIO_GetPin(MON_PIN[i])) ? 1 : 0; }
        btn_idle = cfg.button_pin == 0xFF ? 1 : ((g_read() & GPIO_GetPin(cfg.button_pin)) ? 1 : 0);
        btn_last = btn_idle;
        if (crown_mode == 0) { crown_last = QDEC_GetAxisCount(QDEC, QDEC_AXIS_Y); }   /* v19 FIX: QDEC clock is off in crown mode 1 -
                                                         reading it froze the chip right after every ramp (v18 after 00:48) */
        crown_sw_last = crown_sw;
        oos_state = OS_RUN;
        oos_run_since = now;
        touch_next_probe = now;
        g_full_rows = cfg.full_rows;
        bright_cap = 0; bright_set = 0xFF; cap_until = now + 5000;
        ui_boot_lit(now);                   /* haptic timers: 1 s later (one load step at a time on a weak supply) */
        bat_next = now + 1500;                   /* logo fades in, held, then the face */
        oos_frame(now);
        break;
    case OS_RUN:
    case OS_SLEEP:
        if (!hap_ready && now - oos_run_since > 1000) { hap_init(); }
        if (bright_cap < 255)
        {
            if ((int32_t)(now - cap_until) >= 0) { bright_cap = 255; }
            else
            {
                uint16_t v = vbat_now();
                if (v > 2000 && v < ramp_vmin) { ramp_vmin = v; }
                if (v > 2000 && v + 150 < v_idle) { bright_cap = bright_cap > 32 ? (uint8_t)(bright_cap - 24) : 8; if (ramp_holds < 255) { ramp_holds++; } }
                else { bright_cap = bright_cap > 247 ? 255 : (uint8_t)(bright_cap + 8); }
            }
            bright_apply();
        }
        if ((int32_t)(now - bat_next) >= 0)
        {
            bat_next = now + 10000;
            bat_update();
            bool low = bat_real && bat_pct < 6;
            low_cnt = low ? (uint8_t)(low_cnt < 255 ? low_cnt + 1 : 255) : 0;
            if (!low_mode && low_cnt >= 3) { low_mode = true; ui_lowbat(true, now); }
            else if (low_mode && bat_pct >= 10) { low_mode = false; ui_lowbat(false, now); }
            uint32_t want = (low_mode && oos_state == OS_SLEEP) ? 100 : OS_TICK_MS;
            if (want != tick_ms) { tick_ms = want; os_timer_restart(&oos_timer, tick_ms); }
        }
        input_poll(now);
        if (!fails_cleared && now - oos_run_since > 60000) { cfg.fails = 0; cfg_save(); fails_cleared = true; }
        oos_frame(now);
        break;
    default:
        break;
    }
    time_tick(now);
    if (oos_busy && (oos_state == OS_RUN || oos_state == OS_SLEEP))
    {
        /* something is moving: next frame right away (queued behind any Bluetooth work) */
        T_IO_MSG m;
        m.type = IO_MSG_TYPE_RESET_WDG_TIMER;
        m.subtype = 1;
        m.u.param = 0;
        if (app_send_msg_to_apptask(&m)) { oos_tick_sent++; }
    }
}

static void oos_timer_cb(void *p)
{
    (void)p;
    if (oos_tick_sent != oos_tick_seen) { return; }      /* a tick is still queued */
    T_IO_MSG m;
    m.type = IO_MSG_TYPE_RESET_WDG_TIMER;
    m.subtype = 1;
    m.u.param = 0;
    if (app_send_msg_to_apptask(&m)) { oos_tick_sent++; }
}

static void crown_init(void)
{
    crown_mode = 0;
    if (cfg.crown_mode == 1)
    {
        oos_gpio_in(P1_0, PAD_PULL_UP);
        oos_gpio_in(P2_6, PAD_PULL_UP);
        crown_ma = GPIO_GetPin(P1_0); crown_mb = GPIO_GetPin(P2_6);
        platform_delay_us(50);
        uint32_t in = g_read();
        crown_ab = (uint8_t)(((in & crown_ma) ? 2 : 0) | ((in & crown_mb) ? 1 : 0));
        crown_sw_last = crown_sw;
        crown_mode = 1;
        return;
    }
    Pad_Config(P1_0, PAD_PINMUX_MODE, PAD_IS_PWRON, PAD_PULL_UP, PAD_OUT_DISABLE, PAD_OUT_LOW);
    Pad_Config(P2_6, PAD_PINMUX_MODE, PAD_IS_PWRON, PAD_PULL_UP, PAD_OUT_DISABLE, PAD_OUT_LOW);
    Pinmux_Config(P1_0, qdec_phase_a_y);
    Pinmux_Config(P2_6, qdec_phase_b_y);
    RCC_PeriphClockCmd(APBPeriph_QDEC, APBPeriph_QDEC_CLOCK, ENABLE);
    QDEC_InitTypeDef q;
    QDEC_StructInit(&q);
    q.axisConfigY = ENABLE;
    q.debounceEnableY = Debounce_Enable;
    QDEC_Init(QDEC, &q);
    QDEC_Cmd(QDEC, QDEC_AXIS_Y, ENABLE);
    crown_last = QDEC_GetAxisCount(QDEC, QDEC_AXIS_Y);
}

static void oos_init(void)
{
    cfg_load();
    SCB->CPACR |= (3UL << 20) | (3UL << 22);   /* FPU on (the stock firmware uses it too; normally already on) */
    __DSB(); __ISB();
    dwt_init();
    oos_gpio_in(P2_2, PAD_PULL_NONE);
    oos_gpio_in(P2_4, PAD_PULL_UP);
    oos_gpio_in(P3_1, PAD_PULL_UP);
    if (cfg.button_pin != 0xFF) { oos_gpio_in(cfg.button_pin, PAD_PULL_UP); }
    platform_delay_us(100);
    for (uint8_t i = 0; i < 3; i++) { mon_lv[i] = mon_idle[i] = (g_read() & GPIO_GetPin(MON_PIN[i])) ? 1 : 0; }
    crown_init();
    if (cfg.epoch) { epoch_at_set = cfg.epoch; ms_at_set = 0; time_restored = true; }
    uint8_t f = cfg.fails;
    /* 6+ failed starts in a row: 15 s between tries, so a firmware update always has time to connect (only way in) */
    boot_wait_ms = os_sys_time_get() + (f == 0 ? 300 : (f < 3 ? 1500 : (f < 6 ? 3000 : 15000)));
    bat_update();
    oos_state = OS_WAIT_BOOT;
    if (os_timer_create(&oos_timer, "onkar_os", 0x4F53, OS_TICK_MS, true, oos_timer_cb)) { os_timer_start(&oos_timer); }
}

/* firmware update starting: show it, then leave the screen alone */
static void oos_dfu(void)
{
    if (hap_on) { hap_park(); }
    if (oos_state == OS_RUN || oos_state == OS_SLEEP)
    {
        if (oos_state == OS_SLEEP) { lcd_cmd1(0x29, 0x00); oos_state = OS_RUN; }
        ui_dfu();
    }
    oos_state = OS_DFU;
}

/* ------------------------------------------------------------------ laptop commands 0x30-0x35 */
static uint8_t osr[80];
static void oos_cmd(uint8_t conn_id, uint8_t *p, uint16_t len)
{
    osr[0] = p[0] | 0x80;
    uint32_t now = os_sys_time_get();
    switch (p[0])
    {
    case 0x30:      /* 00 stop, 01 start (forgets earlier failures), 02 status */
        if (len >= 2 && p[1] == 1) { cfg.fails = 0; oos_start(); }
        else if (len >= 2 && p[1] == 0) { oos_stop(); oos_state = OS_WAIT_BOOT; boot_wait_ms = now + 60000; }   /* pause only */
        memset(osr + 1, 0, sizeof(osr) - 1);
        osr[1] = oos_state; osr[2] = cfg.fails; osr[3] = 1; osr[4] = touch_addr;
        osr[5] = tbus.scl_pin; osr[6] = tbus.sda_pin; osr[7] = btn_idle; osr[8] = scr; osr[9] = cfg.ui.bright;
        osr[10] = (uint8_t)tp_x; osr[11] = (uint8_t)(tp_x >> 8); osr[12] = (uint8_t)tp_y; osr[13] = (uint8_t)(tp_y >> 8);
        memcpy(&osr[14], &st_frames, 4); memcpy(&osr[18], &touch_reads, 4); memcpy(&osr[22], &touch_events, 4);
        memcpy(&osr[26], &btn_presses, 4); osr[30] = ramp_i; osr[31] = cfg.full_rows;
        memcpy(&osr[32], touch_raw, 8);
        osr[40] = mon_lv[0] | (mon_lv[1] << 1) | (mon_lv[2] << 2);
        memcpy(&osr[41], mon_edges, 6);
        memcpy(&osr[47], &crown_pos, 4);
        osr[51] = cfg.button_pin;
        /* v16 additions */
        memcpy(&osr[52], &st_frame_us, 4); memcpy(&osr[56], &st_wait_us, 4); memcpy(&osr[60], &st_pix, 4);
        memcpy(&osr[64], &st_fps10, 4); memcpy(&osr[68], &st_max_us, 4);
        osr[72] = cfg.ui.face; osr[73] = cfg.ui.accent; osr[74] = cfg.ui.timeout; osr[75] = cfg.ui.always_on;
        osr[76] = (uint8_t)(time_set | (time_restored << 1)); osr[77] = cfg.te_sync; osr[78] = (uint8_t)(ramp_step_ms / 10);
        osr[79] = 19;
        st_max_us = 0;
        reply(conn_id, osr, 80);
        break;
    case 0x31:      /* local time: seconds since 1970 (u32 LE) */
        if (len >= 5)
        {
            memcpy(&epoch_at_set, &p[1], 4); ms_at_set = now; time_set = true;
            cfg.epoch = epoch_at_set; epoch_saved_ms = now; cfg_save();
            full_dirty = true;
        }
        osr[1] = time_set; reply(conn_id, osr, 2);
        break;
    case 0x32:      /* setting key value */
        if (len >= 3)
        {
            uint8_t v = p[2];
            switch (p[1])
            {
            case 1: break;                                  /* v19: always starts */
            case 2: cfg.ui.bright = v < 16 ? 16 : v; plat_bright(cfg.ui.bright); break;
            case 3: cfg.ramp10 = v < 5 ? 5 : v; break;
            case 4: cfg.touch_flags = v & 15; break;
            case 5:
                if (v == 0xFF) { cfg.button_pin = 0xFF; btn_idle = btn_last = 1; }
                else if (pin_valid(v) && v != P1_3 && v != P1_4)
                {
                    cfg.button_pin = v;
                    oos_gpio_in(v, v == P2_2 ? PAD_PULL_NONE : PAD_PULL_UP);
                    platform_delay_us(100);
                    btn_idle = btn_last = (g_read() & GPIO_GetPin(v)) ? 1 : 0;
                }
                break;
            case 7: cfg.touch_rst = v; break;
            case 8: cfg.ui.always_on = v ? 1 : 0; break;
            case 9: cfg.full_rows = v ? 1 : 0; g_full_rows = cfg.full_rows; full_dirty = true; break;
            case 10: cfg.te_sync = v ? 1 : 0; break;
            case 12: cfg.ui.timeout = v % N_TIMEOUT; break;
            case 6: case 13: cfg.ui.face = v % N_FACE; an_set(&face_an, cfg.ui.face); full_dirty = true; break;
            case 14: cfg.ui.accent = v % N_ACCENT; full_dirty = true; break;
            case 15: cfg.ui.haptic = v & 3; full_dirty = true; break;
            case 16: cfg.crown_mode = v ? 1 : 0; crown_init(); break;
            case 17: cfg.crown_div = v ? v : 2; crown_acc = 0; break;
            case 18: cfg.hap_inv = v ? 1 : 0; break;
            case 19: cfg.fastramp = v ? 1 : 0; break;
            default: break;
            }
            cfg_save();
        }
        osr[1] = 0; reply(conn_id, osr, 2);
        break;
    case 0x33:      /* fake input: ev x(u16) y(u16).  1 tap, 2-5 swipe left/right/up/down, 6 long touch,
                       7 button, 8 long button, 9 crown up, 10 crown down */
        if (len >= 6 && (oos_state == OS_RUN || oos_state == OS_SLEEP))
        {
            int16_t x = (int16_t)(p[2] | (p[3] << 8)), y = (int16_t)(p[4] | (p[5] << 8));
            uint8_t ev = p[1];
            if (ev == 7 || ev == 8) { ui_button(ev == 8, now); }
            else if (ev == 9 || ev == 10) { ui_crown(ev == 9 ? 1 : -1, now); }
            else if (ev >= 1 && ev <= 6)
            {
                fk_ev = ev; fk_t0 = now; fk_x0 = fk_x1 = x; fk_y0 = fk_y1 = y;
                if (ev == 2) { fk_x0 = 300; fk_x1 = 60; fk_y0 = fk_y1 = 224; }
                if (ev == 3) { fk_x0 = 60; fk_x1 = 300; fk_y0 = fk_y1 = 224; }
                if (ev == 4) { fk_x0 = fk_x1 = 184; fk_y0 = 430; fk_y1 = 150; }
                if (ev == 5) { fk_x0 = fk_x1 = 184; fk_y0 = 120; fk_y1 = 400; }
            }
            oos_busy = true;
        }
        osr[1] = 0; reply(conn_id, osr, 2);
        break;
    case 0x35:      /* raw I2C on the touch bus: addr nw w[nw] nr -> B5 ok nr data[nr] */
        if (len >= 3 && len >= 3 + p[2])
        {
            uint8_t nw = p[2], nr = len > 3 + nw ? p[3 + nw] : 0;
            if (nr > 32) { nr = 32; }
            if (!tbus.scl) { tbus_open(P0_5, P1_2); }
            osr[1] = tb_xfer(p[1], &p[3], nw, &osr[3], nr) ? 0 : 1;
            osr[2] = nr;
            reply(conn_id, osr, 3 + nr);
        }
        else { osr[1] = 0xEE; reply(conn_id, osr, 2); }
        break;
    case 0x36:      /* haptic: id (0xFF = stop) -> B6 played */
        if (len >= 2 && p[1] == 0xFF) { if (hap_on) { hap_park(); } osr[1] = 1; }
        else
        {
            if (!hap_ready && (oos_state == OS_OFF || oos_state == OS_WAIT_BOOT)) { hap_init(); }     /* screen off: the laptop may test the motor */
            osr[1] = len >= 2 && hap_play(p[1]) ? 1 : 0;
        }
        reply(conn_id, osr, 2);
        break;
    case 0x37:      /* crown + haptics diagnostics (screen off: also starts the 1 kHz sampler for crown mode 1) */
        if (!hap_ready && (oos_state == OS_OFF || oos_state == OS_WAIT_BOOT)) { hap_init(); }
        osr[1] = crown_mode; osr[2] = crown_ab;
        memcpy(&osr[3], (const void *)crown_edges, 4);
        { int32_t c = crown_sw; memcpy(&osr[7], &c, 4); }
        { uint32_t w[3] = {0, 0, 0}; if (crown_mode == 0) { w[0] = QDEC->REG_SR_Y; w[1] = QDEC->REG_CR_Y; w[2] = QDEC->INT_SR; } memcpy(&osr[11], w, 12); }
        osr[23] = hap_on; osr[24] = hap_done; osr[25] = cfg.ui.haptic;
        memcpy(&osr[26], &hap_plays, 4);
        memcpy(&osr[30], &crown_pos, 4);
        osr[34] = cfg.crown_div; osr[35] = cfg.hap_inv;
        bat_update();
        memcpy(&osr[36], &bat_raw_mv, 2); osr[38] = bat_pct; osr[39] = (uint8_t)(bat_real | (low_mode << 1));
        memcpy(&osr[40], &ramp_vmin, 2); memcpy(&osr[42], &v_idle, 2); osr[44] = ramp_holds; osr[45] = oos_state;
        memcpy(&osr[46], &bat_mv, 2);
        reply(conn_id, osr, 48);
        break;
    case 0x38:      /* peek: addr(u32) n(<=16) words, peripheral space only -> B8 n words */
        if (len >= 6)
        {
            uint32_t a; memcpy(&a, &p[1], 4); a &= ~3u;
            uint8_t n = p[5] > 16 ? 16 : p[5];
            if (a >= 0x40000000u && a + n * 4u <= 0x40100000u)
            {
                osr[1] = n;
                for (uint8_t i = 0; i < n; i++) { uint32_t w = *(volatile uint32_t *)(a + i * 4u); memcpy(&osr[2 + i * 4], &w, 4); }
                reply(conn_id, osr, 2 + n * 4);
                break;
            }
        }
        osr[1] = 0xEE; reply(conn_id, osr, 2);
        break;
    case 0x34:      /* re-probe touch now */
        touch_probe();
        osr[1] = touch_addr; osr[2] = tbus.scl_pin; osr[3] = tbus.sda_pin;
        reply(conn_id, osr, 4);
        break;
    default:
        break;
    }
}
