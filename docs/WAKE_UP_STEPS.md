# MatrixPico Zephyr — do this when you wake up

**File to flash:** `zephyr_dense_autorun.uf2`
**What it is:** the real fix. It auto-runs on power-up (no screen needed), steps r_end in ~5 ns increments across Octal450's known-good Zephyr window, and stops + goes solid green on a boot.

---

## Why this one is different (the actual bug we found)

A logic-analyzer tear-down of a real Matrix shows the signature-check glitch window is only **~3–5 ns wide**. Every sweep we've run stepped r_end in **~42 ns jumps** — so we were stepping *over* the window almost every single time, even though the center value (29,502 STBY periods / 118008 @192 MHz) was right. Your console is proven (boots with a Matrix V3), so this was never a NAND/wiring problem — it was resolution. This build steps **1 unit (~5 ns)** across the exact known-good window and leans on the natural per-reboot retries + clock jitter to land it, the same way a real Matrix does (~25% per try).

---

## 1. Power the Pico from the console (recommended)

Feed the console's **3.3 V standby** into the RP2040-Zero's **3V3 pin** (same rail a real Matrix runs on). Keep a short, solid **GND** between Pico and console near the glitch points.
- **Don't** also plug in USB while it's on console power (two supplies back-feed). Console power **or** USB, not both.
- This removes the USB-ground variable you flagged and keeps the Pico synced to the console across its retry reboots.

If you'd rather stay on USB for this first run, that's fine too — just use a wall brick (not a PC) and keep that short dedicated ground.

## 2. Flash + go

1. BOOTSEL → drag `zephyr_dense_autorun.uf2` onto RPI-RP2.
2. Power the Pico from the console (or USB brick).
3. **It starts automatically — no typing.** Nothing else to do.
4. Power on the console and **leave it**. The console will boot-fail-reboot on its own; each reboot is one glitch attempt and the Pico steps to the next timing.

## 3. Watch the onboard LED

| LED | Meaning |
|---|---|
| **Blue flicker** | arming / attempting (normal, this is what you'll see most of the time) |
| **Red blip** | that attempt missed, stepping to the next timing |
| **SOLID GREEN (5 s)** | 🎉 **BOOT.** The glitch landed, the console is up, and the winning timing was saved to flash |

Leave it running for a few hours if needed — it revisits every timing in the window many times, which is exactly what it needs to catch a 3–5 ns window.

## 4. When it goes green (what success looks like)

- The console stays booted; the Pico saved the winning **r_end + anchor** to flash automatically.
- To read the exact numbers: power the Pico from **USB** (console off), open the serial console, and the banner prints:
  `*** previous successful boot saved: r_end=XXXXX anchor=YY pulses ***`
- **Send me those two numbers.** I'll bake them into a permanent "boot every time" build (fixed timing, no sweep, no halt) — that's the finished chip.

## 5. If it runs all night with no green

Tell me **"no boot, ran full night"** and I'll move to the next lever, in order:
1. **Widen the window** — sweep a bit outside 117998–118010 in case our POST anchor sits one code off a real Matrix's.
2. **POST-edge fidelity** — our 3.3 V diode read of the 1.1 V POST line may miss/double an edge and shift the anchor; I'll add a quick POST-quality check and, if needed, a cleaner read.
3. **Anchor offset** — try post anchor ±2 pulses.

Either way we'll know a lot from one overnight run: a green means we're basically done; a clean no-boot at the known-good values points straight at POST fidelity.

---

### Quick reference
- Full EXT_CLK (not EXT+3), Zephyr: r_end center **118008** @192 MHz = **29,502 STBY periods** = ~614.6 µs; pulse **R_LEN=2** (~10 ns); anchor **post_rgh=24 edges = 12 pulses**.
- Branch: `zephyr/extclk-sync` · file `docs/WAKE_UP_STEPS.md`
