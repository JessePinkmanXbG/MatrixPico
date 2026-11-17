# The Matrix EXT_CLK mechanism — solved by simulation (2026-11-17)

I ran the real Matrix FSM (`post_proc.vhd` + `slower.vhd` + `divider_slow.vhd`) in GHDL
and watched the signals. This resolves the apparent deadlock and tells us the one thing
every one of our builds was missing.

## What the simulation showed

Feeding POST so the CPU reaches `post_cnt = 23` and then **dwells** there:

```
  t = 6.4 µs     to_slow  -> 1     (post_cnt hit 23, the code before the check)
  t = 9.43 ms    EXTC     -> 1     (slowdown asserts: 9.4ms = slower.vhd delay_val)
  t = 9.43 ms    callback -> 1     (reset now armed)
  t = 12.00 ms   (post_cnt -> 24, the check; R_END timer starts)
  t = 12.62 ms   RST      -> 0     (GLITCH fires, 614µs after the check edge)
  t = 12.62 ms   RST      -> 1     (released)
```

## The mechanism (now certain)

1. Count POST to **post_cnt = 23** — the POST code right *before* the long pre-check gap.
2. **DWELL** there for `delay_val` (slower.vhd): **9.4 ms** for EXT+3, **382 ms** for full
   EXT_CLK. This dwell is spent *inside the gap*, CPU still at full speed.
3. **THEN** assert EXT_CLK_EN (slow the CPU) — so the slowdown lands inside the gap,
   right before the check.
4. The slowed CPU advances to **post_cnt = 24** (the check). The R_END timer starts off
   that edge.
5. **614 µs later the reset fires** into the slowed signature check.

The deadlock I kept hitting on paper was because I assumed the CPU races past post_cnt=23.
It doesn't — it **dwells** there (the long POST gap your capture showed: 262 ms and 415 ms
right before the check). The 382 ms `delay_val` for full EXT_CLK is sized to fit inside
your 415 ms gap. That dwell is the whole trick, and **no build of ours ever did it** — we
asserted EXT_CLK_EN instantly every time.

## What this means for the port

The new engine (`extclk.pio`, this build) implements the exact mechanism:
`count to the pre-gap code -> DWELL delay_val periods -> assert EXT_CLK_EN -> wait for the
slowed check edge -> time R_END -> pulse reset`. It fits in the PIO and is parameterised:

- `t[0]` = post_count (anchor to the pre-gap code)
- `t[1]` = **dwell periods** (the delay_val — the new, critical knob)
- `t[2]` = r_end (192 MHz units)
- `t[3]` = pulse width, `t[4]` = extc_hold

## Why we capture the real Matrix first (the spy)

The engine is exact; the three numbers (anchor, dwell, r_end) are not yet pinned for *your*
console, and guessing three coupled values blind is exactly the trap we were in. So instead
of guessing, we read them off the chip that already works: `matrixspy` watches your real
Matrix V3 glitch this console and timestamps **when it asserts EXT_CLK_EN relative to POST**
— which is the anchor and the dwell, directly measured. Then we set `t[0]`/`t[1]` to the
measured values and only fine-sweep `t[2]` (r_end), which is already known to ~29,502.

**That is the flawless path: measure the working chip, copy its timing, done — no three-way
blind search.**

## Morning plan

1. **Power-cycle the console clean** (unplug 30 s).
2. **Run `matrixspy`** with the real Matrix V3 installed and the Pico tapping EXT_CLK(GP27)/
   POST(GP28)/RST_SENSE(GP29). Capture it glitching to a boot. Send me the output — the line
   `Matrix asserted EXT_CLK_EN after POST edge #N at t=… ms` is the gold.
3. I set the dwell + anchor to the measured values; you run `zephyr_fsmport.uf2` and we
   fine-sweep r_end. With the timing copied from a working chip, this is the highest-
   probability shot we've had.
