# Hardware safety

Matrix Pico interfaces a 3.3V microcontroller to console nets that are as low
as 1.1V. This is the summary of what the firmware does to avoid harming a
console over time, and the wiring rules that keep it safe.

## What the firmware guarantees

- **The reset line is pure open-drain on every method.** The reset pin is only
  ever an *output pulled LOW* (the glitch) or an *input* (floating). It is
  **never driven to 3.3V**. The line returns high through the console's own
  pull-up. This removes the slow-kill risk of putting 3.3V on the 1.1V reset
  net that clamp diodes normally guard against. Verified in a PIO emulator:
  reset is driven high for 0 cycles on both the RGH 1.2 and EXT_CLK engines.
- **The slowdown line (PLL_BYPASS / EXT_CLK_EN) is driven to its inactive LOW
  state from startup**, so it is never left floating at the console before the
  first glitch attempt, and sits LOW (inactive) between attempts.
- **POST is input-only** — the firmware never drives it.
- A firmware hang can only leave the reset line pulled low (0V) or floating;
  it cannot source over-voltage.

## Wiring rules that keep it safe (do not skip)

- **Series resistor on the slowdown line.** PLL_BYPASS: 5–10K on Slim
  (Trinity/Corona). EXT_CLK_EN / phat PLL: 22K. This limits current into the
  console net — it is a safety part, not optional.
- **Never power from USB and the console at the same time.** Two 3.3V sources
  fight on the Pico's 3V3 pin, and 5V can backfeed into the console. For
  tuning, power from USB only (console 3v3 wire off). For standalone, power
  from the console's standby 5V — on a full Pico, via a diode into VSYS so USB
  can still be connected later without backfeed.
- **Feed console 3.3V into the Pico's 3V3 pin, not 5V/VSYS.**
- **POST (GP28) is safe to wire directly, no diode.** GP28 is a pure
  high-impedance input (input enabled, pulls off, never driven), so it only
  sinks ~uA of leakage and cannot drive or load the POST net on any console.
  Wiring it straight is electrically harmless.
    - *Reading* is the catch, and it is silicon, not firmware: the Pico's input
      high-threshold is ~1.5V. Slim POST (Trinity/Corona) reads fine on wires.
      Phat POST (Xenon/Zephyr/Falcon/Jasper) is ~1.1V, which is **below** that
      threshold, so it often reads as always-low on a bare wire. If `mon` shows
      POST stuck low on a phat board, a small-signal diode into GP28 (with a
      pull-up) is the level-shift that makes it readable - the only place wires
      alone may fall short, and no firmware can change the threshold.
- **Keep the reset wire short.** Noise on it costs boot reliability.

## The one trade-off

Pure open-drain means the reset line rises through the console's pull-up rather
than being actively driven high. On the console tested (Trinity) this is the
default. If a specific console will not boot reliably open-drain, the 1-cycle
pull-up can be re-enabled in `glitch.pio` — but it is **off by default**
because it momentarily places 3.3V on the 1.1V net, which is the exact thing
that damages CPUs over time.

## Still your responsibility

- Back up the NAND before flashing anything.
- Double-check every solder point and that nothing is wired in reverse.
- These are old consoles; a bad board can fail for reasons unrelated to this.
