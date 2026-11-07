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

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
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
#define MP_SYS_CLK_KHZ 192000   /* multiple of 12/48 MHz; 4 cycles per STBY_CLK */
#endif
#ifndef MP_T_SET
#define MP_DRV_PROG        glitch_side_program
#define MP_DRV_CFG(o)      glitch_side_program_get_default_config(o)
#define MP_DRV_WIDTH(w)    (w)
#else
#define MP_DRV_PROG        glitch_drv_program
#define MP_DRV_CFG(o)      glitch_drv_program_get_default_config(o)
#define MP_DRV_WIDTH(w)    ((w) > 0 ? (w) - 1 : 0)
#endif
#ifndef MP_RST_DRIVEN
#define MP_RST_DRIVEN 0          /* TEST BUILDS ONLY: 1 = original PicoRGH push-pull reset drive */
#endif

/* ---- Profiles ----------------------------------------------------------- */
#define NUM_TIMINGS     5
#define NUM_PROFILES    6
#define PROFILE_NAME_LEN 16
/* Test variants get their own magic so they never load each other's saved timings. */
#define CONFIG_MAGIC    (0x4D505836u ^ ((MP_SYS_CLK_KHZ == 200000) ? 0x100u : 0u) ^ 0x400u)  /* diag layout: runtime options in "reserved" */
#define CONFIG_MAGIC_V5 0x4D505835u   /* "MPX5" - 5-profile layout, migrated on load */
#define NUM_PROFILES_V5 5
#define CONFIG_VERSION  2   /* bumped for autotune: existing saves re-tune from factory */

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
#define TRI_T { 3, 108786906, 7287960, 47, 50000 }    /* 200 MHz; delay_to_rst = measured window centre (was 7288000, ~40 cyc high) */
#else
#define TRI_T { 3, 104435430, 6996480, 45, 48000 }    /* same real-time at 192 MHz */
#endif

