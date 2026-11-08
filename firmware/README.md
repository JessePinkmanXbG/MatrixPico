# Shipped firmware

| file | what it is |
|------|------------|
| `trinity.uf2` | Locked Trinity baseline (md5 1d4a532da94caa71646764a99f1db292). Built from commit `aa608d9` (2026-11-03): the original PicoRGH glitch engine at 200 MHz, no tracking, no self-cal, no extra features. Best-measured Trinity build: 17 of 30 boots on attempt 1, 29 of 30 within 2 attempts, never above 3 on the baseline runs. |

Files here are frozen once shipped: build, test on the console, then replace.
Do not recompile them in CI - a different build changed boot behaviour.

Safety note: this build drives the reset line high (original PicoRGH behaviour; open-drain
never booted on Trinity). The Trinity reset point reads as logic-high on a floating Pico
input (above ~2V, so not a 1.1V net), but it has NOT been measured. See docs/SAFETY.md.
