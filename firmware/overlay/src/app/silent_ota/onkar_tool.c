/*
 * ONKAR-LIFE v3 tool service for the BSW051 (Realtek RTL8762D).
 *
 * Bluetooth comes up first (Realtek silent_ota sample, untouched). This file only
 * runs when the laptop asks, over the "simple" service:
 *   write  0xB002  -> command        notify 0xB003  <- reply (opcode | 0x80)
 *
 * Safety (new in v3):
 *   - H_0 / H_1 / H_2 (MICBIAS, 32 kHz crystal) are refused by every command.
 *   - Hardware watchdog (8 s). Armed only after the app task has proven it is alive
 *     (3 heartbeats), so a mistake here can never cause a boot loop.
 *     Paused automatically as soon as a firmware update starts.
 *   - Dead-man switch: every pin we drive is released (input + pull-down, which also
 *     turns a motor transistor off) when the laptop disconnects or goes quiet for 30 s.
 *   - Long loops (pin hunt) kick the watchdog themselves.
 *
 * Commands:
 *   01                         ping                          -> 81 'O' 'K' ver
 *   02                         battery (chip VBAT pin)        -> 82 raw(u16) mv(u16) pct err
 *   03 pin level               hold pin as output             -> 83 pin level | 83 EE pin (refused)
 *   04 pin pull                read one pin                   -> 84 pin value
 *   05 pull                    read all safe pins             -> 85 bits[5] skipped[5]
 *   08 pin level ms(u16)       pulse (<= 1000 ms), then release -> 88 pin
 *   0A pin                     release pin                    -> 8A pin
 *   0B                         release everything             -> 8B count
 *   10                         status                         -> 90 ver uptime(u32) reset_reason wdg driven[5] hb(u32)
 *   11                         ADC sweep P2_0..P2_7 + VBAT    -> 91 then 9 x (raw u16, mv u16)
 *   12 scl sda                 I2C scan                       -> 92 status count map[16]
 *   13 scl sda addr reg n      I2C read n (<= 64)             -> 93 status n data
 *   14 scl sda addr n data..   I2C write                      -> 94 status
 *   15 rst flags mask[5] [cs]  display-bus hunt (optional fixed CS); flags: pull(bits0-1) protos(bits4-6, 0=all)   -> 95 status ncand nhits {cs clk mosi miso proto d0 d1 d2 d3}...
 *   16 cs clk mosi miso dc flags nw w.. nr    SPI transfer    -> 96 nr data
 *   17 cs clk mosi dc flags nh h.. hi lo count(u32)  pixel fill -> 97 status
 *   18                         read-only pad dump pins 0..38  -> 98 {b0 b1 pinmux} x 39
 *   19 pin ms(u16)             float test then exact restore  -> 99 pin 0 | 99 pin 80 (boot output, skipped)
 * v4: every pin goes back to its exact boot state when released (not pull-down).
 * v11 (stock boot order, decoded from flash 0x84DCF0 / 0x83E5E8 / 0x8602CC):
 *   20 flags                   stock power-up; flags bit0 = also claim P0_6/P0_7 (D2/D3)   -> A0 0
 *   21 ms(u16)                 count screen TE pulses on P3_0 -> A1 edges(u16) level
 *   22 op a1 a2 a3 n dummy ch baud   SPIC2 read -> A2 sr n data[n]
 *   23 x0 x1 y0 y1 (u16) hi lo quad (opcode 0x32) DMA rectangle fill -> A3 status
 *   24                         exact stock rail cycle 0x84AD88 including H_1/H_2 -> A4 0
 *   P0_0 (motor PWM, idle high) is parked HIGH at boot.
 * v16: ONKAR OS 16 (onkar_os.inc + onkar_gfx.inc + onkar_ui.inc): anti-aliased dirty-rect renderer, boot animation,
 *   3 watch faces, control centre, apps; starts by itself 3 s after power-up (back-off after failed starts).
 *   32 keys: 1 autostart 2 bright 3 ramp 4 touch_flags 5 button 7 touch_rst 8 always_on 9 full_rows 10 te_sync
 *            12 timeout 13 face 14 accent.   30 02 status = 80 bytes (frame time/fps at 52..71)
 * v18 dropped the old screen bring-up commands 11 15-17 19-1B 1D-1F 20 22-24 (the OS owns the panel).
 * v18: haptics (motor P0_0 = TIM2 25 kHz PWM, TIM3 1 kHz pattern tick in RAM), crown mode 1 (pins sampled at 1 kHz).
 *   36 id                      play haptic pattern (0 tick 1 soft 2 tap 3 click 4 double 5 success 6 long 7 bump 8 alarm 9 boot; FF stop)
 *   37                         crown/haptic diagnostics       38 addr(u32) n   peek n words of peripheral space
 *   32 keys 15 haptic(0-3) 16 crown_mode 17 crown_div 18 hap_inv
 * v14: ONKAR OS (onkar_os.inc): screen + touch + button UI, starts by itself 6 s after boot (guarded).
 *   30 00|01|02 stop / start / status     31 epoch(u32)  set local time     32 key value  settings
 *   33 ev x(u16) y(u16)  fake input        34  re-probe touch     35 addr nw w.. nr  raw I2C on the touch bus
 * v13:
 *   25 pin period_us(u16) high(u16, 0.1 us units)   hardware PWM on TIM5 (runs while Bluetooth stays up) -> A5 status
 *                              high 0 = stop + pin low; high >= period = pin held HIGH. Released by 0B / dead-man like any pin.
 */
#include <string.h>
#include "rtl876x.h"
#include "rtl876x_rcc.h"
#include "rtl876x_pinmux.h"
#include "rtl876x_gpio.h"
#include "rtl876x_adc.h"
#include "rtl876x_wdg.h"
#include "adc_lib.h"
#include "platform_utils.h"
#include "os_timer.h"
#include "os_sched.h"
#include "app_msg.h"
#include "app_task.h"
#include "simple_ble_service.h"
#include "onkar_tool.h"
#include "qspi_lcd_platform.h"
#include "rtl876x_gdma.h"
#include "rtl876x_spic.h"
#include "rtl876x_tim.h"

#define ONKAR_FW_VER        19
#define PIN_MAX             35          /* P4_3; 36..38 are H_0..H_2 and are never touched */
#define DEADMAN_MS          30000
#define HUNT_MAX_CAND       32
#define HUNT_MAX_HITS       20

extern T_SERVER_ID g_simp_service_id;

static uint64_t driven_mask = 0;
static bool adc_cal_done = false;
static uint64_t last_cmd_ms = 0;
static void *hb_timer = NULL;
static volatile uint32_t hb_sent = 0, hb_seen = 0, hb_seen_at_last_tick = 0;
static bool wdg_armed = false, wdg_paused = false;
static uint8_t boot_reset_reason = 0;

/* ------------------------------------------------------------------ helpers */

static void reply(uint8_t conn_id, uint8_t *d, uint16_t n)
{
    simp_ble_service_send_v3_notify(conn_id, g_simp_service_id, d, n);
}

static bool pin_valid(uint8_t pin)
{
    return (pin <= PIN_MAX) && (pin != 31);
}

static uint64_t bit(uint8_t pin) { return ((uint64_t)1) << pin; }

static void wdg_kick(void)
{
    if (wdg_armed && !wdg_paused) { WDG_Restart(); }
}

/* ------------------------------------------------------------------ pins
 * v4: before we touch a pin the first time, its exact boot state (2 AON pad bytes +
 * pinmux function) is saved. "Release" puts those exact bytes back, so a pin the
 * board relies on (power hold, chip enable...) returns to what it was at boot.
 */
