# Shipped firmware

| file | what it is |
|------|------------|
| `trinity.uf2` | Locked Trinity baseline (md5 1d4a532da94caa71646764a99f1db292). Built from commit `aa608d9` (2026-11-03): the original PicoRGH glitch engine at 200 MHz, no tracking, no self-cal, no extra features. Best-measured Trinity build: 17 of 30 boots on attempt 1, 29 of 30 within 2 attempts, never above 3 on the baseline runs. |

Files here are frozen once shipped: build, test on the console, then replace.
Do not recompile them in CI - a different build changed boot behaviour.

Safety note: this build drives the reset line high (original PicoRGH behaviour; open-drain
never booted on Trinity). The Trinity reset point reads as logic-high on a floating Pico
input (above ~2V, so not a 1.1V net), but it has NOT been measured. See docs/SAFETY.md.

## TEST BUILD on branch tool/measure (not the shipping branch)

`trinity.uf2` on this branch is a **measurement build**, md5 f11629637d4b8c77a2db852d00a2e510
(source: branch `baseline/led-orange`, commit 7f4bd87). It is the locked baseline glitch path
(aa608d9) plus: orange LED until a boot is confirmed (3 POST edges) then solid green, and a
timing log (`stats`, `clearstats`) used to choose a safe early-fail cutoff for faster retries.
The locked baseline (md5 1d4a532da94caa71646764a99f1db292) is on `fix/trinity-use-test7`.
