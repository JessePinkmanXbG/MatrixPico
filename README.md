# Matrix Pico

A serial-tunable **RGH 1.2** glitcher for the Xbox 360, running on a Raspberry
Pi Pico / RP2040. It does what a Matrix-style CPLD glitch chip does — counts
POST, times a reset glitch, asserts PLL bypass — but lets you **tune the timing
live over USB** and store a **named profile per console model**, so one board
can be dialed in across several Xbox 360 revisions without reflashing.

> Status: experimental. The Trinity profile ships with the original author's
> real timing values; Corona/Jasper/Falcon are seeded but **unverified** and
> must be dialed in. See [Tuning](#tuning).

## Why

Classic glitch chips are tuned by flashing a timing file. On a Pico that would
mean the BOOTSEL dance for every value change. Matrix Pico instead exposes the
five timing values over a USB serial console: change them, hit power, watch the
console boot (or not), repeat — seconds per attempt instead of minutes. Once a
profile boots reliably, `save` writes it to the Pico's flash so the glitcher
runs headless inside the console with no PC attached.

## Wiring (RGH 1.2)

| Pico pin | Xbox 360 point        | Notes                                             |
|----------|-----------------------|---------------------------------------------------|
| GP26     | CPU_RESET             | the glitch pulse — keep this wire **short**. The line is 1.1V, so the firmware leaves it floating except for the pulse plus one ~5 ns pull-up cycle |
| GP27     | CPU_PLL_BYPASS        | **series resistor**: 5–10K on Slim, 22K on Phat   |
| GP28     | POST                  | monitored bit (hard-coded as GPIO 28 in the PIO)  |
| GP16     | onboard RGB LED       | RP2040-Zero WS2812: blue = glitching, green = booted |
| 3V3      | switched 3.3V         | a rail that is live only when the console is **on** |
| GND      | GND                   |                                                   |

Feed the motherboard's 3.3V into the Pico's **3V3 pin**, not 5V/VSYS. Point
locations differ per motherboard — look up the RGH 1.2 points for your
revision. The reset line is pure open-drain (never driven to 3.3V), so no
clamp diode is needed for the reset net. **Read [docs/SAFETY.md](docs/SAFETY.md)
before wiring** — the series resistor and the power rules there protect the
console.

RGH 1.2 via PLL bypass is the right method for **Trinity, Corona, Falcon and
Jasper**. **Xenon and Zephyr** are unreliable on plain RGH 1.2 and want the
EXT_CLK method instead (a different point and different firmware) — not covered
by this build yet.

## Serial console

Connect to the Pico's USB serial port (115200, any terminal — `screen`,
`minicom`, PuTTY, the Arduino serial monitor).

```
help                 show commands
status               active profile + its 5 timing values
list                 list all profiles
show <name|idx>      print one profile's values
use <name|idx>       make a profile active
set <idx> <value>    set timing <idx> (0-4) on the active profile
name <newname>       rename the active profile
save                 write all profiles to flash (persists across power loss)
load                 reload profiles from flash (discard edits)
defaults             restore built-in defaults
attempts             reset the attempt counter
```

Timing indices:

| idx | name          | role                                              |
|-----|---------------|---------------------------------------------------|
| 0   | post_count    | POST pulses to count before arming                |
| 1   | delay_to_pll  | cycles from the counted POST to PLL bypass up     |
| 2   | delay_to_rst  | cycles from POST high to CPU_RESET pulled low     |
| 3   | glitch_width  | cycles CPU_RESET is held low (the glitch itself)  |
| 4   | pll_hold      | cycles PLL bypass is held after the glitch        |

Edits are live immediately (the glitch loop picks them up on the next attempt)
but are **not** persisted until you `save`.

## Tuning

1. `use trinity` (or `corona`) to select the board on your bench.
2. Power the console. Watch for a boot (XeLL / dashboard) and note the time.
3. If it doesn't boot, nudge `glitch_width` (idx 3) and `delay_to_rst` (idx 2)
   in small steps and try again.
4. When it boots reliably within a reasonable number of retries, `save`.

A failed glitch is harmless — the console just resets and retries; bad values
cost boot time, not hardware.

## Build

Needs the Pico SDK and the ARM GCC toolchain.

```sh
mkdir build && cd build
cmake ..          # PICO_SDK_PATH must point at the SDK
make
```

The flashable `MatrixPico.uf2` lands in `build/`. Hold BOOTSEL, plug the Pico
in, copy the `.uf2` onto the `RPI-RP2` drive. CI also builds the `.uf2` on every
push and attaches it as a workflow artifact (see `.github/workflows/build.yml`).

## Board profiles & methods

Select a profile with `use <name>`; each carries its own method and timings.

| Profile | Method | Status |
|---|---|---|
| trinity | RGH 1.2 | proven (balika011's values) |
| corona | RGH 1.2 | seeded — [tune it](docs/CORONA.md) |
| falcon / jasper | RGH 1.2 | seeded — tune like Corona |
| zephyr | EXT_CLK | experimental — [hardware-test build](docs/ZEPHYR_EXTCLK.md) |

RGH 1.2 and EXT_CLK are separate PIO engines; the firmware loads the right one
for the active profile. On every method the reset line is open-drain (pulled
low then released, never driven to 3.3V), so the 1.1V reset net is protected
without clamp diodes.

## Credits

Matrix Pico stands entirely on earlier work:

- **GliGli** and **Tiros** — the original Reset Glitch Hack.
- **Balázs Triszka (balika011)** — [PicoRGH](https://github.com/X360Tools/PicoRGH),
  the RP2040 RGH 1.2 implementation this is built on. The glitch PIO program is
  his, unmodified.
- **X360Tools** — hosting and the Trinity work.
- **Octal450** — modern RGH timing-file research.
- The wider Xbox 360 homebrew scene, whose shared timing knowledge made the
  per-board profiles possible.

For the EXT_CLK engine (zephyr profile):

- **15432, GliGli, Octal450** — [EXT_CLK](https://github.com/Octal450/EXT_CLK),
  the open-source VHDL the timing and sequence were decoded from.
- **wurthless-elektroniks** — [RGH1.3 / EXT+3](https://github.com/wurthless-elektroniks/RGH1.3)
  and its RP2040 reference (public domain), the basis for the open-drain
  reset and the 1.1V signal-level handling.

New in Matrix Pico: the USB serial tuning console, per-board profiles, the
sweep auto-tuner, flash persistence, the open-drain reset safety change, and
the two-core split that keeps the console responsive.

## License

GPL-2.0, inherited from PicoRGH. See [LICENSE](LICENSE). If you build on this,
keep it open and keep the credits.
