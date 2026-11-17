/*
 * Matrix Pico - serial-tunable RGH 1.2 glitcher for Xbox 360 on the RP2040
 *
 * Based on PicoRGH by Balázs Triszka <balika011@gmail.com> (2022), which in
 * turn implements the Reset Glitch Hack discovered by GliGli and Tiros.
 *
 * Additions in Matrix Pico (GPLv2, same as the original):
 *   - USB serial console for live timing tuning (no reflashing per attempt)
 *   - Named per-board timing profiles (trinity / corona / jasper / falcon)
 *   - Profiles persisted to the Pico's flash so a dialed-in console keeps
 *     booting headless, with no PC attached
 *   - Glitch loop runs on core1 so the console stays responsive over USB
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdint.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "glitch.pio.h"
#include "extclk.pio.h"

/* Status LED. The RP2040-Zero's onboard RGB is a WS2812 on GP16; a classic
 * Pico has a plain LED on GP25. Set USE_WS2812 to match your board. */
#define USE_WS2812 1
#if USE_WS2812
#include "ws2812.pio.h"
#define LED_PIN 16
#else
#define LED_PIN 25
#endif

/* ---- Motherboard wiring (Pico GPIO -> Xbox 360 point) -------------------
 * GP26 -> CPU_RESET      (glitch pulse)
 * GP27 -> CPU_PLL_BYPASS (series resistor: 5-10K on Slim, 22K on Phat)
 * GP28 -> POST           (bit monitored; referenced as GPIO 28 in glitch.pio)
 * GP29 -> EXT_CLK only: arm-sense on a 3.3V reset point (reset OUT stays 1.1V)
 * GP16 -> onboard WS2812 (RP2040-Zero): blue = glitching, green = booted.
 *                          Classic Pico: set USE_WS2812 0 to use GP25.
 * 3V3 / GND -> switched 3.3V and ground
 *
 * For EXT_CLK (zephyr), GP27 goes to CPU_EXT_CLK_EN and GP26 is open-drain to
 * the 1.1V reset net. See docs/ZEPHYR_EXTCLK.md.
 *
 * POST is hard-coded to GPIO 28 inside glitch.pio AND extclk.pio. If you move
 * it, change it in all places.
 * ------------------------------------------------------------------------ */
#define CPU_RESET   26   /* reset pulse OUT (both methods); also arm-sense for RGH 1.2 */
#define PLL_BYPASS  27   /* RGH 1.2: CPU_PLL_BYPASS.  EXT_CLK: CPU_EXT_CLK_EN (same pin) */
#define POST_PIN    28
#define RESET_SENSE 29   /* EXT_CLK only: arm-sense on a 3.3V reset point (reset OUT is 1.1V) */

/* Glitch method per profile. */
#define METHOD_RGH12   0   /* Trinity/Corona/Falcon/Jasper - PLL bypass */
#define METHOD_EXTCLK  1   /* Xenon/Zephyr - EXT_CLK_EN slowdown */

#ifndef MP_SYS_CLK_KHZ
#define MP_SYS_CLK_KHZ 200000   /* ~5 ns per PIO cycle; hardware-validated on Trinity */
#endif
/* No-screen auto-run: when powered from the console (no USB to type a command),
 * start the dense EXT_CLK search automatically on the zephyr profile. */
#ifndef MP_AUTORUN
#define MP_AUTORUN 1
#endif
#ifndef MP_EXTCLK_PROFILE
#define MP_EXTCLK_PROFILE 4     /* index of "zephyr" in the profiles[] table below */
#endif
/* Speed: force a console reboot on a hung (failed) attempt instead of waiting
 * ~7s for the SMC watchdog. Safe because it only fires on frozen POST in a
 * 3-7s window; a real boot keeps POST moving. Set MP_FORCE_REBOOT=0 to disable. */
#ifndef MP_FORCE_REBOOT
#define MP_FORCE_REBOOT 0   /* OFF for unattended runs: a successful boot can have a
                             * >2s POST-quiet stretch (CD/kernel/XeLL load) that the
                             * hang-detector would mistake for a failure and reboot,
                             * killing the very boot we want before the 12s confirm. */
#endif
#ifndef MP_FREEZE_MS
#define MP_FREEZE_MS 2200       /* POST still this long while held high = hang */
#endif
/* Marker stored in the (otherwise-unused for EXT_CLK) t[1] slot once a boot has
 * landed, so the winning r_end (t[2]) and anchor (t[0]) survive a power-cycle
 * and can be read back from the banner on USB. */
#ifndef EXTCLK_LOCK_MAGIC
#define EXTCLK_LOCK_MAGIC 0xA5u
#endif

/* ---- Profiles ----------------------------------------------------------- */
#define NUM_TIMINGS     5
#define NUM_PROFILES    6
#define PROFILE_NAME_LEN 16
#define CONFIG_MAGIC    0x4D505837u   /* "MPX7" - bump if layout/clock/engine changes */
#define CONFIG_MAGIC_V5 0x4D505835u   /* "MPX5" - 5-profile layout, migrated on load */
#define NUM_PROFILES_V5 5
#define CONFIG_VERSION  1

/* Timing value meanings (index -> role), same order they are fed to the PIO:
 *   [0] post_count   : number of POST pulses to count before arming
 *   [1] delay_to_pll : cycles from the counted POST to pulling PLL bypass up
 *   [2] delay_to_rst : cycles from POST going high to pulling CPU_RESET down
 *   [3] glitch_width : cycles CPU_RESET is held low (the glitch itself)
 *   [4] pll_hold     : cycles PLL bypass is held after the glitch
 */
static const char *TIMING_NAMES[NUM_TIMINGS] = {
    "post_count",
    "delay_to_pll",
    "delay_to_rst",
    "glitch_width",
    "pll_hold",
};

/* EXT_CLK reuses the 5 slots with different meanings. */
static const char *TIMING_NAMES_EXTCLK[NUM_TIMINGS] = {
    "post_count",
    "slow_delay",
    "r_end",
    "pulse_width",
    "extc_hold",
};

static inline const char *tname(uint32_t method, int i)
{
    return (method == METHOD_EXTCLK) ? TIMING_NAMES_EXTCLK[i] : TIMING_NAMES[i];
}

typedef struct {
    char     name[PROFILE_NAME_LEN];
    uint32_t method;                  /* METHOD_RGH12 or METHOD_EXTCLK */
    uint32_t t[NUM_TIMINGS];
} profile_t;

typedef struct {
    uint32_t  magic;
    uint32_t  version;
    uint32_t  active;                 /* index of the active profile */
    uint32_t  reserved;
    profile_t profiles[NUM_PROFILES];
} config_t;

_Static_assert(sizeof(config_t) <= 256, "config must fit one flash page");

/* Built-in defaults.
 *
 * "trinity" carries the original author's real Trinity RGH 1.2 values, so it
 * is a genuine starting point, not a placeholder. The other three are SEEDED
 * with the Trinity values only so the firmware has something to run; they are
 * NOT verified for those boards and MUST be dialed in before they are trusted.
 */
#if MP_SYS_CLK_KHZ == 200000
#define TRI_T { 3, 108786906, 7288000, 47, 50000 }    /* proven 200 MHz values */
#else
#define TRI_T { 3, 104435430, 6996480, 45, 48000 }    /* same real-time at 192 MHz */
#endif

static const config_t DEFAULT_CONFIG = {
    .magic   = CONFIG_MAGIC,
    .version = CONFIG_VERSION,
    .active  = 0,
    .reserved = 0,
    .profiles = {
        /* RGH 1.2, 192 MHz values (200 MHz x 0.96, same real-time timing).
         * trinity carries balika011's proven numbers; others are seeds. */
        { "trinity", METHOD_RGH12,  TRI_T },
        { "corona",  METHOD_RGH12,  TRI_T },  /* dial in */
        { "jasper",  METHOD_RGH12,  TRI_T },  /* dial in */
        { "falcon",  METHOD_RGH12,  TRI_T },  /* dial in */
        /* EXT_CLK seeds from Octal450's Zephyr VHDL @192MHz: r_end 118008,
         * pulse 2, slow_delay ~382ms (73,344,000 cyc), T_BUF 50000, ~12 pulses.
         * These are STRUCTURAL SEEDS - must be dialed in on hardware. */
        { "zephyr",  METHOD_EXTCLK, { 12, 18336000u, 118008, 2, 50000 } },     /* [1]=dwell periods (~382ms); r_end in 192MHz units; window 117998-118010 */
        /* Xenon: same EXT_CLK engine; Octal450's VHDL differs from Zephyr only in
         * R_END (118002 vs 118008 @192MHz). Untested - seed, dial in with sweep. */
        { "xenon",   METHOD_EXTCLK, { 12, 100, 118002, 2, 50000 } },           /* r_end in Matrix 192MHz units */
    },
};

/* Live config in RAM. core1 reads the active profile at the top of each
 * glitch cycle; core0 edits it from the serial console. */
static config_t g_cfg;

/* Flash storage: last sector of the device's flash. */
#define FLASH_TARGET_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
static const uint8_t *g_flash_cfg = (const uint8_t *)(XIP_BASE + FLASH_TARGET_OFFSET);

/* Shared glitch counter so the console can report activity. */
static volatile uint32_t g_attempts = 0;
typedef struct { uint32_t e15, etot, ms; uint8_t done; absolute_time_t t0; } hist_t;
static hist_t g_hist[16];
static volatile uint32_t g_hist_n = 0;       /* attempts started since boot */
static volatile uint32_t g_last_edges = 0;   /* POST edges seen 4s after the last glitch */
static volatile uint32_t g_boot_edges = 3;   /* (legacy, unused by the boot judge now) */
#define BOOT_HOLD_MS 12000  /* > the ~7s SMC fail-hold we measured, < an infinite boot */

