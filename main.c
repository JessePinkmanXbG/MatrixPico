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
        { "zephyr",  METHOD_EXTCLK, { 12, 73344000, 118008, 2, 50000 } },     /* dial in */
        /* Xenon: same EXT_CLK engine; Octal450's VHDL differs from Zephyr only in
         * R_END (118002 vs 118008 @192MHz). Untested - seed, dial in with sweep. */
        { "xenon",   METHOD_EXTCLK, { 12, 73344000, 118002, 2, 50000 } },     /* dial in */
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
static volatile uint32_t g_boot_edges = 3;   /* edges needed to call it a real boot */

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

#define ADC_EDGE_MAX 80
static uint32_t g_e_us[2][ADC_EDGE_MAX];   /* [0]=POST, [1]=reset-sense */
static uint8_t  g_e_hi[2][ADC_EDGE_MAX];
static uint32_t g_e_n[2];
static uint16_t g_raw_min[2], g_raw_max[2];

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
    dma_channel_start(dma_chan);     /* DMA first, so no early sample is lost and parity holds */
    adc_run(true);
    g_e_n[0] = g_e_n[1] = 0;
    g_raw_min[0] = g_raw_min[1] = 0xFFFF; g_raw_max[0] = g_raw_max[1] = 0;
    bool hs[2] = {false, false}, hinit[2] = {false, false};

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
            {   /* full edge list with hysteresis + min/max, for repeatability analysis */
                uint32_t c = is_post ? 0 : 1;
                uint32_t hi = is_post ? ADC_POST_THRESH : ADC_RST_THRESH;
                uint32_t lo = is_post ? 400 : 1400;
                if (raw < g_raw_min[c]) g_raw_min[c] = raw;
                if (raw > g_raw_max[c]) g_raw_max[c] = raw;
                bool ev = false, evhi = false;
                if (!hinit[c]) { hinit[c] = true; hs[c] = raw > hi; }
                else if (!hs[c] && raw > hi) { hs[c] = true;  ev = true; evhi = true; }
                else if (hs[c] && raw < lo)  { hs[c] = false; ev = true; evhi = false; }
                if (ev && g_e_n[c] < ADC_EDGE_MAX) { g_e_us[c][g_e_n[c]] = tus; g_e_hi[c][g_e_n[c]] = evhi; g_e_n[c]++; }
            }
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

    /* levels actually seen (3.3V full scale = 4095) */
    printf("levels: POST min %lu max %lu mV | RST(29) min %lu max %lu mV\n",
           (unsigned long)(g_raw_min[0] * 3300u / 4095u), (unsigned long)(g_raw_max[0] * 3300u / 4095u),
           (unsigned long)(g_raw_min[1] * 3300u / 4095u), (unsigned long)(g_raw_max[1] * 3300u / 4095u));
    const char *nm[2] = {"POST(28)", "RST(29) "};
    for (int c = 0; c < 2; c++) {
        printf("%s edges (%lu): ", nm[c], (unsigned long)g_e_n[c]);
        for (uint32_t k = 0; k < g_e_n[c]; k++)
            printf("%c%lu.%03lu ", g_e_hi[c][k] ? 'H' : 'L',
                   (unsigned long)(g_e_us[c][k] / 1000), (unsigned long)(g_e_us[c][k] % 1000));
        printf("\n");
    }
    /* first RST rising edge is the anchor; show every POST edge relative to it (us) */
    uint32_t anchor = 0; bool have = false;
    for (uint32_t k = 0; k < g_e_n[1]; k++) if (g_e_hi[1][k]) { anchor = g_e_us[1][k]; have = true; break; }
    if (have) {
        printf("POST edges relative to first RST rise (us): ");
        for (uint32_t k = 0; k < g_e_n[0]; k++)
            printf("%c%ld ", g_e_hi[0][k] ? 'H' : 'L', (long)g_e_us[0][k] - (long)anchor);
        printf("\n");
    }
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
 * Fast blue flicker for 2.5s from the moment we arm; solid green for 5s on a
 * confirmed boot; red for 1s on a failed attempt. Everything is non-blocking
 * (except the green hold) so a red/blue phase never delays arming the next try. */
#define GREEN_GAP_MS 500   /* dark gap between the blue flicker and the green */
static int g_led_cur = -1;                       /* 0 off, 1 blue, 2 red, 3 green */
static absolute_time_t g_flick_t0, g_flick_end, g_red_end;   /* zero-init = nil_time */

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
    absolute_time_t n = get_absolute_time();
    if (absolute_time_diff_us(n, g_red_end) > 0) { led_set(2); return; }
    if (absolute_time_diff_us(n, g_flick_end) > 0) {
        int64_t el = absolute_time_diff_us(g_flick_t0, n);
        led_set(((el / 17000) & 1) == 0 ? 1 : 0);
        return;
    }
    led_set(0);
}

static void led_flicker_start(void)
{
    g_flick_t0 = get_absolute_time();
    g_flick_end = delayed_by_ms(g_flick_t0, 2500);
}

static void led_red_for(uint ms)
{
    g_red_end = make_timeout_time_ms(ms);
    led_tick();
}

