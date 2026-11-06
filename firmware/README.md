# Shipped firmware

| file | what it is |
|------|------------|
| `trinity.uf2` | Bench-validated Trinity firmware ("test7", source tag `trinity-test7` = commit feb465e, built with `MP_DEFS="MP_SYS_CLK_KHZ=200000"`). 10/10 first-attempt boots on the test console. |

Files here are frozen once shipped: build, test on the console, then replace.
Do not recompile them in CI - a different build changed boot behaviour.


## trinity.uf2 (current)

Built from branch `track/judge` (commit e4d702a source tree; binary committed as-is, do not
recompile in CI). RGH1.2 test7 glitch path, unchanged, plus:
- adaptive tracking of delay_to_rst (follows the cold/warm drift; value kept in flash),
- judge: 4 s window, >=3 POST edges, early green after flicker + 0.5 s,
- diagnostic commands `tune`, `tunelog`, `tunecenter`, `tunestop`, `hist`, `opt` (leave alone in normal use).
Bench (Trinity): cold 1.4, warm 1.2 attempts/boot, no false reds. md5 e4d702a7eb78f5a7413dcb2236c184f1.


## trinity.uf2 (current — track4)

Built from branch `track/stale-miss`. Same test7 glitch path, adaptive tracking
of delay_to_rst, 4 s / >=3-edge judge, plus: misses older than 20 s are forgotten
so a power-down "attempt" can't push the next boot off-centre.
Bench (Trinity, cold): ~1.75 attempts/boot over 20 boots, centre holds at
7288004, no false reds. md5 b83504339951397b69e522635b267eb5.