/* Sweep (auto-tune) state, shared core0 <-> core1. When active, core1 applies
 * sweep_val to the active profile's timing[sweep_idx] on each attempt and steps
 * it after every miss; on a boot it latches the landing value and stops. */
static volatile bool     sweep_active = false;
static volatile int      sweep_idx = 0;
static volatile uint32_t sweep_start = 0;
static volatile uint32_t sweep_end = 0;
static volatile int32_t  sweep_step = 0;
static volatile uint32_t sweep_val = 0;
static volatile uint32_t sweep_tick = 0;         /* bumped each step, for core0 */
static volatile bool     sweep_landed = false;
static volatile uint32_t sweep_landed_val = 0;

/* 2-D auto-sweep: sweeps r_end (index 2) as the inner loop and the EXT_CLK
 * anchor (POST pulses to the r_end reference) as the outer loop, so a single
 * run covers both coupled unknowns. Stops on a real boot. */
static volatile bool     g_autosweep = false;
static volatile uint32_t g_as_anchor_lo = 10, g_as_anchor_hi = 13;
static volatile uint32_t g_extclk_anchor = 12;     /* current anchor, in POST pulses */
static volatile uint32_t g_as_anchor_landed = 0;
static volatile bool     g_as_anchor_changed = false;

/* Set when a sweep lands a boot: core1 stops attempting further glitches so a
 * console that just came up stays up, instead of being re-glitched forever.
 * Cleared by starting a new sweep, switching profiles, or 'resume'. */
static volatile bool     g_halt = false;

/* ---- adcmon: ADC/DMA edge-capture diagnostic ----------------------------
 * Reads POST (GP28 / ADC2) and the 3.3V reset-sense (GP29 / ADC3) as ANALOG
 * inputs instead of digital GPIO, so a 1.1V POST "high" (which may sit below
 * the Pico's ~2V digital input threshold) can still be seen and timed.
 *
 * This is a one-shot diagnostic, run once per power-cycle (same workflow as
 * 'mon'): it takes over GP28/GP29 for its capture window, then hands them
 * back to core1 as plain digital inputs. Because core1's glitch loop also
 * reads those same two pins, core1 is paused (multicore_lockout) for the
 * whole window so it never sees them mid-reconfiguration.
 *
 * Sampling: round-robin ADC across channels 2 and 3 at full speed (clkdiv 0),
 * 96 cycles/conversion @ 48 MHz = exactly 2us/sample, so each channel is
 * re-sampled every 4us. A DMA channel streams the ADC FIFO into a small
 * power-of-two ring buffer in SRAM (hardware address wrap via
 * channel_config_set_ring); core0 polls the DMA write pointer and walks any
 * newly-written samples, classifying every raw sample by parity (round robin
 * always starts on channel 2, so even samples are POST, odd are reset-sense)
 * and looking for the first LOW->HIGH crossing on each channel. */
#define ADC_BUF_SAMPLES   4096                      /* power of two */
#define ADC_BUF_BYTES     (ADC_BUF_SAMPLES * 2)
#define ADC_RING_BITS     13                        /* log2(ADC_BUF_BYTES) */
#define ADC_SAMPLE_US     2                          /* exact @ clkdiv 0, 96 cyc/conv, 48MHz */
#define ADC_POST_THRESH   700    /* ~1.1V POST high reads ~1365/4095; threshold well above 0 */
#define ADC_RST_THRESH    2048   /* 3.3V logic; mid-scale */
#define ADC_HIST_N        8
#define ADC_DEFAULT_SECS  8

/* MUST be aligned to its own size: the DMA ring wrap only masks the low
 * ADC_RING_BITS of the address, so if this buffer isn't naturally aligned to
 * ADC_BUF_BYTES, the wrap point won't line up with the buffer's start and
 * writes can land outside it. */
static uint16_t g_adc_buf[ADC_BUF_SAMPLES] __attribute__((aligned(ADC_BUF_BYTES)));

typedef struct {
    bool     post_edge;    /* saw a LOW->HIGH POST transition this run */
    uint32_t post_us;      /* time of that edge, us from start of capture */
    bool     post_toggled; /* POST changed state at all (wire-good check) */
    bool     rst_edge;
    uint32_t rst_us;
    bool     rst_toggled;
} adc_hist_t;

static adc_hist_t g_adc_hist[ADC_HIST_N];
static uint32_t   g_adc_hist_n = 0;

static void print_adc_hist(void)
{
    if (g_adc_hist_n == 0) { printf("no adcmon runs yet\n"); return; }
    uint32_t n = g_adc_hist_n, from = n > ADC_HIST_N ? n - ADC_HIST_N : 0;
    uint32_t pmin = 0, pmax = 0, rmin = 0, rmax = 0; bool pfirst = true, rfirst = true;
    printf("run   POST-edge(ms)   RST-edge(ms)    POST-wire  RST-wire\n");
    for (uint32_t k = from; k < n; k++) {
        adc_hist_t *h = &g_adc_hist[k % ADC_HIST_N];
        if (h->post_edge) {
            printf("#%-4lu %-16lu.%03lu ", (unsigned long)(k + 1),
                   (unsigned long)(h->post_us / 1000), (unsigned long)(h->post_us % 1000));
        } else {
            printf("#%-4lu %-16s ", (unsigned long)(k + 1), "no edge");
        }
        if (h->rst_edge) {
            printf("%-15lu.%03lu ", (unsigned long)(h->rst_us / 1000), (unsigned long)(h->rst_us % 1000));
        } else {
            printf("%-15s ", "no edge");
        }
        printf("%-10s %s\n", h->post_toggled ? "ok" : "DEAD", h->rst_toggled ? "ok" : "DEAD");
        if (h->post_edge) {
            if (pfirst || h->post_us < pmin) pmin = h->post_us;
            if (pfirst || h->post_us > pmax) pmax = h->post_us;
            pfirst = false;
        }
        if (h->rst_edge) {
            if (rfirst || h->rst_us < rmin) rmin = h->rst_us;
            if (rfirst || h->rst_us > rmax) rmax = h->rst_us;
            rfirst = false;
        }
    }
    if (!pfirst)
        printf("POST edge spread over these runs: %lu us (min %lu.%03lu ms, max %lu.%03lu ms)\n",
               (unsigned long)(pmax - pmin), (unsigned long)(pmin / 1000), (unsigned long)(pmin % 1000),
               (unsigned long)(pmax / 1000), (unsigned long)(pmax % 1000));
    if (!rfirst)
        printf("RST  edge spread over these runs: %lu us (min %lu.%03lu ms, max %lu.%03lu ms)\n",
               (unsigned long)(rmax - rmin), (unsigned long)(rmin / 1000), (unsigned long)(rmin % 1000),
               (unsigned long)(rmax / 1000), (unsigned long)(rmax % 1000));
}

static void cmd_adcmon(uint32_t secs)
{
    printf("adcmon: capturing POST(28)/RESET_SENSE(29) as ANALOG for %lus.\n",
           (unsigned long)secs);
    printf("Power the console OFF now, start it, and let it run through this window.\n");
    printf("(pausing the glitch loop for the capture - this is expected)\n");

    uint32_t attempts_before = g_attempts;   /* sanity check: core1 must not advance this below */

    multicore_lockout_start_blocking();   /* core1 must not touch GP28/29 mid-reconfig */

    adc_init();
    adc_gpio_init(POST_PIN);     /* GP28 -> ADC2 */
    adc_gpio_init(RESET_SENSE);  /* GP29 -> ADC3 */
    adc_select_input(2);                         /* round robin starts here: POST first */
    adc_set_round_robin((1u << 2) | (1u << 3));
    adc_fifo_setup(true, true, 1, false, false); /* fifo on, DREQ on, thresh 1, no err, no shift */
    adc_set_clkdiv(0);                           /* full speed: 2us/sample */

    int dma_chan = dma_claim_unused_channel(true);
    dma_channel_config cfg = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_16);
    channel_config_set_read_increment(&cfg, false);
    channel_config_set_write_increment(&cfg, true);
    channel_config_set_dreq(&cfg, DREQ_ADC);
    channel_config_set_ring(&cfg, true, ADC_RING_BITS);   /* wrap the write address in SRAM */
    dma_channel_configure(dma_chan, &cfg, g_adc_buf, &adc_hw->fifo, 0xFFFFFFFFu, false);

    adc_fifo_drain();
    adc_run(true);
    dma_channel_start(dma_chan);

    const uint32_t mask = ADC_BUF_SAMPLES - 1;
    uint32_t last_idx = 0;
    uint64_t total_raw = 0;
    bool post_state = false, rst_state = false, post_init = false, rst_init = false;
    bool post_edge = false, rst_edge = false, post_toggled = false, rst_toggled = false;
    uint32_t post_edge_us = 0, rst_edge_us = 0;

    absolute_time_t t_end = make_timeout_time_ms(secs * 1000);
    while (absolute_time_diff_us(get_absolute_time(), t_end) > 0) {
        uint32_t wa = dma_hw->ch[dma_chan].write_addr;
        uint32_t cur_idx = (uint32_t)((wa - (uintptr_t)g_adc_buf) / 2) & mask;
        uint32_t new_count = (cur_idx - last_idx) & mask;
        for (uint32_t i = 0; i < new_count; i++) {
            uint32_t idx = (last_idx + i) & mask;
            uint16_t raw = g_adc_buf[idx] & 0x0FFFu;
            uint32_t tus = (uint32_t)((total_raw + i) * ADC_SAMPLE_US);
            bool is_post = (((total_raw + i) & 1u) == 0);  /* round robin: even=ch2, odd=ch3 */
            if (is_post) {
                bool high = raw > ADC_POST_THRESH;
                if (post_init && high != post_state) {
                    post_toggled = true;
                    if (high && !post_edge) { post_edge = true; post_edge_us = tus; }
                }
                post_state = high; post_init = true;
            } else {
                bool high = raw > ADC_RST_THRESH;
                if (rst_init && high != rst_state) {
                    rst_toggled = true;
                    if (high && !rst_edge) { rst_edge = true; rst_edge_us = tus; }
                }
                rst_state = high; rst_init = true;
            }
        }
        total_raw += new_count;
        last_idx = cur_idx;
        sleep_ms(1);   /* buffer wraps every ~8.2ms; 1ms polling keeps us well ahead of that */
    }

    dma_channel_abort(dma_chan);
    adc_run(false);
    adc_fifo_drain();
    adc_set_round_robin(0);
    dma_channel_unclaim(dma_chan);

    /* Hand GP28/29 back to core1 as plain digital inputs, matching main()'s
     * original POST_PIN setup (pull-down) - core1 re-inits RESET_SENSE itself
     * every attempt, but POST_PIN is only set up once at boot, so redo it here. */
    gpio_set_function(POST_PIN, GPIO_FUNC_SIO);
    gpio_set_input_enabled(POST_PIN, true);
    gpio_set_dir(POST_PIN, GPIO_IN);
    gpio_set_pulls(POST_PIN, false, true);
    gpio_set_function(RESET_SENSE, GPIO_FUNC_SIO);
    gpio_set_input_enabled(RESET_SENSE, true);
    gpio_set_dir(RESET_SENSE, GPIO_IN);

    multicore_lockout_end_blocking();

    uint32_t attempts_during = g_attempts - attempts_before;
    if (attempts_during > 0) {
        printf("WARNING: %lu glitch attempt(s) ran on core1 DURING this capture -\n"
               "the lockout did not hold. Readings above may reflect a real glitch\n"
               "pulse hitting the console, not a clean power-on. Do not trust this\n"
               "run's edge times; treat as a firmware bug and stop testing until\n"
               "this prints 0 attempts.\n", (unsigned long)attempts_during);
    }

    adc_hist_t *h = &g_adc_hist[g_adc_hist_n % ADC_HIST_N];
    h->post_edge = post_edge; h->post_us = post_edge_us; h->post_toggled = post_toggled;
    h->rst_edge  = rst_edge;  h->rst_us  = rst_edge_us;  h->rst_toggled  = rst_toggled;
    g_adc_hist_n++;

    printf("POST(28):  %s", post_edge ? "edge seen" : "no LOW->HIGH edge seen");
    if (post_edge) printf(" @ %lu.%03lu ms", (unsigned long)(post_edge_us / 1000), (unsigned long)(post_edge_us % 1000));
    printf("  (%s)\n", post_toggled ? "wire toggled - good" : "DEAD - check this wire");
    printf("RST (29):  %s", rst_edge ? "edge seen" : "no LOW->HIGH edge seen");
    if (rst_edge) printf(" @ %lu.%03lu ms", (unsigned long)(rst_edge_us / 1000), (unsigned long)(rst_edge_us % 1000));
    printf("  (%s)\n", rst_toggled ? "wire toggled - good" : "DEAD - check this wire");
    printf("\n");
    print_adc_hist();
}