extern const uint16_t PINADDR_TABLE[];
static uint8_t orig_b0[PIN_MAX + 1], orig_b1[PIN_MAX + 1], orig_pmx[PIN_MAX + 1];
static uint64_t orig_saved = 0;

static uint8_t pmx_get(uint8_t pin)
{
    return (PINMUX->CFG[pin >> 2] >> ((pin & 3) << 3)) & 0xFF;
}

static void remember(uint8_t pin)
{
    if (pin > PIN_MAX || (orig_saved & (((uint64_t)1) << pin))) { return; }
    uint8_t a = (uint8_t)PINADDR_TABLE[pin];
    orig_b0[pin] = btaon_fast_read_safe(a);
    orig_b1[pin] = btaon_fast_read_safe(a + 1);
    orig_pmx[pin] = pmx_get(pin);
    orig_saved |= ((uint64_t)1) << pin;
}

static void restore(uint8_t pin)
{
    if (pin > PIN_MAX || !(orig_saved & (((uint64_t)1) << pin))) { return; }
    uint8_t a = (uint8_t)PINADDR_TABLE[pin];
    /* pad first (so the pin never floats through a wrong function), then pinmux */
    btaon_fast_write_safe(a, orig_b0[pin]);
    btaon_fast_write_safe(a + 1, orig_b1[pin]);
    Pinmux_Config(pin, orig_pmx[pin]);
}

/* boot-time state says this pad is a software-driven output: somebody relies on it */
static bool pin_is_sw_output(uint8_t pin)
{
    uint8_t a = (uint8_t)PINADDR_TABLE[pin];
    uint8_t b0 = btaon_fast_read_safe(a), b1 = btaon_fast_read_safe(a + 1);
    return ((b1 & 0x02) == 0) && (b0 & 0x02);
}

static void pin_hold(uint8_t pin, uint8_t level)
{
    remember(pin);
    Pinmux_Config(pin, IDLE_MODE);
    Pad_Config(pin, PAD_SW_MODE, PAD_IS_PWRON, PAD_PULL_NONE, PAD_OUT_ENABLE,
               level ? PAD_OUT_HIGH : PAD_OUT_LOW);
    driven_mask |= bit(pin);
}

/* released = back to the exact state it had at boot */
static void pin_release(uint8_t pin)
{
    if (orig_saved & bit(pin)) { restore(pin); }
    else
    {
        Pinmux_Config(pin, IDLE_MODE);
        Pad_Config(pin, PAD_SW_MODE, PAD_IS_PWRON, PAD_PULL_DOWN, PAD_OUT_DISABLE, PAD_OUT_LOW);
    }
    driven_mask &= ~bit(pin);
}

/* v13: hardware PWM on TIM5 (TIM6 is the SDK's watchdog timer). 40 MHz clock = 4 counts per 0.1 us. */
#define PWM_TIM         TIM5
#define PWM_PINMUX      timer_pwm5
static bool pwm_on = false;

static bool pwm_tool = false;           /* started by the laptop tool (0x25), not by the OS */

static void pwm_stop(void)
{
    if (pwm_on) { TIM_Cmd(PWM_TIM, DISABLE); pwm_on = false; }
    pwm_tool = false;
}

/* period in us, high in 0.1 us, 0 < high*4 < period*40 */
static void pwm_apply(uint8_t pin, uint32_t period, uint32_t high, bool track)
{
    uint32_t total = period * 40, hc = high * 4;
    if (pwm_on)
    {
        TIM_PWMChangeFreqAndDuty(PWM_TIM, hc - 1, total - hc - 1);
    }
    else
    {
        if (track) { remember(pin); }
        RCC_PeriphClockCmd(APBPeriph_TIMER, APBPeriph_TIMER_CLOCK, ENABLE);
        TIM_TimeBaseInitTypeDef t;
        TIM_StructInit(&t);
        t.TIM_Mode = TIM_Mode_UserDefine;
        t.TIM_PWM_En = PWM_ENABLE;
        t.TIM_PWM_High_Count = hc - 1;
        t.TIM_PWM_Low_Count = total - hc - 1;
        TIM_TimeBaseInit(PWM_TIM, &t);
        TIM_Cmd(PWM_TIM, ENABLE);
        pwm_on = true;
    }
    Pinmux_Config(pin, PWM_PINMUX);
    Pad_Config(pin, PAD_PINMUX_MODE, PAD_IS_PWRON, PAD_PULL_NONE, PAD_OUT_ENABLE, PAD_OUT_LOW);
    if (track) { driven_mask |= bit(pin); pwm_tool = true; }
}

static uint8_t release_all(void)
{
    uint8_t n = 0;
    if (pwm_tool) { pwm_stop(); }
    for (uint8_t pin = 0; pin <= PIN_MAX; pin++)
    {
        if (driven_mask & bit(pin)) { pin_release(pin); n++; }
    }
    driven_mask = 0;
    return n;
}

static PAD_Pull_Mode pull_of(uint8_t pull)
{
    return pull == 1 ? PAD_PULL_UP : (pull == 2 ? PAD_PULL_DOWN : PAD_PULL_NONE);
}

/* pin owned by the GPIO block (fast register access via DATADIR/DATAOUT/DATAIN) */
static void gpio_take(uint8_t pin, uint8_t pull)
{
    remember(pin);
    Pinmux_Config(pin, DWGPIO);
    Pad_Config(pin, PAD_PINMUX_MODE, PAD_IS_PWRON, pull_of(pull), PAD_OUT_ENABLE, PAD_OUT_LOW);
    GPIO_InitTypeDef g;
    GPIO_StructInit(&g);
    g.GPIO_Pin = GPIO_GetPin(pin);
    g.GPIO_Mode = GPIO_Mode_IN;
    g.GPIO_ITCmd = DISABLE;
    GPIO_Init(&g);
}

static inline void g_out(uint32_t m, uint8_t level)
{
    if (level) { GPIO->DATAOUT |= m; } else { GPIO->DATAOUT &= ~m; }
    GPIO->DATADIR |= m;
}
static inline void g_in(uint32_t m) { GPIO->DATADIR &= ~m; }
static inline void g_set(uint32_t m) { GPIO->DATAOUT |= m; }
static inline void g_clr(uint32_t m) { GPIO->DATAOUT &= ~m; }
static inline uint32_t g_read(void) { return GPIO->DATAIN; }
static inline void tiny_delay(void) { for (volatile uint8_t i = 0; i < 4; i++) { __NOP(); } }

/* returns 0/1, or 0x80|level if the pin is a software-driven output (not touched) */
static uint8_t pin_read(uint8_t pin, uint8_t pull)
{
    uint8_t a = (uint8_t)PINADDR_TABLE[pin];
    uint8_t b0 = btaon_fast_read_safe(a), b1 = btaon_fast_read_safe(a + 1);
    if (((b1 & 0x02) == 0) && (b0 & 0x02)) { return 0x80 | (b0 & 1); }
    bool was_saved = (orig_saved & bit(pin)) != 0;
    uint8_t sb0 = b0, sb1 = b1, spmx = pmx_get(pin);
    if (pull == 3) { pull = (b0 & 0x04) ? ((b0 & 0x08) ? 2 : 1) : 0; }   /* keep current pull */
    gpio_take(pin, pull);
    platform_delay_us(30);
    uint8_t v = (g_read() & GPIO_GetPin(pin)) ? 1 : 0;
    btaon_fast_write_safe(a, sb0);
    btaon_fast_write_safe(a + 1, sb1);
    Pinmux_Config(pin, spmx);
    if (!was_saved) { orig_saved &= ~bit(pin); }
    return v;
}

/* ------------------------------------------------------------------ ADC */

