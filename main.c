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
 * GP16 -> onboard WS2812 (RP2040-Zero): blue = glitching, green = booted.
 *                          Classic Pico: set USE_WS2812 0 to use GP25.
 * 3V3 / GND -> switched 3.3V and ground
 *
 * POST is hard-coded to GPIO 28 inside glitch.pio. If you move it, change it
 * in BOTH places.
 * ------------------------------------------------------------------------ */
#define CPU_RESET   26
#define PLL_BYPASS  27
#define POST_PIN    28

#define MP_SYS_CLK_KHZ 266000

/* ---- Profiles ----------------------------------------------------------- */
#define NUM_TIMINGS     5
#define NUM_PROFILES    4
#define PROFILE_NAME_LEN 16
#define CONFIG_MAGIC    0x4D505832u   /* "MPX2" - bump if layout changes */
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

typedef struct {
    char     name[PROFILE_NAME_LEN];
    uint32_t t[NUM_TIMINGS];
} profile_t;

typedef struct {
    uint32_t  magic;
    uint32_t  version;
    uint32_t  active;                 /* index of the active profile */
    uint32_t  reserved;
    profile_t profiles[NUM_PROFILES];
} config_t;

/* Built-in defaults.
 *
 * "trinity" carries the original author's real Trinity RGH 1.2 values, so it
 * is a genuine starting point, not a placeholder. The other three are SEEDED
 * with the Trinity values only so the firmware has something to run; they are
 * NOT verified for those boards and MUST be dialed in before they are trusted.
 */
static const config_t DEFAULT_CONFIG = {
    .magic   = CONFIG_MAGIC,
    .version = CONFIG_VERSION,
    .active  = 0,
    .reserved = 0,
    .profiles = {
        { "trinity", { 3, 144686585, 9693040, 63, 66500 } },
        { "corona",  { 3, 144686585, 9693040, 63, 66500 } },  /* UNVERIFIED - dial in */
        { "jasper",  { 3, 144686585, 9693040, 63, 66500 } },  /* UNVERIFIED - dial in */
        { "falcon",  { 3, 144686585, 9693040, 63, 66500 } },  /* UNVERIFIED - dial in */
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

/* ---- Flash persistence -------------------------------------------------- */

static void config_load(void)
{
    const config_t *fc = (const config_t *)g_flash_cfg;
    if (fc->magic == CONFIG_MAGIC && fc->version == CONFIG_VERSION &&
        fc->active < NUM_PROFILES) {
        memcpy(&g_cfg, fc, sizeof(config_t));
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
#else
static void led_init(void) { gpio_init(LED_PIN); gpio_set_dir(LED_PIN, GPIO_OUT); }
static inline void led_off(void)   { gpio_put(LED_PIN, 0); }
static inline void led_blue(void)  { gpio_put(LED_PIN, 1); }
static inline void led_green(void) { gpio_put(LED_PIN, 1); }
#endif

/* ---- Glitch core (core1) ------------------------------------------------ */

static void run_glitch_once(PIO pio, uint sm, uint offset)
{
    const uint32_t *t = g_cfg.profiles[g_cfg.active].t;

    gpio_init(CPU_RESET);
    gpio_set_dir(CPU_RESET, GPIO_IN);
    gpio_set_slew_rate(CPU_RESET, GPIO_SLEW_RATE_FAST);
    gpio_set_drive_strength(CPU_RESET, GPIO_DRIVE_STRENGTH_12MA);
    while (!gpio_get(CPU_RESET))
        tight_loop_contents();

    pio_sm_config c = glitch_program_get_default_config(offset);
    sm_config_set_sideset_pins(&c, CPU_RESET);

    pio_gpio_init(pio, CPU_RESET);
    gpio_set_slew_rate(CPU_RESET, GPIO_SLEW_RATE_FAST);
    gpio_set_drive_strength(CPU_RESET, GPIO_DRIVE_STRENGTH_12MA);

    pio_gpio_init(pio, PLL_BYPASS);
    gpio_set_slew_rate(PLL_BYPASS, GPIO_SLEW_RATE_FAST);
    gpio_set_drive_strength(PLL_BYPASS, GPIO_DRIVE_STRENGTH_12MA);

    pio_sm_set_pins_with_mask(pio, sm, (1u << CPU_RESET) | (0u << PLL_BYPASS),
                              (1u << CPU_RESET) | (1u << PLL_BYPASS));
    pio_sm_set_pindirs_with_mask(pio, sm, (1u << CPU_RESET) | (1u << PLL_BYPASS),
                                 (1u << CPU_RESET) | (1u << PLL_BYPASS));

    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);

    led_blue();          /* blue flashes once per glitch attempt */

    sleep_ms(200);

    pio_sm_put_blocking(pio, sm, t[0]);   /* post_count   */
    pio_sm_put_blocking(pio, sm, t[1]);   /* delay_to_pll */
    pio_sm_put_blocking(pio, sm, t[2]);   /* delay_to_rst */
    pio_sm_put_blocking(pio, sm, t[3]);   /* glitch_width */
    pio_sm_put_blocking(pio, sm, t[4]);   /* pll_hold     */

    pio_sm_get_blocking(pio, sm);         /* wait for the glitch to complete */

    pio_sm_set_enabled(pio, sm, false);
    pio_sm_restart(pio, sm);

    led_off();

    gpio_init(CPU_RESET);
    gpio_set_dir(CPU_RESET, GPIO_IN);

    /* If CPU_RESET stays high, the console came up -> solid green. If it drops
     * quickly, the glitch missed and we loop round for another attempt. */
    absolute_time_t t0 = get_absolute_time();
    bool booted = false;
    while (gpio_get(CPU_RESET)) {
        if (!booted && absolute_time_diff_us(t0, get_absolute_time()) > 2000000) {
            led_green();
            booted = true;
        }
        tight_loop_contents();
    }
    led_off();

    g_attempts++;
}

static void core1_main(void)
{
    /* Allow core0 to pause us while it writes flash. */
    multicore_lockout_victim_init();

    led_init();
    led_off();

    PIO pio = pio0;
    uint sm = 0;
    uint offset = pio_add_program(pio, &glitch_program);

    while (true)
        run_glitch_once(pio, sm, offset);
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
    printf("[%d] %-8s%s\n", i, g_cfg.profiles[i].name,
           (i == (int)g_cfg.active) ? "  <= active" : "");
    for (int k = 0; k < NUM_TIMINGS; k++)
        printf("      %d %-12s = %lu\n", k, TIMING_NAMES[k],
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
        "  attempts             reset the attempt counter\n"
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
               TIMING_NAMES[idx], (unsigned long)v);
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
    } else if (!strcmp(cmd, "attempts")) {
        g_attempts = 0;
        printf("attempt counter reset.\n");
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
    /* Overclock for glitch-pulse resolution (~3.75 ns/cycle at 266 MHz). */
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    set_sys_clock_khz(MP_SYS_CLK_KHZ, true);

    uint32_t freq = clock_get_hz(clk_sys);
    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                    freq, freq);

    stdio_init_all();      /* USB CDC serial console */

    config_load();

    multicore_launch_core1(core1_main);

    sleep_ms(500);
    print_banner();
    print_status();
    printf("> ");

    while (true) {
        console_task();
        sleep_ms(2);
    }
    return 0;
}