/* ---- Flash persistence -------------------------------------------------- */

static void config_load(void)
{
    const config_t *fc = (const config_t *)g_flash_cfg;
    if (fc->magic == CONFIG_MAGIC && fc->version == CONFIG_VERSION &&
        fc->active < NUM_PROFILES) {
        memcpy(&g_cfg, fc, sizeof(config_t));
    } else if (0 &&
               fc->magic == CONFIG_MAGIC_V5 && fc->version == CONFIG_VERSION &&
               fc->active < NUM_PROFILES_V5) {
        /* Old 5-profile save: keep the user's tuned profiles, add the new ones. */
        memcpy(&g_cfg, &DEFAULT_CONFIG, sizeof(config_t));
        g_cfg.active = fc->active;
        memcpy(g_cfg.profiles, fc->profiles, NUM_PROFILES_V5 * sizeof(profile_t));
    } else {
        memcpy(&g_cfg, &DEFAULT_CONFIG, sizeof(config_t));
    }
}

/* Must stop core1 before touching flash: it executes from XIP. */
static void config_save(void)
{
    uint8_t buf[FLASH_PAGE_SIZE];
    memset(buf, 0xFF, sizeof(buf));
    memcpy(buf, &g_cfg, sizeof(config_t));   /* config_t fits in one page */

    multicore_lockout_start_blocking();
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(FLASH_TARGET_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(FLASH_TARGET_OFFSET, buf, FLASH_PAGE_SIZE);
    restore_interrupts(ints);
    multicore_lockout_end_blocking();
}

/* ---- Status LED --------------------------------------------------------- */
#if USE_WS2812
static PIO  led_pio = pio1;
static uint led_sm  = 0;

static inline void led_raw(uint32_t grb) { pio_sm_put_blocking(led_pio, led_sm, grb << 8u); }
static inline uint32_t led_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 8) | ((uint32_t)g << 16) | (uint32_t)b;
}
static void led_init(void) {
    uint off = pio_add_program(led_pio, &ws2812_program);
    ws2812_program_init(led_pio, led_sm, off, LED_PIN, 800000, false);
}
static inline void led_off(void)   { led_raw(0); }
static inline void led_blue(void)  { led_raw(led_rgb(0, 0, 24)); }
static inline void led_green(void) { led_raw(led_rgb(0, 24, 0)); }
static inline void led_red(void)   { led_raw(led_rgb(24, 0, 0)); }
#else
static void led_init(void) { gpio_init(LED_PIN); gpio_set_dir(LED_PIN, GPIO_OUT); }
static inline void led_off(void)   { gpio_put(LED_PIN, 0); }
static inline void led_blue(void)  { gpio_put(LED_PIN, 1); }
static inline void led_green(void) { gpio_put(LED_PIN, 1); }
static inline void led_red(void)   { gpio_put(LED_PIN, 1); }
#endif

/* ---- Glitch core (core1) ------------------------------------------------ */

/* ---- LED state machine (core1) ------------------------------------------
 * Readable states: SOLID BLUE the whole time it's arming/attempting a glitch,
 * a clear SOLID RED flash on each failed attempt, and SOLID GREEN that LATCHES
 * and stays on (until the Pico is re-powered) on a confirmed boot - so a boot
 * that lands overnight is still obvious in the morning. Non-blocking. */
#define GREEN_GAP_MS 500   /* (retained for compatibility) */
static int g_led_cur = -1;                       /* 0 off, 1 blue, 2 red, 3 green */
static absolute_time_t g_red_end;                /* red-flash deadline; zero = nil_time */
static bool g_green_latched = false;             /* confirmed boot -> green stays on */

static void led_set(int c)
{
    if (c == g_led_cur) return;
    g_led_cur = c;
    switch (c) {
    case 1:  led_blue();  break;
    case 2:  led_red();   break;
    case 3:  led_green(); break;
    default: led_off();   break;
    }
}

static void led_tick(void)
{
    if (g_green_latched) { led_set(3); return; }                 /* booted: stay green */
    if (absolute_time_diff_us(get_absolute_time(), g_red_end) > 0) { led_set(2); return; } /* red flash */
    led_set(1);                                                  /* default: solid blue = attempting */
}

static void led_flicker_start(void)            /* start of an attempt: clear red, go solid blue */
{
    g_red_end = nil_time;
    if (!g_green_latched) led_set(1);
}

static void led_red_for(uint ms)               /* failed attempt: clear solid red for ms */
{
    if (g_green_latched) return;
    g_red_end = make_timeout_time_ms(ms);
    led_set(2);
}

static void led_green_hold(void)               /* confirmed boot: latch solid green, stays on */
{
    g_green_latched = true;
    g_red_end = nil_time;
    led_set(3);
}


static const pio_program_t *eng_prog(int e)
{
    return (e == 1) ? &extclk_program : &glitch_program;
}

