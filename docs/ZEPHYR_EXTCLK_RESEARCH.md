# MatrixPico — Zephyr EXT_CLK bring-up: research & verification

**Date:** 2026-11-16  ·  **Deadline target:** 48 h (by 2026-11-18)
**Purpose:** verify, against authoritative sources, that our wiring and method are correct before we spend more nights sweeping — and convert the findings into actionable next steps.

---

## Update (session 2) — the likely real bug

- **NAND is ruled out:** the console boots with a real Matrix V3, so bootchain/XeLL/wiring are all proven. This is a firmware/resolution problem, not a prerequisite one.
- **It was full EXT_CLK, not EXT+3** → `delay_val=4475`, Octal450's Zephyr window `117998–118010` @192 MHz.
- **The glitch window is only ~3–5 ns wide** (logic-analyzer RE of a real Matrix: POST-anchored timer, reset pulse fired to ~ns precision, 5.208 ns effective resolution). **Our autosweep stepped r_end in 8-unit (~42 ns) jumps — ~10× wider than the window — so we skated over it almost every attempt.** This, not the r_end value, is the most likely reason a *proven* console never booted.
- **Fix shipped:** `zephyr_dense_autorun.uf2` steps r_end by **1 unit (~5 ns)** across the known-good window, hedges the anchor ±1 pulse, auto-runs with no screen (console-powered), shows status on the LED, and auto-saves the landing. See `docs/WAKE_UP_STEPS.md`.
- **USB power** (user's question): not the main blocker, but real — a 10 ns pulse wants a short common ground; powering the Pico from the console's 3.3 V standby (like a Matrix) removes the variable.

Updated suspect order: **(1) r_end step resolution vs the 3–5 ns window** ← fixed in this build; (2) POST-edge fidelity / anchor; (3) ground/USB power; (4) anchor offset.

---

## 0. TL;DR (read this first)

1. **Wiring is correct.** Our four glitch wires (RST, POST, STBY_CLK, EXT_CLK) exactly match the official Matrix EXT_CLK pinout, and 48 MHz STBY_CLK is the documented value. Nothing to change on the solder side.
2. **The method is real and our structure is close** — EXT_CLK is Octal450's open-source method specifically for **Xenon and Zephyr**.
3. **I have to correct myself on r_end.** The shipping Matrix VHDL is unambiguous: the CPU is **slowed, not stopped**, and the reset fires **r_end = 118008 ticks @192 MHz = ~29,502 STBY periods (~614 µs)** after the POST anchor. My "clock-stop → small r_end" reframe from last night was a **wrong read of the 0-POST test** (a CPU slowed ~128× just *looks* stopped over a 20 s window). **The small-r_end sweep will not boot — stop it.** The correct r_end zone is ~29,502, which we *did* sweep — so the blocker is not the r_end value.
4. **The #1 suspect is now a prerequisite, not timing:** the console's **NAND must already be flashed with a glitch image (Glitch2 + RGH3, containing XeLL).** A glitch on a **retail/stock NAND boots nothing**, no matter how perfect the timing. This single item explains "everything verified, still no boot" better than anything else. **→ Go/No-Go #1 below.**
5. **Path forward:** confirm the NAND, then stop blind-sweeping and **port the Matrix EXT+3 FSM 1:1** (POST-reset feedback, slowdown delay, reset gating) instead of our simplified 2-phase engine.

---

## 1. Verified wiring (authoritative)

Official Matrix EXT_CLK chip pinout (ConsoleMods): **A = RST, B = POST, C = STBY_CLK, F = EXT_CLK.** The Matrix CPLD pin map (wurthless EXT+3 `main.ucf`) confirms the same four nets: `RST` (P8), `POST` (P40), `CLK`=STBY (P29), `EXTC`=EXT_CLK (P22), plus `DBG` (P27).

| Signal | Matrix pin | Our Pico pin | Zephyr motherboard point | Notes |
|---|---|---|---|---|
| **CPU_RESET** | A (RST) | SET base bit0 | CPU_RST bottom pad | Open-drain: drive LOW to glitch, float to release. Matrix uses slow slew. ✅ matches ours |
| **POST** | B (POST) | GP28 (diode + pull-up) | POST, bottom pad (or top, scrape) | CPLD reads **POST bit 0 only**; schmitt input. Our diode read is the right idea — **fidelity is the risk** (see §4) |
| **STBY_CLK** | C (CLK) | GP15 | STBY_CLK (non-Xenon: top or bottom) | **48 MHz confirmed** as the documented value. ✅ our 47.97 MHz reading is correct |
| **EXT_CLK_EN** | F (EXTC) | SET base bit1 | EXT_CLK bottom pad | **Static enable**, not a driven clock (see §2). ✅ our approach is structurally right |
| 3.3 V / 5 V / GND | — | — | standard | SDA/SCL are **ignored** with EXT_CLK |
| *RESET_SENSE (GPU reset done)* | *(not a Matrix wire)* | GP29 | R3P6/R3P7 area | **Our addition** for auto-boot detection only. A real Matrix doesn't use it — fine to keep for the sweeper |

**Takeaway:** every glitch wire matches the documented Matrix install. No wiring change needed.

---

## 2. How EXT_CLK actually works (the correction)

- EXT_CLK is **Octal450's open-source method** that uses the **EXT_CLK_EN point instead of CPU_PLL_BYPASS** to slow the CPU on **Xenon/Zephyr**.
- **EXTC is a static level, not a clock.** In `slower.vhd`, `EXTC <= slow` where `slow` is a 0/1 level. The `divider_slow`/`clk_doubler` blocks only generate internal timing references; they do **not** feed a clock onto the EXT_CLK wire. So asserting EXT_CLK_EN high = "slow the CPU now," exactly like PLL_BYPASS. **Our EXT_CLK_EN output is the right mechanism.**
- **Slowed, not stopped.** PT Security measured PLL_BYPASS as a **~128× slowdown** on fat boards (slim ~640×). EXT_CLK is the same style of bypass. A CPU slowed ~128× crawls so slowly that **it produces no POST codes within a 20 s window** — which is exactly what our `extctest`/hold-from-power-on tests showed. That test proved EXT_CLK_EN *works*; it did **not** prove the CPU stops. **My small-r_end conclusion was wrong.**
- **The glitch target (what the pulse actually corrupts):** PT Security found the reset pulse does **not** flip the branch (`cmpwi/beq`) — it corrupts an **arithmetic instruction (`addi`/`mr`) that zeroes a source register**, so the `memcmp` of the next bootloader's SHA-1 reads equal and the signature check passes.

### The real Matrix timing (Zephyr), from the shipping VHDL

`post_proc.vhd` (EXT+3, Matrix/Coolrunner), counting at **192 MHz** (STBY 48 MHz → `clk_doubler` ×2 → both edges ×2):

| Constant | Value (Zephyr) | Meaning |
|---|---|---|
| `post_rgh` | **24** | start the r_end counter when post_cnt reaches 24 **POST edges** |
| `R_END` | **118008** | reset fires at this tick @192 MHz = **~614.6 µs** = **~29,502 STBY periods** |
| `R_LEN` | **2** | reset pulse width = 2 ticks @192 MHz ≈ **10.4 ns** (pulse of 2–3 works; wider/shorter fails) |
| `T_BUF` | 50000 | timeout buffer after R_END |
| `post_max` | 31 | cap on post_cnt |
| `delay_val` (`slower.vhd`) | **110** (EXT+3) / 4475 (full EXT_CLK) | EXT_CLK_EN asserts this many **÷(2048·2) STBY clocks** after post_cnt=23 → **~9.4 ms** (EXT+3) / ~382 ms (full) |

Xenon differs only slightly: `R_END = 118002`.

**Two subtleties our 2-phase engine does not reproduce (likely fidelity gaps):**
1. **POST-reset feedback:** in the VHDL, when RST pulses low, `post_cnt` resets to 0. The FSM self-regulates against this; our engine fires once and stops.
2. **Slowdown is delayed and the reset is gated by `callback`:** the Matrix asserts EXT_CLK_EN ~9.4 ms (EXT+3) after the anchor and only arms the reset once that slowdown is live. We assert EXT_CLK_EN immediately at the anchor. The *relative* placement of slowdown vs. memcmp may be off.

---

## 3. Prerequisites — the Go/No-Go that gates everything

Confirmed by Free60, ConsoleMods, and PT Security: a glitch only makes a **modified bootchain** pass its signature check. There must *be* a modified bootchain on the NAND to boot.

### ✅ / ❌ GO/NO-GO #1 — Is the NAND flashed with a glitch image?
- The console needs a **custom NAND image** containing modified SMC, CB_A, CB_X, patched CB_B, CD, and **XeLL** — built in **J-Runner with Glitch2 + RGH3 enabled** — and **written to the console** before glitching.
- **If this Zephyr still has its retail NAND, no timing will ever boot it.** The glitch would be making a *retail* CB pass, which just continues a normal (locked) boot — there is nothing hacked to land in, and XeLL isn't there to show success.
- A successful glitch on a properly-flashed console **boots XeLL within ~2 minutes** (our >12 s reset-hold detector is consistent with catching that).

**Question for you:** Has this specific Zephyr been **dumped, had an ECC/Glitch2+RGH3 image built, and flashed back** (i.e. would it boot XeLL with a known-good Matrix)? If not — that's almost certainly why nothing boots, and it's a NAND-flash task, not a timing task.

### Other prerequisites (verified present or N/A)
- **Chip compatibility:** EXT_CLK supports Coolrunner/Matrix/X360ACE. We're emulating a Matrix — fine.
- **Timing files:** only X360ACE needs separate timing files; Matrix timing is the VHDL we already have.
- **Zephyr quirk (informational):** Zephyr/Xenon "mostly fail when CPU_PLL_BYPASS is de-asserted," which is why there's no RGH3 for them — not a blocker for EXT_CLK, but it's why EXT_CLK (not PLL-bypass RGH3) is the correct method for this board. We're on the right method.

---

## 4. Why we're not booting — ranked suspects

Given wiring is verified and r_end ~29,502 was already swept:

1. **NAND not flashed with a glitch image (retail NAND).** Best single explanation for "perfect everything, no boot." → Go/No-Go #1.
2. **POST-anchor fidelity.** The Matrix counts *every* edge of POST bit 0 through a schmitt input. If our diode+pull-up misses or doubles any bit-0 toggle, our `post_cnt` drifts off the real anchor and the r_end window lands in the wrong place. Worth scoping the POST line quality (rise time into GP28).
3. **FSM fidelity.** Our 2-phase engine omits the POST-reset feedback and the delayed/gated slowdown (§2). A faithful 1:1 port behaves like a real Matrix.
4. **r_end step vs. window.** The success window is ~10–40 ns; 2-STBY-period steps (41.6 ns) can step over it. Fine-step or dither the fine delay.

Note: r_end **value** is *not* a top suspect anymore — the VHDL pins it at ~29,502 and we swept there.

---

## 5. Recommended next steps (within 48 h)

**Step 1 — Settle Go/No-Go #1 (NAND).** Confirm the Zephyr is flashed with a Glitch2+RGH3 image (would boot XeLL on a known Matrix). If unsure: dump NAND ×2 in J-Runner, build the image with Glitch2+RGH3, flash it back. **Do this before any more sweeping.** Everything below assumes a glitch-ready NAND.

**Step 2 — Abandon the small-r_end sweep** (my error). Re-center on r_end ≈ 29,502.

**Step 3 — Port the Matrix EXT+3 FSM 1:1** instead of the 2-phase engine:
- post counter on POST bit 0 with the **post_cnt=0-on-RST feedback**;
- r_end counter at 192 MHz-equivalent (STBY ×4), `R_END=118008`, `R_LEN=2`;
- slowdown asserted via the `slower` delay (`delay_val=110`), reset **gated by `callback`**;
- this reproduces the known-good Zephyr behavior rather than approximating it.

**Step 4 — Verify POST-line fidelity** (scope or Pico-capture rise time at GP28) so `post_cnt` tracks the real anchor exactly.

**Step 5 — Only then** fine-sweep the small residual uncertainty (fine delay dither around 29,502), not the coarse r_end.

---

## 6. Sources

- [Reset Glitch Hack — Free60 Wiki](https://free60.org/Hacks/Reset_Glitch_Hack/)
- [RGH / EXT_CLK — ConsoleMods Wiki](https://consolemods.org/wiki/Xbox_360:RGH/EXT_CLK)
- [Xbox 360 security in details: the long way to RGH3 — PT Security](https://swarm.ptsecurity.com/xbox-360-security-in-details-the-long-way-to-rgh3/)
- [wurthless-elektroniks / RGH1.3 (EXT+3 Matrix VHDL)](https://github.com/wurthless-elektroniks/RGH1.3) — `post_proc.vhd`, `slower.vhd`, `divider_slow.vhd`, `main.ucf` (local copy analyzed)
- [Octal450 / EXT_CLK](https://github.com/Octal450/EXT_CLK/) — original method (referenced by the above)
- [kooscode / srgh-matrix-trinity (logic-analyzer reverse-engineering)](https://github.com/kooscode/srgh-matrix-trinity)
