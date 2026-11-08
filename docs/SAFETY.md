# Hardware safety

Matrix Pico interfaces a 3.3V microcontroller to console nets that are as low
as 1.1V. This is the summary of what the firmware does to avoid harming a
console over time, and the wiring rules that keep it safe.

## What the firmware does

- **RGH 1.2 (Trinity, Corona, Falcon, Jasper) uses the original PicoRGH engine.**
  Once the console itself has raised its reset line, the Pico drives CPU_RESET
  high (matching the console), pulls it low for the glitch pulse, then releases
  it. The pin goes back to a plain input as soon as the glitch finishes, and the
  Pico never drives it while the console is holding reset low (arming waits for
  the console to raise reset first).
- **Why it is not open-drain.** We built and bench-tested a floating
  (open-drain) reset. The Trinity never booted with it (0 of 5), and the original
  program boots reliably (9 of 10 first attempts). The reset line has to be
  driven up, as in every other RGH 1.2 chip.
- **Is the reset point 3.3V?** We have no multimeter reading yet. The evidence:
  the Trinity reset point reads as logic-high on the Pico's input while
  floating (above ~2V, so not a 1.1V net); RGH3 pulses it from the SMC (a 3.3V
  part); and the original PicoRGH has driven it this way for years. Treat this
  as likely, not proven - **measure the reset point (with the console on, against
  ground) before wiring if you can.** If it reads near 1.1V, do not use this firmware.
- **The slowdown line (PLL_BYPASS / EXT_CLK_EN) is driven LOW (inactive) from
  startup**, and only goes high during the glitch window. It goes through the
  series resistor (below), which limits the current.
- **POST is input-only.** The firmware never drives it. The pad keeps the
  RP2040 default weak pull-down.
- **EXT_CLK (Zephyr/Xenon) still uses the open-drain engine** (not yet
  hardware-tested).
- A firmware hang can leave the reset line pulled low or released; it cannot
  hold the line high past the end of an attempt.

## Wiring rules that keep it safe (do not skip)

- **Series resistor on the slowdown line.** PLL_BYPASS: 5–10K on Slim
  (Trinity/Corona). EXT_CLK_EN / phat PLL: 22K. This limits current into the
  console net — it is a safety part, not optional.
- **USB or console power on the 3V3 pin - never both.** Two 3.3V sources
  fight on the Pico's 3V3 pin. Flash with the console off. To tune over USB,
  take the console's 3V3 wire off the Pico. For standalone use, feed the
  console's 3.3V into the Pico's 3V3 pin and leave USB unplugged while the
  console is on. Most users never need USB after flashing: the tool flashes the
  Pico with the console off.
- **POST (GP28) is safe to wire directly, no diode.** GP28 is an
  input only (never driven; the default weak pull-down only), so it can't drive
  the POST net on any console and only sinks microamps from it.
  Wiring it straight is electrically harmless.
    - *Reading* is the catch, and it is silicon, not firmware: the Pico's input
      high-threshold is ~1.5V. Slim POST (Trinity/Corona) reads fine on wires.
      Phat POST (Xenon/Zephyr/Falcon/Jasper) is ~1.1V, which is **below** that
      threshold, so it often reads as always-low on a bare wire. If `mon` shows
      POST stuck low on a phat board, a small-signal diode into GP28 (with a
      pull-up) is the level-shift that makes it readable - the only place wires
      alone may fall short, and no firmware can change the threshold.
- **Keep the reset wire short.** Noise on it costs boot reliability.

## Still your responsibility

- Back up the NAND before flashing anything.
- Double-check every solder point and that nothing is wired in reverse.
- These are old consoles; a bad board can fail for reasons unrelated to this.