static void run_glitch_once(PIO pio, uint sm, uint offset, uint method)
{
    /* RGH 1.2 uses the original PicoRGH program, which drives CPU_RESET (high
     * between pulses, low for the glitch). EXT_CLK keeps its open-drain engine. */
    const bool driven = (method != METHOD_EXTCLK);

    if (sweep_active)
        g_cfg.profiles[g_cfg.active].t[sweep_idx] = sweep_val;

    const uint32_t *t = g_cfg.profiles[g_cfg.active].t;

    /* RGH 1.2 senses the reset line it also pulses (GP26). EXT_CLK can't read
     * the 1.1V reset net, so it arms off a separate 3.3V reset point (GP29). */
    uint arm_pin = (method == METHOD_EXTCLK) ? RESET_SENSE : CPU_RESET;

    gpio_init(arm_pin);
    gpio_set_dir(arm_pin, GPIO_IN);
    if (method == METHOD_EXTCLK) {
        /* ANCHOR FIX: on this board POST starts ~tens of ms BEFORE GP29
         * (GPU_RST_DONE) rises. Arming on GP29-HIGH would start the POST count
         * mid-sequence and mis-place EXT_CLK_EN/glitch. Instead wait for GP29 LOW
         * (console in reset) so the PIO below catches POST from its very first
         * edge - matching the Matrix, which counts POST from reset. */
        while (gpio_get(arm_pin))
            led_tick();
    } else {
        while (!gpio_get(arm_pin))
            led_tick();                   /* RGH1.2: arm when reset releases (HIGH) */
    }

    pio_sm_config c = (method == METHOD_EXTCLK)
        ? extclk_program_get_default_config(offset)
        : glitch_program_get_default_config(offset);
    sm_config_set_set_pins(&c, CPU_RESET, 2);   /* bit0 = RESET, bit1 = PLL/EXTC */
    if (driven) sm_config_set_sideset_pins(&c, CPU_RESET);
    else sm_config_set_out_shift(&c, false, true, 32);  /* EXT_CLK engine uses autopull (out x,32) */

    pio_gpio_init(pio, CPU_RESET);
    gpio_disable_pulls(CPU_RESET);
    gpio_set_slew_rate(CPU_RESET, GPIO_SLEW_RATE_FAST);
    gpio_set_drive_strength(CPU_RESET, GPIO_DRIVE_STRENGTH_12MA);

    pio_gpio_init(pio, PLL_BYPASS);
    gpio_set_slew_rate(PLL_BYPASS, GPIO_SLEW_RATE_FAST);
    gpio_set_drive_strength(PLL_BYPASS, GPIO_DRIVE_STRENGTH_12MA);

    /* RGH 1.2: arming only happens once the console itself has raised reset, so
     * we start by driving it HIGH (matching the console) and PLL LOW. Both pins
     * go back to inputs the moment the glitch finishes.
     * EXT_CLK: reset stays an input (floating); only EXT_CLK_EN is driven. */
    if (driven) {
        pio_sm_set_pins_with_mask(pio, sm, (1u << CPU_RESET),
                                  (1u << CPU_RESET) | (1u << PLL_BYPASS));
        pio_sm_set_pindirs_with_mask(pio, sm, (1u << CPU_RESET) | (1u << PLL_BYPASS),
                                     (1u << CPU_RESET) | (1u << PLL_BYPASS));
    } else {
        pio_sm_set_pins_with_mask(pio, sm, 0u,
                                  (1u << CPU_RESET) | (1u << PLL_BYPASS));
        pio_sm_set_pindirs_with_mask(pio, sm, (1u << PLL_BYPASS),
                                     (1u << CPU_RESET) | (1u << PLL_BYPASS));
    }

    /* EXT_CLK reads POST through a fast diode (GP28 anode -> FT6U1 cathode); the
     * internal pull-up is what makes that diode level-shift the 1.1V POST into a
     * clean 0.6/1.7V digital swing the PIO can read. RGH 1.2 (driven) reads POST
     * bare, so only pull up for EXT_CLK. */
    if (!driven) gpio_pull_up(POST_PIN); else gpio_disable_pulls(POST_PIN);

    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);

    led_flicker_start();      /* 2.5s fast blue flicker, non-blocking */
    /* RGH1.2 (driven) waited ~204ms here so POST counting starts at the proven
     * moment. EXT_CLK must NOT: on this board POST begins during that window, so
     * the delay made us miss the first POST edges and undercount the anchor. Arm
     * (GP29 just went low = reset) then start counting immediately, so the PIO's
     * first 'wait 1 gpio POST' catches the boot's very first POST edge - matching
     * a Matrix, which counts POST from reset. */
    if (driven) {
        absolute_time_t dl = delayed_by_ms(get_absolute_time(), 204);
        while (absolute_time_diff_us(get_absolute_time(), dl) > 0) led_tick();
    }

    /* RGH1.2 feeds 5 values; EXT_CLK feeds 6 (r_end splits into coarse STBY
     * periods + fine sys cycles). Reset-low lasts (y + 1) cycles -> width-1. */
    if (driven) {
        pio_sm_put_blocking(pio, sm, t[0]);                  /* post_count */
        pio_sm_put_blocking(pio, sm, t[1]);                  /* delay_to_pll */
        pio_sm_put_blocking(pio, sm, t[2]);                  /* delay_to_rst */
        pio_sm_put_blocking(pio, sm, t[3]);                  /* glitch_width */
        pio_sm_put_blocking(pio, sm, t[4]);                  /* pll_hold */
    } else {
        /* EXT_CLK faithful-FSM port (GHDL-confirmed mechanism):
         *   ph1  = POST pulses to the code before the long pre-check gap (post_cnt=23)
         *   dwell= t[1] STBY periods spent in that gap before EXT_CLK_EN asserts
         *          (slower.vhd delay_val) - THE piece every earlier build missed
         *   ph2  = 1 pulse: wait for the next (now-slowed) POST edge = the check
         *   r_end= STBY periods off that edge, then the reset pulse.
         * post_count (ph1) is the sweepable anchor; dwell (t[1]) positions the
         * slowdown inside the gap; r_end (t[2]) fine-places the reset. */
        uint32_t ph1   = g_autosweep ? g_extclk_anchor : t[0];
        uint32_t dwell = t[1];                               /* delay_val in STBY periods */
        uint32_t coarse = t[2] >> 2;                         /* r_end 192MHz-units -> STBY periods */
        uint32_t fine   = t[2] & 3u;                         /* remainder -> ~5ns sys cycles */
        pio_sm_put_blocking(pio, sm, ph1);                   /* post_count (to post_cnt=23) */
        pio_sm_put_blocking(pio, sm, dwell);                 /* dwell periods (delay_val) */
        pio_sm_put_blocking(pio, sm, 1u);                    /* phase2 = 1: the slowed check edge */
        pio_sm_put_blocking(pio, sm, coarse);                /* r_end coarse (STBY periods) */
        pio_sm_put_blocking(pio, sm, t[3] > 0 ? t[3] - 1 : 0); /* pulse_width - 1 */
        pio_sm_put_blocking(pio, sm, fine);                  /* fine sys cycles */
        pio_sm_put_blocking(pio, sm, t[4]);                  /* extc_hold */
    }

    /* Wait for the glitch to complete, but don't block forever: if the console
     * never POSTs enough to reach the anchor (powered off, stuck, or mid-reset-
     * loop), phase1 would hang here. Time out, release EXT_CLK_EN, and re-arm. */
    absolute_time_t gs = get_absolute_time();
    bool fired = true;
    while (pio_sm_is_rx_fifo_empty(pio, sm)) {
        led_tick();
        if (absolute_time_diff_us(gs, get_absolute_time()) > 20000000) { fired = false; break; }
    }
    if (fired) pio_sm_get_blocking(pio, sm);   /* glitch complete */

    pio_sm_set_enabled(pio, sm, false);
    pio_sm_restart(pio, sm);

    if (!fired) {
        /* aborted: make sure EXT_CLK_EN isn't left asserted, then retry (no miss
         * scored, no sweep step - this attempt simply didn't happen). */
        gpio_init(PLL_BYPASS);
        gpio_set_dir(PLL_BYPASS, GPIO_OUT);
        gpio_put(PLL_BYPASS, 0);
        return;
    }

    gpio_init(arm_pin);
    gpio_set_dir(arm_pin, GPIO_IN);

    /* A hung (failed) glitch also leaves reset high for seconds, so reset-high
     * alone is NOT proof of a boot. A real boot keeps changing POST codes, so we
     * count POST edges for 4s after the glitch and require g_boot_edges of
     * them. The count is exposed via the 'last' command for calibration. */
    absolute_time_t t0 = get_absolute_time();
    hist_t *hh = &g_hist[g_hist_n % 16];
    hh->e15 = 0; hh->etot = 0; hh->ms = 0; hh->done = 0; hh->t0 = t0;
    g_hist_n++;
    bool booted = false;
    uint32_t edges = 0;
    bool lastp = gpio_get(POST_PIN);
    absolute_time_t last_edge_t = t0;     /* for force-reboot: when POST last moved */
    /* Real boot vs failed glitch, with only one POST bit:
     * a real boot holds the console OUT of reset (arm_pin stays HIGH) forever;
     * a failed glitch hangs at most a few seconds before the SMC watchdog resets
     * it, dropping arm_pin. So "booted" = arm_pin stays HIGH for BOOT_HOLD_MS
     * (longer than any hang). Edge counting (3 edges) false-landed on the normal
     * CB POST burst that happens on EVERY attempt - this does not. POST edges are
     * still tallied for the 'last' command. */
    while (gpio_get(arm_pin)) {
        bool p = gpio_get(POST_PIN);
        if (p != lastp) { edges++; lastp = p; hh->etot = edges; last_edge_t = get_absolute_time(); }
        led_tick();
        int64_t el = absolute_time_diff_us(t0, get_absolute_time());
        if (el >= (int64_t)BOOT_HOLD_MS * 1000) {
            booted = true;
            g_last_edges = edges;
            hh->e15 = edges;
            led_green_hold();              /* held out of reset = real boot: solid green */
            break;
        }
#if MP_FORCE_REBOOT
        /* SPEED (EXT_CLK only): a failed glitch hangs with POST frozen while the
         * console sits out of reset, then the SMC watchdog reboots it ~7s later.
         * Don't wait for the watchdog - once POST has been completely still for
         * FREEZE_MS while held high, it's a hang (a real boot keeps toggling POST
         * as XeLL loads), so pulse CPU_RESET ourselves to restart now. Gated to a
         * 3-7s window: before 3s avoids the normal CB POST burst, and past ~7s we
         * let it ride to the BOOT_HOLD confirm (a hang would already be gone), so
         * this can never cut off a genuinely-booting console. */
        if (method == METHOD_EXTCLK && el >= 3000 * 1000 && el < 7000 * 1000 &&
            absolute_time_diff_us(last_edge_t, get_absolute_time()) >= (int64_t)MP_FREEZE_MS * 1000) {
            gpio_init(CPU_RESET);
            gpio_set_dir(CPU_RESET, GPIO_OUT);
            gpio_put(CPU_RESET, 0);        /* assert console reset (open-drain low) */
            sleep_ms(40);
            gpio_set_dir(CPU_RESET, GPIO_IN);  /* release: float, console reboots */
            break;                          /* scored as a miss; sweep steps, next attempt */
        }
#endif
    }
    hh->ms = (uint32_t)(absolute_time_diff_us(t0, get_absolute_time()) / 1000);
    hh->done = 1;
    if (!booted) { g_last_edges = edges; hh->e15 = edges; led_red_for(1000); }

    if (sweep_active) {
        if (booted) {
            sweep_landed_val = sweep_val;
            g_as_anchor_landed = g_extclk_anchor;
            sweep_landed = true;
            sweep_active = false;
            g_autosweep = false;
            g_halt = true;      /* stay off the console now that it has booted */
        } else {
            int64_t nv = (int64_t)sweep_val + (int64_t)sweep_step;
            if ((sweep_step > 0 && nv > (int64_t)sweep_end) ||
                (sweep_step < 0 && nv < (int64_t)sweep_end)) {
                nv = (int64_t)sweep_start;        /* wrap around */
                if (g_autosweep) {                /* finished an r_end pass: step the anchor */
                    uint32_t a = g_extclk_anchor + 1;
                    if (a > g_as_anchor_hi) a = g_as_anchor_lo;
                    g_extclk_anchor = a;
                    g_as_anchor_changed = true;
                }
            }
            sweep_val = (uint32_t)nv;
            sweep_tick++;
        }
    }

    g_attempts++;
}