static uint8_t mv_to_pct(uint16_t mv)
{
    static const uint16_t mv_tab[]  = {3400, 3500, 3600, 3650, 3700, 3750, 3800, 3900, 4000, 4100, 4200};
    static const uint8_t  pct_tab[] = {0,    6,    14,   22,   32,   42,   52,   65,   78,   90,   100};
    if (mv <= mv_tab[0]) { return 0; }
    for (uint8_t i = 1; i < sizeof(mv_tab) / sizeof(mv_tab[0]); i++)
    {
        if (mv <= mv_tab[i])
        {
            return pct_tab[i - 1] + (uint8_t)((uint32_t)(mv - mv_tab[i - 1]) * (pct_tab[i] - pct_tab[i - 1]) /
                                              (mv_tab[i] - mv_tab[i - 1]));
        }
    }
    return 100;
}

/* one-shot conversion of one schedule entry, divide mode (0..3.3 V on pins) */
static uint16_t adc_once(uint16_t sch, uint16_t *raw_out, int8_t *err_out)
{
    if (!adc_cal_done)
    {
        ADC_CalibrationInit();
        adc_cal_done = true;
    }
    RCC_PeriphClockCmd(APBPeriph_ADC, APBPeriph_ADC_CLOCK, ENABLE);
    ADC_InitTypeDef s;
    ADC_StructInit(&s);
    s.ADC_SampleTime = 255;
    s.ADC_SchIndex[0] = sch;
    s.ADC_Bitmap = 0x01;
    s.ADC_PowerAlwaysOnEn = ADC_POWER_ALWAYS_ON_ENABLE;
    ADC_Init(ADC, &s);
    ADC_INTConfig(ADC, ADC_INT_ONE_SHOT_DONE, ENABLE);
    ADC_ClearINTPendingBit(ADC, ADC_INT_ONE_SHOT_DONE);
    ADC_Cmd(ADC, ADC_ONE_SHOT_MODE, ENABLE);
    uint32_t guard = 0;
    while (ADC_GetINTStatus(ADC, ADC_INT_ONE_SHOT_DONE) == RESET && guard < 200000) { guard++; }
    ADC_ClearINTPendingBit(ADC, ADC_INT_ONE_SHOT_DONE);
    uint16_t raw = ADC_ReadRawData(ADC, 0);
    ADC_Cmd(ADC, ADC_ONE_SHOT_MODE, DISABLE);
    ADC_ErrorStatus err = NO_ERROR;
    float v = ADC_GetVoltage(DIVIDE_SINGLE_MODE, (int32_t)raw, &err);
    if (raw_out) { *raw_out = raw; }
    if (err_out) { *err_out = (guard >= 200000) ? -9 : (int8_t)err; }
    return (v > 0 && v < 10000) ? (uint16_t)v : 0;
}

uint16_t onkar_battery_read(uint16_t *raw_out, uint8_t *pct_out, int8_t *err_out)
{
    uint16_t mv = adc_once(INTERNAL_VBAT_MODE, raw_out, err_out);
    if (pct_out) { *pct_out = mv_to_pct(mv); }
    return mv;
}

/* ------------------------------------------------------------------ I2C (bit-banged, open drain) */

typedef struct { uint32_t scl, sda; } T_I2C;

static void i2c_hi(uint32_t m) { g_in(m); }          /* released: external/internal pull-up */
static void i2c_lo(uint32_t m) { g_out(m, 0); }
static void i2c_dly(void) { platform_delay_us(4); }
static bool i2c_scl_wait(T_I2C *b)
{
    for (uint16_t i = 0; i < 500; i++) { if (g_read() & b->scl) { return true; } platform_delay_us(2); }
    return false;
}
static void i2c_start(T_I2C *b)
{
    i2c_hi(b->sda); i2c_hi(b->scl); i2c_scl_wait(b); i2c_dly();
    i2c_lo(b->sda); i2c_dly(); i2c_lo(b->scl); i2c_dly();
}
static void i2c_stop(T_I2C *b)
{
    i2c_lo(b->sda); i2c_dly(); i2c_hi(b->scl); i2c_scl_wait(b); i2c_dly(); i2c_hi(b->sda); i2c_dly();
}
static bool i2c_wbyte(T_I2C *b, uint8_t v)          /* returns true on ACK */
{
    for (int8_t i = 7; i >= 0; i--)
    {
        if ((v >> i) & 1) { i2c_hi(b->sda); } else { i2c_lo(b->sda); }
        i2c_dly(); i2c_hi(b->scl); i2c_scl_wait(b); i2c_dly(); i2c_lo(b->scl);
    }
    i2c_hi(b->sda); i2c_dly(); i2c_hi(b->scl); i2c_scl_wait(b); i2c_dly();
    bool ack = (g_read() & b->sda) == 0;
    i2c_lo(b->scl); i2c_dly();
    return ack;
}
static uint8_t i2c_rbyte(T_I2C *b, bool ack)
{
    uint8_t v = 0;
    i2c_hi(b->sda);
    for (int8_t i = 7; i >= 0; i--)
    {
        i2c_dly(); i2c_hi(b->scl); i2c_scl_wait(b); i2c_dly();
        v = (v << 1) | ((g_read() & b->sda) ? 1 : 0);
        i2c_lo(b->scl);
    }
    if (ack) { i2c_lo(b->sda); } else { i2c_hi(b->sda); }
    i2c_dly(); i2c_hi(b->scl); i2c_scl_wait(b); i2c_dly(); i2c_lo(b->scl); i2c_hi(b->sda);
    return v;
}

/* 0 ok, 1 SDA stuck low, 2 SCL stuck low, 3 bad pins */
static uint8_t i2c_open(T_I2C *b, uint8_t scl, uint8_t sda)
{
    if (!pin_valid(scl) || !pin_valid(sda) || scl == sda) { return 3; }
    b->scl = GPIO_GetPin(scl); b->sda = GPIO_GetPin(sda);
    if (b->scl == b->sda) { return 3; }                 /* share one GPIO line */
    RCC_PeriphClockCmd(APBPeriph_GPIO, APBPeriph_GPIO_CLOCK, ENABLE);
    gpio_take(scl, 1); gpio_take(sda, 1);
    driven_mask |= bit(scl) | bit(sda);
    platform_delay_us(50);
    if ((g_read() & b->scl) == 0) { return 2; }
    if ((g_read() & b->sda) == 0)
    {
        for (uint8_t i = 0; i < 9 && (g_read() & b->sda) == 0; i++)   /* clock a stuck slave free */
        {
            i2c_lo(b->scl); i2c_dly(); i2c_hi(b->scl); i2c_dly();
        }
        if ((g_read() & b->sda) == 0) { return 1; }
        i2c_stop(b);
    }
    return 0;
}

/* ------------------------------------------------------------------ SPI (bit-banged) */

typedef struct { uint32_t cs, clk, mosi, miso, dc; uint8_t flags; } T_SPI;
#define SPI_F_9BIT    0x01      /* 3-wire 9-bit: D/C bit before each byte (first byte = command) */
#define SPI_F_DC      0x02      /* separate D/C pin: low for first byte, high after */
#define SPI_F_MISO_IS_MOSI 0x04 /* 3-wire: read back on the same line */

static inline void spi_bit_out(T_SPI *s, uint8_t b)
{
    if (b) { g_set(s->mosi); } else { g_clr(s->mosi); }
    tiny_delay(); g_set(s->clk); tiny_delay(); g_clr(s->clk);
}
static void spi_wbyte(T_SPI *s, uint8_t v, bool is_cmd)
{
    if (s->flags & SPI_F_9BIT) { spi_bit_out(s, is_cmd ? 0 : 1); }
    if (s->flags & SPI_F_DC) { if (is_cmd) { g_clr(s->dc); } else { g_set(s->dc); } }
    for (int8_t i = 7; i >= 0; i--) { spi_bit_out(s, (v >> i) & 1); }
}
static uint8_t spi_rbyte(T_SPI *s)
{
    uint8_t v = 0;
    for (int8_t i = 7; i >= 0; i--)
    {
        g_set(s->clk); tiny_delay();
        v = (v << 1) | ((g_read() & s->miso) ? 1 : 0);
        g_clr(s->clk); tiny_delay();
    }
    return v;
}

