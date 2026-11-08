# Trinity baseline (locked)

File: trinity_baseline_1d4a532d.uf2
md5:  1d4a532da94caa71646764a99f1db292

Our best-measured Trinity firmware. Booted reliably on the test console:
- 17 of 30 boots on attempt 1 (57%), 29 of 30 within 2 attempts.

Contains none of the later features (no self-cal, autotune, probe, timeline,
or forced reset). Source commit not identified; it predates the current branches.

Do not change this file. Any new work goes in a separate build and must beat
this baseline on a 20-boot run before it replaces it.