static void core1_main(void)
{
    /* Allow core0 to pause us while it writes flash. */
    multicore_lockout_victim_init();

    led_init();
    led_off();

    /* Safety: drive the slowdown line (PLL_BYPASS / EXT_CLK_EN) to its inactive
     * LOW state from the very start, so it is never left floating at the console
     * during the window before the first arm. The reset pin stays an input
     * until the console itself has raised reset and we arm (see run_glitch_once). */
    gpio_init(PLL_BYPASS);
    gpio_put(PLL_BYPASS, 0);
    gpio_set_dir(PLL_BYPASS, GPIO_OUT);
    gpio_init(CPU_RESET);
    gpio_set_dir(CPU_RESET, GPIO_IN);

    PIO pio = pio0;
    uint sm = 0;
    int loaded = -1;
    uint offset = 0;

    while (true) {
        /* (Re)load the engine for the active profile (0 = RGH1.2, 1 = EXT_CLK);
         * the two programs don't fit in one PIO together, so we swap on change. */
        uint m = g_cfg.profiles[g_cfg.active].method;
        int eng = (m == METHOD_EXTCLK) ? 1 : 0;
        if (eng != loaded) {
            if (loaded >= 0) {
                pio_sm_set_enabled(pio, sm, false);
                pio_remove_program(pio, eng_prog(loaded), offset);
            }
            offset = pio_add_program(pio, eng_prog(eng));
            loaded = eng;
        }
        if (g_halt) {
            /* A landed sweep already left the LED on solid green; just keep
             * it alive (lets a red/green timeout run its course, and still
             * lets core0 pause us for 'save'/'adcmon') without touching the
             * console at all until the user types 'resume' or starts a new
             * sweep/profile. */
            led_tick();
            continue;
        }
        run_glitch_once(pio, sm, offset, m);
    }
}

/* ---- Serial console (core0) --------------------------------------------- */

static void print_banner(void)
{
    printf("\n");
    printf("=== Matrix Pico - RGH 1.2 serial glitcher ===\n");
    printf("Based on PicoRGH (balika011) / RGH by GliGli & Tiros. GPLv2.\n");
    printf("Type 'help' for commands.\n");
}

static void print_profile(int i)
{
    uint32_t m = g_cfg.profiles[i].method;
    printf("[%d] %-8s (%s)%s\n", i, g_cfg.profiles[i].name,
           (m == METHOD_EXTCLK) ? "EXT_CLK" : "RGH1.2",
           (i == (int)g_cfg.active) ? "  <= active" : "");
    for (int k = 0; k < NUM_TIMINGS; k++)
        printf("      %d %-12s = %lu\n", k, tname(m, k),
               (unsigned long)g_cfg.profiles[i].t[k]);
}

static void print_status(void)
{
    printf("active profile: %s   attempts since boot: %lu\n",
           g_cfg.profiles[g_cfg.active].name, (unsigned long)g_attempts);
    print_profile(g_cfg.active);
}

static void print_list(void)
{
    for (int i = 0; i < NUM_PROFILES; i++)
        printf("[%d] %-8s%s\n", i, g_cfg.profiles[i].name,
               (i == (int)g_cfg.active) ? "  <= active" : "");
}

static void print_help(void)
{
    printf(
        "commands:\n"
        "  help                 this text\n"
        "  status               active profile + its 5 timing values\n"
        "  list                 list all profiles\n"
        "  show <name|idx>      print one profile's values\n"
        "  use <name|idx>       make a profile active\n"
        "  set <idx> <value>    set timing <idx> (0-4) on active profile\n"
        "  name <newname>       rename the active profile (max 15 chars)\n"
        "  save                 write all profiles to flash (persists)\n"
        "  load                 reload profiles from flash (discard edits)\n"
        "  defaults             restore built-in defaults (not saved yet)\n"
        "  hist                 last 16 attempts: POST edges + how long reset stayed high\n"
        "  last                 POST edges counted after the last glitch\n"
        "  bootedges [n]        POST edges needed to count as a real boot (default 3)\n"
        "  attempts             reset the attempt counter\n"
        "  mon                  watch CPU_RESET/POST live for 6s (power-cycle to test)\n"
        "  adcmon [secs]        ADC-capture POST(28)/RESET_SENSE(29), default 8s\n"
        "                       (sees sub-2V POST highs digital 'mon' may miss; one\n"
        "                       power-cycle per run, keeps last 8 in a history table)\n"
        "  adchist              reprint the adcmon history without a new capture\n"
        "  sweep <i> <a> <b> [s] auto-step timing i from a to b until it boots\n"
        "                       (stops attempting once it lands - console stays\n"
        "                       booted; 'resume' or a new sweep/use starts again)\n"
        "  resume               clear a landed-sweep halt and resume attempts\n"
        "  sweepstop            cancel an active sweep\n"
        "timing index: 0 post_count  1 delay_to_pll  2 delay_to_rst\n"
        "              3 glitch_width 4 pll_hold\n");
}

static int find_profile(const char *s)
{
    if (isdigit((unsigned char)s[0]) && s[1] == '\0') {
        int i = s[0] - '0';
        if (i >= 0 && i < NUM_PROFILES) return i;
        return -1;
    }
    for (int i = 0; i < NUM_PROFILES; i++)
        if (strncmp(s, g_cfg.profiles[i].name, PROFILE_NAME_LEN) == 0)
            return i;
    return -1;
}

/* Dense EXT_CLK auto-tune. The signature-check glitch window is only ~3-5 ns
 * wide (logic-analyzer RE of a real Matrix), so the old 8-unit (~42 ns) r_end
 * step skated over it almost every time. This sweeps r_end in 1-unit (~5 ns at
 * 200 MHz) steps across Octal450's known-good Zephyr window (117998-118010 in
 * 192 MHz units) while hedging the POST anchor +/-1 pulse around post_rgh=24
 * (= 12 pulses). One attempt runs per console reboot, so leaving it running
 * revisits every (r_end, anchor) point many times and clock jitter lands it. */
static void autosweep_setup(void)
{
    g_cfg.profiles[g_cfg.active].t[3] = 2;          /* reset pulse = 2 units (~10 ns) = R_LEN=2 */
    sweep_idx   = 2;                                /* inner: r_end (Matrix 192 MHz units) */
    sweep_start = 117968; sweep_end = 118048; sweep_step = 1;   /* dense 5 ns steps, +/-200ns */
    sweep_val   = sweep_start;
    /* MEASURED: the capture put the signature check right after POST edge #26 =
     * 13 pulses, identically on two consecutive boots. Pin the anchor there. */
    g_as_anchor_lo = 13; g_as_anchor_hi = 13;
    g_extclk_anchor = 13;
    g_as_anchor_changed = true;
    sweep_landed = false;
    g_autosweep = true;
    sweep_active = true;
    g_halt = false;
}