static bool spi_open(T_SPI *s, uint8_t cs, uint8_t clk, uint8_t mosi, uint8_t miso, uint8_t dc, uint8_t flags)
{
    if (!pin_valid(cs) || !pin_valid(clk) || !pin_valid(mosi)) { return false; }
    if (miso != 0xFF && !pin_valid(miso)) { return false; }
    if ((flags & SPI_F_DC) && !pin_valid(dc)) { return false; }
    RCC_PeriphClockCmd(APBPeriph_GPIO, APBPeriph_GPIO_CLOCK, ENABLE);
    s->flags = flags;
    s->cs = GPIO_GetPin(cs); s->clk = GPIO_GetPin(clk); s->mosi = GPIO_GetPin(mosi);
    s->miso = (miso == 0xFF) ? 0 : GPIO_GetPin(miso);
    s->dc = (flags & SPI_F_DC) ? GPIO_GetPin(dc) : 0;
    gpio_take(cs, 0); gpio_take(clk, 0); gpio_take(mosi, 0);
    driven_mask |= bit(cs) | bit(clk) | bit(mosi);
    if (miso != 0xFF && miso != mosi) { gpio_take(miso, 0); driven_mask |= bit(miso); }
    if (flags & SPI_F_DC) { gpio_take(dc, 0); g_out(s->dc, 1); driven_mask |= bit(dc); }
    g_out(s->cs, 1); g_out(s->clk, 0); g_out(s->mosi, 0);
    return true;
}

/* ------------------------------------------------------------------ display-bus hunt
 * Every combination of (cs, clk, mosi) among the candidate pins sends three kinds of
 * "read display ID" frames, then clocks 32 bits back while sampling ALL candidate pins at
 * once (GPIO DATAIN). A pin whose 32 bits are identical on two repeats and are neither
 * all-0 nor all-1 is a hit: something on the other end answered.
 *   proto 0: [0x04] + 32 read clocks              (SPI / 3-wire RDDID)
 *   proto 1: [0x03 0x00 0x04 0x00] + 32 clocks    (QSPI AMOLED drivers in 1-line read)
 *   proto 2: 9-bit [0|0x04] + 32 clocks           (3-wire 9-bit LCD drivers)
 */
typedef struct { uint8_t cs, clk, mosi, miso, proto; uint32_t data; } T_HIT;

static void hunt_frame(uint32_t cs, uint32_t clk, uint32_t mosi, uint8_t proto, uint32_t *samples)
{
    T_SPI s = { cs, clk, mosi, 0, 0, (uint8_t)(proto == 2 ? SPI_F_9BIT : 0) };
    g_out(mosi, 0); g_out(clk, 0); g_out(cs, 1);
    tiny_delay();
    g_clr(cs);
    if (proto == 1) { spi_wbyte(&s, 0x03, false); spi_wbyte(&s, 0x00, false); spi_wbyte(&s, 0x04, false); spi_wbyte(&s, 0x00, false); }
    else { spi_wbyte(&s, 0x04, true); }
    g_in(mosi);                                  /* let a 3-wire device drive the shared line */
    for (uint8_t i = 0; i < 32; i++)
    {
        g_set(clk); tiny_delay();
        samples[i] = g_read();
        g_clr(clk); tiny_delay();
    }
    g_set(cs);
    g_in(cs); g_in(clk);
}

static uint16_t hunt(uint8_t rst, uint8_t pull, uint8_t protos, uint8_t fixed_cs, uint64_t mask, uint8_t *out, uint16_t out_max, uint8_t *ncand_out)
{
    uint8_t cand[HUNT_MAX_CAND]; uint32_t cm[HUNT_MAX_CAND]; uint8_t n = 0;
    uint32_t used = 0, all = 0;
    RCC_PeriphClockCmd(APBPeriph_GPIO, APBPeriph_GPIO_CLOCK, ENABLE);
    if (rst != 0xFF && pin_valid(rst) && !(driven_mask & bit(rst)))
    {
        mask &= ~bit(rst);
        pin_hold(rst, 0); platform_delay_ms(20); pin_hold(rst, 1); platform_delay_ms(150);
    }
    else if (rst != 0xFF) { mask &= ~bit(rst); }      /* already held (reset done earlier) */
    for (uint8_t pin = 0; pin <= PIN_MAX && n < HUNT_MAX_CAND; pin++)
    {
        if (!(mask & bit(pin)) || !pin_valid(pin) || (driven_mask & bit(pin))) { continue; }
        if (pin_is_sw_output(pin)) { continue; }
        uint32_t m = GPIO_GetPin(pin);
        if (m == 0 || (used & m)) { continue; }      /* two pads on one GPIO line: keep the first */
        used |= m; cand[n] = pin; cm[n] = m; n++; all |= m;
        gpio_take(pin, pull);
    }
    *ncand_out = n;
    uint16_t hits = 0;
    uint32_t s1[32], s2[32];
    uint32_t combos = 0;
    for (uint8_t a = 0; a < n && hits < HUNT_MAX_HITS; a++)
    {
        if (fixed_cs != 0xFF && cand[a] != fixed_cs) { continue; }   /* v5: one CS per command keeps each call ~1 s */
        for (uint8_t b = 0; b < n && hits < HUNT_MAX_HITS; b++)
        {
            if (b == a) { continue; }
            for (uint8_t c = 0; c < n && hits < HUNT_MAX_HITS; c++)
            {
                if (c == a || c == b) { continue; }
                if ((++combos & 0x3F) == 0) { wdg_kick(); }
                for (uint8_t proto = 0; proto < 3 && hits < HUNT_MAX_HITS; proto++)
                {
                    if (!(protos & (1 << proto))) { continue; }
                    hunt_frame(cm[a], cm[b], cm[c], proto, s1);
                    hunt_frame(cm[a], cm[b], cm[c], proto, s2);
                    /* fast filter over all pins at once: stable, not stuck 0, not stuck 1 */
                    uint32_t diff = 0, ones = 0xFFFFFFFF, zeros = 0xFFFFFFFF;
                    for (uint8_t i = 0; i < 32; i++)
                    {
                        diff |= s1[i] ^ s2[i]; ones &= s1[i]; zeros &= ~s1[i];
                    }
                    uint32_t good = all & ~diff & ~ones & ~zeros & ~cm[a] & ~cm[b];
                    if (!good) { continue; }
                    for (uint8_t d = 0; d < n && hits < HUNT_MAX_HITS; d++)
                    {
                        if (!(good & cm[d])) { continue; }
                        uint32_t v1 = 0;
                        for (uint8_t i = 0; i < 32; i++) { v1 = (v1 << 1) | ((s1[i] & cm[d]) ? 1 : 0); }
                        /* v6: a line that just drifts up/down after we let go (0111..., 0000111...) has one
                           edge; a real chip answering has several */
                        uint32_t edges = v1 ^ (v1 >> 1), ne = 0;
                        for (uint8_t i = 0; i < 31; i++) { ne += (edges >> i) & 1; }
                        if (ne < 3) { continue; }
                        uint8_t *h = &out[hits * 9];
                        if ((uint16_t)(hits + 1) * 9 > out_max) { break; }
                        h[0] = cand[a]; h[1] = cand[b]; h[2] = cand[c]; h[3] = cand[d]; h[4] = proto;
                        h[5] = v1 >> 24; h[6] = v1 >> 16; h[7] = v1 >> 8; h[8] = v1;
                        hits++;
                    }
                }
            }
        }
    }
    for (uint8_t i = 0; i < n; i++) { pin_release(cand[i]); }
    return hits;
}