static const config_t DEFAULT_CONFIG = {
    .magic   = CONFIG_MAGIC,
    .version = CONFIG_VERSION,
    .active  = 0,
    .reserved = 0x6,   /* DIAG default = A2-equivalent: bit1 driven reset, bit2 POST pull-down */
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
static volatile uint32_t g_last_edges = 0;   /* POST edges seen 1.5s after the last glitch */
static volatile uint32_t g_boot_edges = 3;   /* edges needed to call it a real boot */
/* ---- Wire-free self-calibration (selfcal) --------------------------------
 * Each boot, measure a deterministic console POST interval (the edge just after
 * PROBE_A_US to the edge just after PROBE_B_US from reset release) in the Pico's
 * own time, and scale delay_to_rst by how far that measurement drifts from
 * PROBE_NOM_US. This cancels the Pico-vs-console clock drift over the ~36 ms
 * that actually reaches the glitch. Opt-in (reserved bit4); default OFF. */
static volatile bool     g_selfcal = false;
static uint32_t          g_probe_a_us  = 15000;     /* first probe edge: first POST edge after this */
static uint32_t          g_probe_b_us  = 300000;    /* second probe edge: first POST edge after this */
static uint32_t          g_probe_nom_us = 329723;   /* nominal probe interval (from this console's timeline) */
static volatile uint32_t g_last_probe_us = 0;       /* diagnostics: last measured probe + applied delay */
static volatile uint32_t g_last_drst = 0;
/* ---- Autotune (flash/wire/play) -----------------------------------------
 * On a fresh install the firmware tunes delay_to_rst itself over the first boots
 * (it still glitches, so the console boots meanwhile), then captures this board's
 * probe nominal, turns self-cal on, and locks in - no tool, no commands.
 * Done is remembered in reserved bit5; selfcal in bit4. */
static volatile bool g_autotune = false;
#define AT_HALF   150u     /* autotune sweep half-range (cycles); covers the window + margin, converges fast */
#define AT_STEP   3u       /* autotune sweep step */
#define AT_MIN_OK 14u      /* successful boots needed before locking in */

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
    g_selfcal = (g_cfg.reserved & 16u) != 0;
    g_autotune = !(g_cfg.reserved & 32u) && g_cfg.profiles[g_cfg.active].method == METHOD_RGH12;
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


/* ---- Autonomous centering (tune mode) -----------------------------------
 * Each attempt uses a random delay_to_rst (t[2]) inside +-half of the saved
 * base value, and logs (bucket, booted?) to its own flash sector. Power-cycle
 * the console ~20 times with NO USB attached; later 'tunelog' shows where it
 * boots and 'tunecenter' sets t[2] to the middle of that window. The saved
 * profile is never changed until 'tunecenter'. */
#define TUNE_OFFSET   (PICO_FLASH_SIZE_BYTES - 2 * FLASH_SECTOR_SIZE)
#define TUNE_MAGIC    0x54554E33u
#define TUNE_IDX      2
#define TUNE_MAX_REC  1920u                       /* (4096-256)/2 */
#define TUNE_MAX_BKT  601
typedef struct { uint32_t magic, base, half, step, idx; } tune_hdr_t;
static uint32_t g_tune_idx = TUNE_IDX;
static const uint8_t *g_tune_flash = (const uint8_t *)(XIP_BASE + TUNE_OFFSET);
static volatile bool     g_tune_active = false;
static uint32_t          g_tune_base, g_tune_half, g_tune_step;
static uint32_t          g_tune_n = 0;            /* records stored */
static volatile uint32_t g_tune_b = 0;            /* bucket of the running attempt */
static volatile uint16_t g_tq[16];
static volatile uint32_t g_tq_h = 0, g_tq_t = 0;
static uint32_t          g_rng = 2463534242u;

static inline uint32_t tune_nb(void) { return 2u * (g_tune_half / g_tune_step) + 1u; }
static inline uint32_t tune_val(uint32_t b) { return g_tune_base - g_tune_half + b * g_tune_step; }
static inline const uint16_t *tune_rec(void) { return (const uint16_t *)(g_tune_flash + 256); }

static void tune_load(void)
{
    const tune_hdr_t *h = (const tune_hdr_t *)g_tune_flash;
    g_tune_active = false;
    if (h->magic != TUNE_MAGIC || h->step == 0 || h->half == 0 ||
        h->half / h->step > 300 || h->base < h->half || h->idx >= NUM_TIMINGS) return;
    g_tune_idx = h->idx;
    g_tune_base = h->base; g_tune_half = h->half; g_tune_step = h->step;
    uint32_t n = 0;
    while (n < TUNE_MAX_REC && tune_rec()[n] != 0xFFFF) n++;
    g_tune_n = n;
    g_tune_active = true;
}

static void tune_flash_page(uint32_t off, const uint8_t *page)
{
    multicore_lockout_start_blocking();
    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(off, page, FLASH_PAGE_SIZE);
    restore_interrupts(ints);
    multicore_lockout_end_blocking();
}

static void tune_start(uint32_t idx, uint32_t base, uint32_t half, uint32_t step)
{
    uint8_t pg[FLASH_PAGE_SIZE];
    memset(pg, 0xFF, sizeof(pg));
    tune_hdr_t h = { TUNE_MAGIC, base, half, step, idx };
    memcpy(pg, &h, sizeof(h));
    multicore_lockout_start_blocking();
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(TUNE_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(TUNE_OFFSET, pg, FLASH_PAGE_SIZE);
    restore_interrupts(ints);
    multicore_lockout_end_blocking();
    tune_load();
}

static void tune_stop(void)
{
    multicore_lockout_start_blocking();
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(TUNE_OFFSET, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
    multicore_lockout_end_blocking();
    g_tune_active = false;
}

/* core1 -> core0 mailbox; core0 does the flash write between attempts. */
static void tune_post(uint32_t b, bool ok)
{
    uint32_t h = g_tq_h;
    if (h - g_tq_t >= 16) return;
    g_tq[h % 16] = (uint16_t)((b << 1) | (ok ? 1u : 0u));
    g_tq_h = h + 1;
}

static void tune_service(void)
{
    while (g_tq_t != g_tq_h) {
        uint16_t r = g_tq[g_tq_t % 16];
        g_tq_t++;
        if (!g_tune_active || g_tune_n >= TUNE_MAX_REC) continue;
        uint32_t byteoff = 256u + g_tune_n * 2u;
        uint32_t page = byteoff & ~(uint32_t)(FLASH_PAGE_SIZE - 1);
        uint8_t pg[FLASH_PAGE_SIZE];
        memset(pg, 0xFF, sizeof(pg));
        memcpy(pg + (byteoff - page), &r, 2);
        tune_flash_page(TUNE_OFFSET + page, pg);
        g_tune_n++;
    }
}

/* Pure analysis, host-testable. succ[]: success count per bucket (nb buckets).
 * Returns number of successes used; *center_b = middle bucket of the window
 * after trimming stragglers far from the median. */
static uint32_t tune_analyze(const uint16_t *succ, uint32_t nb, uint32_t *center_b,
                             uint32_t *lo_b, uint32_t *hi_b)
{
    static uint16_t v[TUNE_MAX_REC];
    uint32_t n = 0;
    for (uint32_t b = 0; b < nb; b++)
        for (uint32_t k = 0; k < succ[b] && n < TUNE_MAX_REC; k++) v[n++] = (uint16_t)b;
    if (n == 0) return 0;                      /* v[] is already sorted by bucket */
    uint32_t med = v[n / 2];
    static uint16_t d[TUNE_MAX_REC];
    for (uint32_t i = 0; i < n; i++) d[i] = (uint16_t)(v[i] > med ? v[i] - med : med - v[i]);
    for (uint32_t i = 1; i < n; i++) {         /* insertion sort of deviations */
        uint16_t x = d[i]; int j = (int)i - 1;
        while (j >= 0 && d[j] > x) { d[j + 1] = d[j]; j--; }
        d[j + 1] = x;
    }
    uint32_t mad = d[n / 2];
    uint32_t lim = 4u * (mad < 2 ? 2 : mad);
    uint32_t mn = 0xFFFFFFFFu, mx = 0, used = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t dv = v[i] > med ? v[i] - med : med - v[i];
        if (dv > lim) continue;
        if (v[i] < mn) mn = v[i];
        if (v[i] > mx) mx = v[i];
        used++;
    }
    *lo_b = mn; *hi_b = mx; *center_b = (mn + mx) / 2;
    return used;
}

static void tune_report(bool apply)
{
    if (!g_tune_active) { printf("tune mode is not active (use 'tune').\n"); return; }
    static uint16_t succ[TUNE_MAX_BKT], tries[TUNE_MAX_BKT];
    uint32_t nb = tune_nb();
    memset(succ, 0, sizeof(succ)); memset(tries, 0, sizeof(tries));
    uint32_t okn = 0;
    for (uint32_t i = 0; i < g_tune_n; i++) {
        uint16_t r = tune_rec()[i];
        uint32_t b = r >> 1;
        if (b >= nb) continue;
        tries[b]++;
        if (r & 1) { succ[b]++; okn++; }
    }
    printf("tune: %s base %lu +-%lu step %lu | %lu attempts logged, %lu boots\n",
           TIMING_NAMES[g_tune_idx], (unsigned long)g_tune_base, (unsigned long)g_tune_half, (unsigned long)g_tune_step,
           (unsigned long)g_tune_n, (unsigned long)okn);
    /* histogram: 20 groups */
    uint32_t grp = (nb + 19) / 20;
    for (uint32_t g = 0; g * grp < nb; g++) {
        uint32_t s = 0, t = 0;
        for (uint32_t b = g * grp; b < (g + 1) * grp && b < nb; b++) { s += succ[b]; t += tries[b]; }
        printf("  %8lu..%-8lu %3lu/%-3lu ", (unsigned long)tune_val(g * grp),
               (unsigned long)tune_val((g + 1) * grp - 1 < nb ? (g + 1) * grp - 1 : nb - 1),
               (unsigned long)s, (unsigned long)t);
        for (uint32_t k = 0; k < s && k < 40; k++) putchar('#');
        putchar('\n');
    }
    uint32_t cb, lb, hb;
    uint32_t used = tune_analyze(succ, nb, &cb, &lb, &hb);
    if (used < 8) {
        printf("need at least 8 boots to centre (have %lu). keep power-cycling.\n", (unsigned long)used);
        return;
    }
    uint32_t cv = tune_val(cb);
    printf("window %lu..%lu (width %lu cycles) -> centre %lu (current saved %lu)\n",
           (unsigned long)tune_val(lb), (unsigned long)tune_val(hb),
           (unsigned long)((hb - lb) * g_tune_step), (unsigned long)cv, (unsigned long)g_tune_base);
    if (apply) {
        g_cfg.profiles[g_cfg.active].t[g_tune_idx] = cv;
        tune_stop();
        config_save();
        printf("centre applied and saved; tune mode OFF.\n");
    } else {
        printf("'tunecenter' applies it. 'tunestop' leaves tune mode without changing anything.\n");
    }
}


/* ---- Adaptive tracking --------------------------------------------------
 * The glitch window drifts ~50 cycles between a cold and a warm console.
 * Each boot's first attempt starts at the value that last booted (an EMA so
 * one edge hit can't drag it), and each consecutive miss tries a widening
 * spread around it. The value is also kept in flash (own sector) so it
 * survives the Pico losing power. Only t[2] moves, clamped to +-TRK_CLAMP of
 * the saved profile value. */
#define TRK_OFFSET   (PICO_FLASH_SIZE_BYTES - 3 * FLASH_SECTOR_SIZE)
#define TRK_REC_MAX  1024u
#define TRK_STEP     12
#define TRK_SLOTS    11            /* 0,+1,-1,... +-5 steps = +-60 cycles */
#define TRK_CLAMP    150
static const uint32_t *g_trk_flash = (const uint32_t *)(XIP_BASE + TRK_OFFSET);
static uint32_t          g_trk_base = 0;          /* saved profile t[2] */
static uint32_t          g_trk_val = 0;           /* tracked centre */
static uint32_t          g_trk_n = 0;             /* flash records */
static uint32_t          g_trk_miss = 0;          /* misses since last boot / power-up */
static uint32_t          g_trk_used = 0;          /* value used by the running attempt */
static absolute_time_t   g_trk_last_end;           /* when the previous attempt ended (nil at power-up) */
static volatile uint32_t g_trk_post = 0;          /* success value pending flash write */

static void trk_load(void)
{
    g_trk_base = g_cfg.profiles[g_cfg.active].t[2];
    g_trk_val = g_trk_base;
    uint32_t n = 0, last = 0;
    while (n < TRK_REC_MAX && g_trk_flash[n] != 0xFFFFFFFFu) { last = g_trk_flash[n]; n++; }
    g_trk_n = n;
    if (n && last + TRK_CLAMP >= g_trk_base && last <= g_trk_base + TRK_CLAMP) g_trk_val = last;
}

static uint32_t trk_pick(void)
{
    static const int8_t seq[TRK_SLOTS] = { 0, 1, -1, 2, -2, 3, -3, 4, -4, 5, -5 };
    int32_t v = (int32_t)g_trk_val + (int32_t)seq[g_trk_miss % TRK_SLOTS] * TRK_STEP;
    int32_t lo = (int32_t)g_trk_base - TRK_CLAMP, hi = (int32_t)g_trk_base + TRK_CLAMP;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (uint32_t)v;
}

static void trk_service(void)
{
    uint32_t v = g_trk_post;
    if (!v) return;
    g_trk_post = 0;
    if (g_trk_n >= TRK_REC_MAX) {                 /* sector full: start over with this value */
        multicore_lockout_start_blocking();
        uint32_t ints = save_and_disable_interrupts();
        flash_range_erase(TRK_OFFSET, FLASH_SECTOR_SIZE);
        restore_interrupts(ints);
        multicore_lockout_end_blocking();
        g_trk_n = 0;
    }
    uint32_t byteoff = g_trk_n * 4u;
    uint32_t page = byteoff & ~(uint32_t)(FLASH_PAGE_SIZE - 1);
    uint8_t pg[FLASH_PAGE_SIZE];
    memset(pg, 0xFF, sizeof(pg));
    memcpy(pg + (byteoff - page), &v, 4);
    tune_flash_page(TRK_OFFSET + page, pg);
    g_trk_n++;
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
static inline void led_purple(void){ led_raw(led_rgb(16, 0, 24)); }
#else
static void led_init(void) { gpio_init(LED_PIN); gpio_set_dir(LED_PIN, GPIO_OUT); }
static inline void led_off(void)   { gpio_put(LED_PIN, 0); }
static inline void led_blue(void)  { gpio_put(LED_PIN, 1); }
static inline void led_green(void) { gpio_put(LED_PIN, 1); }
static inline void led_purple(void){ gpio_put(LED_PIN, 1); }
static inline void led_red(void)   { gpio_put(LED_PIN, 1); }
#endif

/* ---- Glitch core (core1) ------------------------------------------------ */

/* ---- LED state machine (core1) ------------------------------------------
 * Fast blue flicker for 2.5s from the moment we arm; solid green for 5s on a
 * confirmed boot; red for 1s on a failed attempt. Everything is non-blocking
 * (except the green hold) so a red/blue phase never delays arming the next try. */
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
    case 4:  led_purple();break;
    default: led_off();   break;
    }
}

static void led_tick(void)
{
    absolute_time_t n = get_absolute_time();
    if (absolute_time_diff_us(n, g_red_end) > 0) { led_set(2); return; }
    if (absolute_time_diff_us(n, g_flick_end) > 0) {
        int64_t el = absolute_time_diff_us(g_flick_t0, n);
        led_set(((el / 17000) & 1) == 0 ? (g_autotune ? 4 : 1) : 0);
        return;
    }
    led_set(0);
}

#define GREEN_GAP_MS 500   /* dark gap between the blue flicker and the green */
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
    return (e == 1) ? &extclk_program : (e == 2) ? &glitch_pu_program
         : (e == 3) ? &MP_DRV_PROG : &glitch_program;
}