static void led_green_hold(void)               /* confirmed boot: solid green 5s */
{
    g_red_end = nil_time;
    g_flick_end = nil_time;
    led_set(3);
    sleep_ms(5000);
    led_set(0);
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
    while (!gpio_get(arm_pin))
        led_tick();                       /* lets a red phase time out while we wait */

    pio_sm_config c = (method == METHOD_EXTCLK)
        ? extclk_program_get_default_config(offset)
        : glitch_program_get_default_config(offset);
    sm_config_set_set_pins(&c, CPU_RESET, 2);   /* bit0 = RESET, bit1 = PLL/EXTC */
    if (driven) sm_config_set_sideset_pins(&c, CPU_RESET);

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

    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);

    led_flicker_start();      /* 2.5s fast blue flicker, non-blocking */
    /* The proven build waited ~200ms here before sending the timing values; keep
     * that so POST counting starts at exactly the same moment. */
    { absolute_time_t dl = delayed_by_ms(get_absolute_time(), 204);
      while (absolute_time_diff_us(get_absolute_time(), dl) > 0) led_tick(); }

    /* Same 5-value feed for both engines (slots mean different things per
     * method). Reset-low lasts (y + 1) cycles, so pass width-1. */
    pio_sm_put_blocking(pio, sm, t[0]);                      /* post_count */
    pio_sm_put_blocking(pio, sm, t[1]);                      /* delay_to_pll / slow_delay */
    pio_sm_put_blocking(pio, sm, t[2]);                      /* delay_to_rst / r_end */
    pio_sm_put_blocking(pio, sm, driven ? t[3] : (t[3] > 0 ? t[3] - 1 : 0));   /* glitch_width / pulse_width */
    pio_sm_put_blocking(pio, sm, t[4]);                      /* pll_hold / extc_hold */

    while (pio_sm_is_rx_fifo_empty(pio, sm))
        led_tick();
    pio_sm_get_blocking(pio, sm);         /* glitch complete */

    pio_sm_set_enabled(pio, sm, false);
    pio_sm_restart(pio, sm);

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
    bool judged = false;
    bool booted = false;
    bool red_done = false;
    uint32_t edges = 0;
    bool lastp = gpio_get(POST_PIN);
    while (gpio_get(arm_pin)) {
        bool p = gpio_get(POST_PIN);
        if (p != lastp) { edges++; lastp = p; hh->etot = edges; }
        led_tick();
        /* Green as soon as the POST edges prove a boot - but never before the 2.5s
         * blue flicker has finished plus a short dark gap (GREEN_GAP_MS), so the
         * sequence always reads blue -> pause -> green. */
        if (!judged && edges >= g_boot_edges &&
            absolute_time_diff_us(get_absolute_time(), delayed_by_ms(g_flick_end, GREEN_GAP_MS)) <= 0) {
            judged = true;
            g_last_edges = edges;
            hh->e15 = edges;
            booted = true;
            led_green_hold();
        }
        if (!judged && absolute_time_diff_us(t0, get_absolute_time()) > 4000000) {
            judged = true;
            g_last_edges = edges;
            hh->e15 = edges;
            if (edges >= g_boot_edges) {
                booted = true;
                led_green_hold();          /* boot confirmed: solid green 5s */
            } else {
                led_red_for(1000);         /* hung: red 1s, keep waiting for the reset */
                red_done = true;
            }
        }
    }
    hh->ms = (uint32_t)(absolute_time_diff_us(t0, get_absolute_time()) / 1000);
    hh->done = 1;
    if (!judged) { g_last_edges = edges; hh->e15 = edges; }   /* reset dropped before 4s: a miss */
    if (!booted && !red_done) led_red_for(1000);

    if (sweep_active) {
        if (booted) {
            sweep_landed_val = sweep_val;
            sweep_landed = true;
            sweep_active = false;
            g_halt = true;      /* stay off the console now that it has booted */
        } else {
            int64_t nv = (int64_t)sweep_val + (int64_t)sweep_step;
            if ((sweep_step > 0 && nv > (int64_t)sweep_end) ||
                (sweep_step < 0 && nv < (int64_t)sweep_end))
                nv = (int64_t)sweep_start;        /* wrap around */
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
    } else if (!strcmp(cmd, "mon")) {
        printf("monitoring pins for 6s - power-cycle the console now...\n");
        bool rh = false, rl = false, ph = false, pl = false;
        absolute_time_t t0 = get_absolute_time();
        while (absolute_time_diff_us(t0, get_absolute_time()) < 6000000) {
            if (gpio_get(CPU_RESET)) rh = true; else rl = true;
            if (gpio_get(POST_PIN))  ph = true; else pl = true;
            sleep_ms(1);
        }
        printf("CPU_RESET(26): %s\n",
               (rh && rl) ? "TOGGLED  (seen high & low - wire good)" :
               (rh ? "always HIGH" : "always LOW  (not seeing the console)"));
        printf("POST(28):      %s\n",
               (ph && pl) ? "TOGGLED  (seen high & low - wire good)" :
               (ph ? "always HIGH (stuck)" : "always LOW  (no POST - check this wire)"));
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

    multicore_launch_core1(core1_main);

    sleep_ms(500);
    print_banner();
    print_status();
    printf("> ");

    uint32_t last_tick = 0;
    while (true) {
        console_task();
        if (sweep_active && sweep_tick != last_tick) {
            last_tick = sweep_tick;
            printf("  sweep: %s = %lu\n", tname(g_cfg.profiles[g_cfg.active].method, sweep_idx),
                   (unsigned long)sweep_val);
        }
        if (sweep_landed) {
            sweep_landed = false;
            printf("\n*** GLITCH LANDED: %s = %lu ***  ('save' to keep)\n> ",
                   tname(g_cfg.profiles[g_cfg.active].method, sweep_idx), (unsigned long)sweep_landed_val);
        }
        sleep_ms(2);
    }
    return 0;
}
