# Corona — RGH 1.2

Corona (Slim, 4GB) uses the **same RGH 1.2 engine as Trinity** — no new
firmware, just the `corona` profile and its own wiring points. Because the
Pico generates its own clock, the usual "Corona RGH 1.2 needs a glitch chip
with a built-in oscillator" requirement doesn't apply: there's no STBY_CLK /
oscillator pin to wire.

## Wiring (Pico GPIO → Corona point)

Same five wires as Trinity, at Corona's locations:

| Pico | Corona point |
|---|---|
| GP26 | CPU_RESET |
| GP27 | CPU_PLL_BYPASS (5–10K series resistor, like any Slim) |
| GP28 | POST |
| 3V3  | a switched 3.3V (J2C3-style power header) |
| GND  | GND |

Look up the exact RGH 1.2 Corona points for your board revision — they differ
from Trinity's positions. The signal set is identical.

## Dialing it in

The `corona` profile ships seeded with the Trinity values, which are **not**
right for Corona — they're just a runnable starting point. Tune with the
sweep tool exactly like Trinity:

1. `use corona`, connect over USB (Pico on USB power for tuning).
2. `mon` + power-cycle to confirm CPU_RESET and POST are live.
3. `sweep 3 30 70 1` to find a working `glitch_width` (low edge),
   then `sweep 3 70 30 -1` for the high edge, and `set 3 <center>`.
4. If it won't land at all, sweep `delay_to_rst` (idx 2) across a wide range.
5. `save` once it boots reliably.

## 4GB NAND note
The 4GB Corona NAND is flashed differently from a 16MB Trinity (eMMC / the
4GB method, noinmem). That's unrelated to the glitch chip — the Pico side is
identical to Trinity once the glitch NAND is on there.