/* Record the verdict of an attempt for tune mode / tracking (called once per attempt). */
static void judge_note(bool ok)
{
    if (g_tune_active) { tune_post(g_tune_b, ok); return; }
    /* fixed-timing build: no per-attempt state is kept */
}

static void run_glitch_once(PIO pio, uint sm, uint offset, uint method, bool pullup, bool driven)
{
    uint32_t mv_idx = g_tune_active ? g_tune_idx : 2;
    uint32_t t2_orig = g_cfg.profiles[g_cfg.active].t[mv_idx], t2_applied = t2_orig;
    bool t2_moved = false;
    if (sweep_active)
        g_cfg.profiles[g_cfg.active].t[sweep_idx] = sweep_val;
    else if (g_tune_active) {
        g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
        g_rng += (uint32_t)time_us_32();
        g_tune_b = (g_rng >> 8) % tune_nb();
        g_cfg.profiles[g_cfg.active].t[g_tune_idx] = tune_val(g_tune_b);
        t2_applied = tune_val(g_tune_b); t2_moved = true;
    }
    /* FIXED TIMING: the saved profile value is used for every attempt. Nothing
     * is written to flash during boots, so boot quality can never drift from
     * stored state. Use 'tune' only as an explicit, opt-in diagnostic. */

    const uint32_t *t = g_cfg.profiles[g_cfg.active].t;

    /* RGH 1.2 senses the reset line it also pulses (GP26). EXT_CLK can't read
     * the 1.1V reset net, so it arms off a separate 3.3V reset point (GP29). */
    uint arm_pin = (method == METHOD_EXTCLK) ? RESET_SENSE : CPU_RESET;

    gpio_init(arm_pin);
    gpio_set_dir(arm_pin, GPIO_IN);
    while (!gpio_get(arm_pin))
        led_tick();                       /* lets a red phase time out while we wait */
    uint32_t sc_t0 = time_us_32();        /* reset-release moment (selfcal probe reference) */

    pio_sm_config c = (method == METHOD_EXTCLK)
        ? extclk_program_get_default_config(offset)
        : (driven ? MP_DRV_CFG(offset)
          : pullup ? glitch_pu_program_get_default_config(offset)
                   : glitch_program_get_default_config(offset));
    sm_config_set_set_pins(&c, CPU_RESET, 2);   /* bit0 = RESET, bit1 = PLL/EXTC */
#ifndef MP_T_SET
    if (driven) sm_config_set_sideset_pins(&c, CPU_RESET);
#endif

    pio_gpio_init(pio, CPU_RESET);
    gpio_disable_pulls(CPU_RESET);
    gpio_set_slew_rate(CPU_RESET, GPIO_SLEW_RATE_FAST);
    gpio_set_drive_strength(CPU_RESET, GPIO_DRIVE_STRENGTH_12MA);

    pio_gpio_init(pio, PLL_BYPASS);
    gpio_set_slew_rate(PLL_BYPASS, GPIO_SLEW_RATE_FAST);
    gpio_set_drive_strength(PLL_BYPASS, GPIO_DRIVE_STRENGTH_12MA);

    /* Both latches low. PLL is an output; RESET starts as an INPUT (floating)
     * so we never hold the 1.1V reset line at 3.3V. glitch.pio only switches
     * RESET to output for the pulse plus one pull-up cycle. */
    if (driven) {
        /* TEST BUILD: original PicoRGH behaviour, reset driven HIGH for the window. */
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

    /* Measure the probe when self-cal OR autotune is active; only APPLY the
     * correction for self-cal. Autotune measures it so it can store this board's
     * probe nominal when it finalises. */
    bool sc = (g_selfcal || g_autotune) && (method != METHOD_EXTCLK);
    bool sc_apply = g_selfcal && (method != METHOD_EXTCLK);
    uint32_t drst = t[2];
    uint32_t width_val = driven ? MP_DRV_WIDTH(t[3]) : (t[3] > 0 ? t[3] - 1 : 0);

    if (!sc) {
        /* The proven build waited ~200ms here before sending the timing values; keep
         * that so POST counting starts at exactly the same moment. */
        { absolute_time_t dl = delayed_by_ms(get_absolute_time(), 204);
          while (absolute_time_diff_us(get_absolute_time(), dl) > 0) led_tick(); }
        pio_sm_put_blocking(pio, sm, t[0]);                  /* post_count */
        pio_sm_put_blocking(pio, sm, t[1]);                  /* delay_to_pll */
        pio_sm_put_blocking(pio, sm, t[2]);                  /* delay_to_rst */
        pio_sm_put_blocking(pio, sm, width_val);             /* glitch_width */
        pio_sm_put_blocking(pio, sm, t[4]);                  /* pll_hold */
    } else {
        /* SELFCAL: keep the 204 ms pre-delay, but poll POST throughout to time the
         * probe interval, then feed a drift-corrected delay_to_rst just-in-time
         * (the PIO does not pull delay_to_rst until after its 544 ms delay_pll). */
        bool lvl = gpio_get(POST_PIN);
        uint32_t t_a = 0, t_b = 0;
        bool have_a = false, have_b = false;
        #define SC_POLL() do { \
            bool v_ = gpio_get(POST_PIN); \
            if (v_ != lvl) { lvl = v_; uint32_t dt_ = time_us_32() - sc_t0; \
                if (!have_a && dt_ > g_probe_a_us) { t_a = time_us_32(); have_a = true; } \
                else if (have_a && !have_b && dt_ > g_probe_b_us) { t_b = time_us_32(); have_b = true; } } \
        } while (0)

        /* 204 ms pre-delay, polling POST */
        { absolute_time_t dl = delayed_by_ms(get_absolute_time(), 204);
          while (absolute_time_diff_us(get_absolute_time(), dl) > 0) { SC_POLL(); led_tick(); } }
        pio_sm_put_blocking(pio, sm, t[0]);                  /* post_count */
        pio_sm_put_blocking(pio, sm, t[1]);                  /* delay_to_pll */

        /* keep polling until the second probe edge is captured (or give up) */
        uint32_t guard = time_us_32();
        while (!have_b && (time_us_32() - guard) < 600000u) { SC_POLL(); led_tick(); }

        if (have_a && have_b && t_b > t_a) {
            uint32_t probe = t_b - t_a;
            g_last_probe_us = probe;
            if (sc_apply) {
                uint64_t corr = (uint64_t)t[2] * probe / g_probe_nom_us;
                uint32_t lo = t[2] - t[2] / 32, hi = t[2] + t[2] / 32;   /* clamp +-~3% */
                if (corr < lo) corr = lo; if (corr > hi) corr = hi;
                drst = (uint32_t)corr;
            }
        } else {
            g_last_probe_us = 0;                             /* probe failed: use nominal */
        }
        g_last_drst = drst;
        pio_sm_put_blocking(pio, sm, drst);                  /* delay_to_rst (corrected) */
        pio_sm_put_blocking(pio, sm, width_val);             /* glitch_width */
        pio_sm_put_blocking(pio, sm, t[4]);                  /* pll_hold */
        #undef SC_POLL
    }
    if (t2_moved && g_cfg.profiles[g_cfg.active].t[mv_idx] == t2_applied)
        g_cfg.profiles[g_cfg.active].t[mv_idx] = t2_orig;

    while (pio_sm_is_rx_fifo_empty(pio, sm))
        led_tick();
    pio_sm_get_blocking(pio, sm);         /* glitch complete */

    pio_sm_set_enabled(pio, sm, false);
    pio_sm_restart(pio, sm);

    gpio_init(arm_pin);
    gpio_set_dir(arm_pin, GPIO_IN);

    /* A hung (failed) glitch also leaves reset high for seconds, so reset-high
     * alone is NOT proof of a boot. A real boot keeps changing POST codes, so we
     * count POST edges for 1.5s after the glitch and require g_boot_edges of
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
        /* Green as soon as the POST edges prove a boot, but never before the 2.5s blue
         * flicker has ended plus a short dark gap, so it always reads blue -> pause -> green. */
        if (!judged && edges >= g_boot_edges &&
            absolute_time_diff_us(get_absolute_time(), delayed_by_ms(g_flick_end, GREEN_GAP_MS)) <= 0) {
            judged = true;
            g_last_edges = edges;
            hh->e15 = edges;
            booted = true;
            judge_note(true);
            led_green_hold();
        }
        if (!judged && absolute_time_diff_us(t0, get_absolute_time()) > 4000000) {
            judged = true;
            g_last_edges = edges;
            hh->e15 = edges;
            judge_note(edges >= g_boot_edges);
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
    if (!judged) { g_last_edges = edges; hh->e15 = edges; judge_note(false); }   /* reset dropped before 1.5s: a miss */
    if (!booted && !red_done) led_red_for(1000);

    if (sweep_active) {
        if (booted) {
            sweep_landed_val = sweep_val;
            sweep_landed = true;
            sweep_active = false;
        } else {
            int64_t nv = (int64_t)sweep_val + (int64_t)sweep_step;
            if ((sweep_step > 0 && nv > (int64_t)sweep_end) ||
                (sweep_step < 0 && nv < (int64_t)sweep_end))
                nv = (int64_t)sweep_start;        /* wrap around */
            sweep_val = (uint32_t)nv;
            sweep_tick++;
        }
    }

    g_trk_last_end = get_absolute_time();
    g_attempts++;
}

/* ---- POST timeline capture (wire-free drift study) ----------------------
 * Records the microsecond time of every POST edge during one boot, WITHOUT
 * glitching (the console hangs at the glitch point, which is fine - we only
 * want to see where the POST edges fall). Dumped later over USB with
 * 'timeline'. Lets us find a POST edge closer to the glitch to re-sync on, or
 * a stable inter-edge interval to use as a clock-ratio probe. */
#define TL_OFFSET      (PICO_FLASH_SIZE_BYTES - 3 * FLASH_SECTOR_SIZE)
#define TL_MAGIC_ARMED 0x544C4152u
#define TL_MAGIC_DONE  0x544C444Eu
#define TL_MAX         480u
static const uint8_t *g_tl_flash = (const uint8_t *)(XIP_BASE + TL_OFFSET);
static volatile bool   g_tl_armed = false;
typedef struct { uint32_t magic, n; } tl_hdr_t;

static void tl_write(uint32_t magic, const uint32_t *ts, uint32_t n)
{
    static uint8_t buf[FLASH_SECTOR_SIZE];
    memset(buf, 0xFF, sizeof(buf));
    tl_hdr_t h = { magic, n };
    memcpy(buf, &h, sizeof(h));
    if (n > TL_MAX) n = TL_MAX;
    if (n) memcpy(buf + 256, ts, n * 4);
    multicore_lockout_start_blocking();
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(TL_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(TL_OFFSET, buf, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
    multicore_lockout_end_blocking();
}

/* core0 runs this when armed: capture one boot's POST edges, store, disarm. */
static void tl_capture(void)
{
    static uint32_t ts[TL_MAX];
    gpio_init(CPU_RESET);     gpio_set_dir(CPU_RESET, GPIO_IN);
    gpio_init(POST_PIN);      gpio_set_dir(POST_PIN, GPIO_IN);
    led_blue();                                   /* solid blue = capturing */
    { uint32_t w0 = time_us_32();                 /* wait reset release, but give up after 12s */
      while (!gpio_get(CPU_RESET)) {
          if (time_us_32() - w0 > 12000000u) { led_off(); return; }  /* no boot: stay armed */
          tight_loop_contents();
      } }
    uint32_t t0 = time_us_32(), last_edge = t0, n = 0;
    bool lvl = gpio_get(POST_PIN);
    for (;;) {
        bool v = gpio_get(POST_PIN);
        uint32_t now = time_us_32();
        if (v != lvl) { if (n < TL_MAX) ts[n++] = now - t0; lvl = v; last_edge = now; }
        if (n >= TL_MAX) break;
        if (n > 0 && (now - last_edge) > 800000u) break;  /* boot hung: done */
        if ((now - t0) > 2500000u) break;                 /* 2.5s hard cap */
    }
    tl_write(TL_MAGIC_DONE, ts, n);
    g_tl_armed = false;
    led_green();                                  /* solid green = capture done */
}

static void tl_dump(void)
{
    const tl_hdr_t *h = (const tl_hdr_t *)g_tl_flash;
    if (h->magic != TL_MAGIC_DONE) { printf("no capture stored. run 'timeline' to arm, boot once, then 'timeline' again.\n"); return; }
    const uint32_t *ts = (const uint32_t *)(g_tl_flash + 256);
    uint32_t n = h->n > TL_MAX ? TL_MAX : h->n;
    printf("POST timeline: %lu edges (us from reset release)\n idx   t_us    dt_us\n", (unsigned long)n);
    uint32_t prev = 0;
    for (uint32_t i = 0; i < n; i++) {
        printf("%4lu %8lu %8lu\n", (unsigned long)i, (unsigned long)ts[i], (unsigned long)(ts[i] - prev));
        prev = ts[i];
    }
    printf("(last edge at %lu us; glitch would fire ~36 ms after the final POST edge)\n",
           (unsigned long)(n ? ts[n-1] : 0));
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
     * (floating) - it must never source voltage into the 1.1V net. */
#ifndef MP_T_NOPLL
    gpio_init(PLL_BYPASS);
    gpio_put(PLL_BYPASS, 0);
    gpio_set_dir(PLL_BYPASS, GPIO_OUT);
#endif
    gpio_init(CPU_RESET);
    gpio_set_dir(CPU_RESET, GPIO_IN);

    PIO pio = pio0;
    uint sm = 0;
    int loaded = -1;
    uint offset = 0;

    while (true) {
        if (g_tl_armed) { led_blue(); sleep_ms(50); continue; }   /* timeline capture: core1 must not glitch */
        /* (Re)load the engine for the active profile. Programs don't fit in one
         * PIO at once, so we swap when the engine changes:
         *   0 = RGH1.2 open-drain, 1 = EXT_CLK, 2 = RGH1.2 + 1-cycle pull-up (rstpull 1). */
        uint m = g_cfg.profiles[g_cfg.active].method;
        bool pullup = (m != METHOD_EXTCLK) && (g_cfg.reserved & 1u);
        bool driven = (m != METHOD_EXTCLK) && (g_cfg.reserved & 2u);
        int eng = (m == METHOD_EXTCLK) ? 1 : (driven ? 3 : (pullup ? 2 : 0));
        if (eng != loaded) {
            if (loaded >= 0) {
                pio_sm_set_enabled(pio, sm, false);
                pio_remove_program(pio, eng_prog(loaded), offset);
            }
            offset = pio_add_program(pio, eng_prog(eng));
            loaded = eng;
        }
        run_glitch_once(pio, sm, offset, m, pullup, driven);
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
    if (g_autotune) {
        uint32_t nb = tune_nb(), okn = 0;
        for (uint32_t i = 0; i < g_tune_n; i++) { uint16_t r = tune_rec()[i]; if ((r >> 1) < nb && (r & 1)) okn++; }
        printf("autotune: IN PROGRESS (%lu/%u good boots; keep power-cycling)\n", (unsigned long)okn, AT_MIN_OK);
    } else if (g_cfg.reserved & 32u) {
        printf("autotune: done\n");
    }
    printf("selfcal: %s", g_selfcal ? "ON" : "off");
    if (g_selfcal) {
        printf("  (probe %lu-%lu us, nominal %lu", (unsigned long)g_probe_a_us,
               (unsigned long)g_probe_b_us, (unsigned long)g_probe_nom_us);
        if (g_last_probe_us) printf("; last probe %lu us -> delay_to_rst %lu",
               (unsigned long)g_last_probe_us, (unsigned long)g_last_drst);
        else if (g_last_drst) printf("; last probe FAILED, used nominal");
        printf(")");
    }
    printf("\n");
    if (g_tune_active)
        printf("*** TUNE MODE ON: t[2] randomised %lu +-%lu, %lu attempts logged. 'tunelog' / 'tunecenter' / 'tunestop' ***\n",
               (unsigned long)g_tune_base, (unsigned long)g_tune_half, (unsigned long)g_tune_n);
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
        "  opt [name 0|1]       diagnostic options: driven, postpull, rstpull\n"
        "  rstpull [0|1]        RGH1.2 reset 1-cycle pull-up (diagnostic, default 0=safe)\n"
        "  tune [half] [step] [idx]  auto-centre timing idx (default 2 = delay_to_rst; 3 = width), run console unplugged\n"
        "  tunelog / tunecenter / tunestop   show results / apply centre / leave tune mode\n"
        "  hist                 last 16 attempts: POST edges + how long reset stayed high\n"
        "  last                 POST edges counted after the last glitch\n"
        "  bootedges [n]        POST edges needed to count as a real boot (default 6)\n"
        "  attempts             reset the attempt counter\n"
        "  mon                  watch CPU_RESET/POST live for 6s (power-cycle to test)\n"
        "  sweep <i> <a> <b> [s] auto-step timing i from a to b until it boots\n"
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
        g_trk_base = g_cfg.profiles[i].t[2]; g_trk_val = g_trk_base; g_trk_miss = 0;
        printf("active -> %s (unsaved; 'save' to persist)\n", g_cfg.profiles[i].name);
    } else if (!strcmp(cmd, "set")) {
        char *a = strtok(NULL, " \t");
        char *b = strtok(NULL, " \t");
        if (!a || !b) { printf("err: set <idx 0-4> <value>\n"); return; }
        int idx = atoi(a);
        if (idx < 0 || idx >= NUM_TIMINGS) { printf("err: idx 0-4\n"); return; }
        uint32_t v = (uint32_t)strtoul(b, NULL, 0);
        g_cfg.profiles[g_cfg.active].t[idx] = v;
        if (idx == 2) { g_trk_base = v; g_trk_val = v; g_trk_miss = 0; }
        printf("%s.%s = %lu (unsaved)\n", g_cfg.profiles[g_cfg.active].name,
               tname(g_cfg.profiles[g_cfg.active].method, idx), (unsigned long)v);
    } else if (!strcmp(cmd, "name")) {
        char *a = strtok(NULL, " \t");
        if (!a) { printf("err: name <newname>\n"); return; }
        strncpy(g_cfg.profiles[g_cfg.active].name, a, PROFILE_NAME_LEN - 1);
        g_cfg.profiles[g_cfg.active].name[PROFILE_NAME_LEN - 1] = '\0';
        printf("renamed -> %s (unsaved)\n", g_cfg.profiles[g_cfg.active].name);
    } else if (!strcmp(cmd, "save")) {
        if (g_tune_active) { printf("err: tune mode is on (t[2] is being randomised). 'tunecenter' or 'tunestop' first.\n"); return; }
        config_save();
        printf("saved to flash.\n");
    } else if (!strcmp(cmd, "load")) {
        config_load();
        printf("reloaded from flash.\n");
        print_status();
    } else if (!strcmp(cmd, "defaults")) {
        memcpy(&g_cfg, &DEFAULT_CONFIG, sizeof(config_t));
        printf("defaults restored (unsaved; 'save' to persist).\n");
    } else if (!strcmp(cmd, "rstpull")) {
        char *a = strtok(NULL, " \t");
        if (a) {
            if (atoi(a)) g_cfg.reserved |= 1u; else g_cfg.reserved &= ~1u;
        }
        printf("rstpull = %u  (%s; 'save' to persist)\n", (unsigned)(g_cfg.reserved & 1u),
               (g_cfg.reserved & 1u) ? "1-cycle 3.3V pull-up ON - bench diagnostic only"
                                      : "pure open-drain, safe default");
    } else if (!strcmp(cmd, "opt")) {
        char *a = strtok(NULL, " \t"), *b = strtok(NULL, " \t");
        if (a && b) {
            uint32_t bit = !strcmp(a, "driven") ? 2u : !strcmp(a, "postpull") ? 4u
                         : !strcmp(a, "rstpull") ? 1u : 0u;
            if (!bit) printf("unknown option '%s'\n", a);
            else if (atoi(b)) g_cfg.reserved |= bit; else g_cfg.reserved &= ~bit;
            gpio_set_pulls(POST_PIN, false, (g_cfg.reserved & 4u) != 0);
        }
        printf("options (usage: opt <driven|postpull|rstpull> <0|1>, then 'save'):\n"
               "  driven   = %u  (1: reset driven HIGH during window like original PicoRGH; 0: open-drain)\n"
               "  postpull = %u  (1: POST pull-down on like original; 0: POST pure hi-Z)\n"
               "  rstpull  = %u  (open-drain only: 1-cycle pull-up)\n"
               "  clock    = %u kHz\n",
               (unsigned)!!(g_cfg.reserved & 2u), (unsigned)!!(g_cfg.reserved & 4u),
               (unsigned)!!(g_cfg.reserved & 1u), (unsigned)MP_SYS_CLK_KHZ);
    } else if (!strcmp(cmd, "hist")) {
        uint32_t n = g_hist_n, from = n > 16 ? n - 16 : 0;
        printf("attempt  POST-edges@1.5s  total-edges  reset-high-time\n");
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
        printf("sweeping %s from %lu to %lu step %ld\n", tname(g_cfg.profiles[g_cfg.active].method, idx),
               (unsigned long)s, (unsigned long)e, (long)st);
        printf("power-cycle the console; it steps each miss until it boots.\n");
    } else if (!strcmp(cmd, "tune")) {
        char *a = strtok(NULL, " \t");
        char *b = strtok(NULL, " \t");
        uint32_t half = a ? (uint32_t)strtoul(a, NULL, 0) : 600;
        uint32_t step = b ? (uint32_t)strtoul(b, NULL, 0) : 5;
        char *cx = strtok(NULL, " \t");
        uint32_t tidx = cx ? (uint32_t)strtoul(cx, NULL, 0) : TUNE_IDX;
        if (tidx >= NUM_TIMINGS) { printf("err: idx 0-4\n"); return; }
        uint32_t base = g_cfg.profiles[g_cfg.active].t[tidx];
        if (step == 0 || half < step || half / step > 300 || half > 5000 || base < half) {
            printf("err: tune [half<=5000] [step]  (half/step <= 300)\n"); return;
        }
        if (g_cfg.profiles[g_cfg.active].method != METHOD_RGH12) { printf("err: tune is RGH1.2 only\n"); return; }
        tune_start(tidx, base, half, step);
        printf("TUNE MODE ON: %s %lu +-%lu step %lu. Old log erased.\n",
               TIMING_NAMES[tidx], (unsigned long)base, (unsigned long)half, (unsigned long)step);
        printf("Unplug USB, power-cycle the console ~20 boots, then plug USB (console OFF) and run 'tunelog'.\n");
    } else if (!strcmp(cmd, "tunelog")) {
        tune_report(false);
    } else if (!strcmp(cmd, "tunecenter")) {
        tune_report(true);
    } else if (!strcmp(cmd, "tunestop")) {
        tune_stop();
        printf("tune mode OFF, saved profile untouched.\n");
    } else if (!strcmp(cmd, "retune")) {
        tune_stop();
        g_cfg.reserved &= ~(16u | 32u);          /* clear selfcal + autotune-done */
        g_selfcal = false;
        g_autotune = (g_cfg.profiles[g_cfg.active].method == METHOD_RGH12);
        if (g_autotune)
            tune_start(2, g_cfg.profiles[g_cfg.active].t[2], AT_HALF, AT_STEP);
        config_save();
        printf("RETUNE: autotune re-armed from delay_to_rst=%lu. Power-cycle the console ~15 times.\n",
               (unsigned long)g_cfg.profiles[g_cfg.active].t[2]);
    } else if (!strcmp(cmd, "selfcal")) {
        char *a = strtok(NULL, " \t");
        if (a) {
            if (atoi(a)) g_cfg.reserved |= 16u; else g_cfg.reserved &= ~16u;
            g_selfcal = (g_cfg.reserved & 16u) != 0;
        }
        printf("selfcal = %u  (wire-free drift correction; 'save' to persist)\n", (unsigned)g_selfcal);
        printf("probe window %lu..%lu us, nominal %lu us. Set nominal with 'probenom <us>'.\n",
               (unsigned long)g_probe_a_us, (unsigned long)g_probe_b_us, (unsigned long)g_probe_nom_us);
    } else if (!strcmp(cmd, "probenom")) {
        char *a = strtok(NULL, " \t");
        char *b = strtok(NULL, " \t");
        char *cc = strtok(NULL, " \t");
        if (a) g_probe_nom_us = (uint32_t)strtoul(a, NULL, 0);
        if (b) g_probe_a_us   = (uint32_t)strtoul(b, NULL, 0);
        if (cc) g_probe_b_us  = (uint32_t)strtoul(cc, NULL, 0);
        printf("probe nominal %lu us, window %lu..%lu us\n",
               (unsigned long)g_probe_nom_us, (unsigned long)g_probe_a_us, (unsigned long)g_probe_b_us);
    } else if (!strcmp(cmd, "timeline")) {
        char *a = strtok(NULL, " \t");
        const tl_hdr_t *h = (const tl_hdr_t *)g_tl_flash;
        if (!a && h->magic == TL_MAGIC_DONE) {
            tl_dump();
        } else {
            tl_write(TL_MAGIC_ARMED, NULL, 0);
            g_tl_armed = true;
            printf("TIMELINE ARMED. Unplug USB, power the console ON once (it will NOT boot - expected),\n"
                   "let it sit ~5s, power OFF, plug USB back in (console OFF), and run 'timeline' to see the POST map.\n");
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

/* core0: once enough autotune boots have logged, compute the centre, store this
 * board\'s probe nominal, enable self-cal, and lock it in. */
static void autotune_check(void)
{
    if (!g_autotune || !g_tune_active) return;
    static uint16_t succ[TUNE_MAX_BKT];
    uint32_t nb = tune_nb();
    if (nb > TUNE_MAX_BKT) return;
    memset(succ, 0, sizeof(succ));
    uint32_t okn = 0;
    for (uint32_t i = 0; i < g_tune_n; i++) {
        uint16_t r = tune_rec()[i]; uint32_t b = r >> 1;
        if (b < nb && (r & 1)) { succ[b]++; okn++; }
    }
    if (okn < AT_MIN_OK) return;
    uint32_t cb, lb, hb;
    uint32_t used = tune_analyze(succ, nb, &cb, &lb, &hb);
    if (used < 8) return;
    uint32_t cv = tune_val(cb);
    uint32_t base = g_tune_base;                 /* reject a wild centre (bad data) and keep sweeping */
    if (cv + 1 < (base > AT_HALF ? base - AT_HALF : 0) || cv > base + AT_HALF) return;
    g_cfg.profiles[g_cfg.active].t[2] = cv;
    if (g_last_probe_us) g_probe_nom_us = g_last_probe_us;   /* this board\'s probe reference */
    g_cfg.reserved |= 16u;      /* self-cal ON */
    g_cfg.reserved |= 32u;      /* autotune DONE */
    tune_stop();
    g_selfcal = true;
    g_autotune = false;
    config_save();
    printf("\n*** AUTOTUNE COMPLETE: delay_to_rst = %lu, probe_nom = %lu us, self-cal ON, saved. ***\n> ",
           (unsigned long)cv, (unsigned long)g_probe_nom_us);
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

    /* POST is a pure high-impedance input: input enabled, pulls OFF, never
     * driven. So wiring GP28 straight to a 1.1V POST net is electrically safe
     * with no diode - the Pico only sinks input leakage (~uA), it cannot drive
     * or load the net. (Whether a 1.1V high READS is a separate question - see
     * docs/SAFETY.md; on phat it is below the Pico's threshold.) */
    gpio_set_input_enabled(POST_PIN, true);
    config_load();
    tune_load();
    /* DIAG: option bit2 = POST pull-down (reset default of the RP2040 pad). */
    gpio_set_pulls(POST_PIN, false, (g_cfg.reserved & 4u) != 0);

    if (((const tl_hdr_t *)g_tl_flash)->magic == TL_MAGIC_ARMED) g_tl_armed = true;

    multicore_launch_core1(core1_main);

    if (g_tl_armed) tl_capture();   /* armed: record this boot's POST timeline, then disarm */

    sleep_ms(500);
    print_banner();
    print_status();
    printf("> ");

    uint32_t last_tick = 0;
    while (true) {
        console_task();
        tune_service();
        autotune_check();
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