static void handle_line(char *line)
{
    char *cmd = strtok(line, " \t");
    if (!cmd) return;

    if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
        print_help();
    } else if (!strcmp(cmd, "status") || !strcmp(cmd, "p")) {
        print_status();
    } else if (!strcmp(cmd, "list")) {
        print_list();
    } else if (!strcmp(cmd, "show")) {
        char *a = strtok(NULL, " \t");
        int i = a ? find_profile(a) : (int)g_cfg.active;
        if (i < 0) { printf("err: no such profile\n"); return; }
        print_profile(i);
    } else if (!strcmp(cmd, "use")) {
        char *a = strtok(NULL, " \t");
        int i = a ? find_profile(a) : -1;
        if (i < 0) { printf("err: use <name|idx>\n"); return; }
        g_cfg.active = (uint32_t)i;
        g_halt = false;
        printf("active -> %s (unsaved; 'save' to persist)\n", g_cfg.profiles[i].name);
    } else if (!strcmp(cmd, "set")) {
        char *a = strtok(NULL, " \t");
        char *b = strtok(NULL, " \t");
        if (!a || !b) { printf("err: set <idx 0-4> <value>\n"); return; }
        int idx = atoi(a);
        if (idx < 0 || idx >= NUM_TIMINGS) { printf("err: idx 0-4\n"); return; }
        uint32_t v = (uint32_t)strtoul(b, NULL, 0);
        g_cfg.profiles[g_cfg.active].t[idx] = v;
        printf("%s.%s = %lu (unsaved)\n", g_cfg.profiles[g_cfg.active].name,
               tname(g_cfg.profiles[g_cfg.active].method, idx), (unsigned long)v);
    } else if (!strcmp(cmd, "name")) {
        char *a = strtok(NULL, " \t");
        if (!a) { printf("err: name <newname>\n"); return; }
        strncpy(g_cfg.profiles[g_cfg.active].name, a, PROFILE_NAME_LEN - 1);
        g_cfg.profiles[g_cfg.active].name[PROFILE_NAME_LEN - 1] = '\0';
        printf("renamed -> %s (unsaved)\n", g_cfg.profiles[g_cfg.active].name);
    } else if (!strcmp(cmd, "save")) {
        config_save();
        printf("saved to flash.\n");
    } else if (!strcmp(cmd, "load")) {
        config_load();
        printf("reloaded from flash.\n");
        print_status();
    } else if (!strcmp(cmd, "defaults")) {
        memcpy(&g_cfg, &DEFAULT_CONFIG, sizeof(config_t));
        printf("defaults restored (unsaved; 'save' to persist).\n");
    } else if (!strcmp(cmd, "hist")) {
        uint32_t n = g_hist_n, from = n > 16 ? n - 16 : 0;
        printf("attempt  POST-edges@4s    total-edges  reset-high-time\n");
        for (uint32_t k = from; k < n; k++) {
            hist_t *h = &g_hist[k % 16];
            uint32_t ms = h->done ? h->ms
                : (uint32_t)(absolute_time_diff_us(h->t0, get_absolute_time()) / 1000);
            printf("#%-7lu %-16lu %-12lu %lu ms%s\n", (unsigned long)(k + 1),
                   (unsigned long)h->e15, (unsigned long)h->etot, (unsigned long)ms,
                   h->done ? "" : "  (still high = running/booted)");
        }
        if (n == 0) printf("no attempts yet\n");
    } else if (!strcmp(cmd, "last")) {
        printf("POST edges seen after the last glitch: %lu  (boot needs >= %lu)\n",
               (unsigned long)g_last_edges, (unsigned long)g_boot_edges);
    } else if (!strcmp(cmd, "bootedges")) {
        char *a = strtok(NULL, " \t");
        if (a) g_boot_edges = (uint32_t)strtoul(a, NULL, 0);
        printf("bootedges = %lu\n", (unsigned long)g_boot_edges);
    } else if (!strcmp(cmd, "attempts")) {
        g_attempts = 0;
        printf("attempt counter reset.\n");
    } else if (!strcmp(cmd, "rsttest")) {
        /* Does our reset output actually reset the console? GP26 is the 1.1V
         * open-drain reset. Drive it LOW (assert reset) for 20s; a working wire
         * holds the console in reset (GP29 stays LOW, POST stays dead). If the
         * console boots normally anyway, the reset point/wire is ineffective and
         * no glitch timing can ever work. Default level 0 = drive LOW. */
        char *a = strtok(NULL, " \t");
        uint32_t lvl = a ? (uint32_t)strtoul(a, NULL, 0) : 0;
        multicore_lockout_start_blocking();
        gpio_init(RESET_SENSE); gpio_set_dir(RESET_SENSE, GPIO_IN);
        if (lvl) { gpio_init(CPU_RESET); gpio_set_dir(CPU_RESET, GPIO_IN); gpio_disable_pulls(CPU_RESET); }  /* release = float */
        else     { gpio_init(CPU_RESET); gpio_set_dir(CPU_RESET, GPIO_OUT); gpio_put(CPU_RESET, 0); }         /* drive LOW = assert reset */
        printf("(engine paused) CPU_RESET(26) %s for 20s - power-cycle the console now...\n",
               lvl ? "RELEASED (float)" : "driven LOW (reset asserted)");
        bool sh=false, sl=false, ph=false, pl=false;
        uint32_t s_hi_max=0, pedges=0;
        bool last_s=gpio_get(RESET_SENSE), last_p=gpio_get(POST_PIN);
        absolute_time_t t0=get_absolute_time(), seg=t0;
        while (absolute_time_diff_us(t0, get_absolute_time()) < 20000000) {
            bool sv=gpio_get(RESET_SENSE); if(sv)sh=true; else sl=true;
            if (sv!=last_s){ if(last_s){uint32_t d=(uint32_t)(absolute_time_diff_us(seg,get_absolute_time())/1000); if(d>s_hi_max)s_hi_max=d;} seg=get_absolute_time(); last_s=sv; }
            bool pv=gpio_get(POST_PIN); if(pv)ph=true; else pl=true;
            if (pv!=last_p){ pedges++; last_p=pv; }
            sleep_us(200);
        }
        if (last_s){uint32_t d=(uint32_t)(absolute_time_diff_us(seg,get_absolute_time())/1000); if(d>s_hi_max)s_hi_max=d;}
        gpio_init(CPU_RESET); gpio_set_dir(CPU_RESET, GPIO_IN); gpio_disable_pulls(CPU_RESET);
        multicore_lockout_end_blocking();
        printf("with CPU_RESET %s:\n", lvl ? "released" : "driven LOW");
        printf("  RST_SENSE(29): %s   longest HIGH: %lu ms\n",
               (sh&&sl)?"toggled":(sh?"always HIGH":"always LOW"), (unsigned long)s_hi_max);
        printf("  POST(28): %s   edges: %lu\n",
               (ph&&pl)?"toggled":(ph?"always HIGH":"always LOW"), (unsigned long)pedges);
        printf("  (driving LOW should hold console in reset: GP29 LOW + POST dead. If it boots anyway = reset wire ineffective)\n");
    } else if (!strcmp(cmd, "extctest")) {
        /* Does EXT_CLK_EN actually slow the CPU? Drive GP27 to <level> and watch
         * how the console behaves vs the ~7s baseline from 'mon'. If EXT_CLK_EN
         * works, slowing the CPU changes GP29's hold time and POST's rate a lot;
         * if GP29 still holds ~7s and POST looks normal, the EXT_CLK_EN point or
         * polarity is wrong and no r_end can ever land. */
        char *a = strtok(NULL, " \t");
        uint32_t lvl = a ? (uint32_t)strtoul(a, NULL, 0) : 1;
        multicore_lockout_start_blocking();   /* STOP core1 so it can't fight us for GP27 */
        gpio_init(PLL_BYPASS); gpio_set_dir(PLL_BYPASS, GPIO_OUT); gpio_put(PLL_BYPASS, lvl ? 1 : 0);
        gpio_init(RESET_SENSE); gpio_set_dir(RESET_SENSE, GPIO_IN);
        printf("(engine paused; GP27 held by USB side only)\n");
        printf("EXT_CLK_EN(27) held %s for 20s - power-cycle the console now...\n", lvl ? "HIGH" : "LOW");
        bool sh=false, sl=false, ph=false, pl=false;
        uint32_t s_hi_max=0, pedges=0;
        bool last_s = gpio_get(RESET_SENSE), last_p = gpio_get(POST_PIN);
        absolute_time_t t0=get_absolute_time(), seg=t0;
        while (absolute_time_diff_us(t0, get_absolute_time()) < 20000000) {
            bool sv=gpio_get(RESET_SENSE); if(sv)sh=true; else sl=true;
            if (sv!=last_s){ if(last_s){uint32_t d=(uint32_t)(absolute_time_diff_us(seg,get_absolute_time())/1000); if(d>s_hi_max)s_hi_max=d;} seg=get_absolute_time(); last_s=sv; }
            bool pv=gpio_get(POST_PIN); if(pv)ph=true; else pl=true;
            if (pv!=last_p){ pedges++; last_p=pv; }
            sleep_us(200);
        }
        if (last_s){uint32_t d=(uint32_t)(absolute_time_diff_us(seg,get_absolute_time())/1000); if(d>s_hi_max)s_hi_max=d;}
        gpio_put(PLL_BYPASS, 0);
        multicore_lockout_end_blocking();     /* resume core1 */
        printf("with EXT_CLK_EN held %s:\n", lvl ? "HIGH" : "LOW");
        printf("  RST_SENSE(29): %s   longest HIGH: %lu ms\n",
               (sh&&sl)?"toggled":(sh?"always HIGH":"always LOW"), (unsigned long)s_hi_max);
        printf("  POST(28): %s   edges: %lu\n",
               (ph&&pl)?"toggled":(ph?"always HIGH":"always LOW"), (unsigned long)pedges);
        printf("  (compare to 'mon' baseline ~7000ms. BIG change = EXT_CLK_EN works; same = wrong point/polarity)\n");
    } else if (!strcmp(cmd, "captureslow")) {
        /* Measure r_end directly: arm, count N POST pulses (full speed) to the
         * check anchor, assert EXT_CLK_EN to slow the CPU, then timestamp POST
         * edges. The CPU creeps to the (now slowed) signature check and hangs; the
         * delay from EXT_CLK_EN assert to the last POST edge before that hang IS
         * r_end in real time -> STBY periods = that delay x 48. Engine stays off;
         * this just slows + watches, no glitch pulse. */
        #define CAPS_MAX 2500u
        static uint32_t caps_t[CAPS_MAX]; static uint8_t caps_c[CAPS_MAX];
        char *a = strtok(NULL, " \t");
        uint32_t npulse = a ? (uint32_t)strtoul(a,NULL,0) : 10;
        multicore_lockout_start_blocking();
        gpio_init(POST_PIN);    gpio_set_dir(POST_PIN, GPIO_IN);    gpio_pull_up(POST_PIN);
        gpio_init(RESET_SENSE); gpio_set_dir(RESET_SENSE, GPIO_IN);
        gpio_init(PLL_BYPASS);  gpio_set_dir(PLL_BYPASS, GPIO_OUT); gpio_put(PLL_BYPASS, 0);  /* EXT_CLK_EN low */
        printf("CAPTURESLOW: arm on reset, count %lu POST pulses, assert EXT_CLK_EN, watch 15s.\n", (unsigned long)npulse);
        printf("Power-cycle the console now...\n");
        /* arm: wait for GP29 LOW (reset), then catch POST from the first edge */
        while (gpio_get(RESET_SENSE)) { if (getchar_timeout_us(0) != PICO_ERROR_TIMEOUT) { } tight_loop_contents(); }
        /* count npulse POST pulses (rising+falling) at full speed */
        for (uint32_t k=0;k<npulse;k++){
            while(!gpio_get(POST_PIN)) tight_loop_contents();
            while(gpio_get(POST_PIN)) tight_loop_contents();
        }
        gpio_put(PLL_BYPASS, 1);                      /* EXT_CLK_EN HIGH: slow the CPU NOW */
        uint64_t t0=time_us_64(), tend=t0+15000000ull; /* watch 15s: slowed edges are 100s of ms apart */
        uint32_t n=0; bool lp=gpio_get(POST_PIN), ls=gpio_get(RESET_SENSE);
        while (time_us_64() < tend && n < CAPS_MAX) {
            uint32_t now=(uint32_t)(time_us_64()-t0);
            bool p=gpio_get(POST_PIN);
            if (p!=lp){ caps_t[n]=now; caps_c[n]=(p?1u:0u); n++; lp=p; if(n>=CAPS_MAX)break; }
            bool sg=gpio_get(RESET_SENSE);
            if (sg!=ls){ caps_t[n]=now; caps_c[n]=2u|(sg?1u:0u); ls=sg; n++; }
        }
        gpio_put(PLL_BYPASS, 0);                      /* release EXT_CLK_EN */
        multicore_lockout_end_blocking();
        /* last POST edge before the longest gap = the slowed check */
        uint32_t pedges=0, prev=0, gap=0, gap_end_t=0, last_post_before_gap=0; bool hp=false;
        for (uint32_t i=0;i<n;i++) if((caps_c[i]&2u)==0){ pedges++;
            if(hp){ uint32_t g=caps_t[i]-prev; if(g>gap){gap=g; last_post_before_gap=prev;} }
            prev=caps_t[i]; hp=true; }
        printf("edges after slowdown: %lu (%lu POST)\n",(unsigned long)n,(unsigned long)pedges);
        printf("last POST edge before the slowed check: t=%lu us after EXT_CLK_EN\n",(unsigned long)last_post_before_gap);
        printf("  -> r_end estimate ~ %lu STBY periods (t_us x 48)\n",(unsigned long)((uint64_t)last_post_before_gap*48ull));
        printf("raw (t_us : sig):\n");
        for (uint32_t i=0;i<n;i++){ const char *nm=(caps_c[i]&2u)?"RST29":"POST "; printf("  %lu %s %s\n",(unsigned long)caps_t[i],nm,(caps_c[i]&1u)?"H":"L"); }
    } else if (!strcmp(cmd, "capture")) {
        /* Pico-as-logic-analyzer: timestamp every POST(28) and RST_SENSE(29) edge
         * during a boot, engine paused. On a FAILED boot POST advances to the
         * signature check then stalls, so the last POST edge before the longest
         * gap = the check, and the edge count to it = the real anchor. Prints the
         * raw edge list + a summary (total edges, the stall, POST edges before it). */
        #define CAP_MAX 3000u
        static uint32_t cap_t[CAP_MAX]; static uint8_t cap_c[CAP_MAX];
        multicore_lockout_start_blocking();
        gpio_init(POST_PIN);    gpio_set_dir(POST_PIN, GPIO_IN);    gpio_pull_up(POST_PIN);
        gpio_init(RESET_SENSE); gpio_set_dir(RESET_SENSE, GPIO_IN);
        printf("CAPTURE: recording POST/RST_SENSE edges for 12s - power-cycle the console now...\n");
        uint32_t n=0; bool lp=gpio_get(POST_PIN), ls=gpio_get(RESET_SENSE);
        uint64_t t0=time_us_64(), tend=t0+12000000ull;
        while (time_us_64() < tend && n < CAP_MAX) {
            uint32_t now=(uint32_t)(time_us_64()-t0);
            bool p=gpio_get(POST_PIN);
            if (p!=lp){ cap_t[n]=now; cap_c[n]=(p?1u:0u); n++; lp=p; if(n>=CAP_MAX)break; }
            bool sg=gpio_get(RESET_SENSE);
            if (sg!=ls){ cap_t[n]=now; cap_c[n]=2u|(sg?1u:0u); ls=sg; n++; }
        }
        multicore_lockout_end_blocking();
        /* summary: POST edge count + longest gap between consecutive POST edges */
        uint32_t pedges=0, prev_post_t=0; bool have_prev=false;
        uint32_t gap_max=0, gap_at_idx=0, gap_at_t=0, post_before_gap=0, pc=0;
        for (uint32_t i=0;i<n;i++){
            if ((cap_c[i]&2u)==0){ /* POST edge */
                pc++;
                if (have_prev){ uint32_t g=cap_t[i]-prev_post_t; if(g>gap_max){gap_max=g; gap_at_idx=i; gap_at_t=prev_post_t; post_before_gap=pc-1;} }
                prev_post_t=cap_t[i]; have_prev=true; pedges++;
            }
        }
        printf("edges: %lu total, %lu POST\n", (unsigned long)n, (unsigned long)pedges);
        printf("longest POST gap: %lu.%03lu ms, starting after POST edge #%lu at t=%lu.%03lu ms\n",
               (unsigned long)(gap_max/1000),(unsigned long)(gap_max%1000),(unsigned long)post_before_gap,
               (unsigned long)(gap_at_t/1000),(unsigned long)(gap_at_t%1000));
        printf("  -> that POST edge count (%lu) is the check anchor; in pulses ~ %lu\n",
               (unsigned long)post_before_gap, (unsigned long)(post_before_gap/2));
        printf("raw edges (t_ms : signal):\n");
        for (uint32_t i=0;i<n;i++){
            const char *nm = (cap_c[i]&2u)?"RST29":"POST ";
            printf("  %lu.%03lu %s %s\n",(unsigned long)(cap_t[i]/1000),(unsigned long)(cap_t[i]%1000),nm,(cap_c[i]&1u)?"H":"L");
        }
    } else if (!strcmp(cmd, "matrixspy")) {
        /* GROUND TRUTH: passively watch a REAL Matrix glitch this console. The Pico
         * drives nothing - it monitors EXT_CLK_EN (GP27, 3.3V, clean), POST (GP28,
         * via the diode), and RESET_SENSE (GP29) and timestamps every edge for 15s.
         * The first EXT_CLK_EN rising edge tells us exactly WHEN the Matrix slows the
         * CPU relative to POST - the one number our FSM analysis couldn't resolve -
         * and the whole timeline shows the slowdown's position and width. The 1.1V
         * reset pulse itself can't be read without a diode, but EXT_CLK is enough.
         * Install the Matrix, tap GP27/28/29 to the same points, power the console. */
        #define SPY_MAX 4000u
        static uint32_t spy_t[SPY_MAX]; static uint8_t spy_c[SPY_MAX];
        multicore_lockout_start_blocking();                 /* make sure core1 drives nothing */
        gpio_init(PLL_BYPASS);  gpio_set_dir(PLL_BYPASS, GPIO_IN);  gpio_disable_pulls(PLL_BYPASS);
        gpio_init(POST_PIN);    gpio_set_dir(POST_PIN, GPIO_IN);    gpio_pull_up(POST_PIN);
        gpio_init(RESET_SENSE); gpio_set_dir(RESET_SENSE, GPIO_IN);
        printf("MATRIXSPY: watching a real Matrix for 15s (EXT_CLK/POST/RST_SENSE).\n");
        printf("Make sure the MATRIX is installed and driving the glitch; power-cycle the console now...\n");
        uint32_t n=0;
        bool le=gpio_get(PLL_BYPASS), lp=gpio_get(POST_PIN), ls=gpio_get(RESET_SENSE);
        uint64_t t0=time_us_64(), tend=t0+15000000ull;
        while (time_us_64() < tend && n < SPY_MAX) {
            uint32_t now=(uint32_t)(time_us_64()-t0);
            bool e=gpio_get(PLL_BYPASS);
            if (e!=le){ spy_t[n]=now; spy_c[n]=4u|(e?1u:0u); le=e; n++; if(n>=SPY_MAX)break; }
            bool p=gpio_get(POST_PIN);
            if (p!=lp){ spy_t[n]=now; spy_c[n]=(p?1u:0u); lp=p; n++; if(n>=SPY_MAX)break; }
            bool sg=gpio_get(RESET_SENSE);
            if (sg!=ls){ spy_t[n]=now; spy_c[n]=2u|(sg?1u:0u); ls=sg; n++; }
        }
        multicore_lockout_end_blocking();
        /* key number: POST edge count before the FIRST EXT_CLK_EN assert (rising) */
        uint32_t pc=0, extc_first_t=0, extc_first_post=0; bool found=false;
        for (uint32_t i=0;i<n;i++){
            if ((spy_c[i]&6u)==0) pc++;                      /* POST edge */
            else if ((spy_c[i]&4u) && (spy_c[i]&1u) && !found){ /* EXT_CLK rising */
                found=true; extc_first_t=spy_t[i]; extc_first_post=pc;
            }
        }
        printf("edges: %lu total\n", (unsigned long)n);
        if (found)
            printf("  -> Matrix asserted EXT_CLK_EN after POST edge #%lu (= %lu pulses), at t=%lu.%03lu ms\n",
                   (unsigned long)extc_first_post,(unsigned long)(extc_first_post/2),
                   (unsigned long)(extc_first_t/1000),(unsigned long)(extc_first_t%1000));
        else
            printf("  -> no EXT_CLK_EN edge seen (Matrix not driving it, not installed, or wrong tap)\n");
        printf("raw edges (t_ms : signal):\n");
        for (uint32_t i=0;i<n;i++){
            const char *nm = (spy_c[i]&4u)?"EXTCLK":((spy_c[i]&2u)?"RST29 ":"POST  ");
            printf("  %lu.%03lu %s %s\n",(unsigned long)(spy_t[i]/1000),(unsigned long)(spy_t[i]%1000),nm,(spy_c[i]&1u)?"H":"L");
        }
    } else if (!strcmp(cmd, "mon")) {
        printf("monitoring GP26/GP28/GP29 for 12s - power-cycle the console now...\n");
        gpio_init(RESET_SENSE); gpio_set_dir(RESET_SENSE, GPIO_IN);
        bool rh=false, rl=false, ph=false, pl=false, sh=false, sl=false;
        uint32_t s_hi_max=0, s_hi_cur=0;   /* longest continuous GP29-high stretch (ms) */
        bool last_s = gpio_get(RESET_SENSE);
        absolute_time_t t0 = get_absolute_time(), seg = t0;
        while (absolute_time_diff_us(t0, get_absolute_time()) < 12000000) {
            if (gpio_get(CPU_RESET)) rh = true; else rl = true;
            if (gpio_get(POST_PIN))  ph = true; else pl = true;
            bool s = gpio_get(RESET_SENSE);
            if (s) sh = true; else sl = true;
            if (s != last_s) {
                if (last_s) { uint32_t d=(uint32_t)(absolute_time_diff_us(seg,get_absolute_time())/1000); if(d>s_hi_max)s_hi_max=d; }
                seg = get_absolute_time(); last_s = s;
            }
            sleep_ms(1);
        }
        if (last_s) { uint32_t d=(uint32_t)(absolute_time_diff_us(seg,get_absolute_time())/1000); if(d>s_hi_max)s_hi_max=d; }
        printf("CPU_RESET(26): %s\n",
               (rh && rl) ? "TOGGLED  (high & low - wire good)" :
               (rh ? "always HIGH" : "always LOW  (not seeing the console)"));
        printf("POST(28):      %s\n",
               (ph && pl) ? "TOGGLED  (high & low - wire good)" :
               (ph ? "always HIGH (stuck)" : "always LOW  (no POST - check wire/diode)"));
        printf("RST_SENSE(29): %s   longest HIGH stretch: %lu ms\n",
               (sh && sl) ? "TOGGLED  (high & low - wire good)" :
               (sh ? "always HIGH (never drops - judge can't see resets!)" : "always LOW  (dead - judge can never confirm a boot!)"),
               (unsigned long)s_hi_max);
        printf("  (for boot detect GP29 must TOGGLE, and a real boot should show a HIGH stretch of several thousand ms)\n");
    } else if (!strcmp(cmd, "adcmon")) {
        char *a = strtok(NULL, " \t");
        uint32_t secs = a ? (uint32_t)strtoul(a, NULL, 0) : ADC_DEFAULT_SECS;
        if (secs == 0) secs = ADC_DEFAULT_SECS;
        cmd_adcmon(secs);
    } else if (!strcmp(cmd, "adchist")) {
        print_adc_hist();
    } else if (!strcmp(cmd, "sweep")) {
        char *a = strtok(NULL, " \t");
        char *b = strtok(NULL, " \t");
        char *c = strtok(NULL, " \t");
        char *d = strtok(NULL, " \t");
        if (!a || !b || !c) {
            printf("err: sweep <idx 0-4> <start> <end> [step]\n");
            return;
        }
        int idx = atoi(a);
        if (idx < 0 || idx >= NUM_TIMINGS) { printf("err: idx 0-4\n"); return; }
        uint32_t s = (uint32_t)strtoul(b, NULL, 0);
        uint32_t e = (uint32_t)strtoul(c, NULL, 0);
        int32_t st = d ? (int32_t)strtol(d, NULL, 0) : (e >= s ? 1 : -1);
        if (st == 0) { printf("err: step cannot be 0\n"); return; }
        sweep_idx = idx;
        sweep_start = s;
        sweep_end = e;
        sweep_step = st;
        sweep_val = s;
        sweep_tick = 0;
        sweep_landed = false;
        sweep_active = true;
        g_halt = false;
        printf("sweeping %s from %lu to %lu step %ld\n", tname(g_cfg.profiles[g_cfg.active].method, idx),
               (unsigned long)s, (unsigned long)e, (long)st);
        printf("power-cycle the console; it steps each miss until it boots.\n");
    } else if (!strcmp(cmd, "autosweep")) {
        /* Dense fine-phase search over Octal450's known-good Zephyr window. */
        autosweep_setup();
        printf("AUTOSWEEP (dense): anchor %lu-%lu x r_end %lu-%lu step %ld (~5ns), pulse=%lu\n",
               (unsigned long)g_as_anchor_lo, (unsigned long)g_as_anchor_hi,
               (unsigned long)sweep_start, (unsigned long)sweep_end, (long)sweep_step,
               (unsigned long)g_cfg.profiles[g_cfg.active].t[3]);
        printf("power-cycle the console and leave it running; one real boot stops it.\n");
    } else if (!strcmp(cmd, "resume")) {
        if (g_halt) {
            g_halt = false;
            printf("resuming glitch attempts.\n");
        } else {
            printf("not halted.\n");
        }
    } else if (!strcmp(cmd, "sweepstop")) {
        sweep_active = false;
        printf("sweep stopped at %s=%lu ('save' to keep).\n",
               tname(g_cfg.profiles[g_cfg.active].method, sweep_idx), (unsigned long)sweep_val);
    } else {
        printf("err: unknown command '%s' (try 'help')\n", cmd);
    }
}