/* ------------------------------------------------------------------ v7 screen search
 * Bus is fixed (hardware screen pins of this chip): cs, clk, sio (+ d1 as a second read line).
 * For one power pin/level (or none): for every reset candidate: pulse reset, then send 4 kinds of
 * "read ID" frames and sample sio + d1. A hit must be: identical twice, >= 3 edges, AND different
 * when the same frame is clocked with CS held HIGH (screen not selected) -> no more drifting-wire ghosts.
 */
static void ss_frame(uint32_t cs, uint32_t clk, uint32_t sio, uint8_t proto, bool select, uint32_t *smp)
{
    T_SPI s = { cs, clk, sio, 0, 0, (uint8_t)(proto == 2 ? SPI_F_9BIT : 0) };
    g_out(cs, 1); g_out(clk, 0); g_out(sio, 0);
    tiny_delay();
    if (select) { g_clr(cs); }
    switch (proto)
    {
    case 0: spi_wbyte(&s, 0x04, true); break;                                   /* RDDID */
    case 1: spi_wbyte(&s, 0x03, false); spi_wbyte(&s, 0x00, false); spi_wbyte(&s, 0x04, false); spi_wbyte(&s, 0x00, false); break;
    case 2: spi_wbyte(&s, 0x04, true); break;                                   /* 9-bit */
    default: spi_wbyte(&s, 0x0B, false); spi_wbyte(&s, 0x00, false); spi_wbyte(&s, 0x04, false); spi_wbyte(&s, 0x00, false);
        for (uint8_t i = 0; i < 8; i++) { g_set(clk); tiny_delay(); g_clr(clk); tiny_delay(); }   /* dummy */
        break;
    }
    g_in(sio);
    for (uint8_t i = 0; i < 32; i++)
    {
        g_set(clk); tiny_delay(); tiny_delay();
        smp[i] = g_read();
        g_clr(clk); tiny_delay(); tiny_delay();
    }
    g_set(cs);
}

static uint32_t ss_bits(uint32_t *smp, uint32_t m)
{
    uint32_t v = 0;
    for (uint8_t i = 0; i < 32; i++) { v = (v << 1) | ((smp[i] & m) ? 1 : 0); }
    return v;
}

static uint8_t ss_edges(uint32_t v)
{
    uint32_t e = v ^ (v >> 1); uint8_t n = 0;
    for (uint8_t i = 0; i < 31; i++) { n += (e >> i) & 1; }
    return n;
}

/* out: hits of 7 bytes {rst, proto, line(0 sio / 1 d1), v[4]} */
static uint16_t screen_search(uint8_t cs, uint8_t clk, uint8_t sio, uint8_t d1, uint8_t pwr, uint8_t pwr_lvl,
                              uint64_t rmask, uint8_t *out, uint16_t out_max, bool keep)
{
    RCC_PeriphClockCmd(APBPeriph_GPIO, APBPeriph_GPIO_CLOCK, ENABLE);
    if (pwr != 0xFF && pin_valid(pwr)) { pin_hold(pwr, pwr_lvl); platform_delay_ms(40); }
    gpio_take(cs, 0); gpio_take(clk, 0); gpio_take(sio, 0);
    if (d1 != 0xFF) { gpio_take(d1, 0); }
    driven_mask |= bit(cs) | bit(clk) | bit(sio) | (d1 != 0xFF ? bit(d1) : 0);
    uint32_t mc = GPIO_GetPin(cs), mk = GPIO_GetPin(clk), ms = GPIO_GetPin(sio), md = d1 != 0xFF ? GPIO_GetPin(d1) : 0;
    uint16_t hits = 0;
    uint32_t a[32], b[32], z[32];
    for (int16_t ri = -1; ri <= PIN_MAX && (uint16_t)(hits + 1) * 7 <= out_max; ri++)
    {
        uint8_t rst = 0xFF;
        if (ri >= 0)
        {
            rst = (uint8_t)ri;
            if (!(rmask & bit(rst)) || !pin_valid(rst) || rst == pwr || rst == cs || rst == clk || rst == sio || rst == d1) { continue; }
            if (pin_is_sw_output(rst)) { continue; }
            pin_hold(rst, 1); platform_delay_ms(2);
            pin_hold(rst, 0); platform_delay_ms(12);
            pin_hold(rst, 1); platform_delay_ms(30);
        }
        wdg_kick();
        bool hit_here = false;
        for (uint8_t proto = 0; proto < 4; proto++)
        {
            ss_frame(mc, mk, ms, proto, true, a);
            ss_frame(mc, mk, ms, proto, true, b);
            ss_frame(mc, mk, ms, proto, false, z);
            for (uint8_t line = 0; line < 2; line++)
            {
                uint32_t m = line ? md : ms;
                if (!m) { continue; }
                uint32_t va = ss_bits(a, m), vb = ss_bits(b, m), vz = ss_bits(z, m);
                if (va != vb || va == vz || ss_edges(va) < 3) { continue; }
                if ((uint16_t)(hits + 1) * 7 > out_max) { break; }
                uint8_t *h = &out[hits * 7];
                h[0] = rst; h[1] = proto; h[2] = line;
                h[3] = va >> 24; h[4] = va >> 16; h[5] = va >> 8; h[6] = va;
                hits++; hit_here = true;
            }
        }
        if (rst != 0xFF && !(keep && hit_here)) { pin_release(rst); }
        if (keep && hit_here) { break; }
    }
    if (!(keep && hits))
    {
        pin_release(cs); pin_release(clk); pin_release(sio);
        if (d1 != 0xFF) { pin_release(d1); }
        if (pwr != 0xFF && pin_valid(pwr)) { pin_release(pwr); }
    }
    return hits;
}

/* ------------------------------------------------------------------ v10: screen through SPIC2 hardware,
 * byte-for-byte the way the stock firmware does it (its writer lives at RAM 0x20AE50 / flash 0x883054).
 */
static bool lcd_hw_ready = false;

static void lcd_hw_frame(const uint8_t *hdr, uint8_t nh, const uint8_t *data, uint8_t nd)
{
    SPIC2->ser = 1;
    SPIC2->ssienr = 0;
    SPIC2->ctrlr0 &= ~0x3F0000;
    SPIC2->ctrlr0 &= ~0x300;
    for (uint8_t i = 0; i < nh; i++) { SPIC2->dr[0].byte = hdr[i]; }
    for (uint8_t i = 0; i < nd; i++) { SPIC2->dr[0].byte = data[i]; }
    SPIC2->ssienr = 1;
    uint32_t guard = 0;
    while ((SPIC2->sr & 1) && guard < 100000) { guard++; }
    SPIC2->ssienr = 0;
}

/* ------------------------------------------------------------------ v11: stock boot order */

static void stock_out(uint8_t pin, uint8_t level)
{
    remember(pin);
    Pinmux_Config(pin, IDLE_MODE);
    Pad_Config(pin, PAD_SW_MODE, PAD_IS_PWRON, level ? PAD_PULL_UP : PAD_PULL_DOWN, PAD_OUT_ENABLE,
               level ? PAD_OUT_HIGH : PAD_OUT_LOW);
    driven_mask |= bit(pin);
}

