# MatrixPico Zephyr — deep analysis of runs 1–4 (2026-11-17, overnight)

Written after the captureslow data + the "console rebooting, LED stuck red" report on run 4.
Everything here is traced from code or measured data, not inferred.

---

## Headline: runs 1–3 never actually fired the glitch

This is code-certain, from `extclk.pio` + the push logic in `main.c`.

The EXT_CLK PIO engine does this in order:
1. `phase1`: count `ph1` POST pulses at full speed.
2. `set pins 0b10` — **assert EXT_CLK_EN (slows/stops the CPU)**.
3. `phase2`: if `ph2 > 0`, **`wait 1 gpio 28` / `wait 0 gpio 28`** — wait for more POST pulses.
4. count r_end, fire the reset pulse.

Runs 1–3 used `ph1 = anchor-1, ph2 = 1`. So after step 2 asserts EXT_CLK_EN, step 3 **waits for a POST pulse that can't come** — because EXT_CLK_EN has just stopped the CPU (captureslow proved this: zero POST for ~7 s). The PIO hangs at `wait 1 gpio 28`, holding EXT_CLK_EN high, until the **SMC watchdog reboots the console ~7 s later**; the reboot's POST finally satisfies the wait, and only *then* does the reset fire — during the reboot, nowhere near the signature check.

**So every run 1–3 attempt was a ~7 s watchdog cycle that fired the reset at the wrong moment. No real glitch was ever delivered.** All the anchor/r_end "tuning" from those runs was on a chip that wasn't glitching — which is why nothing landed and why the behavior looked like benign cycling.

## Run 4 (ph2 = 0) is the first real glitch

Run 4 set `ph1 = anchor, ph2 = 0`. The PIO's `jmp !x anchor_done` skips phase2 entirely, so it goes straight from asserting EXT_CLK_EN to counting r_end and firing. **This is the first build that fires the reset at the intended time** (r_end after the freeze at the measured anchor).

The "console rebooting itself, LED stuck red" is therefore the **first real feedback from an actual glitch attempt**. The reset pulse is now hitting the console every attempt. It's either glitching-but-not-landing, or the pulse/state is resetting the console — and the fast cycling makes red (1 s per miss) dominate the LED.

## What's confirmed, with certainty

- **EXT_CLK_EN (EXTC) is a static level, not a clock.** `slower.vhd`: `EXTC <= slow`, where `slow` is a 0/1 signal. So our static-assert approach matches a real Matrix exactly — this was never a "drive a clock" problem.
- **EXT_CLK_EN slows the CPU extremely hard** (captureslow: 0 POST for ~7 s, then the SMC watchdog reboots on a ~7.2 s period, repeating identically). The CPU is running at ~kHz, not stopped — classic RGH needs a slowed-but-running clock, and this is it.
- **The anchor is measured and rock-solid: the check is right after POST edge #26 (= 13 pulses), identical on consecutive boots.** POST fidelity is not our problem.
- **r_end ≈ 29,502 should be correct** *if* edge-26 is the Matrix's anchor and the slowdown factor matches (it's the same console hardware, so it should). The ±200 ns dense sweep covers it.

## The real open question (where the Matrix differs from us)

A real Matrix is a **closed-loop, self-timed FSM**. Reading the full VHDL:
- `slower.vhd` asserts EXT_CLK_EN a programmed delay (`delay_val`) *after* the POST anchor — **382 ms for full EXT_CLK** (the timing your Matrix V3 uses), 9.4 ms for EXT+3.
- The reset is **gated by `callback`** (only armed once the slowdown is live).
- `post_proc.vhd` **resets its POST counter to 0 whenever the reset fires**, and has a `timeout` path — so the whole thing is a feedback loop that re-tries across console reboots until the slowdown + reset land on the memcmp.

**Our firmware implements none of that feedback.** It's open-loop: count our POST pulses, assert EXT_CLK_EN immediately, wait a fixed r_end, fire once. That's a fundamentally simpler machine than the Matrix's self-timing, and it's the most likely reason a *proven* console still won't boot even now that we're actually firing.

## Confident next steps (in order)

1. **Fully AC power-cycle the console** (unplug 30 s). Hours of interrupted boots, a 15 s EXT_CLK_EN hold during captureslow, and repeated resets can leave the SMC/boot in a confused reset-looping state. Clear it before trusting any run's behavior.
2. **Run the hardened build and *time the cycle*.** If each attempt is ~3–4 s with the console doing a full boot between tries, it's making real attempts (good). If it's sub-second, the console isn't reaching the check and we're spinning — which points straight at #3.
3. **Port the Matrix FSM faithfully** — the real fix. Implement the `delay_val`-positioned slowdown, the `callback` gate, and the POST-reset feedback so our Pico self-times like the chip that actually boots this console, instead of firing open-loop. This is the path that removes the guesswork for good.

## Firmware hardening already applied

- **POST-count timeout**: if the console never POSTs to the anchor within 20 s, the attempt aborts, releases EXT_CLK_EN, and re-arms — so it can never get stuck waiting (the "solid blue then stuck" case).
- **LED**: solid blue while attempting, clear 1 s red per miss, **latched green that stays on** for a boot (so an overnight win is still visible in the morning).

---

*Bottom line: we spent runs 1–3 tuning a chip that wasn't firing. Run 4 fires for the first time. The anchor is measured, EXT_CLK_EN is correct, r_end should be right — the remaining gap is that we're an open-loop approximation of a closed-loop self-timed Matrix, and the fix is to port that self-timing.*