static void console_task(void)
{
    static char buf[64];
    static int len = 0;

    int ch = getchar_timeout_us(0);
    while (ch != PICO_ERROR_TIMEOUT) {
        if (ch == '\r' || ch == '\n') {
            if (len > 0) {
                buf[len] = '\0';
                handle_line(buf);
                len = 0;
            }
            printf("> ");
        } else if (ch == 8 || ch == 127) {   /* backspace */
            if (len > 0) len--;
        } else if (len < (int)sizeof(buf) - 1) {
            buf[len++] = (char)ch;
        }
        ch = getchar_timeout_us(0);
    }
}

int main(void)
{
    /* Overclock for glitch-pulse resolution (~5.2 ns/cycle at 192 MHz).
     * 266 MHz is unstable with USB on some (clone) RP2040-Zero boards. 192 MHz
     * is a multiple of 12/48 MHz (recommended by the RGH1.3 Pico work) and
     * lines up 1:1 with Octal450's 192 MHz EXT_CLK counts. */
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(5);
    set_sys_clock_khz(MP_SYS_CLK_KHZ, true);

    uint32_t freq = clock_get_hz(clk_sys);
    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                    freq, freq);

    stdio_init_all();      /* USB CDC serial console */

    /* POST (GP28) is only ever an INPUT - never driven. The pad keeps the RP2040's
     * default weak pull-down, same as the original PicoRGH, so an unconnected wire
     * reads low. It can only sink microamps from the POST net. (Whether a low-voltage
     * POST net reads as a logic high is a separate question - see docs/SAFETY.md.) */
    gpio_set_input_enabled(POST_PIN, true);
    config_load();
    /* DIAG: option bit2 = POST pull-down (reset default of the RP2040 pad). */
    gpio_set_pulls(POST_PIN, false, true);   /* chip default (pull-down), as in the proven build */

