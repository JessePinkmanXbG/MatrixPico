# Trinity baseline (locked)

File: trinity_baseline_1d4a532d.uf2
md5:  1d4a532da94caa71646764a99f1db292

Source: commit aa608d9 ("Add 'mon' command to watch CPU_RESET/POST pins live", 2026-11-03).
The binary matches a rebuild of that commit byte-for-byte except for the build-date string.

Results on the test console:
- 17 of 30 boots on attempt 1 (57%), 29 of 30 within 2 attempts.

SAFETY WARNING: this build DRIVES the reset line HIGH at 3.3 V (glitch.pio uses side-set
on pin 26 for every instruction). That is NOT open-drain, so it breaks the rule in
docs/SAFETY.md, which was written after this build (commit 6ca68d8, 2026-11-04).
Open-drain reset never booted on Trinity in our tests, so this build's driven reset is
likely why it works. It is the same approach as the original PicoRGH. Risk on Trinity
(Slim, 3.3 V reset point) is unmeasured for long-term use. Do not treat this file as
meeting the open-drain safety rule.

Do not change this file. Any new work goes in a separate build and must beat this baseline
on a 20-boot run before it replaces it.
