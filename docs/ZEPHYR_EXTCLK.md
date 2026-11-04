# Zephyr / Xenon — EXT_CLK (experimental)

EXT_CLK is the method for the early phat consoles (Xenon, Zephyr) that don't
glitch reliably on plain RGH 1.2. Matrix Pico's `zephyr` profile uses a
separate PIO engine (`extclk.pio`) for it.

> **Status: hardware-test build.** The engine is written and verified in a PIO
> emulator (pin behaviour and sequence are correct), but the *timing values*
> are structural seeds derived from Octal450's VHDL — they have not been
> confirmed on a console. Expect to dial them in with `sweep`.

## How it differs from RGH 1.2

| | RGH 1.2 (trinity/corona/…) | EXT_CLK (zephyr) |
|---|---|---|
| Slowdown point | CPU_PLL_BYPASS | **CPU_EXT_CLK_EN** |
| Reset pulse | ~45 cycles, driven high 1 cycle after | **~2–3 cycles, pure open-drain (never driven high)** |
| Arm sense | the reset line itself (GP26) | a **separate 3.3V reset point** (GP29) |
| Timing slots | post_count / delay_to_pll / delay_to_rst / glitch_width / pll_hold | post_count / slow_delay / r_end / pulse_width / extc_hold |

The reset pulse is pure open-drain on purpose: the reset net is 1.1V, so the
firmware only ever pulls it LOW and then releases it to float. It is never
driven to 3.3V, which removes the CPU-damage risk that clamp diodes normally
guard against on this method.

## Wiring (Pico GPIO → Zephyr point)

| Pico | Zephyr point | Notes |
|---|---|---|
| GP26 | /CPU_RST_1V1P_N (reset pulse out) | open-drain, 1.1V — only ever pulled low |
| GP27 | CPU_EXT_CLK_EN | the slowdown line (same pin RGH 1.2 uses for PLL) |
| GP28 | POST bit 0 (FT6U1) | 1.1V — safe to wire direct; may not *read* (below) |
| GP29 | a **3.3V** reset-done point | arm sense; Zephyr is "a Falcon with a Waternoose", so use the Falcon/Jasper reset-sense point |
| 3V3 | a **switched 3.3V** point | same as the Slim — into the Pico's 3V3 pin |
| GND | GND | |

Power from a switched 3.3V exactly like Trinity/Corona. Standby 5V is only a
first-attempt reliability tweak, not required.

Source for the points: EXT_CLK/EXT+3 install notes (A=/CPU_RST_1V1P_N,
B=POST bit 0, C=STBY_CLK, F=CPU_EXT_CLK_EN).

## The honest hardware caveats

1. **POST is 1.1V and the Pico's input threshold is ~2V.** A 1.1V "high" may
   not register as logic-high on GP28. The proven fix (from the RGH1.3 Pico
   work) is one small-signal diode into GP28 with the Pico's internal pull-up
   enabled — that's the one spot where "pure wires" may not be enough. The
   reset *output* (open-drain) and the *3.3V* reset-sense are fine on wires.
2. **Timing seeds, not answers.** `r_end 118008` and `pulse 2` are Octal450's
   Zephyr numbers at 192 MHz; `slow_delay` and `post_count` are structural
   estimates. The exact relationship between the VHDL's gated counter and this
   linear PIO sequence needs hardware tuning.
3. **Free-running clock.** We run at 192 MHz off the Pico's own crystal, not
   locked to the console's STBY_CLK. The RGH1.3 author confirms a free-running
   Pico "works well enough"; true STBY_CLK-counting could be added later for
   tighter sync (STBY_CLK on a spare GPIO).

## Dialing it in

1. Flash an EXT_CLK (or standard glitch2 XeLL) image to the console.
2. `use zephyr`, connect over USB (Pico powered by USB during tuning).
3. `mon` with a power-cycle to confirm POST(28) and the reset-sense toggle.
4. Sweep the big knob first:
   `sweep 2 117990 118030 1`  (r_end)
   then `sweep 3 1 5 1` (pulse_width), then `sweep 1 ...` (slow_delay) if needed.
5. `save` once it lands reliably.

## Credits for this method
15432, GliGli, Octal450 (EXT_CLK VHDL); wurthless-elektroniks (RGH1.3 / EXT+3
and the RP2040 reference). See the main README.
