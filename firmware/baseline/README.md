# Trinity baseline (locked)

File: trinity_baseline_1d4a532d.uf2
md5:  1d4a532da94caa71646764a99f1db292

Source: commit aa608d9 ("Add 'mon' command to watch CPU_RESET/POST pins live", 2026-11-03).
The binary matches a rebuild of that commit byte-for-byte except for the build-date string.

Results on the test console:
- 17 of 30 boots on attempt 1 (57%), 29 of 30 within 2 attempts.

SAFETY NOTE: this build drives the reset line high (original PicoRGH engine), the same
as every Trinity build we have bench-tested; open-drain reset never booted on Trinity
(0 of 5). The Trinity reset point reads as logic-high on the Pico input while floating
(above ~2V, so not a 1.1V net), which suggests a 3.3V reset point, but this is NOT
measured. See docs/SAFETY.md. Measure the reset point against ground with the console on
before relying on it; if it reads near 1.1V, do not use this firmware.

Do not change this file. Any new work goes in a separate build and must beat this baseline
on a 20-boot run before it replaces it.
