# EXT_CLK first-light builds (UNTESTED on hardware)

zephyr_firstlight1.uf2 - active profile `zephyr` (r_end 118008, pulse 2, 192 MHz)
xenon_firstlight1.uf2  - active profile `xenon`  (r_end 118002, pulse 2, 192 MHz)

Source: research/extclk @ b0b257e + MP_ACTIVE default-profile switch (branch zephyr/first-light).
EXT_CLK engine only ever ran in a PIO emulator. Timing values are seeds from Octal450's VHDL.
Reset is pure open-drain (only pulled low, never driven high): the Zephyr/Xenon reset net is 1.1 V.
NOT a proven baseline - there is none yet. First session: `mon` to confirm POST + reset-sense read,
then `sweep 2 117990 118030 1`. See docs/ZEPHYR_EXTCLK.md.