/* stock 0x882F60: same as the SDK init except the clock pad has no pull */
static void spic2_stock_init(bool with_d23)
{
    RCC_PeriphClockCmd(APBPeriph_FLASH2, APBPeriph_FLASH2_CLOCK, ENABLE);
    Pinmux_Config(P4_3, IDLE_MODE);
    Pinmux_Config(P4_0, IDLE_MODE);
    Pinmux_Config(P4_2, IDLE_MODE);
    Pinmux_Config(P4_1, IDLE_MODE);
    Pad_Config(P4_3, PAD_PINMUX_MODE, PAD_IS_PWRON, PAD_PULL_UP,   PAD_OUT_DISABLE, PAD_OUT_LOW);
    Pad_Config(P4_0, PAD_PINMUX_MODE, PAD_IS_PWRON, PAD_PULL_NONE, PAD_OUT_DISABLE, PAD_OUT_LOW);
    Pad_Config(P4_2, PAD_PINMUX_MODE, PAD_IS_PWRON, PAD_PULL_UP,   PAD_OUT_DISABLE, PAD_OUT_LOW);
    Pad_Config(P4_1, PAD_PINMUX_MODE, PAD_IS_PWRON, PAD_PULL_UP,   PAD_OUT_DISABLE, PAD_OUT_LOW);
    if (with_d23)
    {
        Pinmux_Config(P0_6, IDLE_MODE);
        Pinmux_Config(P0_7, IDLE_MODE);
        Pad_Config(P0_6, PAD_PINMUX_MODE, PAD_IS_PWRON, PAD_PULL_UP, PAD_OUT_DISABLE, PAD_OUT_LOW);
        Pad_Config(P0_7, PAD_PINMUX_MODE, PAD_IS_PWRON, PAD_PULL_UP, PAD_OUT_DISABLE, PAD_OUT_LOW);
    }
    SYSBLKCTRL->u_210.BITS_210.BIT_SOC_FLASH_EN2 = 1;
    SYSBLKCTRL->u_2A8.BITS_2A8.SPIC2_MASTER_EN = 1;
    HAL_UPDATE32(0x40000300, BIT31, BIT31);
    SPIC2->ssienr = 0;
    SPIC2->ser = BIT0;
    SPIC2->baudr = 1;
    SPIC2->ctrlr2 |= (BIT16 | BIT17);
    lcd_hw_ready = true;
}

/* stock order: rail cycle 0x84AD88 -> SPIC2 -> IF8080 block cycle -> LCD power + reset 0x8602CC.
   The stock rail cycle also drives H_1/H_2 low; those stay untouched here. */
static void screen_stock_boot(bool with_d23)
{
    stock_out(P2_5, 0);
    stock_out(P1_2, 0);
    stock_out(P0_5, 0);
    platform_delay_ms(50);
    stock_out(P2_5, 1);
    Pad_Config(P1_2, PAD_SW_MODE, PAD_IS_PWRON, PAD_PULL_UP, PAD_OUT_DISABLE, PAD_OUT_LOW);
    Pad_Config(P0_5, PAD_SW_MODE, PAD_IS_PWRON, PAD_PULL_UP, PAD_OUT_DISABLE, PAD_OUT_LOW);
    platform_delay_ms(100);
    wdg_kick();
    spic2_stock_init(with_d23);
    RCC_PeriphClockCmd(APBPeriph_IF8080, APBPeriph_IF8080_CLOCK, DISABLE);
    RCC_PeriphClockCmd(APBPeriph_IF8080, APBPeriph_IF8080_CLOCK, ENABLE);
    stock_out(P2_0, 1);
    stock_out(P0_1, 1);
    platform_delay_ms(50);
    stock_out(P2_0, 0);
    platform_delay_ms(120);
    stock_out(P2_0, 1);
    platform_delay_ms(50);
    wdg_kick();
}

/* stock 0x84AD88, pin for pin: P2_5, H_1, H_2, P1_2, P0_5 low, 50 ms, P2_5 high, 100 ms.
   H_1/H_2 are I2C on this board; afterwards they go back to inputs with pull-up like the stock I2C setup. */
static void rail_stock_cycle(void)
{
    stock_out(P2_5, 0);
    Pad_Config(H_1, PAD_SW_MODE, PAD_IS_PWRON, PAD_PULL_DOWN, PAD_OUT_ENABLE, PAD_OUT_LOW);
    Pad_Config(H_2, PAD_SW_MODE, PAD_IS_PWRON, PAD_PULL_DOWN, PAD_OUT_ENABLE, PAD_OUT_LOW);
    stock_out(P1_2, 0);
    stock_out(P0_5, 0);
    platform_delay_ms(50);
    stock_out(P2_5, 1);
    platform_delay_ms(100);
    Pad_Config(H_1, PAD_SW_MODE, PAD_IS_PWRON, PAD_PULL_UP, PAD_OUT_DISABLE, PAD_OUT_LOW);
    Pad_Config(H_2, PAD_SW_MODE, PAD_IS_PWRON, PAD_PULL_UP, PAD_OUT_DISABLE, PAD_OUT_LOW);
    Pad_Config(P1_2, PAD_SW_MODE, PAD_IS_PWRON, PAD_PULL_UP, PAD_OUT_DISABLE, PAD_OUT_LOW);
    Pad_Config(P0_5, PAD_SW_MODE, PAD_IS_PWRON, PAD_PULL_UP, PAD_OUT_DISABLE, PAD_OUT_LOW);
    wdg_kick();
}

/* the panel pulses TE (P3_0, stock 0x860330) once per frame: edges here = a running panel, no eyes needed */
static uint16_t te_count(uint16_t ms, uint8_t *level)
{
    remember(P3_0);
    RCC_PeriphClockCmd(APBPeriph_GPIO, APBPeriph_GPIO_CLOCK, ENABLE);
    Pinmux_Config(P3_0, DWGPIO);
    Pad_Config(P3_0, PAD_PINMUX_MODE, PAD_IS_PWRON, PAD_PULL_NONE, PAD_OUT_DISABLE, PAD_OUT_LOW);
    GPIO_InitTypeDef g;
    GPIO_StructInit(&g);
    g.GPIO_Pin = GPIO_GetPin(P3_0);
    g.GPIO_Mode = GPIO_Mode_IN;
    g.GPIO_ITCmd = DISABLE;
    GPIO_Init(&g);
    driven_mask |= bit(P3_0);
    uint16_t edges = 0;
    uint8_t last = GPIO_ReadInputDataBit(g.GPIO_Pin);
    for (uint32_t t = 0; t < (uint32_t)ms * 50; t++)
    {
        uint8_t now = GPIO_ReadInputDataBit(g.GPIO_Pin);
        if (now && !last && edges < 0xFFFF) { edges++; }
        last = now;
        platform_delay_us(20);
        if ((t & 0x3FFF) == 0) { wdg_kick(); }
    }
    *level = last;
    return edges;
}

/* ch 1 = dual data: both SIO0 and SIO1 are sampled, so the answer shows up whichever wire the panel uses */
static uint8_t spic2_read(uint8_t op, const uint8_t *a, uint8_t n, uint16_t dummy, uint8_t ch, uint16_t baud, uint8_t *out)
{
    uint32_t c0 = SPIC2->ctrlr0, c1 = SPIC2->ctrlr1, al = SPIC2->auto_length, ad = SPIC2->addr_length, bd = SPIC2->baudr;
    SPIC2->ssienr = 0;
    SPIC2->ser = 1;
    SPIC2->baudr = baud;
    SPIC2->ctrlr0 &= ~(0x3F0000 | 0x300);
    SPIC2->ctrlr0 |= BIT_TMOD(3) | BIT_DATA_CH(ch);
    SPIC2->ctrlr1 = n;
    SPIC2->addr_length = BIT_ADDR_PHASE_LENGTH(3);
    HAL_UPDATE32(&SPIC2->auto_length, 0xfff, dummy);
    SPIC2->dr[0].byte = op;
    SPIC2->dr[0].byte = a[0];
    SPIC2->dr[0].byte = a[1];
    SPIC2->dr[0].byte = a[2];
    SPIC2->ssienr = 1;
    uint32_t guard = 0;
    while ((SPIC2->sr & BIT_BUSY) && guard < 200000) { guard++; }
    uint8_t sr = (uint8_t)SPIC2->sr;
    SPIC2->ssienr = 0;
    for (uint8_t i = 0; i < n; i++) { out[i] = SPIC2->dr[0].byte; }
    SPIC2->auto_length = al;
    SPIC2->addr_length = ad;
    SPIC2->ctrlr1 = c1;
    SPIC2->ctrlr0 = c0;
    SPIC2->baudr = bd;
    return sr;
}