#if MP_AUTORUN
    /* No-screen bring-up: powered from the console there is no serial to type
     * 'autosweep', so start the dense EXT_CLK search automatically. Force the
     * zephyr (EXT_CLK) profile and reseed its fixed timings BEFORE launching
     * core1, so core1 never spends its first cycles running the saved profile
     * (which could be an RGH1.2 one that drives the reset line) on a Zephyr. */
    bool autorun = (MP_EXTCLK_PROFILE < NUM_PROFILES &&
                    g_cfg.profiles[MP_EXTCLK_PROFILE].method == METHOD_EXTCLK);
    if (autorun) {
        g_cfg.active = MP_EXTCLK_PROFILE;
        uint32_t *zt = g_cfg.profiles[g_cfg.active].t;
        /* Faithful-FSM seed: [0] post_count anchor, [1] dwell in STBY periods
         * (~382ms = full EXT_CLK delay_val, pins EXT_CLK_EN inside the pre-check
         * gap), [2] r_end (192MHz units), [3] pulse, [4] extc_hold. The spy pins
         * dwell + anchor exactly from the real Matrix; the sweep refines r_end. */
        zt[0] = 12; zt[1] = 18336000u; zt[2] = 118008; zt[3] = 2; zt[4] = 50000;
        autosweep_setup();
    }
#endif

    multicore_launch_core1(core1_main);

    sleep_ms(500);
    print_banner();
    print_status();

#if MP_AUTORUN
    if (autorun) {
        printf("\n[AUTORUN] dense EXT_CLK autosweep running: r_end %lu-%lu step 1 (~5ns),\n"
               "          anchor %lu-%lu pulses, pulse=2. LED: blue=trying, green=BOOT, red=miss.\n",
               (unsigned long)sweep_start, (unsigned long)sweep_end,
               (unsigned long)g_as_anchor_lo, (unsigned long)g_as_anchor_hi);
    }
#endif
    printf("> ");

    uint32_t last_tick = 0;
    while (true) {
        console_task();
        if (sweep_active && sweep_tick != last_tick) {
            last_tick = sweep_tick;
            if (g_autosweep && g_as_anchor_changed) {
                g_as_anchor_changed = false;
                printf("  == anchor -> %lu pulses ==\n", (unsigned long)g_extclk_anchor);
            }
            printf("  sweep: %s = %lu\n", tname(g_cfg.profiles[g_cfg.active].method, sweep_idx),
                   (unsigned long)sweep_val);
        }
        if (sweep_landed) {
            sweep_landed = false;
            /* Persist the winning timing to flash so a no-screen (console-powered)
             * boot isn't lost on power-cycle. t[2] already holds the landed r_end
             * (run_glitch_once wrote sweep_val before the attempt); stamp the anchor
             * into t[0] and the lock marker into t[1], then save. */
            g_cfg.profiles[g_cfg.active].t[0] = g_as_anchor_landed;
            g_cfg.profiles[g_cfg.active].t[2] = sweep_landed_val;
            config_save();
            printf("\n*** GLITCH LANDED + SAVED: r_end = %lu  anchor = %lu pulses ***\n"
                   "    (persisted to flash; read it back from the banner next USB boot)\n> ",
                   (unsigned long)sweep_landed_val, (unsigned long)g_as_anchor_landed);
        }
        sleep_ms(2);
    }
    return 0;
}
