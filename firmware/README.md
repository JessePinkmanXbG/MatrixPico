# Shipped firmware

| file | what it is |
|------|------------|
| `trinity.uf2` | Bench-validated Trinity firmware ("test7", source tag `trinity-test7` = commit feb465e, built with `MP_DEFS="MP_SYS_CLK_KHZ=200000"`). 10/10 first-attempt boots on the test console. |

Files here are frozen once shipped: build, test on the console, then replace.
Do not recompile them in CI - a different build changed boot behaviour.