static uint32_t fill_word;

/* one DMA block, register for register like the stock pixel path (0x88309E + 0x88314E) */
static bool quad_block(uint32_t words)
{
    GDMA_InitTypeDef d;
    GDMA_StructInit(&d);
    d.GDMA_ChannelNum = 1;
    d.GDMA_DIR = GDMA_DIR_MemoryToPeripheral;
    d.GDMA_BufferSize = words;
    d.GDMA_SourceInc = DMA_SourceInc_Fix;
    d.GDMA_DestinationInc = DMA_DestinationInc_Fix;
    d.GDMA_SourceDataSize = GDMA_DataSize_Word;
    d.GDMA_DestinationDataSize = GDMA_DataSize_Word;
    d.GDMA_SourceMsize = GDMA_Msize_1;
    d.GDMA_DestinationMsize = GDMA_Msize_1;
    d.GDMA_SourceAddr = (uint32_t)&fill_word;
    d.GDMA_DestinationAddr = (uint32_t)&SPIC2->dr[0];
    d.GDMA_DestHandshake = GDMA_Handshake_SPIC2_TX;
    GDMA_Init(GDMA_Channel1, &d);

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
    GDMA_Cmd(1, ENABLE);

    uint32_t guard = 0;
    while (GDMA_GetChannelStatus(1) && guard < 4000000) { guard++; }
    bool ok = guard < 4000000;
    guard = 0;
    while ((SPIC2->sr & BIT_BUSY) && guard < 200000) { guard++; }
    SPIC2->ssienr = 0;
    SPIC2->dmacr = 0;
    SPIC2->ctrlr2 &= ~8;
    SPIC2->imr &= ~0x200;
    return ok;
}

static bool quad_fill(uint16_t x0, uint16_t x1, uint16_t y0, uint16_t y1, uint8_t hi, uint8_t lo)
{
    uint8_t h2a[4] = {0x02, 0x00, 0x2A, 0x00}, h2b[4] = {0x02, 0x00, 0x2B, 0x00};
    uint8_t xr[4] = {x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF};
    uint32_t w = (uint32_t)x1 - x0 + 1;
    uint32_t rows = (w * 2 >= 8096) ? 1 : 8096 / (w * 2);     /* stock sends 11 lines per DMA block */
    RCC_PeriphClockCmd(APBPeriph_GDMA, APBPeriph_GDMA_CLOCK, ENABLE);
    fill_word = hi | ((uint32_t)lo << 8) | ((uint32_t)hi << 16) | ((uint32_t)lo << 24);
    lcd_hw_frame(h2a, 4, xr, 4);
    for (uint32_t y = y0; y <= y1; y += rows)
    {
        uint32_t last = y + rows - 1;
        if (last > y1) { last = y1; }
        uint8_t yr[4] = {y >> 8, y & 0xFF, last >> 8, last & 0xFF};
        lcd_hw_frame(h2b, 4, yr, 4);
        uint32_t bytes = w * 2 * (last - y + 1);
        if (!quad_block((bytes + 3) / 4)) { return false; }
        wdg_kick();
    }
    return true;
}

#include "onkar_os.inc"

/* ------------------------------------------------------------------ watchdog + heartbeat */

static void hb_timer_cb(void *p)
{
    (void)p;
    /* feed only if the app task handled a heartbeat since the last tick */
    if (wdg_armed && !wdg_paused && hb_seen != hb_seen_at_last_tick) { WDG_Restart(); }
    hb_seen_at_last_tick = hb_seen;
    if (hb_sent != hb_seen) { return; }     /* previous one not handled yet: never flood the app queue */
    T_IO_MSG m;
    m.type = IO_MSG_TYPE_RESET_WDG_TIMER;
    m.subtype = 0;
    m.u.param = 0;
    if (app_send_msg_to_apptask(&m)) { hb_sent++; }
}

/* called by the app task for IO_MSG_TYPE_RESET_WDG_TIMER */
void onkar_heartbeat(void)
{
    hb_seen++;
    if (!wdg_armed && !wdg_paused && hb_seen >= 3)
    {
        /* 32.768 kHz / (63 + 1) = 512 Hz, count limit 15 -> 4095 ticks = 8 s, full chip reset */
        WDG_ClockEnable();
        WDG_Config(63, 15, RESET_ALL);
        WDG_Enable();
        WDG_Restart();
        wdg_armed = true;
    }
    if (driven_mask && os_sys_time_get() - last_cmd_ms > DEADMAN_MS) { release_all(); }
}

/* firmware update starting: never let our watchdog or pins get in its way */
void onkar_dfu_activity(void)
{
    if (!wdg_paused)
    {
        wdg_paused = true;
        if (wdg_armed) { WDG_Disable(); }
        release_all();
        oos_dfu();
    }
}

void onkar_on_disconnect(void)
{
    release_all();
    if (wdg_paused)
    {
        /* an update that ends normally reboots the chip; this is an aborted one */
        wdg_paused = false;
        if (wdg_armed) { WDG_Enable(); WDG_Restart(); }
    }
}

void onkar_init(void)
{
    boot_reset_reason = (uint8_t)reset_reason_get();
    last_cmd_ms = os_sys_time_get();
    /* stock 0x84DD02: motor PWM pin idles HIGH; left low, the motor runs as soon as its supply is up */
    Pad_Config(P0_0, PAD_SW_MODE, PAD_IS_PWRON, PAD_PULL_UP, PAD_OUT_ENABLE, PAD_OUT_HIGH);
    if (os_timer_create(&hb_timer, "onkar_hb", 0x4F4B, 1000, true, hb_timer_cb))
    {
        os_timer_start(&hb_timer);
    }
    oos_init();
}

/* ------------------------------------------------------------------ command handler */

static uint8_t r[244];
static uint8_t big[HUNT_MAX_HITS * 9];

