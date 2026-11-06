# EXT_CLK deep-dive — research findings (2026-11-05)

Source material read in full this session:

- **Octal450's EXT_CLK VHDL** — `github.com/Octal450/EXT_CLK`, the authoritative
  timing engine (authors: 15432, GliGli, Octal450). Read the
  `matrix-coolrunner-192mhz` and `-96mhz` variants: `post_proc.vhd`,
  `slower.vhd`, `divider_slow.vhd`, `main.ucf`.
- **wurthless-elektroniks/RGH1.3 + EXT+3** — `github.com/wurthless-elektroniks/RGH1.3`.
  Includes a **working RP2040 implementation** (`rgh13-rp2040-micropython/`,
  `.pio` + `.py`) and the EXT+3 VHDL. Read `storytime.md` (design post-mortem),
  `readme.md`, `install_ext3_xenon.md`, the slower/post_proc VHDL.
- ConsoleMods EXT_CLK page; PT Swarm "long way to RGH3".

Everything below is **from source** unless marked *(needs hardware)*.

---

## 1. Our current seed values are CORRECT (good news)

I had earlier doubted the `zephyr`/`xenon` seeds. The VHDL confirms them. This
is the exact mapping from Octal450's 192 MHz Matrix constants to our 5 slots:

| Our slot | Our seed (192 MHz) | VHDL constant | Verdict |
|---|---|---|---|
| post_count | 12 | `post_rgh = 24` **edges** | ✓ (12 pull/release loops ≈ 24 edges) |
| slow_delay | 73,344,000 | `delay_val 4475` → **382 ms** | ✓ (4475 × 4096 / 48 MHz = 0.382 s; 73,344,000 / 192 MHz = 0.382 s) |
| r_end | 118,008 (Z) / 118,002 (X) | `R_END` 118008 / 118002 | ✓ exact |
| pulse_width | 2 | `R_LEN = 2` | ✓ exact |
| extc_hold | 50,000 | `T_BUF = 50000` | ✓ exact |

So the numbers themselves are not the risk. **At 200 MHz** (our default), scale
the cycle-based ones by 200/192:

- slow_delay 73,344,000 → **76,400,000**
- r_end 118,008 → **122,925** (Zephyr), 118,002 → **122,919** (Xenon)
- extc_hold 50,000 → **52,083**
- post_count 12, pulse_width 2 — unchanged (not cycle-scaled)

(The current seeds are written at 192 MHz. If we ship EXT_CLK at 200 MHz like
Trinity, update the seeds; if we run the EXT_CLK profile at 192 MHz, leave them.)

## 2. How the real engine actually sequences (the subtlety that needs tuning)

Our `extclk.pio` is a **linear** translation: wait N POST pulses → wait
slow_delay → raise EXT_CLK_EN → wait 1 POST → wait r_end → pulse reset low →
release. Octal450's VHDL is **not linear** — it is two gated counters:

1. POST-edge counter (`post_cnt`): clears to 0 whenever it sees the reset line
   low; counts POST edges otherwise.
2. At `post_cnt ≥ 23`: request slowdown. The `slower` waits `delay_val`
   (382 ms) then raises **EXT_CLK_EN** *and* sets `callback = 1`.
3. At `post_cnt ≥ 24`: a cycle counter `cnt` runs up to `R_END`.
4. Reset is pulled low only while `cnt ∈ [R_END−2, R_END)` **AND** `callback = 1`,
   then driven high exactly at `cnt = R_END`.

The important difference: in the VHDL the **glitch is gated by the EXT_CLK_EN
callback**, and `R_END` (≈0.6 ms) is far shorter than the 382 ms slowdown — the
two counters interlock. Our linear PIO approximates this but the ordering of
"raise EXT_CLK_EN" vs "count r_end" is not identical. **This is the #1 thing to
verify/tune on hardware**, not the constants.

## 3. Reset drive: the central safety-vs-reliability decision *(needs hardware)*

This is the most important finding and it mirrors our Trinity result.

- On **Trinity (Slim)** we proved open-drain reset never boots (0/5); the glitch
  needs the reset line **driven high**. The Slim reset point tolerated 3.3 V in
  our tests.
- The **wurthless RP2040 code drives reset HIGH even on the phat 1.1 V line**,
  with this exact warning in their source:
  > "DANGER - this is interfacing 3v3 to a 1v1 line. the longer this is on, the
  > more damage you'll cause to your CPU. to be safe, clamp the reset line with
  > double diodes or a zener."
- Our `extclk.pio` is currently **pure open-drain** (pull low, release to float,
  never driven high) — the safe choice, and the one that fits the no-diode rule.

**The tension:** if Xenon/Zephyr behave like Trinity, pure open-drain may not
glitch at all — but driving 3.3 V into the 1.1 V reset net is what the RGH1.3
author says damages the CPU without a clamp. Resolution options, safest first:

1. **Try pure open-drain first** (current code). It cannot harm the console.
   If it boots, we're done and we kept the no-diode promise.
2. If it won't boot, drive-high **through a series resistor** (limits current
   into the 1.1 V net) — a resistor, not a diode, so arguably still "wires only"
   in spirit. Needs the reset-point voltage measured first.
3. Clamp with a diode/zener (what the scene does) — violates no-diode.

Do **not** silently switch EXT_CLK to driven-high; it's only safe on a net we've
characterised. Flag this to the user as a real decision.

## 4. RP2040-Zero is a worse glitcher than a plain Pico *(important)*

Straight from the RGH1.3 author's RP2040 source comments:

> "glitching performance is very poor on RP2040 Zero (can take 6+ tries) ...
> use a normal pico (or clone) instead."

We are building on the **RP2040-Zero**. This may be part of why Trinity won't
hold a clean 1-attempt boot and keeps drifting. Plausible causes: the Zero's
crystal/oscillator quality, power-rail filtering, and the onboard WS2812 drawing
current and injecting noise. **This is one developer's observation, not a law** —
but it fits our symptoms. Mitigations worth testing:
- keep the WS2812 dark during the glitch window (we already flicker blue — try
  LED fully off from arm until judge);
- clean 3.3 V, short wires away from the coils (ConsoleMods + wurthless both
  stress this);
- as a comparison point, try one plain Pico clone and see if the attempt count
  tightens. If it does, that reframes the whole reliability story.

## 5. STBY_CLK sync — the real fix for the cold/warm drift

The Matrix and all these chips **clock their glitch timing off the console's
STBY_CLK (48 MHz)**, not their own crystal. That is *why* they don't drift: the
countdown and the CPU share one clock. Our Pico free-runs on its own crystal, so
the Pico-vs-console crystal drift is exactly the ~3–7 ppm window movement we
measured on Trinity.

The RP2040 **can** clock `clk_sys` from an external signal on **GPIN0 (GP20)** or
**GPIN1 (GP22)**. Feeding STBY_CLK into GPIN0 and clocking the glitch timing from
it would remove the drift at its source — the single biggest potential
reliability win for the whole project, and it applies to RGH1.2 (Trinity) too,
not just EXT_CLK. Wiring cost: one extra wire (STBY_CLK → GP20). Worth a
dedicated experiment after EXT_CLK basics work. *(needs hardware; non-trivial
firmware change — the PIO clock divider and all cycle constants rescale to
48 MHz.)*

## 6. Confirmed wiring (Xenon/Zephyr), from the EXT+3 install

Glitch-chip pad letters → net (same as classic EXT_CLK):

- **A = /CPU_RST_1V1P_N** — reset, **1.1 V** ("1V1" confirms it)
- **B = FT6U1 = POST bit 0** — 1.1 V
- **C = STBY_CLK** — 48 MHz console clock
- **F = CPU_EXT_CLK_EN** — the slowdown enable (3.3 V-friendly)

Our pin plan (`main.c` / `docs/ZEPHYR_EXTCLK.md`) already matches: GP26 reset,
GP27 EXT_CLK_EN, GP28 POST, GP29 3.3 V arm-sense, plus STBY_CLK → GP20 if we do
§5. POST at 1.1 V still may not read on a 3.3 V GPIO (threshold ~2 V); the known
fix is a small-signal diode into GP28 with the internal pull-up on. That diode is
on the *input* only (safe), separate from the reset-drive question in §3.

## 7. The bigger "insta-boot" lever: EXT+3 / RGH1.3 (fast failure, not just better aim)

The wurthless post-mortem reframes "instant boot". On RGH1.2/EXT_CLK a *failed*
attempt costs a long SMC reset timeout — **3.5 s on Jasper, 5.5 s on Falcon** —
before the next try. RGH3/RGH1.3 instead **watch the boot via POST bits and reset
within ~1 s** when it sees a failure. EXT+3 is the EXT_CLK equivalent and cuts the
slowdown delay from **382 ms → 9.4 ms** (`delay_val` 4475 → 110), so attempts come
far faster.

Mechanism (for later; it needs modified images, not just timing):
- CB_X intermediate loader: glitch runs ~0.5 s sooner and any CB_B works without
  per-board timing files.
- Watchdog on POST bit 6 (CB_X emits 0x54; if it doesn't clear fast, reset now).
- POST bit 7 handles "uncooperative Jasper" (stuck at 0xDB) by power-cycling.

MatrixPico equivalent we *could* build (pure firmware, our advantage as a full
MCU vs a tiny CPLD): after the glitch, watch POST; if the boot-progress edges
stall within a timeout, **force a reset and retry immediately** instead of
waiting for the SMC. We already count POST edges for the LED judge — the same
signal can drive a fast-retry. This is probably a bigger perceived-speed win than
squeezing the hit-rate, and it's within reach without hacked images for the
"retry faster" half (the hacked-image half buys the rest).

## 8. Concrete plan for tomorrow

1. Decide EXT_CLK clock: ship the profile at **192 MHz** (seeds as-is) or 200 MHz
   (rescale per §1). 192 MHz is simplest and matches Octal450 1:1.
2. First hardware bring-up with **pure open-drain** reset (current `extclk.pio`),
   `mon` command first to confirm POST and the 3.3 V arm-sense actually read.
3. If POST doesn't read: add the one input diode on GP28 (§6).
4. If it arms but never boots: that's the §3 reset-drive decision — stop and
   discuss, don't auto-switch to 3.3 V drive.
5. Sweep `r_end` first (that's the live glitch-position knob), then `post_count`.
6. Keep the drift fix (§5, STBY_CLK → GP20) and fast-retry (§7) as the two big
   follow-on experiments once basic EXT_CLK boots.

## Open questions to resolve on hardware

- Does pure open-drain reset glitch on Xenon/Zephyr, or is driven-high required
  like Trinity? (§3 — the key one.)
- Does POST bit 0 at 1.1 V read on GP28 without a diode? (§6)
- Is the RP2040-Zero materially worse here than a plain Pico? (§4)
- Does the linear PIO sequence need reordering to match the gated VHDL? (§2)