void onkar_tool_handle(uint8_t conn_id, uint8_t *p, uint16_t len)
{
    if (p == NULL || len == 0) { return; }
    last_cmd_ms = os_sys_time_get();
    memset(r, 0, sizeof(r));
    r[0] = p[0] | 0x80;

    switch (p[0])
    {
    case 0x01:
        r[1] = 'O'; r[2] = 'K'; r[3] = ONKAR_FW_VER;
        reply(conn_id, r, 4);
        break;

    case 0x02:
        {
            uint16_t raw = 0; uint8_t pct = 0; int8_t err = 0;
            uint16_t mv = onkar_battery_read(&raw, &pct, &err);
            r[1] = raw & 0xff; r[2] = raw >> 8; r[3] = mv & 0xff; r[4] = mv >> 8; r[5] = pct; r[6] = (uint8_t)err;
            reply(conn_id, r, 7);
        }
        break;

    case 0x03:
        if (len >= 3 && pin_valid(p[1])) { pin_hold(p[1], p[2]); r[1] = p[1]; r[2] = p[2]; reply(conn_id, r, 3); }
        else { r[1] = 0xEE; r[2] = len >= 2 ? p[1] : 0; reply(conn_id, r, 3); }
        break;

    case 0x04:
        if (len >= 3 && pin_valid(p[1]))
        {
            RCC_PeriphClockCmd(APBPeriph_GPIO, APBPeriph_GPIO_CLOCK, ENABLE);
            if (driven_mask & bit(p[1])) { r[1] = p[1]; r[2] = 0xDD; }       /* we are driving it */
            else { r[1] = p[1]; r[2] = pin_read(p[1], p[2]); }
            reply(conn_id, r, 3);
        }
        else { r[1] = 0xEE; reply(conn_id, r, 3); }
        break;

    case 0x05:
        {
            uint8_t pull = (len >= 2) ? p[1] : 0;
            uint64_t bits = 0, skipped = 0;
            RCC_PeriphClockCmd(APBPeriph_GPIO, APBPeriph_GPIO_CLOCK, ENABLE);
            for (uint8_t pin = 0; pin < 40; pin++)
            {
                if (!pin_valid(pin) || (driven_mask & bit(pin))) { skipped |= bit(pin); continue; }
                if (pin_read(pin, pull)) { bits |= bit(pin); }
            }
            memcpy(&r[1], &bits, 5);
            memcpy(&r[6], &skipped, 5);
            reply(conn_id, r, 11);
        }
        break;

    case 0x08:
        if (len >= 5 && pin_valid(p[1]))
        {
            uint16_t ms = p[3] | (p[4] << 8);
            if (ms > 1000) { ms = 1000; }
            pin_hold(p[1], p[2]);
            platform_delay_ms(ms);
            pin_release(p[1]);
            r[1] = p[1];
            reply(conn_id, r, 2);
        }
        else { r[1] = 0xEE; reply(conn_id, r, 2); }
        break;

    case 0x0A:
        if (len >= 2 && pin_valid(p[1])) { pin_release(p[1]); r[1] = p[1]; }
        else { r[1] = 0xEE; }
        reply(conn_id, r, 2);
        break;

    case 0x0B:
        r[1] = release_all();
        reply(conn_id, r, 2);
        break;

    case 0x10:
        {
            uint32_t up = (uint32_t)(os_sys_time_get() / 1000);
            r[1] = ONKAR_FW_VER;
            memcpy(&r[2], &up, 4);
            r[6] = boot_reset_reason;
            r[7] = (wdg_armed ? 1 : 0) | (wdg_paused ? 2 : 0);
            memcpy(&r[8], &driven_mask, 5);
            uint32_t hb = hb_seen;
            memcpy(&r[13], &hb, 4);
            reply(conn_id, r, 17);
        }
        break;

    case 0x12:
        if (len >= 3)
        {
            T_I2C b;
            uint8_t st = i2c_open(&b, p[1], p[2]);
            uint8_t cnt = 0;
            if (st == 0)
            {
                for (uint8_t a = 0x08; a < 0x78; a++)
                {
                    i2c_start(&b);
                    bool ack = i2c_wbyte(&b, a << 1);
                    i2c_stop(&b);
                    if (ack) { r[3 + (a >> 3)] |= 1 << (a & 7); cnt++; }
                }
            }
            if (st != 3) { pin_release(p[1]); pin_release(p[2]); }
            r[1] = st; r[2] = cnt;
            reply(conn_id, r, 19);
        }
        break;

    case 0x13:
        if (len >= 6)
        {
            T_I2C b;
            uint8_t n = p[5] > 64 ? 64 : p[5];
            uint8_t st = i2c_open(&b, p[1], p[2]);
            if (st == 0)
            {
                i2c_start(&b);
                if (!i2c_wbyte(&b, p[3] << 1) || !i2c_wbyte(&b, p[4])) { st = 4; }
                else
                {
                    i2c_start(&b);
                    if (!i2c_wbyte(&b, (p[3] << 1) | 1)) { st = 5; }
                    else { for (uint8_t i = 0; i < n; i++) { r[3 + i] = i2c_rbyte(&b, i + 1 < n); } }
                }
                i2c_stop(&b);
            }
            if (st != 3) { pin_release(p[1]); pin_release(p[2]); }
            r[1] = st; r[2] = (st == 0) ? n : 0;
            reply(conn_id, r, 3 + r[2]);
        }
        break;

    case 0x14:
        if (len >= 5 && p[4] <= len - 5)
        {
            T_I2C b;
            uint8_t st = i2c_open(&b, p[1], p[2]);
            if (st == 0)
            {
                i2c_start(&b);
                if (!i2c_wbyte(&b, p[3] << 1)) { st = 4; }
                for (uint8_t i = 0; st == 0 && i < p[4]; i++) { if (!i2c_wbyte(&b, p[5 + i])) { st = 6; } }
                i2c_stop(&b);
            }
            if (st != 3) { pin_release(p[1]); pin_release(p[2]); }
            r[1] = st;
            reply(conn_id, r, 2);
        }
        break;

    case 0x25:      /* v13 hardware PWM: pin period_us(u16) high(u16, 0.1 us) */
        if (len >= 6 && pin_valid(p[1]) && p[1] != P1_3 && p[1] != P1_4)
        {
            uint8_t pin = p[1];
            uint32_t period = p[2] | (p[3] << 8), high = p[4] | (p[5] << 8);
            uint32_t total = period * 40, hc = high * 4;
            if (period < 2 || period > 50000) { r[1] = 0xEE; reply(conn_id, r, 2); break; }
            if (hc == 0 || hc >= total)
            {
                pwm_stop();
                pin_hold(pin, hc ? 1 : 0);
            }
            else { pwm_apply(pin, period, high, true); }
            r[1] = 0;
        }
        else { r[1] = 0xEE; }
        reply(conn_id, r, 2);
        break;

    case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38:
        oos_cmd(conn_id, p, len);
        break;

    case 0x21:      /* count TE pulses: ms(u16) */
        if (len >= 3)
        {
            uint8_t lvl = 0;
            uint16_t ms = p[1] | (p[2] << 8);
            uint16_t e = te_count(ms > 1000 ? 1000 : ms, &lvl);
            r[1] = e & 0xFF; r[2] = e >> 8; r[3] = lvl;
            reply(conn_id, r, 4);
        }
        break;

    case 0x1C:      /* read flash (memory-mapped, read only): addr(u32) n(<=236) -> 9C addr(u32) data */
        if (len >= 6)
        {
            uint32_t addr;
            memcpy(&addr, &p[1], 4);
            uint8_t n = p[5] > 236 ? 236 : p[5];
            if (addr < 0x00800000 || addr + n > 0x01800000) { r[1] = 0xEE; reply(conn_id, r, 2); break; }
            memcpy(&r[1], &addr, 4);
            memcpy(&r[5], (const void *)addr, n);
            reply(conn_id, r, 5 + n);
        }
        break;

    case 0x18:      /* read-only dump of every pad: b0 b1 pinmux, pins 0..38 (H pins only READ) */
        {
            uint8_t k = 1;
            for (uint8_t pin = 0; pin <= 38; pin++)
            {
                if (pin == 31) { r[k++] = 0; r[k++] = 0; r[k++] = 0; continue; }
                uint8_t a = (uint8_t)PINADDR_TABLE[pin];
                r[k++] = btaon_fast_read_safe(a);
                r[k++] = btaon_fast_read_safe(a + 1);
                r[k++] = pmx_get(pin);
            }
            reply(conn_id, r, k);
        }
        break;

    default:
        r[1] = 0xEE;
        reply(conn_id, r, 2);
        break;
    }
}
